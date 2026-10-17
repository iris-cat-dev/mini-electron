'use strict';

// Run in a fresh native mini-electron process. Only this smoke's own renderer
// child is killed; no existing desktop instance or its children are touched.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const { once } = require('node:events');
const { app, BrowserWindow, webContents } = require('electron');

let window;
let finished = false;
const deadline = setTimeout(() => finish(new Error('Renderer loss lifetime smoke timed out')), 30000);
app.on('window-all-closed', () => {});

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  try {
    if (window && !window.isDestroyed()) window.destroy();
  } finally {
    if (error) console.error(error.stack || error);
    else console.log('PASS: renderer loss preserves WebContents and BrowserWindow lifetime until actual close');
    app.exit(error ? 1 : 0);
  }
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  window = new BrowserWindow({
    width: 480, height: 320, show: true,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  const contents = window.webContents;
  const contentsId = contents.id;
  const windowId = window.id;
  let destroyedEvents = 0;
  let goneEvents = 0;
  contents.on('destroyed', () => {
    ++destroyedEvents;
    assert.equal(contents.isDestroyed(), true, 'Destruction is observable during its notification');
    assert.equal(webContents.fromId(contentsId), null);
  });
  contents.on('render-process-gone', () => ++goneEvents);
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent('<!doctype html><title>renderer lifetime smoke</title><body>owned window</body>'));
  assert.equal(contents.isDestroyed(), false);
  const rendererPid = contents.getProcessId();
  assert.ok(Number.isInteger(rendererPid) && rendererPid > 1 && rendererPid !== process.pid);
  const parentPid = Number(execFileSync('/bin/ps', ['-o', 'ppid=', '-p', String(rendererPid)], { encoding: 'utf8' }).trim());
  assert.equal(parentPid, process.pid, 'Only a renderer child belonging to this smoke may be killed');
  const gone = once(contents, 'render-process-gone');
  process.kill(rendererPid, 'SIGKILL');
  await gone;
  assert.equal(goneEvents, 1, 'Child loss must emit render-process-gone exactly once');
  assert.equal(contents.isCrashed(), true);
  assert.equal(contents.isDestroyed(), false, 'Renderer loss is not WebContents destruction');
  assert.equal(destroyedEvents, 0);
  assert.equal(contents.getProcessId(), 0, 'The dead renderer transport must be released');
  assert.equal(window.isDestroyed(), false);
  assert.equal(window.webContents, contents);
  assert.equal(contents.getOwnerBrowserWindow(), window);
  assert.equal(BrowserWindow.fromId(windowId), window);
  assert.equal(BrowserWindow.fromWebContents(contents), window);
  assert.ok(BrowserWindow.getAllWindows().includes(window), 'Native window remains registry-eligible');
  assert.equal(webContents.fromId(contentsId), contents);
  assert.ok(webContents.getAllWebContents().includes(contents));
  // These operations target the existing native NSWindow, not a replacement.
  window.setTitle('renderer gone, native window still owned');
  assert.equal(window.getTitle(), 'renderer gone, native window still owned');
  window.setBounds({ x: 180, y: 180, width: 500, height: 340 });
  assert.equal(window.getBounds().width, 500);
  assert.equal(window.getBounds().height, 340);
  const closed = once(window, 'closed');
  window.close();
  await closed;
  assert.equal(contents.isDestroyed(), true, 'Actual window close destroys WebContents');
  assert.equal(destroyedEvents, 1);
  assert.equal(goneEvents, 1);
  assert.equal(window.isDestroyed(), true);
  assert.equal(BrowserWindow.fromId(windowId), null);
  assert.equal(BrowserWindow.fromWebContents(contents), null);
  assert.ok(!BrowserWindow.getAllWindows().includes(window));
  assert.equal(webContents.fromId(contentsId), null);
  assert.ok(!webContents.getAllWebContents().includes(contents));
  finish();
}).catch(finish);
