'use strict';

// /path/to/mini-electron --app=test/macos-gpu-compositing-smoke.js
// Requires generated headers, Xcode tools, an unlocked macOS graphical session
// and genuine Metal output. Captures only this process's composed owned windows.
// CPU fallback is supported by the product, but is INCOMPLETE GPU coverage here.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, BrowserView, screen } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-gpu-compositing-'));
const windows = [];
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const base = [32, 64, 96, 255];
const initial = [[255, 0, 0, 255], base, [16, 32, 176, 255], [128, 0, 0, 255], [128, 0, 0, 255], [249, 205, 37, 255]];
const later = [[0, 255, 0, 255], base, [24, 48, 136, 255], [191, 0, 0, 255], [64, 0, 0, 255], [249, 205, 37, 255]];
const points = [{ x: 90, y: 40 }, { x: 40, y: 40 }, { x: 200, y: 40 },
  { x: 320, y: 40 }, { x: 440, y: 40 }, { x: 550, y: 330 }];
let native, view, finished = false;
let serial = 0;
const watchdog = setTimeout(() => finish(new Error('GPU composition smoke exceeded 90s watchdog')), 90000);
app.on('window-all-closed', () => {});
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  try {
    if (native) native.resumeRenderer();
    for (const window of windows.reverse()) if (!window.isDestroyed()) window.destroy();
    if (view && !view.webContents.isDestroyed()) view.webContents.destroy();
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
    if (error) console.error(error.stack || error);
    else console.log('PASS: genuine native Metal pixels, composition effects, GPU readback/crop, transitions, paused clipped resize, BrowserView reparent and idle publication stability');
    app.exit(error ? (/INCOMPLETE:/.test(error.message) ? 2 : 1) : 0);
  }
}
function closeColor(actual, expected, tolerance = 10) {
  return actual.length === 4 && actual.every((value, channel) => Math.abs(value - expected[channel]) <= tolerance);
}
function colors(result, expected) {
  return result.samples.length === expected.length && result.samples.every((pixel, index) => closeColor(pixel, expected[index]));
}
async function poll(label, predicate, bound = 5000) {
  const began = Date.now(); let last;
  do {
    last = await predicate();
    if (last === true) return;
    await pause(20);
  } while (Date.now() - began < bound);
  throw new Error(`${label}: no consumer-visible result within ${bound}ms: ${JSON.stringify(last)}`);
}
function frame(fd) {
  const result = native.frames().find(item => item.fd === fd);
  assert.ok(result, `Owned frame ${fd} must remain available`);
  assert.equal(result.outputFormat, 1, 'INCOMPLETE: renderer selected CPU fallback; genuine Metal hardware/output required for this GPU test');
  assert.ok(result.nativeContextId > 0, 'Metal publication must export a real remote CAContext');
  assert.ok(result.presentedGeneration > 0 && result.presentedGeneration % 2 === 0, 'Metal must publish an even, nonzero presented generation');
  assert.ok(result.generation >= result.presentedGeneration && result.generation % 2 === 0);
  assert.ok(result.nativeScaleFactor > 0 && Number.isFinite(result.nativeScaleFactor));
  return result;
}
function newFrame(before) {
  const added = native.frames().filter(item => !before.has(item.fd));
  assert.equal(added.length, 1, 'Each new isolated WebContents must own exactly one new frame mapping');
  return added[0].fd;
}
function create() {
  const area = screen.getPrimaryDisplay().workArea;
  assert.ok(area.width >= 760 && area.height >= 560, 'INCOMPLETE: a display work area of at least 760x560 is required');
  const title = `mini-gpu-${process.pid}-${++serial}`;
  const window = new BrowserWindow({ x: area.x + 20, y: area.y + 20, width: 600, height: 400,
    frame: false, show: true, alwaysOnTop: true, title, backgroundColor: '#f0f0f0',
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  window.webContents.once('render-process-gone', (_, details) =>
    finish(new Error(`GPU renderer exited: ${JSON.stringify(details)}`)));
  windows.push(window);
  return window;
}
async function nativeColors(window, probes, expected, label) {
  let last;
  await poll(label, async () => {
    last = await native.snapshot(window.getTitle(), probes);
    return colors(last, expected) || last;
  });
  console.log(JSON.stringify({ label, ...last }));
  return last;
}
function imageColors(image, logicalWidth, logicalHeight, probes, expected, label) {
  assert.equal(image.isEmpty(), false, `${label}: real explicit GPU readback required`);
  const size = image.getSize();
  assert.ok(size.width > 0 && size.height > 0);
  assert.ok(Math.abs(size.width / logicalWidth - size.height / logicalHeight) < 0.02, `${label}: crop aspect ratio`);
  const decoded = native.imageSamples(image.toPNG(), probes.map(point => ({
    x: Math.floor(point.x * size.width / logicalWidth),
    y: Math.floor(point.y * size.height / logicalHeight),
  })));
  assert.deepEqual({ width: decoded.width, height: decoded.height }, size, `${label}: encoded dimensions`);
  decoded.samples.forEach((rgba, index) =>
    assert.ok(closeColor(rgba, expected[index], 3), `${label}: point ${index} actual=${rgba} expected=${expected[index]}`));
}
async function explicitCaptures(window, expected, label) {
  const full = await window.webContents.capturePage();
  imageColors(full, 600, 400, points, expected, `${label}:full`);
  const crop = { x: 70, y: 20, width: 400, height: 60 };
  const size = full.getSize();
  const image = await window.webContents.capturePage({
    x: Math.round(crop.x * size.width / 600), y: Math.round(crop.y * size.height / 400),
    width: Math.round(crop.width * size.width / 600), height: Math.round(crop.height * size.height / 400),
  });
  imageColors(image, crop.width, crop.height,
    [points[0], points[2], points[3], points[4]].map(point => ({ x: point.x - crop.x, y: point.y - crop.y })),
    [expected[0], expected[2], expected[3], expected[4]], `${label}:crop`);
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const root = path.resolve(__dirname, '..');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'gpu-compositing.node');
  const includes = ['', 'gen', 'third_party/abseil-cpp', 'base/allocator/partition_allocator/src', 'gen/base/allocator/partition_allocator/src', 'third_party/libnode/src'];
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup', '-isysroot', sdk,
    ...includes.map(include => `-I${path.join(root, include)}`), '-framework', 'AppKit', '-framework', 'CoreGraphics',
    '-framework', 'ScreenCaptureKit', '-framework', 'ImageIO',
    path.join(__dirname, 'macos-owned-window-capture.mm'), path.join(__dirname, 'macos-gpu-compositing-helper.mm'), '-o', helper],
  { stdio: 'inherit', timeout: 30000 });
  native = require(helper);
  const before = new Set(native.frames().map(item => item.fd));
  const window = create();
  const title = window.getTitle();
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><title>${title}</title>
    <style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#204060}
    .box{position:absolute;top:20px;width:80px;height:80px}
    #transform{left:20px;width:40px;height:40px;background:red;transform:translateX(60px)}
    #opacity{left:160px;background:blue;opacity:.5}
    #filter{left:280px;background:red;filter:brightness(.5)}
    #under{left:400px;background:red}#backdrop{left:400px;backdrop-filter:brightness(.5)}
    #marker{position:absolute;left:540px;top:320px;width:30px;height:30px;background:#f9cd25}</style>
    <div class="box" id="transform"></div><div class="box" id="opacity"></div><div class="box" id="filter"></div>
    <div class="box" id="under"></div><div class="box" id="backdrop"></div><div id="marker"></div>
    <script>window.nextFrame=()=>{document.getElementById('transform').style.background='lime';
    document.getElementById('opacity').style.opacity='.25';document.getElementById('filter').style.filter='brightness(.75)';
    document.getElementById('backdrop').style.backdropFilter='brightness(.25)'}</script>`));
  await window.webContents.prepareToShow();
  const rootFd = newFrame(before);
  const first = frame(rootFd);
  assert.equal(first.dataSize, 0, 'Normal initial Metal publication must not require an RGBA mirror');
  assert.equal(first.width, Math.round(600 * first.nativeScaleFactor));
  assert.equal(first.height, Math.round(400 * first.nativeScaleFactor));
  await nativeColors(window, points, initial, 'initial real native transform/opacity/filter/backdrop');
  await explicitCaptures(window, initial, 'initial GPU readback');
  await window.webContents.executeJavaScript('nextFrame(); true');
  await nativeColors(window, points, later, 'later real native effects');
  const second = frame(rootFd);
  assert.ok(second.presentedGeneration > first.presentedGeneration, 'Changed real pixels require a later native publication');
  await explicitCaptures(window, later, 'later GPU readback, not stale initial pixels');
  // Let causal capture/paint work settle, then require no idle native publication.
  await pause(300);
  const idle = frame(rootFd).presentedGeneration;
  for (let turn = 0; turn < 8; ++turn) {
    await pause(75);
    assert.equal(frame(rootFd).presentedGeneration, idle, 'Static idle must not publish new GPU frames');
  }
  native.pauseRenderer(window.webContents.getProcessId(), rootFd);
  try {
    const paused = frame(rootFd).presentedGeneration;
    window.setSize(460, 300);
    await pause(120);
    await nativeColors(window, points.slice(0, 5), later.slice(0, 5), 'paused shrink clips without stretching fixed effects');
    window.setSize(700, 480);
    await pause(120);
    const geometryPoints = [{ x: 539, y: 330 }, { x: 541, y: 330 }, { x: 568, y: 330 }, { x: 571, y: 330 },
      { x: 550, y: 319 }, { x: 550, y: 321 }, { x: 550, y: 348 }, { x: 550, y: 351 }, { x: 650, y: 430 }];
    await nativeColors(window, geometryPoints, [base, later[5], later[5], base, base, later[5], later[5], base, [240, 240, 240, 255]],
      'paused enlarge retains original marker size/position and exposes native background');
    assert.equal(frame(rootFd).presentedGeneration, paused, 'Paused producer cannot publish resized content');
  } finally { native.resumeRenderer(); }
  window.setSize(600, 400);
  await nativeColors(window, points, later, 'resumed native layout');

  const beforeView = new Set(native.frames().map(item => item.fd));
  view = new BrowserView({ webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  view.setBounds({ x: 40, y: 160, width: 180, height: 120 });
  window.setBrowserView(view);
  await view.webContents.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent('<!doctype html><style>html,body{margin:0;width:100%;height:100%;background:#d020a0}</style>'));
  await view.webContents.prepareToShow();
  const viewFd = newFrame(beforeView);
  frame(viewFd);
  const guestPoint = [{ x: 100, y: 210 }];
  const magenta = [[208, 32, 160, 255]], cyan = [[16, 208, 224, 255]];
  await nativeColors(window, guestPoint, magenta, 'BrowserView native attach');
  imageColors(await view.webContents.capturePage(), 180, 120, [{ x: 60, y: 50 }], magenta, 'independent BrowserView GPU page capture');
  const other = create();
  await other.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><title>${other.getTitle()}</title><style>html,body{margin:0;background:#204060}</style>`));
  other.setBrowserView(view);
  other.show();
  await nativeColors(other, guestPoint, magenta, 'BrowserView remote layer reparent');
  const guestGeneration = frame(viewFd).presentedGeneration;
  await view.webContents.executeJavaScript("document.body.style.background='#10d0e0';document.documentElement.style.background='#10d0e0';true");
  await nativeColors(other, guestPoint, cyan, 'reparented BrowserView subsequent frame');
  assert.ok(frame(viewFd).presentedGeneration > guestGeneration);
  other.hide();
  window.show();
  await nativeColors(window, guestPoint, [base], 'old BrowserView parent no longer shows guest');
  window.setBrowserView(view);
  await nativeColors(window, guestPoint, cyan, 'BrowserView reattach retains actual latest native content');
  imageColors(await view.webContents.capturePage(), 180, 120, [{ x: 60, y: 50 }], cyan, 'reattached BrowserView GPU page capture is current');

  const beforeAlpha = new Set(native.frames().map(item => item.fd));
  const alphaWindow = new BrowserWindow({ width: 400, height: 300, frame: false,
    transparent: true, show: true, title: `mini-gpu-alpha-${process.pid}`,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false, transparent: true } });
  windows.push(alphaWindow);
  await alphaWindow.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html>
    <title>${alphaWindow.getTitle()}</title><style>html,body{margin:0;background:transparent}
    div{position:fixed;inset:0;background:rgba(255,0,0,.5)}</style><div></div>`));
  await alphaWindow.webContents.prepareToShow();
  frame(newFrame(beforeAlpha));
  imageColors(await alphaWindow.webContents.capturePage(), 400, 300,
    [{ x: 20, y: 20 }], [[128, 0, 0, 128]], 'transparent GPU capture preserves premultiplied color');
  console.log(JSON.stringify({ root: frame(rootFd), browserView: frame(viewFd), coverage: 'actual composed pixels plus native Metal metadata; no CPU/replay substitute' }));
  finish();
}).catch(finish);
