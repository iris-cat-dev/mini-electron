'use strict';

// Interactive Windows desktop, hardware mode:
// out/windows-x64/mini-electron.exe --app=test/windows-gpu-compositor-smoke.js
// Software consumer mode:
// MINI_GPU_SMOKE_EXPECT_SOFTWARE=1 out/windows-x64/mini-electron.exe --disable-gpu --app=test/windows-gpu-compositor-smoke.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserView, BrowserWindow, nativeImage, screen } = require('electron');
const { startNativeInput } = require('./windows-native-input-driver');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-windows-gpu-compositor-'));
app.setPath('userData', path.join(temporary, 'user-data'));
const title = `mini-windows-gpu-compositor-${process.pid}`;
const alphaTitle = `${title}-alpha`;
const expectSoftware = process.env.MINI_GPU_SMOKE_EXPECT_SOFTWARE === '1';
const expectNativeCpu = process.env.MINI_GPU_SMOKE_EXPECT_NATIVE_CPU === '1';
assert.ok(!expectNativeCpu || expectSoftware, 'native CPU fallback expects a CPU renderer');
if (process.env.MINI_GPU_SMOKE_DISABLE_WITH_API === '1') {
  assert.equal(expectSoftware, true, 'API disable mode expects the software backend');
  app.disableHardwareAcceleration();
}
const artifacts = process.env.MINI_NATIVE_INPUT_ARTIFACTS;
const rootInitial = [30, 76, 112];
const rootFinal = [28, 108, 156];
const rootAlternate = [126, 50, 142];
const viewInitial = [220, 92, 36];
const viewFinal = [36, 196, 118];
const alphaArtwork = [238, 52, 100];
const overlayColor = [52, 72, 92];
const overlayHeight = 48;
const initialViewBounds = { x: 390, y: 150, width: 240, height: 140 };
const movedViewBounds = { x: 90, y: 235, width: 240, height: 140 };
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const evidence = { mode: expectNativeCpu ? 'cpu-renderer-native' : expectSoftware ? 'software' : 'hardware', stages: [] };
const windows = [];
let target, view, alphaWindow, driver, finished = false, captureSerial = 0;
const watchdog = setTimeout(() => finish(new Error('Windows GPU compositor smoke exceeded 120s watchdog')), 120000);
app.on('window-all-closed', () => {});

const WS_EX_TOPMOST = 0x00000008;
const WS_EX_TOOLWINDOW = 0x00000080;
const WS_EX_APPWINDOW = 0x00040000;
const WS_EX_NOACTIVATE = 0x08000000;
const hasStyle = (state, flag) => (state.exStyle & flag) === flag;
const delta = (actual, expected) => Math.max(...actual.map((value, index) => Math.abs(value - expected[index])));
const composite = (foreground, alpha, background) => foreground.map((value, index) =>
  Math.round((value * alpha + background[index] * (255 - alpha)) / 255));

async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (error) {
    evidence.failureDiagnostics = await Promise.all(
      [['root', target?.webContents], ['BrowserView', view?.webContents], ['alpha', alphaWindow?.webContents]]
        .filter(([, contents]) => contents && !contents.isDestroyed())
        .map(([name, contents]) => Promise.race([
          (async () => {
            try {
              const info = await contents._getCompositorInfo();
              console.error('GPU failure state:', JSON.stringify({ name, info }));
              const dom = await contents.executeJavaScript(
                '[innerWidth,innerHeight,devicePixelRatio,getComputedStyle(document.documentElement).backgroundColor,getComputedStyle(document.body).backgroundColor]');
              return { name, info, dom };
            } catch (failure) { return { name, error: String(failure) }; }
          })(),
          pause(8000).then(() => ({ name, error: 'renderer diagnostics timed out' })),
        ])));
  }
  let cleanupError;
  if (driver) {
    try { await driver.close(); } catch (failure) { cleanupError = failure; }
  }
  for (const window of windows.reverse()) {
    if (window && !window.isDestroyed()) window.destroy();
  }
  fs.rmSync(temporary, { recursive: true, force: true });
  const failure = error || cleanupError;
  if (failure) console.error(failure.stack || failure, JSON.stringify(evidence));
  else console.log(`PASS: Windows ${evidence.mode} compositor presents native pixels, guests, alpha, captions, restores, captures, and display VSync\n${JSON.stringify(evidence)}`);
  app.exit(failure ? 1 : 0);
}

async function eventually(read, predicate, label, timeout = 6000) {
  const until = Date.now() + timeout;
  let value;
  do {
    value = await read();
    if (predicate(value)) return value;
    await pause(30);
  } while (Date.now() < until);
  assert.fail(`${label}: ${JSON.stringify(value)}`);
}

const request = (action, values = {}, requestedTitle = title) =>
  driver.request(action, { title: requestedTitle, ...values });
const evaluate = source => target.webContents.executeJavaScript(source);
const dip = (value, dpi) => Math.floor(value * dpi / 96 + 0.5);
const physicalPoint = (geometry, point) => ({
  x: geometry.clientX + dip(point.x, geometry.dpi),
  y: geometry.clientY + dip(point.y, geometry.dpi),
});

async function requireNativeDesktop() {
  try {
    await request('focus');
    const state = await eventually(async () => ({
      native: await request('nativeWindowState'),
      javascript: target.isFocused(),
    }), value => value.native.foreground === value.native.window && value.javascript,
    'target did not become the native foreground window');
    return state.native;
  } catch (error) {
    const prerequisite = new Error(`PREREQUISITE: an unlocked interactive native desktop is required: ${error.message}`);
    prerequisite.cause = error;
    throw prerequisite;
  }
}

async function desktopPixel(requestedTitle, point, inactive = false) {
  const geometry = await request('geometry', {}, requestedTitle);
  const physical = physicalPoint(geometry, point);
  const colors = await request('sample', { ...physical, width: 1, height: 1, inactive }, requestedTitle);
  return { rgb: colors[0].rgb, geometry, physical };
}

async function expectDesktopPixel(requestedTitle, point, expected, label, inactive = false, tolerance = 5) {
  const result = await eventually(
    () => desktopPixel(requestedTitle, point, inactive),
    value => delta(value.rgb, expected) <= tolerance,
    label,
  );
  evidence.stages.push({ label, expected, actual: result.rgb, point: result.physical });
  return result;
}

function rgbaAt(image, point) {
  const size = image.getSize();
  assert.ok(point.x >= 0 && point.y >= 0 && point.x < size.width && point.y < size.height,
    `pixel ${JSON.stringify(point)} must be inside ${JSON.stringify(size)}`);
  const bitmap = image.toBitmap();
  const offset = (point.y * size.width + point.x) * 4;
  return [bitmap[offset + 2], bitmap[offset + 1], bitmap[offset], bitmap[offset + 3]];
}

function imagePoint(image, logicalSize, point) {
  const size = image.getSize();
  return {
    x: Math.min(size.width - 1, Math.max(0, Math.floor(point.x * size.width / logicalSize.width))),
    y: Math.min(size.height - 1, Math.max(0, Math.floor(point.y * size.height / logicalSize.height))),
  };
}

function expectImagePixel(image, logicalSize, point, expected, label, tolerance = 4) {
  assert.equal(image.isEmpty(), false, `${label}: capture must not be empty`);
  const actual = rgbaAt(image, imagePoint(image, logicalSize, point));
  assert.ok(delta(actual, expected) <= tolerance,
    `${label}: actual=${JSON.stringify(actual)} expected=${JSON.stringify(expected)}`);
  evidence.stages.push({ label, expected, actual });
}

async function captureNativePixels(requestedTitle, points, inactive, label) {
  const directory = artifacts || temporary;
  fs.mkdirSync(directory, { recursive: true });
  const capturePath = path.join(directory, `windows-gpu-compositor-${process.pid}-${++captureSerial}.png`);
  const geometry = await request('geometry', {}, requestedTitle);
  await request('capture', { path: capturePath, inactive }, requestedTitle);
  const image = nativeImage.createFromPath(capturePath);
  assert.equal(image.isEmpty(), false, `${label}: native desktop capture must decode`);
  const size = image.getSize();
  assert.deepEqual(size, { width: geometry.right - geometry.left, height: geometry.bottom - geometry.top },
    `${label}: native capture dimensions`);
  for (const sample of points) {
    const physical = physicalPoint(geometry, sample.point);
    const actual = rgbaAt(image, { x: physical.x - geometry.left, y: physical.y - geometry.top });
    assert.ok(delta(actual, [...sample.rgb, 255]) <= (sample.tolerance || 5),
      `${label} ${sample.name}: actual=${JSON.stringify(actual)} expected=${JSON.stringify([...sample.rgb, 255])}`);
    evidence.stages.push({ label: `${label} ${sample.name}`, expected: [...sample.rgb, 255], actual });
  }
  return capturePath;
}

async function compositorInfo(contents = target.webContents) {
  assert.equal(typeof contents._getCompositorInfo, 'function',
    'Windows compositor diagnostics API must be present in this instrumented runtime');
  return contents._getCompositorInfo();
}

function assertCompositorInfo(info, label) {
  const expectedBackend = expectSoftware ? 'software' : 'd3d12';
  const expectedPresentation = expectNativeCpu || !expectSoftware ? 'directcomposition' : 'software';
  assert.equal(info.backend, expectedBackend,
    expectSoftware ? `${label}: requested CPU renderer must use software composition` :
      `${label}: PREREQUISITE genuine D3D12 rendering hardware is required`);
  assert.equal(info.presentation, expectedPresentation,
    `${label}: expected ${expectedPresentation} presentation` +
      (expectSoftware && !expectNativeCpu ? ' with GPU disabled' : ' on genuine D3D hardware'));
  for (const field of ['frames', 'readbacks', 'importedFrames']) {
    assert.ok(Number.isSafeInteger(info[field]) && info[field] >= 0, `${label}: valid ${field}`);
  }
  if (expectSoftware) {
    assert.equal(info.importedFrames, 0, `${label}: software presentation has no GPU frame imports`);
    assert.equal(info.readbacks, 0, `${label}: software capture has no GPU readback`);
  }
  assert.ok(info.vsync && ['dxgi', 'dwm', 'timer'].includes(info.vsync.source), `${label}: known VSync source`);
  assert.equal(typeof info.vsync.displayId, 'string', `${label}: VSync display identity`);
  assert.ok(info.vsync.displayId.length > 0, `${label}: selected display identity`);
  assert.ok(Number.isFinite(info.vsync.intervalUs) && info.vsync.intervalUs > 0, `${label}: selected display interval`);
  assert.ok(Number.isSafeInteger(info.vsync.ticks) && info.vsync.ticks >= 0, `${label}: VSync tick count`);
  assert.equal(typeof info.vsync.active, 'boolean', `${label}: VSync activity state`);
}

async function assertViewPlacement(bounds, expectedView, expectedRoot, label) {
  const samples = [
    { name: 'inside', point: { x: bounds.x + 70, y: bounds.y + 60 }, rgb: expectedView },
    { name: 'left clip', point: { x: bounds.x - 4, y: bounds.y + 60 }, rgb: expectedRoot },
    { name: 'right clip', point: { x: bounds.x + bounds.width + 4, y: bounds.y + 60 }, rgb: expectedRoot },
    { name: 'top clip', point: { x: bounds.x + 70, y: bounds.y - 4 }, rgb: expectedRoot },
    { name: 'bottom clip', point: { x: bounds.x + 70, y: bounds.y + bounds.height + 4 }, rgb: expectedRoot },
  ];
  await request('focus');
  for (const sample of samples)
    await expectDesktopPixel(title, sample.point, sample.rgb, `${label} ${sample.name}`);
}

function captionRects(geometry) {
  const width = dip(46, geometry.dpi);
  const right = geometry.clientX + geometry.clientWidth;
  const top = geometry.clientY + dip(1, geometry.dpi);
  const height = Math.max(1, dip(overlayHeight - 1, geometry.dpi));
  const rect = index => ({ left: right - width * (index + 1), right: right - width * index, top, bottom: top + height });
  return { minimize: rect(2), maximize: rect(1), close: rect(0) };
}

async function exerciseAnimationAndVSync(before) {
  const geometry = await request('geometry');
  const maximize = captionRects(geometry).maximize;
  const captionSample = { x: maximize.left + dip(7, geometry.dpi), y: maximize.top + dip(7, geometry.dpi) };
  const hoverPoint = { x: Math.floor((maximize.left + maximize.right) / 2), y: Math.floor((maximize.top + maximize.bottom) / 2) };
  const readPhysical = async point => (await request('sample', { ...point, width: 1, height: 1 }))[0].rgb;
  const baseline = await eventually(() => readPhysical(captionSample), rgb => delta(rgb, overlayColor) <= 4,
    'opaque native caption baseline');
  await request('pointerMove', hoverPoint);
  const hovered = await eventually(() => readPhysical(captionSample), rgb => delta(rgb, baseline) >= 8,
    'native caption hover appearance');

  await evaluate('fixture.startAnimation(); true');
  const vsyncStartAt = Date.now();
  const vsyncStart = await eventually(compositorInfo, info => {
    assertCompositorInfo(info, 'active VSync start');
    return info.vsync.active ? info : false;
  }, 'renderer did not activate its selected VSync clock');
  await pause(300);
  const vsyncEnd = await eventually(compositorInfo, info =>
    info.vsync.active && info.vsync.ticks > vsyncStart.vsync.ticks ? info : false,
  'selected VSync clock did not tick during active animation');
  const vsyncElapsedMs = Date.now() - vsyncStartAt;
  const tickDelta = vsyncEnd.vsync.ticks - vsyncStart.vsync.ticks;
  assert.ok(['dxgi', 'dwm'].includes(vsyncEnd.vsync.source),
    `PREREQUISITE hardware display VSync is unavailable (selected ${vsyncEnd.vsync.source})`);
  assert.equal(vsyncEnd.vsync.displayId, vsyncStart.vsync.displayId, 'active animation stays on its selected display');
  const nominalTicks = vsyncElapsedMs * 1000 /
    Math.min(vsyncStart.vsync.intervalUs, vsyncEnd.vsync.intervalUs);
  assert.ok(tickDelta <= nominalTicks * 1.5 + 4,
    `VSync must not busy-loop on successful non-blocking waits: ${tickDelta} ticks in ${vsyncElapsedMs}ms`);
  await pause(180);
  const hoverAfterFrames = await readPhysical(captionSample);
  assert.ok(delta(hoverAfterFrames, baseline) >= 8,
    `renderer frame overwrote native caption hover: baseline=${baseline}, hovered=${hovered}, after=${hoverAfterFrames}`);

  await evaluate('fixture.stopAnimation()');
  await request('pointerMove', {
    x: geometry.clientX + dip(24, geometry.dpi),
    y: geometry.clientY + dip(overlayHeight + 24, geometry.dpi),
  });
  await eventually(() => readPhysical(captionSample), rgb => delta(rgb, baseline) <= 4,
    'native caption hover clearing after renderer animation');
  await expectDesktopPixel(title, { x: 70, y: 100 }, rootFinal,
    'ordinary animated frame reaches the real desktop');

  const after = await compositorInfo();
  assertCompositorInfo(after, 'ordinary animation diagnostics');
  assert.ok(after.frames > before.frames, 'ordinary animation must publish renderer frames');
  if (!expectSoftware)
    assert.ok(after.importedFrames > before.importedFrames,
      'ordinary GPU animation must import frames into the native consumer');
  assert.equal(after.readbacks, before.readbacks, 'ordinary animation must not perform CPU readback');
  evidence.vsync = {
    source: vsyncEnd.vsync.source,
    displayId: vsyncEnd.vsync.displayId,
    intervalUs: vsyncEnd.vsync.intervalUs,
    selectedHz: 1000000 / vsyncEnd.vsync.intervalUs,
    tickDelta,
    elapsedMs: vsyncElapsedMs,
    observedUsPerTick: vsyncElapsedMs * 1000 / tickDelta,
    observedHz: tickDelta * 1000 / vsyncElapsedMs,
  };
  evidence.caption = { baseline, hovered, afterFrames: hoverAfterFrames };
  return after;
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32', 'PREREQUISITE: Windows is required');
  const area = screen.getPrimaryDisplay().workArea;
  assert.ok(area.width >= 820 && area.height >= 620,
    'PREREQUISITE: an unlocked native desktop with at least 820x620 DIP work area is required');
  driver = await startNativeInput(temporary, title);

  const bounds = { x: area.x + Math.floor((area.width - 720) / 2),
    y: area.y + Math.floor((area.height - 500) / 2), width: 720, height: 500 };
  target = new BrowserWindow({ ...bounds, title, show: false, frame: false,
    titleBarStyle: 'hidden', titleBarOverlay: { color: '#34485c', symbolColor: '#ffffff', height: overlayHeight },
    backgroundColor: '#1e4c70', minWidth: 0, minHeight: 0,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  windows.push(target);
  target.webContents.once('render-process-gone', (_, details) =>
    finish(new Error(`GPU smoke renderer exited: ${JSON.stringify(details)}`)));
  view = new BrowserView({ webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  target.addBrowserView(view);
  view.setBounds(initialViewBounds);
  await Promise.all([
    target.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><meta charset="utf-8">
      <title>${title}</title><style>html,body,#surface{margin:0;width:100%;height:100%;overflow:hidden}
      #surface{position:fixed;inset:0;background:rgb(${rootInitial.join(',')})}</style><div id="surface"></div>
      <script>const surface=document.getElementById('surface');window.fixture={frames:0,running:false,
      transition(color){surface.style.background=color;return new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))},
      startAnimation(){if(this.running)return;this.running=true;const tick=()=>{if(!this.running)return;this.frames++;
      surface.style.background=this.frames%2?'rgb(${rootAlternate.join(',')})':'rgb(${rootFinal.join(',')})';requestAnimationFrame(tick)};requestAnimationFrame(tick)},
      stopAnimation(){this.running=false;surface.style.background='rgb(${rootFinal.join(',')})';
      return new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))}};</script>`)),
    view.webContents.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><style>
      html,body{margin:0;width:100%;height:100%;overflow:hidden;background:rgb(${viewInitial.join(',')})}</style>
      <script>window.setColor=color=>{document.documentElement.style.background=color;document.body.style.background=color;
      return new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))}</script>`)),
  ]);
  target.show();
  target.focus();
  await requireNativeDesktop();
  await expectDesktopPixel(title, { x: 70, y: 100 }, rootInitial, 'initial opaque page desktop pixel');
  await assertViewPlacement(initialViewBounds, viewInitial, rootInitial, 'initial BrowserView bounds');

  const mainState = await request('nativeWindowState');
  evidence.windowState = mainState;
  assert.equal(mainState.foreground, mainState.window, 'ordinary window owns native foreground focus');
  assert.equal(hasStyle(mainState, WS_EX_TOOLWINDOW), false, 'ordinary fixture remains taskbar eligible');
  assert.equal(hasStyle(mainState, WS_EX_NOACTIVATE), false, 'ordinary fixture remains activatable');
  assert.equal(mainState.owner, 0, 'taskbar policy does not manufacture an owner window');

  const initialInfo = await eventually(compositorInfo, info => {
    assertCompositorInfo(info, 'initial compositor diagnostics');
    return info.frames > 0 && (expectSoftware || info.importedFrames > 0) ? info : false;
  }, 'native compositor did not publish and present its initial frame');
  const initialViewInfo = await eventually(() => compositorInfo(view.webContents), info => {
    assertCompositorInfo(info, 'initial BrowserView compositor diagnostics');
    return info.frames > 0 ? info : false;
  }, 'BrowserView renderer did not publish its initial frame');
  await evaluate(`fixture.transition('rgb(${rootFinal.join(',')})')`);
  await expectDesktopPixel(title, { x: 70, y: 100 }, rootFinal, 'opaque page desktop transition');
  const beforeAnimation = await compositorInfo();
  assert.equal(beforeAnimation.readbacks, initialInfo.readbacks,
    'initial ordinary frame transition must not read pixels back');
  const afterAnimation = await exerciseAnimationAndVSync(beforeAnimation);

  await view.webContents.executeJavaScript(`setColor('rgb(${viewFinal.join(',')})')`);
  view.setBounds(movedViewBounds);
  await assertViewPlacement(movedViewBounds, viewFinal, rootFinal, 'moved BrowserView bounds and clipping');
  await expectDesktopPixel(title,
    { x: initialViewBounds.x + initialViewBounds.width / 2, y: initialViewBounds.y + initialViewBounds.height / 2 },
    rootFinal, 'old BrowserView position is cleared');
  const movedViewInfo = await compositorInfo(view.webContents);
  assertCompositorInfo(movedViewInfo, 'updated BrowserView compositor diagnostics');
  assert.ok(movedViewInfo.frames > initialViewInfo.frames, 'BrowserView color update must publish a new frame');
  assert.equal(movedViewInfo.readbacks, initialViewInfo.readbacks,
    'ordinary BrowserView updates must not perform CPU readback');

  target.setSize(680, 450);
  await eventually(async () => {
    const geometry = await request('geometry');
    return geometry.clientWidth === dip(680, geometry.dpi) && geometry.clientHeight === dip(450, geometry.dpi) ? geometry : false;
  }, Boolean, 'native resize did not reach the requested content size');
  await assertViewPlacement(movedViewBounds, viewFinal, rootFinal, 'resized BrowserView remains positioned and clipped');
  await expectDesktopPixel(title, { x: 620, y: 410 }, rootFinal, 'resized opaque edge remains visible');

  target.minimize();
  await eventually(async () => ({ native: await request('geometry'), javascript: target.isMinimized() }),
    state => state.native.minimized && state.javascript, 'native minimize state');
  target.restore();
  target.focus();
  await eventually(async () => ({ native: await request('geometry'), javascript: target.isMinimized(), focused: target.isFocused() }),
    state => !state.native.minimized && !state.javascript && state.focused, 'native restore state');
  await request('focus');
  await expectDesktopPixel(title, { x: 70, y: 100 }, rootFinal, 'restored opaque content retained');
  await expectDesktopPixel(title, { x: movedViewBounds.x + 70, y: movedViewBounds.y + 60 }, viewFinal,
    'restored BrowserView content retained');

  const nativeMainCapture = await captureNativePixels(title, [
    { name: 'root', point: { x: 70, y: 100 }, rgb: rootFinal },
    { name: 'BrowserView', point: { x: movedViewBounds.x + 70, y: movedViewBounds.y + 60 }, rgb: viewFinal },
  ], false, 'opaque composed desktop capture');
  const explicitRoot = await target.webContents.capturePage();
  expectImagePixel(explicitRoot, { width: 680, height: 450 }, { x: 70, y: 100 }, [...rootFinal, 255],
    'explicit opaque RGBA capture');
  const afterRootCapture = expectSoftware ? await compositorInfo() :
    await eventually(compositorInfo,
      info => info.readbacks > afterAnimation.readbacks ? info : false,
      'explicit capture did not increment the GPU renderer readback counter');
  assertCompositorInfo(afterRootCapture, 'explicit opaque capture diagnostics');

  const targetBounds = target.getBounds();
  alphaWindow = new BrowserWindow({ x: targetBounds.x + 60, y: targetBounds.y + 95, width: 240, height: 180,
    title: alphaTitle, show: false, frame: false, transparent: true, backgroundColor: '#00000000',
    focusable: false, skipTaskbar: true, alwaysOnTop: true, hasShadow: false, resizable: false,
    minWidth: 0, minHeight: 0,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false, transparent: true } });
  windows.push(alphaWindow);
  await alphaWindow.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><title>${alphaTitle}</title><style>
    html,body{margin:0;width:100%;height:100%;overflow:hidden;background:transparent}
    #art{position:absolute;left:60px;top:50px;width:110px;height:80px;background:rgba(${alphaArtwork.join(',')},.5)}</style><div id="art"></div>`));
  await request('focusSource');
  alphaWindow.showInactive();
  const alphaState = await eventually(() => request('nativeWindowState', {}, alphaTitle), state => state.visible ? state : false,
    'transparent fixture native visibility');
  assert.notEqual(alphaState.foreground, alphaState.window, 'transparent fixture never steals native focus');
  assert.equal(hasStyle(alphaState, WS_EX_TOPMOST), true, 'transparent fixture stays above its owned colored underlay');
  assert.equal(hasStyle(alphaState, WS_EX_TOOLWINDOW), true, 'transparent fixture is excluded from the taskbar');
  assert.equal(hasStyle(alphaState, WS_EX_APPWINDOW), false, 'transparent fixture does not add a taskbar window');
  assert.equal(hasStyle(alphaState, WS_EX_NOACTIVATE), true, 'transparent fixture is natively nonactivating');
  assert.equal(alphaState.owner, 0, 'taskbar exclusion does not manufacture an owner HWND');

  const alphaInfoBefore = await eventually(() => compositorInfo(alphaWindow.webContents), info => {
    assertCompositorInfo(info, 'transparent compositor diagnostics');
    return info.frames > 0 && (expectSoftware || info.importedFrames > 0) ? info : false;
  }, 'transparent content did not publish and reach the native compositor');
  const desktopBlend = composite(alphaArtwork, 128, rootFinal);
  await expectDesktopPixel(alphaTitle, { x: 15, y: 15 }, rootFinal,
    'transparent pixel reveals colored owned underlay', true);
  await expectDesktopPixel(alphaTitle, { x: 90, y: 80 }, desktopBlend,
    'translucent artwork composites over colored owned underlay', true, 6);
  const nativeAlphaCapture = await captureNativePixels(alphaTitle, [
    { name: 'transparent underlay', point: { x: 15, y: 15 }, rgb: rootFinal },
    { name: 'translucent artwork', point: { x: 90, y: 80 }, rgb: desktopBlend, tolerance: 6 },
  ], true, 'transparent composed desktop capture');

  const explicitAlpha = await alphaWindow.webContents.capturePage();
  expectImagePixel(explicitAlpha, { width: 240, height: 180 }, { x: 15, y: 15 }, [0, 0, 0, 0],
    'explicit transparent RGBA capture');
  expectImagePixel(explicitAlpha, { width: 240, height: 180 }, { x: 90, y: 80 },
    [119, 26, 50, 128], 'explicit premultiplied RGBA artwork capture', 4);
  const alphaInfoAfter = expectSoftware ? await compositorInfo(alphaWindow.webContents) :
    await eventually(() => compositorInfo(alphaWindow.webContents),
      info => info.readbacks > alphaInfoBefore.readbacks ? info : false,
      'explicit transparent capture did not increment the GPU renderer readback counter');
  assertCompositorInfo(alphaInfoAfter, 'explicit transparent capture diagnostics');

  evidence.diagnostics = {
    initial: initialInfo,
    initialBrowserView: initialViewInfo,
    beforeAnimation,
    afterAnimation,
    movedBrowserView: movedViewInfo,
    afterRootCapture,
    alphaBeforeCapture: alphaInfoBefore,
    alphaAfterCapture: alphaInfoAfter,
  };
  if (artifacts) evidence.nativeCaptures = [nativeMainCapture, nativeAlphaCapture];
}).then(() => finish(), finish);
