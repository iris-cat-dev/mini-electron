'use strict';

// Run with a bundled macOS runtime in a graphical session. Observe this test's
// shared frames without capturePage, rAF probes or a reference DisplayLink.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const { once } = require('node:events');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, ipcMain } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-threaded-animation-'));
app.setPath('userData', path.join(temporary, 'user-data'));
app.on('window-all-closed', () => {});
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let window;
let native;
let stage = 'bootstrap';
let finished = false;
const watchdog = setTimeout(() => finish(new Error(`Threaded animation smoke timed out during ${stage}`)), 60000);

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (window && !window.isDestroyed()) window.destroy();
  if (native) native.cleanup();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: CSS animation presents independently of busy Blink JavaScript and becomes idle when stopped');
  app.exit(error ? 1 : 0);
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const root = path.resolve(__dirname, '..');
  const env = { ...process.env };
  delete env.SDKROOT;
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { env, encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--sdk', 'macosx', '--find', 'clang++'], { env, encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'frames.node');
  const includes = ['', 'gen', 'third_party/abseil-cpp', 'base/allocator/partition_allocator/src',
    'gen/base/allocator/partition_allocator/src', 'third_party/libnode/src'];
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup', '-isysroot', sdk,
    ...includes.map(include => `-I${path.join(root, include)}`),
    '-framework', 'AppKit', '-framework', 'CoreGraphics', '-framework', 'CoreVideo', '-framework', 'QuartzCore', '-framework', 'ScreenCaptureKit',
    path.join(__dirname, 'macos-owned-window-capture.mm'), path.join(__dirname, 'macos-vsync-helper.mm'), '-o', helper],
  { env, stdio: 'inherit', timeout: 30000 });
  native = require(helper);
  const channel = `threaded-animation-busy:${process.pid}`;
  const doneChannel = `${channel}:done`;
  const preload = path.join(temporary, 'preload.js');
  fs.writeFileSync(preload, `
    const { contextBridge, ipcRenderer } = require('electron');
    contextBridge.exposeInMainWorld('animationProbe', {
      started() { ipcRenderer.send(${JSON.stringify(channel)}); },
      finished() { ipcRenderer.send(${JSON.stringify(doneChannel)}); }
    });
  `);
  window = new BrowserWindow({ width: 640, height: 400, show: true,
    webPreferences: { preload, sandbox: true, contextIsolation: true, nodeIntegration: false } });
  stage = 'loading the static document';
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><html><head>
    <title>Threaded CSS animation</title>
    <style>body{background:#222}#spinner{width:24px;height:24px;margin:40px;border:3px solid #777;
    border-top-color:white;border-radius:50%}.animate #spinner{animation:spin 1s linear infinite}
    @keyframes spin{to{transform:rotate(360deg)}}</style></head><body><div id="spinner"></div></body></html>`));
  stage = 'starting the CSS animation';
  await window.webContents.executeJavaScript('document.body.classList.add("animate")');
  await pause(700);

  const started = once(ipcMain, channel);
  const completed = once(ipcMain, doneChannel);
  let busyFinished = false;
  completed.then(() => { busyFinished = true; });
  // Let the injection request return before page-owned JavaScript becomes busy.
  await window.webContents.executeJavaScript(`setTimeout(() => {
    animationProbe.started();
    const deadline = performance.now() + 2000;
    while (performance.now() < deadline) {}
    animationProbe.finished();
  }, 50); true`);
  stage = 'waiting for the page-owned busy task';
  const [event] = await started;
  assert.equal(event.sender, window.webContents, 'Only the owned renderer starts the measurement');
  const before = native.frames();
  await pause(700);
  const after = native.frames();
  assert.equal(busyFinished, false, 'Observe presentation while the Blink main thread is still busy');
  assert.ok(before.some(frame => after.some(current => current.fd === frame.fd &&
    current.generation > frame.generation + 10)),
  'A compositor-eligible CSS animation must keep producing native frames while page JavaScript is busy');
  await completed;
  await window.webContents.executeJavaScript('document.getElementById("spinner").style.animation = "none"');
  await pause(600);
  const idleBefore = native.frames();
  await pause(400);
  assert.deepEqual(native.frames(), idleBefore, 'Stopping the animation must stop native frame production');
  finish();
}).catch(finish);
