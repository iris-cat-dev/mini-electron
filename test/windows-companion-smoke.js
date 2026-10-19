'use strict';

// Run on an unlocked Windows desktop:
// out/windows-x64/runtime-dist/electron.exe test/windows-companion-smoke.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, screen } = require('electron');
const { startNativeInput } = require('./windows-native-input-driver');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-windows-companion-'));
const baseTitle = `mini-companion-base-${process.pid}`;
const petTitle = `mini-companion-pet-${process.pid}`;
const background = [24, 69, 99];
const artwork = [219, 81, 129];
const pause = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
const dip = (value, dpi) => Math.floor(value * dpi / 96 + 0.5);
const WS_EX_TOPMOST = 0x00000008;
const WS_EX_TOOLWINDOW = 0x00000080;
const WS_EX_APPWINDOW = 0x00040000;
const WS_EX_LAYERED = 0x00080000;
const WS_EX_NOACTIVATE = 0x08000000;
const hasStyle = (state, flag) => (state.exStyle & flag) === flag;
function assertCompanionStyles(state, { taskbar, focusable }, label) {
  assert.equal(hasStyle(state, WS_EX_LAYERED), true, `${label}: per-pixel layered style`);
  assert.equal(hasStyle(state, WS_EX_TOPMOST), true, `${label}: topmost style`);
  assert.equal(hasStyle(state, WS_EX_TOOLWINDOW), !taskbar, `${label}: tool-window taskbar exclusion`);
  assert.equal(hasStyle(state, WS_EX_APPWINDOW), taskbar, `${label}: app-window taskbar eligibility`);
  assert.equal(hasStyle(state, WS_EX_NOACTIVATE), !focusable, `${label}: native activation eligibility`);
  assert.equal(state.owner, 0, `${label}: taskbar policy must not manufacture an owner window`);
}
let base, companion, driver, finished = false;
const deadline = setTimeout(() => finish(new Error('Windows companion smoke timed out')), 30000);
app.on('window-all-closed', () => {});

async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (driver) {
    try { await driver.close(); }
    catch (cleanupError) { error ||= cleanupError; }
  }
  for (const window of [companion, base]) if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: transparent companion preserves nonactivation, alpha artwork, visibility, and Windows taskbar/focusability transitions');
  app.exit(error ? 1 : 0);
}

app.whenReady().then(async () => {
  const area = screen.getPrimaryDisplay().workArea;
  const bounds = { x: area.x + 120, y: area.y + 120, width: 220, height: 180 };
  base = new BrowserWindow({ x: bounds.x - 20, y: bounds.y - 20, width: 280, height: 240,
    title: baseTitle, frame: false, show: false, backgroundColor: '#184563' });
  await base.loadURL('data:text/html,' + encodeURIComponent(
    '<style>html,body{margin:0;width:100%;height:100%;background:#184563}</style>'));
  base.show();
  companion = new BrowserWindow({ ...bounds, title: petTitle, frame: false, show: false,
    transparent: true, focusable: false, alwaysOnTop: true, hasShadow: false, minWidth: 0, minHeight: 0,
    resizable: false, skipTaskbar: true, backgroundColor: '#00000000' });
  companion.webContents.on('will-navigate', event => event.preventDefault());
  await companion.loadURL('data:text/html,' + encodeURIComponent(
    '<style>html,body{margin:0;background:transparent}#pet{position:absolute;left:40px;top:40px;width:80px;height:80px;background:#db5181}</style><div id="pet"></div>'));
  companion.setIgnoreMouseEvents(true, { forward: true });
  driver = await startNativeInput(temporary, baseTitle);

  const constructed = await driver.request('nativeWindowState', { title: petTitle });
  assert.equal(constructed.visible, false, 'constructor keeps show:false companion hidden');
  assert.equal(constructed.minimized, false, 'constructor does not encode hidden as minimized');
  assertCompanionStyles(constructed, { taskbar: false, focusable: false }, 'constructor');

  for (const label of ['first show', 'show after hide']) {
    await driver.request('focusSource');
    companion.showInactive();
    const native = await driver.request('geometry', { title: petTitle });
    assert.equal(native.minimized, false, `${label}: native companion must not be minimized`);
    assert.equal(companion.isMinimized(), false, `${label}: JavaScript minimized state`);
    assert.equal(companion.isVisible(), true, `${label}: companion visible`);
    assert.deepEqual(companion.getBounds(), bounds, `${label}: requested on-screen bounds preserved`);
    const focus = await driver.request('pointerProbe', { title: petTitle,
      x: native.clientX + dip(70, native.dpi), y: native.clientY + dip(70, native.dpi) });
    assert.equal(focus.foregroundProcess, focus.helperProcess,
      `${label}: showInactive must not activate over another application`);
    await pause(80);
    const sample = async (x, y) => (await driver.request('sample', { title: petTitle,
      x: native.clientX + dip(x, native.dpi), y: native.clientY + dip(y, native.dpi),
      width: 3, height: 3, inactive: true }))[0].rgb;
    assert.deepEqual(await sample(8, 8), background,
      `${label}: transparent pixels must reveal the real underlay`);
    assert.deepEqual(await sample(70, 70), artwork,
      `${label}: native display must contain the companion artwork`);
    const state = await driver.request('nativeWindowState', { title: petTitle });
    assert.equal(state.visible, true, `${label}: native companion visible`);
    assertCompanionStyles(state, { taskbar: false, focusable: false }, label);
    const artifacts = process.env.MINI_NATIVE_INPUT_ARTIFACTS;
    if (artifacts) {
      fs.mkdirSync(artifacts, { recursive: true });
      await driver.request('capture', { title: petTitle, inactive: true,
        path: path.join(artifacts, `windows-companion-${process.pid}.png`) });
    }
    companion.hide();
  }

  await driver.request('focusSource');
  companion.showInactive();
  companion.setSkipTaskbar(false);
  let state = await driver.request('nativeWindowState', { title: petTitle });
  assertCompanionStyles(state, { taskbar: false, focusable: false },
    'explicit skipTaskbar false while nonfocusable');

  companion.setFocusable(true);
  state = await driver.request('nativeWindowState', { title: petTitle });
  assert.equal(state.visible, true, 'setFocusable(true) preserves visibility');
  assertCompanionStyles(state, { taskbar: true, focusable: true },
    'focusable restores explicit taskbar preference');

  companion.setSkipTaskbar(true);
  state = await driver.request('nativeWindowState', { title: petTitle });
  assertCompanionStyles(state, { taskbar: false, focusable: true },
    'dynamic skipTaskbar true');

  companion.setSkipTaskbar(false);
  state = await driver.request('nativeWindowState', { title: petTitle });
  assertCompanionStyles(state, { taskbar: true, focusable: true },
    'dynamic skipTaskbar false');

  companion.setFocusable(false);
  state = await driver.request('nativeWindowState', { title: petTitle });
  assertCompanionStyles(state, { taskbar: false, focusable: false },
    'nonfocusable implies taskbar exclusion');
  companion.setFocusable(true);
  state = await driver.request('nativeWindowState', { title: petTitle });
  assertCompanionStyles(state, { taskbar: true, focusable: true },
    'focusability transition restores explicit taskbar preference');
  const native = await driver.request('geometry', { title: petTitle });
  const focus = await driver.request('pointerProbe', { title: petTitle,
    x: native.clientX + dip(70, native.dpi), y: native.clientY + dip(70, native.dpi) });
  assert.equal(focus.foregroundProcess, focus.helperProcess,
    'runtime taskbar and focusability setters must not activate the companion');
  const sample = async (x, y) => (await driver.request('sample', { title: petTitle,
    x: native.clientX + dip(x, native.dpi), y: native.clientY + dip(y, native.dpi),
    width: 3, height: 3, inactive: true }))[0].rgb;
  assert.deepEqual(await sample(8, 8), background,
    'runtime style transitions preserve transparent pixels');
  assert.deepEqual(await sample(70, 70), artwork,
    'runtime style transitions preserve companion artwork');
  companion.setFocusable(false);
  companion.hide();
  await finish();
}).catch(finish);
