'use strict';

// Run on an unlocked Windows desktop:
// out/windows-x64/runtime-dist/electron.exe test/windows-screen-coordinates-smoke.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, screen } = require('electron');
const { startNativeInput } = require('./windows-native-input-driver');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-windows-screen-coordinates-'));
const baseTitle = `mini-screen-base-${process.pid}`;
const companionTitle = `mini-screen-companion-${process.pid}`;
const background = [24, 69, 99];
const artwork = [219, 81, 129];
const companionSize = { width: 260, height: 228 };
const margin = 24;
let base;
let companion;
let driver;
let finished = false;
const probes = [];
const deadline = setTimeout(() => finish(new Error('Windows screen coordinate smoke timed out')), 45000);
app.on('window-all-closed', () => {});

function assertNear(actual, expected, label, tolerance = 1) {
  assert.ok(Math.abs(actual - expected) <= tolerance,
    `${label}: expected ${expected} (+/- ${tolerance}), got ${actual}`);
}

function assertRectNear(actual, expected, label, tolerance = 1) {
  for (const key of ['x', 'y', 'width', 'height'])
    assertNear(actual[key], expected[key], `${label}.${key}`, tolerance);
}

function rectRight(rect) { return rect.x + rect.width; }
function rectBottom(rect) { return rect.y + rect.height; }
function center(rect) {
  return { x: rect.x + Math.floor(rect.width / 2), y: rect.y + Math.floor(rect.height / 2) };
}

function physicalRectToDip(rect, physicalDisplay, dipDisplay, scale) {
  const left = dipDisplay.x + Math.floor((rect.x - physicalDisplay.x) / scale);
  const top = dipDisplay.y + Math.floor((rect.y - physicalDisplay.y) / scale);
  const right = dipDisplay.x + Math.ceil((rectRight(rect) - physicalDisplay.x) / scale);
  const bottom = dipDisplay.y + Math.ceil((rectBottom(rect) - physicalDisplay.y) / scale);
  return { x: left, y: top, width: right - left, height: bottom - top };
}

function physicalPointToDip(point, physicalDisplay, dipDisplay, scale) {
  return {
    x: dipDisplay.x + Math.floor((point.x - physicalDisplay.x) / scale),
    y: dipDisplay.y + Math.floor((point.y - physicalDisplay.y) / scale)
  };
}

function assertDisplayMatchesNative(display, native, dpi, label) {
  const scale = dpi / 96;
  assertNear(display.scaleFactor, scale, `${label}.scaleFactor`, 0.01);
  assertRectNear(display.bounds,
    physicalRectToDip(native.bounds, native.bounds, display.bounds, scale),
    `${label}.bounds`);
  assertRectNear(display.workArea,
    physicalRectToDip(native.workArea, native.bounds, display.bounds, scale),
    `${label}.workArea`);
}

function assertPhysicalWindowInside(geometry, workArea, label) {
  assert.ok(geometry.left >= workArea.x, `${label}: left edge must be inside work area`);
  assert.ok(geometry.top >= workArea.y, `${label}: top edge must be inside work area`);
  assert.ok(geometry.right <= rectRight(workArea), `${label}: right edge must be inside work area`);
  assert.ok(geometry.bottom <= rectBottom(workArea), `${label}: bottom edge must be inside work area`);
}

async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (driver) {
    try { await driver.close(); }
    catch (cleanupError) { error ||= cleanupError; }
  }
  for (const window of [...probes, companion, base])
    if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: Electron screen APIs and default bottom-right transparent companion use consistent DIP coordinates on real Windows displays');
  app.exit(error ? 1 : 0);
}

app.whenReady().then(async () => {
  const primary = screen.getPrimaryDisplay();
  const area = primary.workArea;
  const defaultBounds = {
    x: area.x + area.width - companionSize.width - margin,
    y: area.y + area.height - companionSize.height - margin,
    ...companionSize
  };

  // Keep the controlled underlay above existing always-on-top desktop pets.
  base = new BrowserWindow({ ...defaultBounds, title: baseTitle, frame: false, show: false,
    alwaysOnTop: true, minWidth: 0, minHeight: 0, backgroundColor: '#184563' });
  await base.loadURL('data:text/html,' + encodeURIComponent(
    '<title>' + baseTitle + '</title><style>html,body{margin:0;width:100%;height:100%;background:#184563}</style>'));
  base.show();

  companion = new BrowserWindow({ ...defaultBounds, title: companionTitle, frame: false, show: false,
    transparent: true, alwaysOnTop: true, hasShadow: false, minWidth: 0, minHeight: 0,
    resizable: false, skipTaskbar: true, backgroundColor: '#00000000' });
  await companion.loadURL('data:text/html,' + encodeURIComponent(
    '<title>' + companionTitle + '</title><style>html,body{margin:0;background:transparent}' +
    '#pet{position:absolute;left:40px;top:40px;width:80px;height:80px;background:#db5181}</style><div id="pet"></div>'));
  companion.show();
  driver = await startNativeInput(temporary, companionTitle);

  const native = await driver.request('nativeScreenState');
  assertDisplayMatchesNative(primary, native.primary, native.windowDpi, 'primary display');
  const targetDisplay = screen.getDisplayNearestPoint(center(defaultBounds));
  assertDisplayMatchesNative(targetDisplay, native.target, native.windowDpi, 'companion target display');
  assert.equal(targetDisplay.id, primary.id, 'default bottom-right companion must target the primary display');
  assert.deepEqual(screen.getDisplayMatching(defaultBounds), targetDisplay,
    'matching lookup must return the display containing the companion');
  assert.deepEqual(companion.getBounds(), defaultBounds,
    'BrowserWindow DIP bounds must roundtrip at the default bottom-right position');

  const geometry = await driver.request('geometry');
  assertPhysicalWindowInside(geometry, native.target.workArea,
    'default bottom-right companion physical bounds');
  const physicalMargin = Math.round(margin * native.windowDpi / 96);
  assertNear(geometry.right, rectRight(native.target.workArea) - physicalMargin,
    'default companion right-edge inset');
  assertNear(geometry.bottom, rectBottom(native.target.workArea) - physicalMargin,
    'default companion bottom-edge inset');

  await driver.request('focus');
  const pointer = {
    x: geometry.clientX + Math.round(70 * geometry.dpi / 96),
    y: geometry.clientY + Math.round(70 * geometry.dpi / 96)
  };
  await driver.request('pointerMove', pointer);
  const cursorNative = await driver.request('nativeScreenState');
  const cursorDip = screen.getCursorScreenPoint();
  assertRectNear({ ...cursorDip, width: 0, height: 0 }, {
    ...physicalPointToDip(cursorNative.cursor, cursorNative.target.bounds,
      targetDisplay.bounds, targetDisplay.scaleFactor), width: 0, height: 0
  }, 'cursor point');
  assert.equal(screen.getDisplayNearestPoint(cursorDip).id, targetDisplay.id,
    'cursor DIP point must resolve to its physical monitor');

  const sample = async (x, y) => (await driver.request('sample', {
    x: geometry.clientX + Math.round(x * geometry.dpi / 96),
    y: geometry.clientY + Math.round(y * geometry.dpi / 96), width: 3, height: 3
  }))[0].rgb;
  assert.deepEqual(await sample(8, 8), background,
    'transparent companion pixels must reveal the real window underneath');
  assert.deepEqual(await sample(70, 70), artwork,
    'companion artwork must remain visible through per-pixel alpha rendering');

  const displays = screen.getAllDisplays();
  assert.equal(displays.filter(display => display.id === primary.id).length, 1,
    'getAllDisplays must contain the primary display exactly once');

  for (const [index, display] of displays.entries()) {
    const point = center(display.bounds);
    assert.equal(screen.getDisplayNearestPoint(point).id, display.id,
      `display ${index}: center nearest-point lookup`);
    assert.equal(screen.getDisplayMatching({ x: point.x - 2, y: point.y - 2, width: 5, height: 5 }).id,
      display.id, `display ${index}: center matching lookup`);

    const requested = { x: point.x - 130, y: point.y - 114, ...companionSize };
    const title = `mini-screen-probe-${process.pid}-${index}`;
    const probe = new BrowserWindow({ ...requested, title, frame: false, show: false,
      minWidth: 0, minHeight: 0 });
    probes.push(probe);
    await probe.loadURL('data:text/html,' + encodeURIComponent(`<title>${title}</title>`));
    assert.deepEqual(probe.getBounds(), requested,
      `display ${index}: BrowserWindow DIP bounds roundtrip`);

    const probeNative = await driver.request('nativeScreenState', { title });
    assertDisplayMatchesNative(display, probeNative.target, probeNative.windowDpi,
      `display ${index}`);
    const probeGeometry = await driver.request('geometry', { title });
    assertPhysicalWindowInside(probeGeometry, probeNative.target.bounds,
      `display ${index}: probe physical bounds`);
  }

  const hasNegativeOrigin = displays.some(display => display.bounds.x < 0 || display.bounds.y < 0);
  if (hasNegativeOrigin) {
    const negative = displays.find(display => display.bounds.x < 0 || display.bounds.y < 0);
    assert.equal(screen.getDisplayNearestPoint(center(negative.bounds)).id, negative.id,
      'negative-origin display lookup must remain in DIP space');
  }

  const scaleFactors = new Set(displays.map(display => display.scaleFactor));
  if (scaleFactors.size > 1) {
    for (const display of displays)
      assert.equal(screen.getDisplayMatching(display.bounds).id, display.id,
        `mixed-DPI full display match ${display.id}`);
  }

  await finish();
}).catch(finish);
