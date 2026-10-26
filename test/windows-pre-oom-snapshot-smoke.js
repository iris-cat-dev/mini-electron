'use strict';

// Run in a fresh native Windows mini-electron process. The host-only override
// exercises the real main-isolate V8 profiler with a ~64 MiB retained heap.
// --then-oom also verifies that a later fatal V8 OOM still produces a minidump.
// --unwritable-dump-directory verifies a failed attempt is not retried.
const assert = require('node:assert/strict');
const { once } = require('node:events');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');

process.env.MINI_ELECTRON_PRE_OOM_SNAPSHOT_PERCENT = '1';
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-pre-oom-'));
const dumpDirectory = path.join(temporary, 'dumps');
fs.mkdirSync(dumpDirectory);
app.setPath('userData', path.join(temporary, 'profile'));
app.setPath('crashDumps', dumpDirectory);
const unwritable = process.argv.includes('--unwritable-dump-directory');
const thenOom = process.argv.includes('--then-oom');
const storageBudget = process.argv.includes('--storage-budget');
const nearHeapLimit = process.argv.includes('--near-heap-limit');
assert.ok(Number(storageBudget) + Number(unwritable) + Number(nearHeapLimit) <= 1,
  'choose one capture refusal case');
if (storageBudget) {
  for (let index = 0; index < 4; index++)
    fs.writeFileSync(path.join(dumpDirectory, `renderer-pre-oom-existing-${index}.heapsnapshot`),
      'Existing snapshots must be preserved, never removed to make room.');
}
if (unwritable) {
  fs.rmdirSync(dumpDirectory);
  fs.writeFileSync(dumpDirectory, 'A file blocks this trusted dump directory.');
}
const sleep = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
const snapshotFiles = () => fs.readdirSync(dumpDirectory)
  .filter(name => name.startsWith('renderer-pre-oom-'));
let window;
let finished = false;
app.on('window-all-closed', () => {});
const timeout = setTimeout(() => finish(new Error('Pre-OOM snapshot smoke timed out')), 180000);

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(timeout);
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: real once-only main-isolate pre-OOM heap snapshot and truthful status');
  app.exit(error ? 1 : 0);
}

function verifyMarkerGraph(snapshot) {
  const meta = snapshot.snapshot.meta;
  const nodeWidth = meta.node_fields.length;
  const nodeType = meta.node_fields.indexOf('type');
  const nodeName = meta.node_fields.indexOf('name');
  const edgeCount = meta.node_fields.indexOf('edge_count');
  const objectType = meta.node_types[nodeType].indexOf('object');
  const markers = new Set();
  for (let node = 0; node < snapshot.nodes.length; node += nodeWidth) {
    if (snapshot.nodes[node + nodeType] === objectType
        && snapshot.strings[snapshot.nodes[node + nodeName]] === 'PreOomSnapshotMarker')
      markers.add(node);
  }
  assert.ok(markers.size > 0, 'the actual retained constructor instance must be a graph node');
  const edgeWidth = meta.edge_fields.length;
  const edgeType = meta.edge_fields.indexOf('type');
  const edgeName = meta.edge_fields.indexOf('name_or_index');
  const edgeTarget = meta.edge_fields.indexOf('to_node');
  const propertyType = meta.edge_types[edgeType].indexOf('property');
  let retainedReference = false;
  let edge = 0;
  for (let node = 0; node < snapshot.nodes.length; node += nodeWidth) {
    for (let index = 0; index < snapshot.nodes[node + edgeCount]; index++, edge += edgeWidth) {
      if (snapshot.edges[edge + edgeType] === propertyType
          && snapshot.strings[snapshot.edges[edge + edgeName]] === 'retainedMarker'
          && markers.has(snapshot.edges[edge + edgeTarget]))
        retainedReference = true;
    }
  }
  assert.ok(retainedReference, 'the retainedMarker property must reference the captured instance');
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  window = new BrowserWindow({ show: false,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  const contents = window.webContents;
  contents.on('render-process-gone', (_, details) => {
    console.log(JSON.stringify({ event: 'render-process-gone', details }));
  });
  await window.loadURL('data:text/html,<title>real pre-OOM snapshot smoke</title>');
  const rendererPid = contents.getProcessId();
  await contents.executeJavaScript(`(() => {
    class PreOomSnapshotMarker {
      constructor() { this.payload = new Array(8 * 1024 * 1024).fill(37.5); }
    }
    globalThis.__preOomSnapshotSmoke = { retainedMarker: new PreOomSnapshotMarker() };
    if (${nearHeapLimit}) {
      // A single task crosses the trigger before yielding to the monitor.
      // The ready capability must not allow profiling a now-near-limit heap.
      globalThis.__preOomSnapshotSmoke.pressure = [];
      for (let index = 0; index < 110; index++)
        __preOomSnapshotSmoke.pressure.push(new Array(8 * 1024 * 1024).fill(53));
    }
    return true;
  })()`);

  let persisted;
  if (unwritable) {
    // Allow the request, host failure, and renderer sampling to settle. Repairing
    // the destination afterward must not turn the failed one-shot into a retry.
    await sleep(5000);
    fs.unlinkSync(dumpDirectory);
    fs.mkdirSync(dumpDirectory);
  } else {
    for (let attempt = 0; attempt < 450; attempt++) {
      const reports = snapshotFiles().filter(name => name.endsWith('.json'));
      for (const report of reports) {
        const value = JSON.parse(fs.readFileSync(path.join(dumpDirectory, report), 'utf8'));
        if (['succeeded', 'failed', 'skipped', 'unavailable'].includes(value.preOomHeapSnapshot.status))
          persisted = value;
      }
      if (persisted) break;
      await sleep(100);
    }
    assert.ok(persisted, 'the host must persist terminal status before renderer termination');
    const status = persisted.preOomHeapSnapshot;
    assert.equal(persisted.rendererPid, rendererPid);
    assert.equal(status.status, storageBudget || nearHeapLimit ? 'skipped' : 'succeeded',
      JSON.stringify(status));
    assert.equal(status.thresholdPercent, 1);
    assert.ok(status.triggerUsedHeapBytes >= status.triggerHeapLimitBytes / 100 - 65);
    if (storageBudget || nearHeapLimit) {
      assert.equal(status.reason, storageBudget ? 'storage-budget' : 'insufficient-headroom');
      assert.ok(status.error > 0);
      assert.equal(status.path, undefined);
      assert.equal(status.serializedBytes, 0);
    } else {
      assert.equal(status.error, 0);
      assert.ok(status.serializedBytes > 0 && status.serializedBytes <= 2 * 1024 ** 3);
      assert.equal(fs.statSync(status.path).size, status.serializedBytes);
      verifyMarkerGraph(JSON.parse(fs.readFileSync(status.path, 'utf8')));
    }
  }

  await contents.executeJavaScript('globalThis.__preOomSnapshotSmoke.more = new Array(1024 * 1024).fill(53); true');
  await sleep(3500);
  const files = snapshotFiles();
  assert.equal(files.filter(name => name.endsWith('.heapsnapshot')).length,
    storageBudget ? 4 : unwritable || nearHeapLimit ? 0 : 1,
    'a renderer must never retry its snapshot attempt');
  assert.equal(files.some(name => name.endsWith('.part')), false);
  if (unwritable) {
    assert.equal(files.length, 0, 'repairing the destination must not retry even its failed sidecar');
    fs.rmdirSync(dumpDirectory);
    fs.writeFileSync(dumpDirectory, 'Still unwritable at process exit.');
  }

  const gone = once(contents, 'render-process-gone');
  if (thenOom) {
    contents.executeJavaScript(`(() => {
      globalThis.__preOomLaterOom = [];
      for (let index = 0; index < 160; index++)
        globalThis.__preOomLaterOom.push(new Array(8 * 1024 * 1024).fill(71));
      throw new Error('Expected V8 OOM');
    })()`).catch(() => {});
  } else {
    process.kill(rendererPid);
  }
  const [, details] = await gone;
  const status = details.preOomHeapSnapshot;
  assert.ok(status, 'render-process-gone must include the snapshot result');
  assert.equal(status.status, unwritable ? 'failed'
    : storageBudget || nearHeapLimit ? 'skipped' : 'succeeded');
  if (unwritable) {
    assert.ok(status.error > 0);
    assert.equal(status.reason, 'file-io');
    assert.equal(status.path, undefined);
  } else {
    assert.equal(status.path, persisted.preOomHeapSnapshot.path);
    assert.ok(fs.existsSync(status.diagnosticsPath));
    assert.equal(snapshotFiles().some(name => name.endsWith('.part')), false);
  }
  if (thenOom) {
    assert.equal(details.reason, 'oom');
    assert.equal(details.crashCaptureStatus, 'captured');
    assert.equal(details.exceptionCode, 0xe0000008);
    if (!unwritable) {
      assert.equal(details.crashDumpError, undefined);
      assert.equal(fs.readFileSync(details.crashDumpPath).toString('ascii', 0, 4), 'MDMP');
    }
  }
  const report = {
    rendererPid, thenOom, unwritable, storageBudget, nearHeapLimit,
    preOomHeapSnapshot: status, memory: details.memory,
    v8: details.v8, crashCaptureStatus: details.crashCaptureStatus,
  };
  if (process.env.MINI_FIX_RESULT_DIR) {
    const mode = unwritable ? 'unwritable' : storageBudget ? 'storage-budget'
      : nearHeapLimit ? 'near-heap-limit' : thenOom ? 'then-oom' : 'success';
    fs.writeFileSync(path.join(process.env.MINI_FIX_RESULT_DIR, `pre-oom-${mode}-report.json`),
      JSON.stringify(report, null, 2) + '\n');
  }
  console.log(JSON.stringify(report));
  finish();
}).catch(finish);
