'use strict';
// Run with out/windows-x64/runtime-dist/electron.exe test/windows-restore-presentation-smoke.js
// on an interactive Windows desktop. The native helper samples the composited desktop,
// not capturePage or a window DC, throughout each restore and for 1.6 seconds afterward.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserView, BrowserWindow, screen } = require('electron');
const { startNativeInput } = require('./windows-native-input-driver');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-windows-restore-presentation-'));
const driverTitle = `mini-windows-restore-driver-${process.pid}`;
const artifacts = process.env.MINI_NATIVE_INPUT_ARTIFACTS;
const artifactStem = `windows-restore-presentation-${process.pid}`;
const pageColor = [23, 91, 149];
const viewColor = [202, 73, 31];
const underlayColor = [107, 40, 124];
const viewBounds = { x: 330, y: 125, width: 260, height: 190 };
const evidence = { cases: [] };
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let driver, underlay, target, targetView, finished = false;
const deadline = setTimeout(() => finish(new Error('Windows restore presentation smoke timed out')), 120000);
app.on('window-all-closed', () => {});

async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (error && driver && target && !target.isDestroyed() && !target.isMinimized() && artifacts) {
    try {
      fs.mkdirSync(artifacts, { recursive: true });
      await driver.request('focus', { title: target.getTitle() });
      await driver.request('capture', {
        title: target.getTitle(), path: path.join(artifacts, `${artifactStem}-failure.png`),
      });
    } catch (diagnosticError) {
      evidence.diagnosticError = String(diagnosticError.stack || diagnosticError);
    }
  }
  evidence.passed = !error;
  evidence.error = error ? String(error.stack || error) : null;
  if (artifacts) {
    try {
      fs.mkdirSync(artifacts, { recursive: true });
      fs.writeFileSync(path.join(artifacts, `${artifactStem}.json`), JSON.stringify(evidence, null, 2));
    } catch (artifactError) { error = error || artifactError; }
  }
  if (driver) {
    try { await driver.close(); }
    catch (cleanupError) { error = error || cleanupError; }
  }
  if (target && !target.isDestroyed()) target.destroy();
  if (underlay && !underlay.isDestroyed()) underlay.destroy();
  try { fs.rmSync(temporary, { recursive: true, force: true }); }
  catch (cleanupError) { error = error || cleanupError; }
  if (error) console.error(error.stack || error);
  else console.log('PASS: Windows normal/maximized native-frame and hidden-titlebar restores immediately present retained webpage and BrowserView pixels');
  app.exit(error ? 1 : 0);
}

async function eventually(read, predicate, label, timeout = 6000) {
  const until = Date.now() + timeout;
  let value;
  do {
    value = await read();
    if (predicate(value)) return value;
    await pause(25);
  } while (Date.now() < until);
  assert.fail(`${label}: ${JSON.stringify(value)}`);
}

const request = (action, values = {}) => driver.request(action, {
  title: target ? target.getTitle() : driverTitle,
  ...values,
});
async function rendererValue(contents, expression) {
  return JSON.parse(await contents.executeJavaScript(`JSON.stringify(${expression})`));
}
const delta = (actual, expected) => Math.max(...actual.map((value, index) => Math.abs(value - expected[index])));
const snapshot = counts => Object.fromEntries(Object.entries(counts).map(([name, count]) => [name, count]));
const difference = (after, before) => Object.fromEntries(Object.keys(after).map(name => [name, after[name] - before[name]]));

function physicalSamples(geometry) {
  const scale = geometry.dpi / 96;
  return [
    { name: 'webpage', x: Math.round(geometry.clientX + 90 * scale),
      y: Math.round(geometry.clientY + 105 * scale), rgb: pageColor },
    { name: 'BrowserView', x: Math.round(geometry.clientX + (viewBounds.x + 90) * scale),
      y: Math.round(geometry.clientY + (viewBounds.y + 90) * scale), rgb: viewColor },
  ];
}

async function expectNativePixels(samples, label) {
  await request('focus');
  for (const sample of samples) {
    await eventually(
      () => request('sample', { x: sample.x, y: sample.y, width: 1, height: 1 }),
      colors => colors.length && delta(colors[0].rgb, sample.rgb) <= 6,
      `${label} ${sample.name} native pixel`,
    );
  }
}

function assertPresentation(result, samples, label) {
  assert.ok(result.durationMs >= 1500, `${label}: sampled only ${result.durationMs}ms`);
  assert.ok(result.frames >= 10, `${label}: too few native samples: ${result.frames}`);
  assert.ok(result.firstStablePresentedMs >= 0 && result.firstStablePresentedMs < 650,
    `${label}: first stable presentation was ${result.firstStablePresentedMs}ms`);
  assert.ok(result.longestMissingMs < 650,
    `${label}: missing presentation lasted ${result.longestMissingMs}ms`);
  assert.ok(result.longestWhiteMs < 250,
    `${label}: white presentation lasted ${result.longestWhiteMs}ms`);
  assert.equal(result.missingAfterStableFrames, 0, `${label}: presentation regressed after becoming stable`);
  assert.equal(result.whiteAfterStableFrames, 0, `${label}: white flash followed stable presentation`);
  assert.equal(result.minimized, false, `${label}: native window remained minimized`);
  assert.equal(result.foreground, true, `${label}: restore did not reactivate the target`);
  assert.equal(result.points.length, samples.length);
  result.points.forEach((point, index) => {
    assert.deepEqual(point.rgb, samples[index].rgb, `${label}: helper expected color`);
    assert.ok(delta(point.firstRgb, underlayColor) <= 6,
      `${label}: minimized underlay pixel ${JSON.stringify(point.firstRgb)}`);
    assert.ok(point.matchingFrames > 0, `${label}: ${samples[index].name} was never presented`);
    assert.equal(point.whiteFrames, 0,
      `${label}: ${samples[index].name} exposed white in ${point.whiteFrames} sampled frames`);
    assert.ok(delta(point.lastRgb, samples[index].rgb) <= 6,
      `${label}: final ${samples[index].name} pixel ${JSON.stringify(point.lastRgb)}`);
  });
}

async function restoreCycle(kind, mode, trigger, counts) {
  const label = `${kind} ${mode} ${trigger}`;
  await request('focus');
  await eventually(() => target.isFocused(), value => value, `${label} initial focus`);
  const beforeEvents = snapshot(counts);
  const beforeBounds = target.getBounds();
  const beforeContent = target.getContentSize();
  const beforeGeometry = await request('geometry');
  const beforeRenderer = await rendererValue(target.webContents,
    '({width:innerWidth,height:innerHeight,resizes:window.restoreProbe.resizes})');
  const beforeView = await rendererValue(targetView.webContents, '({width:innerWidth,height:innerHeight})');
  const samples = physicalSamples(beforeGeometry);
  await expectNativePixels(samples, `${label} baseline`);

  target.minimize();
  await eventually(async () => ({ native: await request('geometry'), javascript: target.isMinimized() }),
    state => state.native.minimized && state.javascript, `${label} minimize`);
  await pause(300);
  assert.deepEqual(target.getContentSize(), beforeContent, `${label}: minimize clobbered content size`);
  assert.deepEqual(await rendererValue(target.webContents, '[innerWidth,innerHeight]'),
    [beforeRenderer.width, beforeRenderer.height], `${label}: minimize resized the webpage viewport`);
  assert.deepEqual(await rendererValue(targetView.webContents, '[innerWidth,innerHeight]'),
    [beforeView.width, beforeView.height], `${label}: minimize resized the BrowserView viewport`);

  const sampleOptions = { samples, duration: 1600, stable: 40, tolerance: 6 };
  let presentation;
  if (trigger === 'native-ShowWindow') {
    presentation = await request('restorePresentation', sampleOptions);
  } else {
    await request('restorePresentationBegin', sampleOptions);
    target.restore();
    presentation = await request('restorePresentationEnd');
  }
  await eventually(async () => ({ native: await request('geometry'), javascript: target.isMinimized(), focused: target.isFocused() }),
    state => !state.native.minimized && !state.javascript && state.focused,
    `${label} completed restore`);

  assertPresentation(presentation, samples, label);
  assert.equal(presentation.maximized, mode === 'maximized', `${label}: native maximize state`);
  assert.deepEqual(target.getBounds(), beforeBounds, `${label}: final bounds`);
  assert.deepEqual(target.getContentSize(), beforeContent, `${label}: final content size`);
  assert.equal(presentation.clientX, beforeGeometry.clientX, `${label}: final client x`);
  assert.equal(presentation.clientY, beforeGeometry.clientY, `${label}: final client y`);
  assert.equal(presentation.clientWidth, beforeGeometry.clientWidth, `${label}: final client width`);
  assert.equal(presentation.clientHeight, beforeGeometry.clientHeight, `${label}: final client height`);
  assert.deepEqual(await rendererValue(target.webContents, '[innerWidth,innerHeight]'),
    [beforeRenderer.width, beforeRenderer.height], `${label}: final webpage viewport`);
  assert.deepEqual(await rendererValue(targetView.webContents, '[innerWidth,innerHeight]'),
    [beforeView.width, beforeView.height], `${label}: final BrowserView viewport`);
  const afterRenderer = await rendererValue(target.webContents, 'window.restoreProbe.resizes');
  assert.equal(afterRenderer, beforeRenderer.resizes, `${label}: unchanged viewport emitted a DOM resize`);

  const events = difference(counts, beforeEvents);
  assert.equal(events.minimize, 1, `${label}: minimize lifecycle count`);
  assert.equal(events.restore, 1, `${label}: restore lifecycle count`);
  assert.equal(events.maximize, 0, `${label}: duplicate maximize lifecycle count`);
  assert.equal(events.unmaximize, 0, `${label}: spurious unmaximize lifecycle count`);
  assert.equal(events.show, 0, `${label}: restore must not duplicate show`);
  assert.equal(events.hide, 0, `${label}: minimize must not emit hide`);
  evidence.cases.push({ label, beforeBounds, beforeContent, beforeGeometry, events, presentation });
}

async function runWindowKind(kind, location) {
  const hidden = kind === 'opaque-hidden-titlebar';
  const title = `mini-windows-restore-${kind}-${process.pid}`;
  target = new BrowserWindow({
    x: location.x, y: location.y, width: location.width, height: location.height,
    minWidth: 0, minHeight: 0, show: false, title,
    ...(hidden ? {
      frame: false,
      titleBarStyle: 'hidden',
      titleBarOverlay: { color: '#123047', symbolColor: '#ffffff', height: 46 },
    } : {}),
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  targetView = new BrowserView({
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  target.addBrowserView(targetView);
  targetView.setBounds(viewBounds);
  const counts = { minimize: 0, restore: 0, maximize: 0, unmaximize: 0,
    focus: 0, blur: 0, resize: 0, show: 0, hide: 0 };
  for (const name of Object.keys(counts)) target.on(name, () => { counts[name]++; });

  await Promise.all([
    target.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><meta charset="utf-8"><title>${title}</title>
      <style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:rgb(${pageColor.join(',')})}</style>
      <script>window.restoreProbe={resizes:0};addEventListener('resize',()=>restoreProbe.resizes++);</script>`)),
    targetView.webContents.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><meta charset="utf-8">
      <style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:rgb(${viewColor.join(',')})}</style>`)),
  ]);
  target.show();
  target.focus();
  await eventually(() => target.isFocused(), value => value, `${kind} initial focus`);
  await pause(120);

  await restoreCycle(kind, 'normal', 'native-ShowWindow', counts);
  await restoreCycle(kind, 'normal', 'javascript-restore', counts);

  const beforeMaximize = counts.maximize;
  target.maximize();
  await eventually(async () => ({ native: await request('geometry'), javascript: target.isMaximized() }),
    state => state.native.maximized && state.javascript, `${kind} maximize`);
  assert.equal(counts.maximize, beforeMaximize + 1, `${kind}: initial maximize lifecycle count`);
  await pause(120);
  await restoreCycle(kind, 'maximized', 'native-ShowWindow', counts);
  await restoreCycle(kind, 'maximized', 'javascript-restore', counts);

  target.destroy();
  target = null;
  targetView = null;
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  const work = screen.getPrimaryDisplay().workArea;
  const width = Math.min(760, work.width - 80);
  const height = Math.min(540, work.height - 80);
  assert.ok(width >= 650 && height >= 440, 'interactive desktop is too small for restore sampling fixture');
  const location = { x: work.x + Math.floor((work.width - width) / 2),
    y: work.y + Math.floor((work.height - height) / 2), width, height };
  driver = await startNativeInput(temporary, driverTitle);
  underlay = new BrowserWindow({ x: work.x, y: work.y, width: work.width, height: work.height,
    minWidth: 0, minHeight: 0, frame: false, show: false,
    resizable: false, minimizable: false, maximizable: false, title: `${driverTitle}-underlay`,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await underlay.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><style>
    html,body{margin:0;width:100%;height:100%;background:rgb(${underlayColor.join(',')})}</style>`));
  underlay.show();
  await runWindowKind('native-frame', location);
  await runWindowKind('opaque-hidden-titlebar', location);
}).then(() => finish(), finish);
