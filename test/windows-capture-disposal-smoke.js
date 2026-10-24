'use strict';

// Run both modes in fresh native Windows processes with --disable-gpu. The
// immediate-exit mode leaves no task-loop turn to drain cancellation callbacks:
// their V8 persistent handles must be released before the isolate is disposed.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-capture-disposal-'));
app.setPath('userData', path.join(temporary, 'profile'));
app.commandLine.appendSwitch('disable-gpu');
app.on('window-all-closed', () => {});
const mode = process.env.MINI_CAPTURE_DISPOSAL_MODE || 'immediate-exit';
let window;
let finished = false;
const deadline = setTimeout(() => finish(new Error('Capture disposal smoke timed out')), 30000);

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  try {
    if (window && !window.isDestroyed()) window.destroy();
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
    if (error) console.error(error.stack || error);
    else console.log(`PASS: pending software capture disposal (${mode})`);
    app.exit(error ? 1 : 0);
  }
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  assert.ok(['immediate-exit', 'continue'].includes(mode), 'unknown capture disposal mode');
  window = new BrowserWindow({ width: 1536, height: 1536, show: false,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadURL('data:text/html,<style>html,body{margin:0;background:rgb(31,71,113)}</style>');
  await window.webContents.executeJavaScript(`(async () => {
    const canvas = document.createElement('canvas');
    canvas.width = canvas.height = 1536;
    const context = canvas.getContext('2d', {alpha: false});
    context.fillStyle = 'rgb(31,71,113)';
    context.fillRect(0, 0, 1536, 1536);
    const image = new Image();
    const loaded = new Promise((resolve, reject) => {
      image.onload = resolve;
      image.onerror = () => reject(new Error('PNG source load failed'));
    });
    image.src = canvas.toDataURL('image/png');
    await loaded;
    image.onload = image.onerror = null;
    document.body.replaceChildren(image);
    canvas.width = canvas.height = 1;
    return true;
  })()`);

  // Queue and destroy in the same host turn. No renderer response can dispatch
  // its owner-sequence completion before Close cancels these requests. This does
  // not assert that hidden capture renders, or depend on it staying suspended.
  const contents = window.webContents;
  const captures = Array.from({ length: 3 }, () => contents.capturePage().then(
    () => ({ resolved: true }), error => ({ error: error.message })));
  window.destroy();
  assert.equal(window.isDestroyed(), true);
  assert.equal(contents.isDestroyed(), true);
  if (mode === 'immediate-exit') {
    // In the failing implementation, Close posted closures owning V8 promises.
    // app.exit disposed the isolate before TaskQueueImpl destroyed those tasks,
    // causing an AV in PromiseBase::~PromiseBase / GlobalHandles::NodeSpace::Free.
    finish();
    return;
  }

  const results = await Promise.all(captures);
  for (const result of results) {
    assert.equal(result.resolved, undefined, 'pending capture must not deliver stale success');
    assert.match(result.error, /renderer closed/i, 'pending capture must reject on Close');
  }
  window = new BrowserWindow({ width: 320, height: 240, show: true,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadURL('data:text/html,<title>host survives capture cancellation</title>');
  assert.equal(await window.webContents.executeJavaScript('6 * 7'), 42);
  assert.equal(window.webContents.isCrashed(), false);
  finish();
}).catch(finish);
