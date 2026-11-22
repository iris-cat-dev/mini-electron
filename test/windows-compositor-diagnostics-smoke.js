'use strict';

// Requires an interactive Windows desktop and hardware D3D12/DComp.
// out/windows-x64/mini-electron.exe --app=test/windows-compositor-diagnostics-smoke.js
// No capturePage, pixel inspection, forced invalidation, or GPU readback.
const assert = require('node:assert/strict');
const { once } = require('node:events');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-compositor-diagnostics-'));
app.setPath('userData', path.join(temporary, 'profile'));
// Use the same real file-document path as Desktop. A data: document's reload is
// a no-op in both the 0.1.41 baseline and this runtime, so it cannot prove reload.
const fixture = path.join(temporary, 'index.html');
fs.writeFileSync(fixture,
  '<!doctype html><style>html,body{margin:0;width:100%;height:100%;background:rgb(31,73,119);overflow:hidden}</style><div>diagnostics smoke</div>');
// CI qualifies the API and honest unavailability without claiming GPU coverage.
const schemaOnly = process.argv.includes('--schema-only');
if (schemaOnly) app.disableHardwareAcceleration();
const windows = [];
const evidence = {};
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let finished = false;
const deadline = setTimeout(() => finish(new Error('Compositor diagnostics smoke timed out')), 90000);
app.on('window-all-closed', () => {});

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  for (const window of windows) if (!window.isDestroyed()) window.destroy();
  try { fs.rmSync(temporary, { recursive: true, force: true }); } catch {}
  if (error) console.error(error.stack || error, JSON.stringify(evidence));
  else console.log('PASS: compositor diagnostics schema1 ' +
    (schemaOnly ? 'CPU fallback availability (no GPU qualification)' :
      'identities, lifecycle, isolation and no readback') + '\n' + JSON.stringify(evidence));
  app.exit(error ? 1 : 0);
}

function exact(value, label) {
  assert.equal(typeof value, 'string', `${label} must be an exact decimal string`);
  assert.match(value, /^(0|[1-9][0-9]*)$/, label);
  return BigInt(value);
}

function events(records, label) {
  assert.ok(Array.isArray(records) && records.length <= 64, `${label}: bounded event history`);
  let previous = 0n;
  for (const event of records) {
    const seq = exact(event.seq, `${label}.seq`);
    assert.ok(seq > previous, `${label}: ordered, nonduplicated sequence`);
    previous = seq;
    exact(event.timeUs, `${label}.timeUs`);
    exact(event.identity, `${label}.identity`);
    exact(event.sourceId, `${label}.sourceId`);
    exact(event.frameId, `${label}.frameId`);
    assert.equal(typeof event.type, 'string');
    for (const key of ['layerId', 'width', 'height', 'result']) assert.ok(Number.isInteger(event[key]), `${label}.${key}`);
  }
}

function inspect(info, contents, allowPrimedHandoff = false) {
  assert.equal(info.diagnostics?.schemaVersion, 1, 'schema1 capability must be implemented, not just metadata-advertised');
  assert.equal(info.backend, 'd3d12', 'PREREQUISITE: hardware D3D12 compositor required');
  const { producer: p, consumer: c, presenter: d } = info.diagnostics;
  events(p.events, 'producer');
  events(d.events, 'presenter');
  if (!p.available || !c.available || !d.available) return false;
  events(c.events, 'consumer');
  assert.equal(d.physicalScanoutObserved, false, 'Present metadata is not physical scanout evidence');
  assert.equal(p.failed, false);
  assert.equal(p.error, 0);
  assert.equal(p.deviceRemovedReason, 0);
  assert.equal(d.deviceRemovedReason, 0);
  for (const [name, value] of Object.entries({ outputDeviceId: p.outputDeviceId, sourceId: p.sourceId,
    consumerId: c.consumerId, retainedTextureId: c.retainedTextureId, presenterId: d.presenterId }))
    assert.ok(exact(value, name) > 0n, `${name} must identify a real resource lifetime`);
  assert.equal(p.readyValue, String(exact(p.frameId, 'producer.frameId') * 2n));
  assert.equal(c.readyValue, String(exact(c.frameId, 'consumer.frameId') * 2n));
  assert.ok(exact(c.importedFrames, 'consumer.importedFrames') > 0n);
  assert.equal(c.rejectedFrames, '0', 'ordinary valid frames must not be rejected');
  assert.equal(p.readbacks, '0', 'this metadata-only lifecycle exercise must never read GPU pixels');
  const layer = d.layers.find(value => value.layerId === contents.id);
  if (!layer || layer.pendingSwapChainId !== '0' || layer.waiting || layer.priming) return false;
  if (p.publishedSourceId !== c.sourceId || p.publishedFrameId !== c.frameId) return false;
  const frameMatches = layer.presentedSourceId === c.sourceId && layer.presentedFrameId === c.frameId;
  const primedHandoff = layer.presentedSourceId === '0' && layer.presentedFrameId === '0';
  if (!frameMatches && !(allowPrimedHandoff && primedHandoff)) return false;
  assert.equal(c.rendererPid, p.rendererPid, 'consumer belongs to the actual renderer');
  assert.equal(p.publishedReadyValue, c.readyValue);
  assert.equal(c.width, p.width);
  assert.equal(c.height, p.height);
  assert.equal(layer.backingWidth, c.width);
  assert.equal(layer.backingHeight, c.height);
  assert.equal(layer.backingIsGpu, true);
  assert.equal(layer.lastPresentResult, 0);
  assert.ok(exact(layer.swapChainId, 'swapChainId') > 0n);
  assert.ok(exact(layer.visualId, 'visualId') > 0n);
  assert.ok(exact(layer.presentSuccesses, 'presentSuccesses') > 0n);
  assert.equal(layer.presentFailures, '0');
  assert.equal(d.commitFailures, '0');
  assert.ok(exact(d.commitCount, 'commitCount') > 0n);
  assert.ok(Number.isInteger(p.canvasSaveCount) && p.canvasSaveCount > 0);
  for (const bounds of [p.lastDamage, p.canvasClip]) {
    for (const key of ['x', 'y', 'width', 'height']) assert.ok(Number.isInteger(bounds[key]));
  }
  assert.ok(p.events.some(event => event.type === 'backing-created' && event.sourceId === c.sourceId &&
    event.width === c.width && event.height === c.height), 'producer lifecycle identifies the consumed backing');
  assert.ok(c.events.some(event => ['source-imported', 'source-replaced'].includes(event.type) &&
    event.sourceId === c.sourceId), 'consumer lifecycle identifies the imported producer source');
  assert.ok(d.events.some(event => event.type === 'swapchain-created' && event.identity === layer.swapChainId &&
    event.layerId === contents.id && event.width === c.width && event.height === c.height),
  'swapchain creation event describes the actual layer backing');
  assert.ok(d.events.some(event => ['swapchain-attached', 'swapchain-replaced'].includes(event.type) && event.identity === layer.swapChainId &&
    event.layerId === contents.id), 'swapchain attachment event matches the visible native layer');
  return { info, p, c, d, layer };
}

async function settled(contents, predicate = () => true, allowPrimedHandoff = false) {
  const until = Date.now() + 10000;
  let last, previous, stable = 0;
  do {
    last = await contents._getCompositorInfo();
    const state = inspect(last, contents, allowPrimedHandoff);
    if (state && predicate(state)) {
      const token = [state.p.frameId, state.c.frameId, state.d.presentCalls].join(':');
      stable = token === previous ? stable + 1 : 0;
      previous = token;
      if (stable >= 3) return state;
    } else { stable = 0; previous = undefined; }
    await pause(60);
  } while (Date.now() < until);
  assert.fail('compositor metadata did not stabilize at the expected lifecycle boundary: ' + JSON.stringify(last));
}

function identities(state) {
  return { rendererPid: state.p.rendererPid, outputDeviceId: state.p.outputDeviceId, sourceId: state.p.sourceId,
    consumerId: state.c.consumerId, retainedTextureId: state.c.retainedTextureId,
    presenterId: state.d.presenterId, visualId: state.layer.visualId, swapChainId: state.layer.swapChainId };
}

async function resizeAndPaint(window, previous, label) {
  const contents = window.webContents;
  evidence.phase = `${label}-handoff`;
  const bounds = window.getBounds();
  // Resize relative to actual native bounds, not an incidental minimum size.
  window.setSize(bounds.width + 90, bounds.height + 60);
  const handoff = await settled(contents, state => state.p.sourceId !== previous.p.sourceId &&
    state.layer.swapChainId !== previous.layer.swapChainId, true);
  evidence[`${label}Handoff`] = handoff.info;
  if (handoff.layer.presentedFrameId === '0') {
    // kPrimed clears the final-Present cache. Queries must expose this state,
    // not secretly finish it by invalidating or presenting the window.
    const queried = inspect(await contents._getCompositorInfo(), contents, true);
    assert.ok(queried);
    assert.equal(queried.layer.presentedFrameId, '0', 'metadata must preserve a pending final Present');
    assert.equal(queried.p.frameId, handoff.p.frameId, 'a query must not repaint during handoff');
    assert.equal(queried.d.presentCalls, handoff.d.presentCalls, 'a query must not complete the native handoff');
  }
  // A deliberate ordinary page paint is a separate intervention, not a
  // diagnostic side effect or a claim that an idle priming handoff completed.
  evidence.phase = `${label}-explicit-paint`;
  await contents.executeJavaScript("document.body.style.backgroundColor = 'rgb(37,89,141)'; true");
  const painted = await settled(contents, state => BigInt(state.p.frameId) > BigInt(handoff.p.frameId));
  assert.deepEqual(identities(painted), identities(handoff), 'ordinary painting preserves the replacement resources');
  return painted;
}

async function create(x, y) {
  const window = new BrowserWindow({ x, y, width: 340, height: 260, show: false,
    title: 'mini-compositor-diagnostics-smoke' });
  windows.push(window);
  window.webContents.on('render-process-gone', (_, details) => finish(new Error('Unexpected renderer exit: ' + JSON.stringify(details))));
  await window.loadFile(fixture);
  window.showInactive();
  return window;
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32', 'PREREQUISITE: Windows required');
  evidence.phase = schemaOnly ? 'cpu-fallback' : 'initial-frame';
  if (schemaOnly) {
    const window = new BrowserWindow({ show: false });
    windows.push(window);
    await window.loadFile(fixture);
    const info = await window.webContents._getCompositorInfo();
    assert.equal(info.diagnostics?.schemaVersion, 1);
    assert.equal(info.backend, 'software');
    assert.equal(info.diagnostics.producer.available, false,
      'CPU fallback must report the GPU producer as unavailable, not healthy zeros');
    assert.equal(info.diagnostics.consumer.available, false,
      'CPU fallback must not invent an imported GPU texture');
    assert.equal(typeof info.diagnostics.presenter.available, 'boolean');
    assert.equal(info.diagnostics.presenter.physicalScanoutObserved, false);
    evidence.cpuFallback = info;
    return;
  }
  const first = await create(30, 30);
  const initial = await settled(first.webContents);
  evidence.initial = initial.info;
  evidence.phase = 'query-stability';
  const initialIds = identities(initial);
  for (let index = 0; index < 5; ++index) {
    evidence.phase = `query-stability-${index}`;
    const queried = inspect(await first.webContents._getCompositorInfo(), first.webContents);
    assert.ok(queried);
    assert.deepEqual(identities(queried), initialIds, 'metadata queries must not recreate graphics resources');
    assert.equal(queried.p.frameId, initial.p.frameId, 'metadata queries must not repaint a static page');
    assert.equal(queried.d.presentCalls, initial.d.presentCalls, 'metadata queries must not implicitly Present');
    assert.equal(queried.p.readbacks, initial.p.readbacks, 'metadata queries must not read back GPU pixels');
  }

  evidence.phase = 'reload-navigation';
  const loaded = once(first.webContents, 'did-finish-load', { signal: AbortSignal.timeout(10000) });
  first.webContents.reload();
  await loaded;
  evidence.phase = 'reload-frame';
  const reloaded = await settled(first.webContents, state => BigInt(state.p.frameId) > BigInt(initial.p.frameId));
  evidence.reloaded = reloaded.info;
  assert.deepEqual(identities(reloaded), initialIds, 'document reload must preserve unchanged native backing identities');
  assert.equal(reloaded.p.readbacks, initial.p.readbacks, 'reload and diagnostic queries must not read back');

  const resized = await resizeAndPaint(first, reloaded, 'resize');
  evidence.resized = resized.info;
  assert.ok(resized.d.events.some(event => event.type === 'swapchain-replaced' &&
    event.identity === resized.layer.swapChainId && event.layerId === first.webContents.id),
  'resize records replacement of the actual native layer backing');
  assert.ok(resized.c.events.some(event => event.type === 'source-replaced' &&
    event.sourceId === resized.c.sourceId), 'resize records actual consumer source replacement');
  for (const field of ['rendererPid', 'outputDeviceId', 'consumerId', 'presenterId', 'visualId'])
    assert.equal(identities(resized)[field], initialIds[field], `resize preserves ${field}`);
  assert.notEqual(resized.c.retainedTextureId, reloaded.c.retainedTextureId, 'resize replaces the retained consumer texture');
  assert.ok(BigInt(resized.p.backingResizes) > BigInt(reloaded.p.backingResizes));
  assert.notEqual(resized.c.width, reloaded.c.width);
  assert.equal(resized.p.readbacks, initial.p.readbacks, 'resize does not need readback');

  const second = await create(500, 60);
  const other = await settled(second.webContents);
  evidence.other = other.info;
  assert.notEqual(other.c.consumerId, resized.c.consumerId, 'different windows must not share the consumer');
  assert.notEqual(other.d.presenterId, resized.d.presenterId, 'different windows have independent presenters');
  assert.notEqual(other.layer.swapChainId, resized.layer.swapChainId);
  assert.notEqual(`${other.p.rendererPid}:${other.p.outputDeviceId}`, `${resized.p.rendererPid}:${resized.p.outputDeviceId}`);
  const oldContents = first.webContents;
  first.destroy();
  assert.equal(oldContents.isDestroyed(), true);
  await assert.rejects(Promise.resolve().then(() => oldContents._getCompositorInfo()));
  const survivor = await settled(second.webContents);
  evidence.survivor = survivor.info;
  assert.deepEqual(identities(survivor), identities(other), 'destroying another window must not reset the survivor');
  assert.equal(survivor.p.readbacks, other.p.readbacks);
  assert.deepEqual(survivor.d.events, other.d.events, 'destroyed-window history must not contaminate survivor events');
  const survivorResized = await resizeAndPaint(second, other, 'survivorResize');
  evidence.survivorResized = survivorResized.info;
  assert.equal(survivorResized.c.consumerId, other.c.consumerId, 'survivor consumer remains usable after peer destruction');
  assert.equal(survivorResized.p.readbacks, other.p.readbacks);

  const replacement = await create(30, 30);
  const replacementState = await settled(replacement.webContents);
  evidence.replacement = replacementState.info;
  assert.notEqual(replacementState.d.presenterId, resized.d.presenterId);
  const deadResources = new Set([resized.d.presenterId, resized.layer.visualId, resized.layer.swapChainId]);
  assert.ok(replacementState.d.events.every(event => !deadResources.has(event.identity)),
    'a new window, even with HWND reuse, must not inherit dead-window lifecycle history');
}).then(() => finish(), finish);
