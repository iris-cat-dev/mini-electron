'use strict';
// Run with out/windows-x64/runtime-dist/electron.exe test/windows-caption-overlay-smoke.js
// on an interactive Windows desktop. Native helper coordinates are physical screen pixels;
// caption points below are derived from the window client origin/extent and per-window DPI.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');
const { startNativeInput } = require('./windows-native-input-driver');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-windows-caption-overlay-'));
const title = `mini-windows-caption-overlay-${process.pid}`;
const artifacts = process.env.MINI_NATIVE_INPUT_ARTIFACTS;
const artifactStem = `windows-caption-overlay-${process.pid}`;
const overlayHeight = 52;
const headerColors = { light: [240, 244, 248], dark: [24, 30, 38] };
const evidence = { title, stages: [], actions: [] };
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let window, driver, finished = false, closeEvents = 0, closedEvents = 0;
const request = (...args) => driver.request(...args);
const deadline = setTimeout(() => finish(new Error('Windows caption overlay smoke timed out')), 90000);
app.on('window-all-closed', () => {});

async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (error && driver && window && !window.isDestroyed() && !window.isMinimized() && artifacts) {
    try {
      fs.mkdirSync(artifacts, { recursive: true });
      await request('focus');
      await request('capture', { path: path.join(artifacts, `${artifactStem}-failure.png`) });
      const frame = await window.webContents.capturePage();
      const rendererPath = path.join(artifacts, `${artifactStem}-renderer.png`);
      fs.writeFileSync(rendererPath, frame.toPNG());
      evidence.rendererCapture = rendererPath;
    } catch (diagnosticError) {
      evidence.diagnosticError = String(diagnosticError.stack || diagnosticError);
    }
  }
  evidence.passed = !error;
  evidence.closeEvents = closeEvents;
  evidence.closedEvents = closedEvents;
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
  if (window && !window.isDestroyed()) window.destroy();
  try { fs.rmSync(temporary, { recursive: true, force: true }); }
  catch (cleanupError) { error = error || cleanupError; }
  if (error) console.error(error.stack || error);
  else console.log('PASS: Windows caption overlay alpha, repaint, hover and native caption actions');
  app.exit(error ? 1 : 0);
}

async function evaluate(source) {
  const result = await window.webContents.executeJavaScript(source);
  return result && typeof result === 'object' && 'value' in result ? result.value : result;
}

async function eventually(read, predicate, label, timeout = 5000) {
  const until = Date.now() + timeout;
  let value;
  do {
    value = await read();
    if (predicate(value)) return value;
    await pause(35);
  } while (Date.now() < until);
  assert.fail(`${label}: ${JSON.stringify(value)}`);
}

const dip = (value, dpi) => Math.floor(value * dpi / 96 + 0.5);
function captionRects(geometry) {
  assert.ok(geometry.clientWidth >= dip(46 * 3, geometry.dpi), 'window must fit all caption buttons');
  const width = dip(46, geometry.dpi);
  const topInset = geometry.maximized ? 0 : dip(1, geometry.dpi);
  const height = Math.max(1, dip(overlayHeight - (geometry.maximized ? 0 : 1), geometry.dpi));
  const right = geometry.clientX + geometry.clientWidth;
  const top = geometry.clientY + topInset;
  const rect = indexFromRight => ({ left: right - width * (indexFromRight + 1), top,
    right: right - width * indexFromRight, bottom: top + height });
  return { minimize: rect(2), maximize: rect(1), close: rect(0) };
}

function pointIn(rect, dpi, centered = false) {
  if (centered) return { x: Math.floor((rect.left + rect.right) / 2), y: Math.floor((rect.top + rect.bottom) / 2) };
  return { x: rect.left + Math.max(2, dip(7, dpi)), y: rect.top + Math.max(2, dip(7, dpi)) };
}

async function moveAway(geometry) {
  if (!geometry) geometry = await request('geometry');
  const x = geometry.clientX + Math.min(geometry.clientWidth - 2, Math.max(2, dip(20, geometry.dpi)));
  const y = geometry.clientY + Math.min(geometry.clientHeight - 2, Math.max(2, dip(overlayHeight + 18, geometry.dpi)));
  await request('pointerMove', { x, y });
  return geometry;
}

async function readPixel(point, size = 5) {
  await request('focus');
  const colors = await request('sample', { x: point.x, y: point.y, width: size, height: size });
  return colors[0].rgb;
}

const colorDelta = (actual, expected) => Math.max(...actual.map((value, index) => Math.abs(value - expected[index])));
async function expectPixel(point, expected, tolerance, label) {
  const actual = await eventually(() => readPixel(point), value => colorDelta(value, expected) <= tolerance, label);
  evidence.stages.push({ label, point, expected, actual, tolerance });
  return actual;
}

async function expectRestoreGlyph(geometry, label) {
  const rect = captionRects(geometry).maximize;
  const size = dip(10, geometry.dpi), offset = dip(2, geometry.dpi);
  const left = Math.floor((rect.left + rect.right - size) / 2);
  const top = Math.floor((rect.top + rect.bottom - size) / 2);
  const frontMiddle = Math.floor((size - offset) / 2);
  const points = {
    frontLeft: { x: left, y: top + offset + frontMiddle },
    frontTop: { x: left + frontMiddle, y: top + offset },
    frontBottom: { x: left + frontMiddle, y: top + size - 1 },
    backTop: { x: left + Math.floor(size / 2), y: top },
    backRight: { x: left + size - 1, y: top + frontMiddle },
  };
  for (const [edge, point] of Object.entries(points)) {
    const actual = await eventually(() => readPixel(point, 1),
      value => colorDelta(value, [255, 255, 255]) <= 3, `${label} ${edge}`);
    evidence.stages.push({ label: `${label} ${edge}`, point, actual });
  }
}

async function expectStableCaptionDuringMotion(geometry, label) {
  await request('focus');
  const buttons = captionRects(geometry);
  const size = dip(10, geometry.dpi);
  const samples = Object.entries(buttons).map(([name, rect]) => {
    const left = Math.floor((rect.left + rect.right - size) / 2);
    const top = Math.floor((rect.top + rect.bottom - size) / 2);
    return { x: left + Math.floor(size / 2), y: top + (name === 'maximize' ? 0 : Math.floor(size / 2)) };
  });
  // Native bounds settle before DWM finishes the maximize/restore transition.
  for (const point of samples) {
    await eventually(() => readPixel(point, 1),
      value => value.every(channel => channel >= 240), `${label} initial glyph`);
  }
  const moves = Object.values(buttons).map(rect => pointIn(rect, geometry.dpi));
  moves.push({ x: geometry.clientX + dip(20, geometry.dpi), y: geometry.clientY + dip(overlayHeight + 18, geometry.dpi) });
  const motion = await request('samplePointerMotion', { moves, samples, cycles: 6 });
  evidence.stages.push({ label, samples, ...motion });
  assert.ok(motion.frames > 0, `${label} must sample native presentation`);
  assert.deepEqual(motion.missing, [0, 0, 0], `${label}: caption glyphs disappeared during pointer movement`);
  await moveAway(geometry);
}

function composite(foreground, alpha, background) {
  return foreground.map((value, index) => Math.round((value * alpha + background[index] * (255 - alpha)) / 255));
}

async function setHeader(mode) {
  assert.ok(headerColors[mode]);
  await evaluate(`new Promise(resolve => {
    document.getElementById('header').style.backgroundColor = ${JSON.stringify(`rgb(${headerColors[mode].join(',')})`)};
    document.body.dataset.header = ${JSON.stringify(mode)};
    requestAnimationFrame(() => requestAnimationFrame(resolve));
  })`);
}

async function setOverlay(color) {
  window.setTitleBarOverlay({ color, symbolColor: '#ffffff', height: overlayHeight });
  await pause(40);
}

async function assertStage(label, color, foreground, alpha, background) {
  await setOverlay(color);
  const geometry = await moveAway();
  const buttons = captionRects(geometry);
  const headerPoint = { x: geometry.clientX + Math.max(2, dip(20, geometry.dpi)),
    y: buttons.minimize.top + Math.max(2, dip(7, geometry.dpi)) };
  await expectPixel(headerPoint, background, 3, `${label} header fixture`);
  return expectPixel(pointIn(buttons.minimize, geometry.dpi), composite(foreground, alpha, background), 5, label);
}

async function clickCaption(name) {
  const geometry = await request('geometry');
  const point = pointIn(captionRects(geometry)[name], geometry.dpi, true);
  evidence.actions.push({ action: `click-${name}`, geometry, point });
  await request('click', point);
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  driver = await startNativeInput(temporary, title);
  window = new BrowserWindow({
    x: 240, y: 90, width: 860, height: 560, title, frame: false,
    titleBarOverlay: { color: '#2468ac', symbolColor: '#ffffff', height: overlayHeight },
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  window.on('close', () => { closeEvents++; });
  window.on('closed', () => { closedEvents++; evidence.destroyedDuringClosed = window.isDestroyed(); });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html>
    <meta charset="utf-8"><title>${title}</title><style>
    html,body{margin:0;width:100%;height:100%;overflow:hidden;background:rgb(240,244,248)}
    #header{position:fixed;inset:0 0 auto 0;height:110px;background:rgb(240,244,248)}
    #content{position:absolute;top:110px;inset-inline:0;bottom:0;background:#8090a0}
    </style><div id="header"></div><div id="content"></div>`));
  window.show();
  window.focus();
  await request('focus');
  await eventually(() => evaluate('document.visibilityState'), state => state === 'visible', 'fixture visibility');

  await setHeader('light');
  await assertStage('opaque overlay', '#2468ac', [36, 104, 172], 255, headerColors.light);
  await assertStage('invalid color preserves previous overlay', '#12g456', [36, 104, 172], 255, headerColors.light);
  await assertStage('opaque short overlay', '#38a', [51, 136, 170], 255, headerColors.light);
  await assertStage('transparent eight-digit overlay', '#00000000', [0, 0, 0], 0, headerColors.light);
  await assertStage('partial eight-digit overlay', '#d0206080', [208, 32, 96], 128, headerColors.light);
  await assertStage('transparent short overlay', '#0000', [0, 0, 0], 0, headerColors.light);
  await setHeader('dark');
  await assertStage('transparent overlay after dark header switch', '#00000000', [0, 0, 0], 0, headerColors.dark);
  await assertStage('partial short overlay', '#0f08', [0, 255, 0], 136, headerColors.dark);
  await assertStage('transparent overlay after partial alpha', '#00000000', [0, 0, 0], 0, headerColors.dark);

  await setOverlay('#405060');
  let geometry = await moveAway();
  let maximizeRect = captionRects(geometry).maximize;
  const samplePoint = pointIn(maximizeRect, geometry.dpi);
  const baseline = await expectPixel(samplePoint, [64, 80, 96], 4, 'pre-hover caption background');
  const hoverPoint = pointIn(maximizeRect, geometry.dpi, true);
  await request('pointerMove', hoverPoint);
  const hovered = await eventually(() => readPixel(samplePoint), value => colorDelta(value, baseline) >= 8, 'native caption hover appearance');
  evidence.stages.push({ label: 'native caption hover appearance', point: samplePoint, baseline, actual: hovered });
  await moveAway(geometry);
  await expectPixel(samplePoint, baseline, 3, 'caption hover clearing');
  await expectStableCaptionDuringMotion(geometry, 'restored opaque caption motion');

  await clickCaption('maximize');
  await eventually(async () => ({ native: await request('geometry'), javascript: window.isMaximized() }),
    state => state.native.maximized && state.javascript, 'native maximize action');
  evidence.actions.push({ action: 'maximized', geometry: await request('geometry') });
  geometry = await moveAway();
  await expectRestoreGlyph(geometry, 'opaque maximized restore glyph');
  await expectStableCaptionDuringMotion(geometry, 'maximized opaque caption motion');
  await setOverlay('#00000000');
  await expectRestoreGlyph(geometry, 'transparent maximized restore glyph');
  await expectStableCaptionDuringMotion(geometry, 'maximized transparent caption motion');
  await request('pointerMove', pointIn(captionRects(geometry).maximize, geometry.dpi, true));
  await expectRestoreGlyph(geometry, 'hovered transparent restore glyph');
  await moveAway(geometry);
  if (artifacts) {
    fs.mkdirSync(artifacts, { recursive: true });
    const capturePath = path.join(artifacts, `${artifactStem}-maximized.png`);
    await request('capture', { path: capturePath });
    evidence.maximizedCapture = capturePath;
  }
  await clickCaption('maximize');
  await eventually(async () => ({ native: await request('geometry'), javascript: window.isMaximized() }),
    state => !state.native.maximized && !state.javascript, 'native restore action');
  evidence.actions.push({ action: 'restored', geometry: await request('geometry') });
  await expectStableCaptionDuringMotion(await moveAway(), 'restored transparent caption motion');

  await clickCaption('minimize');
  await eventually(async () => ({ native: await request('geometry'), javascript: window.isMinimized() }),
    state => state.native.minimized && state.javascript, 'native minimize action');
  evidence.actions.push({ action: 'minimized', geometry: await request('geometry') });
  window.restore();
  window.focus();
  await eventually(async () => ({ native: await request('geometry'), javascript: window.isMinimized() }),
    state => !state.native.minimized && !state.javascript, 'restore after native minimize');
  await request('focus');

  if (artifacts) {
    fs.mkdirSync(artifacts, { recursive: true });
    const capturePath = path.join(artifacts, `${artifactStem}.png`);
    await request('capture', { path: capturePath });
    evidence.capture = capturePath;
  }

  await clickCaption('close');
  await eventually(() => ({ destroyed: window.isDestroyed(), closeEvents, closedEvents }),
    state => state.destroyed && state.closeEvents === 1 && state.closedEvents === 1, 'native close action');
  assert.equal(closeEvents, 1, 'native close button must emit close exactly once');
  assert.equal(closedEvents, 1, 'native close button must destroy the window exactly once');
  assert.equal(evidence.destroyedDuringClosed, true, 'closed listeners must observe a destroyed window');
}).then(() => finish(), finish);
