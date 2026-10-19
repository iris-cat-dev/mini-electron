'use strict';
// Default uses genuine AppKit input to owned native windows, without permission.
// MINI_COMPANION_PHYSICAL=1 additionally proves WindowServer routing using HID.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-companion-'));
const windows = [];
let finished = false;
const deadline = setTimeout(() => finish(new Error('Companion smoke timed out')), 60000);
app.on('window-all-closed', () => {});
let native;
function finish(error) {
  if (finished) return;
  finished = true;
  if (native) native.restorePointer();
  clearTimeout(deadline);
  for (const window of windows) if (!window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: native transparent inactive companion policies, ignored-window keyboard input, and AppKit renderer input' +
    (process.env.MINI_COMPANION_PHYSICAL === '1' ? ', physical movement forwarding and mouse passthrough' : ''));
  app.exit(error ? 1 : 0);
}
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const evaluate = (window, source) => window.webContents.executeJavaScript(source);
async function eventually(read, expected) {
  const until = Date.now() + 4000;
  let actual;
  do {
    actual = await read();
    if (actual === expected) return;
    await pause(25);
  } while (Date.now() < until);
  assert.equal(actual, expected);
}
app.whenReady().then(async () => {
  const root = path.resolve(__dirname, '..');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'companion.node');
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup',
    '-isysroot', sdk, `-I${path.join(root, 'third_party/libnode/src')}`,
    '-framework', 'AppKit', '-framework', 'ApplicationServices',
    path.join(__dirname, 'macos-companion-helper.mm'), '-o', helper], { stdio: 'inherit', timeout: 30000 });
  native = require(helper);
  const keyboardHelper = path.join(temporary, 'keyboard.node');
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup',
    '-isysroot', sdk, `-I${path.join(root, 'third_party/libnode/src')}`,
    '-framework', 'AppKit', '-framework', 'Carbon',
    path.join(__dirname, 'macos-shortcuts-helper.mm'), '-o', keyboardHelper],
  { stdio: 'inherit', timeout: 30000 });
  const keyboard = require(keyboardHelper);
  const bounds = { x: 240, y: 240, width: 240, height: 240 };
  const baseTitle = `mini-companion-base-${process.pid}`;
  const petTitle = `mini-companion-overlay-${process.pid}`;
  const keyboardTitle = `mini-companion-keyboard-${process.pid}`;
  const base = new BrowserWindow({ ...bounds, frame: false, title: baseTitle });
  windows.push(base);
  const html = title => 'data:text/html,' + encodeURIComponent(`<!doctype html><title>${title}</title>
    <style>html,body{margin:0;width:100%;height:100%;background:transparent}</style>
    <script>window.downs=0;window.moves=0;document.addEventListener('mousedown',()=>++downs);document.addEventListener('mousemove',()=>++moves);</script>`);
  await base.loadURL(html(baseTitle));
  base.show();
  console.log('base native focus before wait:', native.snapshot(baseTitle));
  await eventually(() => base.isFocused(), true);
  console.log('base native focus after wait:', native.snapshot(baseTitle));
  const beforePid = native.snapshot(baseTitle).frontmostPid;
  const keyboardWindow = new BrowserWindow({ ...bounds, x: bounds.x + bounds.width + 40,
    title: keyboardTitle, frame: false });
  windows.push(keyboardWindow);
  await keyboardWindow.loadURL('data:text/html,' + encodeURIComponent(`<!doctype html><title>${keyboardTitle}</title>
    <input autofocus><script>window.downs=0;window.inputEvents=0;
    document.addEventListener('mousedown',()=>++downs);
    document.querySelector('input').addEventListener('input',()=>++inputEvents);</script>`));
  keyboardWindow.show();
  keyboardWindow.focus();
  await eventually(() => keyboardWindow.isFocused(), true);
  keyboardWindow.setIgnoreMouseEvents(true);
  assert.equal(native.snapshot(keyboardTitle).ignoresMouseEvents, true);
  native.appKitPointer(keyboardTitle, 20, 20, true);
  await pause(50);
  assert.equal(await evaluate(keyboardWindow, 'window.downs'), 0);
  assert.equal(keyboardWindow.isFocused(), true);
  keyboard.key(keyboardTitle, 0, 0);
  await eventually(() => evaluate(keyboardWindow, 'window.inputEvents'), 1);
  assert.equal(await evaluate(keyboardWindow, 'document.querySelector("input").value.length'), 1);
  keyboardWindow.hide();
  base.focus();
  await eventually(() => base.isFocused(), true);
  const overlay = new BrowserWindow({ ...bounds, title: petTitle, frame: false, show: false,
    transparent: true, focusable: false, alwaysOnTop: true, hasShadow: false, resizable: false });
  windows.push(overlay);
  await overlay.loadURL(html(petTitle));
  overlay.setIgnoreMouseEvents(true, { forward: true });
  overlay.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true });
  overlay.showInactive();
  const state = native.snapshot(petTitle);
  for (const key of ['visible', 'ignoresMouseEvents', 'alwaysOnTop', 'allWorkspaces', 'fullScreenAuxiliary']) assert.equal(state[key], true, key);
  for (const key of ['key', 'canBecomeKey', 'opaque', 'rendererOpaque', 'hasShadow']) assert.equal(state[key], false, key);
  assert.equal(state.backgroundAlpha, 0);
  assert.equal(state.frontmostPid, beforePid);
  assert.equal(base.isFocused(), true);
  native.appKitPointer(petTitle, 70, 70, false);
  await eventually(async () => (await evaluate(overlay, 'window.moves')) > 0, true);
  if (process.env.MINI_COMPANION_PHYSICAL === '1') {
    native.pointer(petTitle, 70, 70, true);
    await eventually(() => evaluate(base, 'window.downs'), 1);
    assert.equal(await evaluate(overlay, 'window.downs'), 0);
  }
  overlay.setIgnoreMouseEvents(false);
  assert.equal(native.snapshot(petTitle).ignoresMouseEvents, false);
  native.appKitPointer(petTitle, 90, 90, true);
  await eventually(() => evaluate(overlay, 'window.downs'), 1);
  overlay.setIgnoreMouseEvents(true, { forward: true });
  assert.equal(native.snapshot(petTitle).ignoresMouseEvents, true);
  assert.equal(base.isFocused(), true);
  overlay.setVisibleOnAllWorkspaces(false);
  assert.equal(native.snapshot(petTitle).allWorkspaces, false);
  assert.equal(native.snapshot(petTitle).fullScreenAuxiliary, false);
  finish();
}).catch(finish);
