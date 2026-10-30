'use strict';

// Run in a fresh native mac runtime; this opens real browser-process FDs and
// launches real sandbox children, rather than checking spawn-action wiring.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const { once } = require('node:events');
const { app, BrowserWindow } = require('electron');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
let window;
let held = [];
let finished = false;
app.on('window-all-closed', () => {});
const deadline = setTimeout(() => finish(new Error('mac FD remap deadline exceeded')), 60000);

function releaseDescriptors() {
  for (const fd of held) {
    fs.closeSync(fd);
    assert.throws(() => fs.fstatSync(fd), { code: 'EBADF' }, 'host reservation is released');
  }
  held = [];
}

function reserveDescriptors(firstFree) {
  let fd;
  do {
    fd = fs.openSync('/dev/null', 'r');
    held.push(fd);
  } while (fd < 203);
  // Keep lower descriptors occupied and free the desired alias interval.
  // A firstFree of 204 instead keeps all six destinations occupied in the host.
  for (const reserved of held.filter(value => value >= firstFree))
    fs.closeSync(reserved);
  held = held.filter(value => value < firstFree);
  const probe = fs.openSync('/dev/null', 'r');
  try {
    assert.equal(probe, firstFree, 'real host allocation reaches the requested FD boundary');
  } finally {
    fs.closeSync(probe);
  }
  assert.ok(held.length > 0, 'real host files remain open throughout renderer launch');
}

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  try {
    if (window && !window.isDestroyed()) window.destroy();
    releaseDescriptors();
  } catch (cleanupError) {
    error = error || cleanupError;
  }
  if (error) console.error(error.stack || error);
  else console.log('PASS: mac sandbox renderer low, aliased 198..203, and high FD remaps');
  app.exit(error ? 1 : 0);
}

async function exerciseRenderer(label) {
  window = new BrowserWindow({
    show: false, width: 320, height: 240,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  const contents = window.webContents;
  let gone;
  contents.on('render-process-gone', (_, details) => { gone = details; });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(
    '<!doctype html><title>FD remap</title><body><input id="value"><div id="result"></div></body>'));
  const pid = contents.getProcessId();
  assert.ok(pid > 0 && pid !== process.pid, `${label}: distinct real renderer process`);
  assert.deepEqual(await contents.executeJavaScript(`(() => {
    document.getElementById('value').value = 'private channels survived';
    document.getElementById('result').textContent = document.getElementById('value').value;
    document.title = 'FD remap changed';
    return [document.title, document.getElementById('result').textContent, typeof require];
  })()`), ['FD remap changed', 'private channels survived', 'undefined'], `${label}: sandbox page operations`);
  // A second navigation and round trip exercise the surviving control transport.
  await window.loadURL('data:text/html,<title>second FD remap page</title><body>second page</body>');
  assert.equal(await contents.executeJavaScript('document.body.textContent'), 'second page', label);
  assert.equal(gone, undefined, `${label}: diagnostics do not consume renderer control messages`);
  for (const fd of held)
    assert.ok(fs.fstatSync(fd).isCharacterDevice(), `${label}: host reservation remains open`);
  const closed = once(window, 'closed');
  window.close();
  await closed;
  assert.equal(contents.isDestroyed(), true, `${label}: close releases WebContents`);
  window = undefined;
  let exited = false;
  for (let attempt = 0; attempt < 100 && !exited; ++attempt) {
    try { process.kill(pid, 0); }
    catch (error) {
      if (error.code !== 'ESRCH') throw error;
      exited = true;
    }
    if (!exited) await sleep(20);
  }
  assert.ok(exited, `${label}: close reaps the actual renderer child`);
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  await exerciseRenderer('low FD');
  for (const firstFree of [198, 199, 200, 201, 202, 203, 204]) {
    reserveDescriptors(firstFree);
    await exerciseRenderer(`first free FD ${firstFree}`);
    releaseDescriptors();
  }
  // Check that descriptors were released sufficiently for another normal spawn.
  await exerciseRenderer('low FD after high FD teardown');
  finish();
}).catch(finish);
