'use strict';

// Run in a fresh native mini-electron process. This intentionally exhausts only
// the disposable sandboxed renderer owned by this smoke.
const assert = require('node:assert/strict');
const { once } = require('node:events');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-renderer-memory-'));
app.setPath('userData', path.join(temporary, 'profile'));

let window;
let finished = false;
const deadline = setTimeout(() => finish(new Error('Renderer memory smoke timed out')), 180000);
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
    else console.log('PASS: Windows renderer V8 OOM preserves the true exit status and remains sandbox-bounded');
    app.exit(error ? 1 : 0);
  }
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  window = new BrowserWindow({
    width: 480, height: 320, show: false,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  const contents = window.webContents;
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(
    '<!doctype html><title>disposable renderer memory smoke</title>'));
  const rendererPid = contents.getProcessId();
  assert.ok(Number.isInteger(rendererPid) && rendererPid > 0 && rendererPid !== process.pid);

  const gone = once(contents, 'render-process-gone');
  const pressure = contents.executeJavaScript(`(() => {
    globalThis.__rendererMemorySmoke = [];
    for (;;) globalThis.__rendererMemorySmoke.push(new Array(8 * 1024 * 1024).fill(37));
  })()`);
  pressure.catch(() => {});
  const [, details] = await gone;

  assert.equal(details.reason, 'oom');
  assert.equal(details.exitCode, 0xe0000008);
  assert.equal(contents.isCrashed(), true);
  assert.equal(contents.getProcessId(), 0);
  assert.equal(window.isDestroyed(), false);
  finish();
}).catch(finish);
