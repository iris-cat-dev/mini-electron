'use strict';
// Run a fresh mac native runtime per case: default, --disabled, --invalid,
// --storage-budget, --file-refusal, or --early-close. No profiler mocks.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');
const cases = ['disabled', 'invalid', 'storage-budget', 'file-refusal', 'early-close'];
const selected = cases.filter(name => process.argv.includes(`--${name}`));
assert.ok(selected.length <= 1, 'one case per fresh renderer');
const mode = selected[0] || 'capture';
process.env.MINI_ELECTRON_PRE_OOM_SNAPSHOT_PERCENT = mode === 'disabled' ? '0' : mode === 'invalid' ? '66' : '1';
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-mac-pre-oom-'));
const dumps = path.join(root, 'dumps');
fs.mkdirSync(dumps);
app.setPath('userData', path.join(root, 'profile'));
app.setPath('crashDumps', dumps);
if (mode === 'storage-budget') for (let i = 0; i < 4; ++i)
  fs.writeFileSync(path.join(dumps, `renderer-pre-oom-existing-${i}.heapsnapshot.part`), 'preserve');
if (mode === 'file-refusal') { fs.rmdirSync(dumps); fs.writeFileSync(dumps, 'not a directory'); }
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const files = () => fs.statSync(dumps).isDirectory() ? fs.readdirSync(dumps) : [];
function graph(snapshot) {
  const meta = snapshot.snapshot.meta;
  const width = meta.node_fields.length;
  const type = meta.node_fields.indexOf('type');
  const name = meta.node_fields.indexOf('name');
  const count = meta.node_fields.indexOf('edge_count');
  const object = meta.node_types[type].indexOf('object');
  const markers = new Set();
  for (let n = 0; n < snapshot.nodes.length; n += width)
    if (snapshot.nodes[n + type] === object && snapshot.strings[snapshot.nodes[n + name]] === 'MacPreOomMarker') markers.add(n);
  assert.ok(markers.size > 0, 'actual constructor instance is a heapgraph node');
  const ew = meta.edge_fields.length;
  const et = meta.edge_fields.indexOf('type');
  const en = meta.edge_fields.indexOf('name_or_index');
  const target = meta.edge_fields.indexOf('to_node');
  const property = meta.edge_types[et].indexOf('property');
  let retained = false, edge = 0;
  for (let n = 0; n < snapshot.nodes.length; n += width)
    for (let i = 0; i < snapshot.nodes[n + count]; ++i, edge += ew)
      if (snapshot.edges[edge + et] === property && snapshot.strings[snapshot.edges[edge + en]] === 'retainedMarker' && markers.has(snapshot.edges[edge + target])) retained = true;
  assert.ok(retained, 'retainedMarker property targets the real marker instance');
}
let window, finished = false;
app.on('window-all-closed', () => {});
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(timeout);
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(root, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log(`PASS: mac real pre-OOM ${mode}`);
  app.exit(error ? 1 : 0);
}
const timeout = setTimeout(() => finish(new Error('mac pre-OOM deadline exceeded')), 120000);
app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  window = new BrowserWindow({ show: false, webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  const contents = window.webContents;
  let gone;
  contents.on('render-process-gone', (_, details) => { gone = details; });
  await window.loadURL('data:text/html,<title>mac real snapshot</title>');
  const pid = contents.getProcessId();
  assert.ok(pid > 0 && pid !== process.pid, 'owning renderer is a distinct OS process');
  let cancellationObserved;
  let reservationWatcher;
  if (mode === 'early-close') {
    // Arm before allocating: do not await executeJavaScript, which can return
    // only after an owning-isolate snapshot has already finished.
    cancellationObserved = new Promise((resolve, reject) => {
      reservationWatcher = fs.watch(dumps, () => {
        try {
          const list = files();
          const partial = list.find(n => n.endsWith('.heapsnapshot.part'));
          if (!partial) return;
          assert.equal(list.some(n => n.endsWith('.heapsnapshot')), false,
            'capture must not already be committed when cancellation starts');
          assert.equal(fs.statSync(path.join(dumps, partial)).size, 0,
            'cancel at host reservation, before receiving the real byte stream');
          reservationWatcher.close();
          window.destroy();
          resolve();
        } catch (error) {
          reservationWatcher.close();
          reject(error);
        }
      });
    });
  }
  const allocation = contents.executeJavaScript(`(() => {
    class MacPreOomMarker { constructor() { this.payload = new Array(8 * 1024 * 1024).fill(37.5); } }
    globalThis.__macPreOom = { retainedMarker: new MacPreOomMarker() };
    return true;
  })()`);
  if (mode === 'early-close') {
    allocation.catch(() => {}); // Window destruction rejects pending script work.
    await cancellationObserved;
    await sleep(2500);
    const list = files();
    assert.equal(list.filter(n => n.endsWith('.part')).length, 0, 'close joins cleanup and leaves no partial snapshot/metadata');
    assert.equal(list.filter(n => n.endsWith('.heapsnapshot')).length, 0, 'an early-close attempt cannot finalize');
    const reports = list.filter(n => n.endsWith('.json'));
    assert.equal(reports.length, 1, 'cancellation persists a local report');
    const cancelled = JSON.parse(fs.readFileSync(path.join(dumps, reports[0]), 'utf8')).preOomHeapSnapshot;
    assert.equal(cancelled.status, 'failed', JSON.stringify(cancelled));
    assert.equal(cancelled.reason, 'renderer-stopped');
    assert.equal(cancelled.path, undefined);
    assert.ok(cancelled.error > 0);
    // Deleting the profile after close must not race a later host sidecar writer.
    fs.rmSync(dumps, { recursive: true });
    await sleep(500);
    assert.equal(fs.existsSync(dumps), false);
    finish(); return;
  }
  await allocation;
  if (mode === 'file-refusal') {
    await sleep(4000);
    fs.unlinkSync(dumps); fs.mkdirSync(dumps);
    await contents.executeJavaScript('globalThis.__macPreOom.more = new Array(1024 * 1024).fill(19); true');
    await sleep(3500);
    assert.equal(files().filter(n => n.endsWith('.heapsnapshot')).length, 0, 'failed destination is never retried after repair');
    process.kill(pid, 'SIGKILL');
    for (let i = 0; i < 100 && !gone; ++i) await sleep(100);
    assert.ok(gone, 'actual process exit is independently reported');
    assert.equal(gone.preOomHeapSnapshot.status, 'failed', JSON.stringify(gone));
    assert.equal(gone.preOomHeapSnapshot.reason, 'file-io');
    assert.ok(gone.preOomHeapSnapshot.error > 0);
    assert.equal(gone.preOomHeapSnapshot.path, undefined);
    finish(); return;
  }
  let report;
  for (let i = 0; i < 600 && !report; ++i) {
    for (const n of files().filter(n => n.endsWith('.json'))) {
      const parsed = JSON.parse(fs.readFileSync(path.join(dumps, n), 'utf8'));
      if (['succeeded', 'skipped', 'failed', 'disabled', 'unavailable'].includes(parsed.preOomHeapSnapshot.status)) report = parsed;
    }
    if (!report) await sleep(100);
  }
  assert.ok(report, 'local parseable terminal JSON');
  assert.equal(report.rendererPid, pid);
  const status = report.preOomHeapSnapshot;
  const expected = mode === 'disabled' ? 'disabled' : mode === 'invalid' ? 'unavailable' : mode === 'storage-budget' ? 'skipped' : 'succeeded';
  assert.equal(status.status, expected, JSON.stringify(status));
  if (mode === 'capture') {
    assert.equal(status.error, 0);
    assert.equal(status.heapGraphSource, 'renderer-reported');
    assert.ok(status.mainSampleAgeMilliseconds >= 0 && status.mainSampleAgeMilliseconds <= 2000);
    assert.ok(status.captureUsedHeapBytes > 0 && status.captureHeapLimitBytes > status.captureUsedHeapBytes);
    assert.ok(status.footprintBytes > 0 && status.systemAvailableBytes > 0);
    assert.ok(status.serializedBytes > 0 && status.serializedBytes <= 2 * 1024 ** 3);
    assert.equal(fs.statSync(status.path).size, status.serializedBytes);
    assert.equal(fs.statSync(status.path).mode & 0o777, 0o600);
    graph(JSON.parse(fs.readFileSync(status.path, 'utf8')));
  } else {
    assert.equal(status.path, undefined);
    if (mode === 'invalid') assert.equal(status.reason, 'invalid-configuration');
    if (mode === 'storage-budget') assert.equal(status.reason, 'storage-budget');
  }
  const before = files().sort();
  await contents.executeJavaScript('globalThis.__macPreOom.more = new Array(1024 * 1024).fill(19); true');
  await sleep(3500);
  assert.deepEqual(files().sort(), before, 'once only including terminal refusal/configuration');
  assert.equal(files().filter(n => n.endsWith('.part')).length, mode === 'storage-budget' ? 4 : 0);
  process.kill(pid, 'SIGKILL');
  for (let i = 0; i < 100 && !gone; ++i) await sleep(100);
  assert.ok(gone, 'actual OS exit event arrives independently of snapshot');
  assert.notEqual(gone.reason, 'oom', 'signal exit is not fabricated as V8 OOM');
  assert.equal(gone.preOomHeapSnapshot.status, expected);
  assert.deepEqual(gone.preOomHeapSnapshot, status,
    'exit details and sidecar use the same frozen host result');
  assert.equal(files().filter(n => n.endsWith('.dmp')).length, 0, 'no fabricated minidump');
  finish();
}).catch(finish);
