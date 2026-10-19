'use strict';

// Invoke: /path/to/mini-electron --app=test/macos-selection-colors-smoke.js
// Requires a macOS graphical session and Xcode command-line tools. Captures
// real Blink pixels through the native renderer view; no Screen Recording
// permission is needed. PNG evidence remains in the printed temporary folder.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-selection-colors-'));
const windows = [];
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let native, finished = false;
const watchdog = setTimeout(() => finish(new Error('Selection smoke exceeded 60s watchdog')), 60000);
app.on('window-all-closed', () => {});
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  for (const window of windows) if (!window.isDestroyed()) window.destroy();
  console.log(`Native selection PNG evidence: ${temporary}`);
  if (error) console.error(error.stack || error);
  else console.log('PASS: native active/inactive filename and input selection, CSS Highlight, light/dark');
  app.exit(error ? 1 : 0);
}
async function eventually(label, predicate) {
  let last;
  for (let attempt = 0; attempt < 60; ++attempt) {
    last = await predicate();
    if (last === true) return;
    await pause(50);
  }
  throw new Error(`${label}: ${JSON.stringify(last)}`);
}
const closeColor = (actual, expected) => actual.every((value, index) => Math.abs(value - expected[index]) <= 8);
function visibleHighlight(pixel, background) {
  return Math.max(...pixel) > 32 && pixel.some(value => Math.abs(value - background) > 12);
}
function nativeSelection(pixel, nativeColor, background) {
  if (!pixel.some(value => Math.abs(value - background) > 12)) return false;
  // Native-view capture passes through display color management. Preserve the
  // system color's hue, not incidental RGB values or Blink's alpha algorithm.
  for (let a = 0; a < 3; ++a) {
    for (let b = 0; b < 3; ++b) {
      if (nativeColor[a] - nativeColor[b] > 16 && pixel[a] - pixel[b] < 8) return false;
    }
  }
  return Math.max(...nativeColor.slice(0, 3)) - Math.min(...nativeColor.slice(0, 3)) > 16 ||
    Math.max(...pixel) - Math.min(...pixel) <= 12;
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const root = path.resolve(__dirname, '..');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'selection-colors.node');
  const includes = ['', 'gen', 'third_party/abseil-cpp', 'base/allocator/partition_allocator/src',
    'gen/base/allocator/partition_allocator/src', 'third_party/libnode/src'];
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup', '-isysroot', sdk,
    ...includes.map(include => `-I${path.join(root, include)}`), '-framework', 'AppKit',
    path.join(__dirname, 'macos-selection-colors-helper.mm'), '-o', helper], { stdio: 'inherit', timeout: 30000 });
  native = require(helper);
  const blocker = new BrowserWindow({ width: 100, height: 100, x: 0, y: 0, show: false, title: 'selection-focus-blocker' });
  windows.push(blocker);
  for (const dark of [false, true]) {
    const scheme = dark ? 'dark' : 'light';
    const title = `mini-selection-${scheme}-${process.pid}`;
    const window = new BrowserWindow({ title, width: 640, height: 420, show: true,
      webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
    windows.push(window);
    const evaluate = script => window.webContents.executeJavaScript(script);
    await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html>
      <meta name="color-scheme" content="${scheme}"><title>${title}</title>
      <style>html{color-scheme:${scheme};background:${dark ? '#181818' : 'white'}}
      body{margin:24px;font:32px monospace;color:${dark ? 'white' : 'black'}}
      #filename{display:inline-block;white-space:pre}input{display:block;margin-top:24px;
      font:32px monospace;width:540px;padding:0;border:0;background:transparent;color:inherit}
      #highlight{margin-top:24px;width:240px;height:48px;background:Highlight;color:HighlightText}</style>
      <span id="filename">example-file.txt        </span><input id="input" value="example-file.txt        ">
      <div id="highlight"></div>`));
    const colors = native.nativeColors(dark);
    const background = dark ? 24 : 255;
    assert.ok(colors.active.slice(0, 3).some(value => value > 32), 'Native active color must not be black');
    for (const kind of ['filename', 'input']) {
      blocker.hide();
      window.show();
      window.focus();
      await eventually(`${scheme}/${kind} focused`, async () =>
        window.isFocused() && await evaluate('document.hasFocus()'));
      const boxes = JSON.parse(await evaluate(`(() => {
        const target = document.getElementById('${kind}');
        if ('${kind}' === 'input') { target.focus(); target.select(); }
        else { document.activeElement.blur(); const range = document.createRange();
          range.selectNodeContents(target); const selection = getSelection();
          selection.removeAllRanges(); selection.addRange(range); }
        const r = target.getBoundingClientRect(), h = document.getElementById('highlight').getBoundingClientRect();
        return JSON.stringify([[r.x + 330, r.y + 8, 50, 18], [h.x + 20, h.y + 10, 80, 20]]);
      })()`));
      const activeFile = path.join(temporary, `${scheme}-${kind}-active.png`);
      let activePixels;
      await eventually(`${scheme}/${kind} actual active selection and CSS Highlight pixels`, () => {
        activePixels = native.snapshot(title, boxes, activeFile);
        return (visibleHighlight(activePixels[0], background) &&
          nativeSelection(activePixels[0], colors.active, background) &&
          visibleHighlight(activePixels[1], background)) || { activePixels, colors };
      });
      blocker.show();
      blocker.focus();
      await eventually(`${scheme}/${kind} lost native focus`, async () =>
        !window.isFocused() && !await evaluate('document.hasFocus()'));
      const inactiveFile = path.join(temporary, `${scheme}-${kind}-inactive.png`);
      let inactivePixels;
      await eventually(`${scheme}/${kind} actual inactive selection pixels`, () => {
        inactivePixels = native.snapshot(title, boxes, inactiveFile);
        return (nativeSelection(inactivePixels[0], colors.inactive, background) &&
          !closeColor(inactivePixels[0], activePixels[0]) &&
          closeColor(inactivePixels[1], activePixels[1])) || { inactivePixels, activePixels, colors };
      });
      console.log(JSON.stringify({ scheme, kind, colors, activePixels, inactivePixels, activeFile, inactiveFile }));
    }
    window.destroy();
  }
  finish();
}).catch(finish);
