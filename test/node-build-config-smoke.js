'use strict';

// Run in both application mode (--app=test/node-build-config-smoke.js) and
// Node mode (ELECTRON_RUN_AS_NODE=1 <runtime> test/node-build-config-smoke.js).
// Uses actual heap-snapshot object sizes and numeric representations, not
// generated source text or a platform-specific expected configuration.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { writeHeapSnapshot } = require('node:v8');
const { Worker, isMainThread, parentPort } = require('node:worker_threads');

const configurationFields = [
  'v8_enable_pointer_compression',
  'v8_enable_31bit_smis_on_64bit_arch',
  'v8_enable_sandbox',
];

function checkFrozen(value) {
  if (value === null || typeof value !== 'object') return;
  assert.ok(Object.isFrozen(value), 'every process.config object is frozen');
  for (const child of Object.values(value)) checkFrozen(child);
}

function inspectBuild() {
  checkFrozen(process.config);
  const configuration = {};
  for (const field of configurationFields) {
    const value = process.config.variables[field];
    assert.ok(value === 0 || value === 1, `${field} is a numeric build flag`);
    configuration[field] = value;
  }

  class MiniElectronBuildLayoutProbe {}
  const probe = new MiniElectronBuildLayoutProbe();
  probe.shortSlots = new Array(256).fill(probe);
  probe.longSlots = new Array(512).fill(probe);
  // The leading object forces tagged (not unboxed-double) elements. Parsing
  // at runtime avoids compiler constant-pool HeapNumbers for small integers.
  probe.integers = [{}];
  for (const value of ['1073741823', '1073741824', '-1073741824', '-1073741825', '2147483647', '2147483648']) {
    probe.integers.push(Number.parseInt(value, 10));
  }
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-node-layout-'));
  globalThis.__miniElectronBuildLayoutProbe = probe;
  let snapshot;
  try {
    const filename = path.join(temporary, 'layout.heapsnapshot');
    writeHeapSnapshot(filename, { exposeNumericValues: true });
    snapshot = JSON.parse(fs.readFileSync(filename, 'utf8'));
  } finally {
    delete globalThis.__miniElectronBuildLayoutProbe;
    fs.rmSync(temporary, { recursive: true, force: true });
  }

  const { nodes, edges, strings } = snapshot;
  const meta = snapshot.snapshot.meta;
  const nodeWidth = meta.node_fields.length;
  const edgeWidth = meta.edge_fields.length;
  const nodeType = meta.node_fields.indexOf('type');
  const nodeName = meta.node_fields.indexOf('name');
  const nodeSize = meta.node_fields.indexOf('self_size');
  const nodeEdges = meta.node_fields.indexOf('edge_count');
  const edgeType = meta.edge_fields.indexOf('type');
  const edgeName = meta.edge_fields.indexOf('name_or_index');
  const edgeTarget = meta.edge_fields.indexOf('to_node');
  const nodeTypes = meta.node_types[nodeType];
  const edgeTypes = meta.edge_types[edgeType];
  const edgeOffsets = new Map();
  let offset = 0;
  let probeNode;
  for (let node = 0; node < nodes.length; node += nodeWidth) {
    edgeOffsets.set(node, offset);
    offset += nodes[node + nodeEdges] * edgeWidth;
    if (nodeTypes[nodes[node + nodeType]] === 'object' &&
        strings[nodes[node + nodeName]] === 'MiniElectronBuildLayoutProbe') {
      assert.equal(probeNode, undefined, 'unique layout probe');
      probeNode = node;
    }
  }
  assert.notEqual(probeNode, undefined, 'native snapshot contains the live probe');
  function target(node, type, name) {
    const begin = edgeOffsets.get(node);
    const end = begin + nodes[node + nodeEdges] * edgeWidth;
    for (let edge = begin; edge < end; edge += edgeWidth) {
      const kind = edgeTypes[edges[edge + edgeType]];
      const label = kind === 'element' || kind === 'hidden'
        ? edges[edge + edgeName] : strings[edges[edge + edgeName]];
      if (kind === type && label === name) return edges[edge + edgeTarget];
    }
    assert.fail(`missing native ${type} edge ${name}`);
  }
  const shortSlots = target(target(probeNode, 'property', 'shortSlots'), 'internal', 'elements');
  const longSlots = target(target(probeNode, 'property', 'longSlots'), 'internal', 'elements');
  const taggedBytes = (nodes[longSlots + nodeSize] - nodes[shortSlots + nodeSize]) / 256;
  const pointerBytes = ['ia32', 'arm'].includes(process.arch) ? 4 : 8;
  assert.ok(taggedBytes === 4 || taggedBytes === 8, 'native tagged-slot width');
  assert.equal(taggedBytes, configuration.v8_enable_pointer_compression ? 4 : pointerBytes,
    'reported pointer compression matches measured native tagged slots');
  if (configuration.v8_enable_pointer_compression) assert.equal(pointerBytes, 8);

  const integers = target(probeNode, 'property', 'integers');
  const has31BitSmis = taggedBytes === 4 || configuration.v8_enable_31bit_smis_on_64bit_arch === 1;
  const expectedSmis = [true, !has31BitSmis, true, !has31BitSmis, !has31BitSmis, false];
  for (let index = 0; index < expectedSmis.length; ++index) {
    const number = target(integers, 'element', index + 1);
    assert.equal(strings[nodes[number + nodeName]], expectedSmis[index] ? 'smi number' : 'heap number',
      `reported SMI layout matches native integer boundary at index ${index}`);
  }
  return { configuration, taggedBytes, has31BitSmis: Boolean(has31BitSmis) };
}

function inspectWorker() {
  return new Promise((resolve, reject) => {
    const worker = new Worker(__filename);
    const timeout = setTimeout(() => {
      worker.terminate();
      reject(new Error('layout worker exceeded its 30-second deadline'));
    }, 30000);
    let report;
    worker.on('message', value => { report = value; });
    worker.on('error', error => {
      clearTimeout(timeout);
      reject(error);
    });
    worker.on('exit', code => {
      clearTimeout(timeout);
      if (code !== 0) return reject(new Error(`layout worker exited with ${code}`));
      if (!report) return reject(new Error('layout worker did not report its native configuration'));
      resolve(report);
    });
  });
}

async function main() {
  const report = inspectBuild();
  assert.deepEqual(await inspectWorker(), report, 'worker and main native build/layout agree');
  if (!process.argv.includes('--node-child')) {
    // Exercise Node-only startup even when this smoke starts as a normal app.
    const output = execFileSync(process.execPath, [__filename, '--node-child'], {
      env: { ...process.env, ELECTRON_RUN_AS_NODE: '1' },
      encoding: 'utf8',
      timeout: 60000,
      killSignal: 'SIGKILL',
    });
    const line = output.split(/\r?\n/).find(value => value.startsWith('NODE_BUILD_CONFIG '));
    assert.ok(line, 'Node-only child reports its native layout');
    assert.deepEqual(JSON.parse(line.slice('NODE_BUILD_CONFIG '.length)), report,
      'Node-only and application startup agree');
  }
  if (process.argv.includes('--node-child')) {
    console.log(`NODE_BUILD_CONFIG ${JSON.stringify(report)}`);
  } else {
    console.log(`PASS: deeply frozen process.config, native tagged slots and SMI boundaries, Node-only startup, and workers ${JSON.stringify(report)}`);
  }
}

if (!isMainThread) {
  try {
    parentPort.postMessage(inspectBuild());
  } catch (error) {
    console.error(error.stack || error);
    process.exitCode = 1;
  }
} else if (Object.hasOwn(process.env, 'ELECTRON_RUN_AS_NODE')) {
  main().catch(error => {
    console.error(error.stack || error);
    process.exitCode = 1;
  });
} else {
  const { app } = require('electron');
  app.whenReady().then(main).then(
    () => app.exit(0),
    error => { console.error(error.stack || error); app.exit(1); },
  );
}
