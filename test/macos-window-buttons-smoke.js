'use strict';

// Requires a source checkout and Xcode. The helper queues AppKit mouse events
// for the application's native pump, covering layout, hit testing, and callbacks
// entered from outside JavaScript. It does not invoke window actions directly.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-window-buttons-'));
const title = `mini-window-buttons-${process.pid}`;
let finished = false;
let window;
const deadline = setTimeout(() => finish(new Error('Native window buttons smoke timed out')), 60000);
app.on('window-all-closed', () => {});

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  try {
    if (window && !window.isDestroyed()) window.destroy();
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
    if (error) console.error(error.stack || error);
    else console.log('PASS: macOS window buttons retain equal spacing, native hit targets, and close/minimize/zoom actions');
    app.exit(error ? 1 : 0);
  }
}

const pause = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));

async function eventually(read, expected, message) {
  const until = Date.now() + 5000;
  let actual;
  do {
    actual = read();
    if (actual === expected) return;
    await pause(20);
  } while (Date.now() < until);
  assert.equal(actual, expected, message);
}

function closeEnough(actual, expected, message) {
  assert.ok(Math.abs(actual - expected) <= 0.01,
    `${message}: expected ${expected}, received ${actual}`);
}

function assertLayout(native, expectedY) {
  const snapshot = native.snapshot(title);
  const buttons = [snapshot.close, snapshot.minimize, snapshot.zoom];
  closeEnough(snapshot.close.x, 16, 'close button x position');
  for (const button of buttons) {
    closeEnough(button.top, expectedY, 'button top margin');
    assert.equal(button.hitTarget, true, 'button center must resolve to its native NSButton');
  }
  const firstGap = snapshot.minimize.x - snapshot.close.x - snapshot.close.width;
  const secondGap = snapshot.zoom.x - snapshot.minimize.x - snapshot.minimize.width;
  closeEnough(secondGap, firstGap, 'all window buttons must use the same native gap');
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const root = path.resolve(__dirname, '..');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'window-buttons.node');
  const includes = ['', 'gen', 'third_party/abseil-cpp', 'base/allocator/partition_allocator/src',
    'gen/base/allocator/partition_allocator/src', 'third_party/libnode/src'];
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup',
    '-isysroot', sdk, ...includes.map(include => `-I${path.join(root, include)}`), '-framework', 'AppKit',
    path.join(__dirname, 'macos-window-buttons-helper.mm'), '-o', helper],
  { stdio: 'inherit', timeout: 30000 });
  const native = require(helper);

  window = new BrowserWindow({
    width: 720,
    height: 480,
    title,
    titleBarStyle: 'hidden',
    titleBarOverlay: true,
    trafficLightPosition: { x: 16, y: 14 },
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html>
    <title>${title}</title><style>
      html,body { margin:0; width:100%; height:100%; background:#181818; }
      #caption { position:absolute; inset:0 0 auto; height:64px; -webkit-app-region:drag; }
    </style><div id="caption"></div>`));

  assertLayout(native, 14);
  window.setWindowButtonPosition({ x: 16, y: 9 });
  assertLayout(native, 9);
  window.setWindowButtonPosition({ x: 16, y: 14.5 });
  assertLayout(native, 14.5);

  assert.equal(native.click(title, 'minimize'), true);
  await eventually(() => window.isMinimized(), true,
    'clicking the native minimize button must minimize the BrowserWindow');
  window.restore();
  await eventually(() => window.isMinimized(), false,
    'restoring after the minimize action must complete');

  assert.equal(native.click(title, 'zoom'), true);
  await eventually(() => window.isMaximized(), true,
    'clicking the native zoom button must maximize the BrowserWindow');
  window.unmaximize();
  await eventually(() => window.isMaximized(), false,
    'unmaximizing after the zoom action must complete');
  assertLayout(native, 14.5);

  let closeEvents = 0;
  window.on('close', () => ++closeEvents);
  assert.equal(native.click(title, 'close'), true);
  await eventually(() => window.isDestroyed(), true,
    'clicking the native close button must destroy the BrowserWindow');
  assert.equal(closeEvents, 1, 'the native close button must emit close exactly once');
  window = undefined;
  finish();
}).catch(finish);
