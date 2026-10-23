'use strict';

// Run with the native runtime: mini-electron --app=test/fullscreen-lifecycle-smoke.js
// Exercises DOM/native state reconciliation and destruction of an in-flight request.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-fullscreen-review-'));
app.setPath('userData', path.join(temporary, 'profile'));
app.on('window-all-closed', () => {});
let window, finished = false;
const evidence = { transitions: [] };
const watchdog = setTimeout(() => finish(new Error('Fullscreen lifecycle timed out')), 25000);
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (window && !window.isDestroyed()) {
    evidence.nativeFullscreenAtFinish = window.isFullScreen();
    window.destroy();
  }
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  console.log(JSON.stringify({ ...evidence, passed: !error }));
  app.exit(error ? 1 : 0);
}
async function deadline(promise, label) {
  let timer;
  try {
    return await Promise.race([promise, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(label + ' did not settle; nativeFullscreen=' + window.isFullScreen())), 5000);
    })]);
  } finally { clearTimeout(timer); }
}
async function nativeState(expected) {
  for (let i = 0; i < 100 && window.isFullScreen() !== expected; ++i) await pause(30);
  assert.equal(window.isFullScreen(), expected);
}
app.whenReady().then(async () => {
  window = new BrowserWindow({ width: 640, height: 480, show: false });
  window.webContents.on('render-process-gone', (_event, details) => finish(new Error(JSON.stringify(details))));
  await window.loadURL('data:text/html,<title>DOM fullscreen lifecycle</title><div id="target">fullscreen target</div>');
  window.show();
  for (let cycle = 0; cycle < 2; ++cycle) {
    const entered = await deadline(window.webContents.executeJavaScript(
      'document.documentElement.requestFullscreen().then(() => document.fullscreenElement === document.documentElement)', true), 'DOM fullscreen enter');
    assert.equal(entered, true);
    await nativeState(true);
    const exited = await deadline(window.webContents.executeJavaScript(
      'document.exitFullscreen().then(() => document.fullscreenElement === null)'), 'DOM fullscreen exit');
    assert.equal(exited, true);
    await nativeState(false);
    evidence.transitions.push({ entered, exited });
  }
  assert.equal(await deadline(window.webContents.executeJavaScript(
    'document.getElementById("target").requestFullscreen().then(() => document.fullscreenElement.id)', true), 'element fullscreen enter'), 'target');
  await window.webContents.executeJavaScript('document.getElementById("target").remove(); true');
  await nativeState(false);
  evidence.removedElementExited = true;
  const pending = window.webContents.executeJavaScript('document.documentElement.requestFullscreen().then(() => true)', true);
  const cancelled = assert.rejects(pending, /destroy|closed|context|renderer|cancel/i);
  window.destroy();
  await cancelled;
  evidence.pendingEnterCancelled = true;
  finish();
}).catch(finish);
