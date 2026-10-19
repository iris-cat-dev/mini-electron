'use strict';

// Exercise the actual native StorageAreaStub -> CachedStorageArea observer path
// on a fresh HTTP origin. A renderer crash fails the smoke; none is hidden.
const assert = require('node:assert/strict');
const http = require('node:http');
const { once } = require('node:events');
const { app, BrowserWindow } = require('electron');
let window;
let server;
let finished = false;
let phase = 'startup';
const deadline = setTimeout(() => finish(new Error(`Native storage mutation smoke timed out during ${phase}`)), 60000);
app.on('window-all-closed', () => {});
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  try {
    if (window && !window.isDestroyed()) window.destroy();
    if (server) server.close();
  } finally {
    if (error) console.error(error.stack || error);
    else console.log('PASS: native localStorage mutation acknowledgements survive repeated writes, deletes and clears');
    app.exit(error ? 1 : 0);
  }
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  server = http.createServer((request, response) => {
    response.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    response.end('<!doctype html><title>native storage mutation smoke</title><body>local storage</body>');
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  window = new BrowserWindow({ show: false, webPreferences: {
    sandbox: true, contextIsolation: true, nodeIntegration: false,
  } });
  window.webContents.on('render-process-gone', (...args) =>
    finish(new Error('Storage mutation killed renderer: ' + JSON.stringify(args.at(-1)))));
  await window.loadURL(`http://127.0.0.1:${server.address().port}/`);
  phase = 'installing mutation fixture';
  await window.webContents.executeJavaScript(`window.storageSmokeRound = () => {
    const key = '__mini_native_storage_probe';
    const values = ['first', 'second', 'second', '', '中文', 'emoji 😀'];
    localStorage.clear();
    for (const value of values) {
      localStorage.setItem(key, value);
      if (localStorage.getItem(key) !== value) throw new Error('Local storage write was not visible');
    }
    localStorage.removeItem(key);
    if (localStorage.getItem(key) !== null) throw new Error('Delete did not remove key');
    localStorage.setItem(key, 'before clear');
    localStorage.clear();
    return JSON.stringify({ removed: localStorage.getItem(key), remaining: localStorage.length });
  }; true;`);
  for (let round = 0; round < 100; ++round) {
    phase = `mutation batch ${round + 1}`;
    const result = await window.webContents.executeJavaScript('window.storageSmokeRound()');
    assert.deepEqual(JSON.parse(result), { removed: null, remaining: 0 });
    // Exercise a burst in one JS stack, then allow posted acknowledgements to
    // drain before the next burst.
    await new Promise(resolve => setTimeout(resolve, 0));
  }
  phase = 'persisting final value';
  const result = await window.webContents.executeJavaScript(
    'localStorage.setItem("__mini_native_storage_probe", "persisted"); localStorage.getItem("__mini_native_storage_probe")');
  assert.equal(result, 'persisted');
  assert.equal(window.webContents.isCrashed(), false);
  phase = 'reloading persisted origin';
  const loaded = once(window.webContents, 'did-finish-load');
  window.reload();
  await loaded;
  const persisted = await window.webContents.executeJavaScript('localStorage.getItem("__mini_native_storage_probe")');
  assert.equal(persisted, 'persisted');
  finish();
}).catch(finish);
