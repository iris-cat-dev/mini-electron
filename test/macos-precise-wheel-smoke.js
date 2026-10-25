'use strict';

// Invoke: out/mac-arm64/mini-electron --app=test/macos-precise-wheel-smoke.js
// Real AppKit pixel events check the DOM contract consumed by xterm/VS Code.
// No application activation or synthetic DOM wheel events are required.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-precise-wheel-'));
app.setPath('userData', path.join(temporary, 'profile'));
const title = `mini-precise-wheel-${process.pid}`;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let window, finished = false;
const watchdog = setTimeout(() => finish(new Error('Native precise-wheel smoke exceeded 45s')), 45000);
app.on('window-all-closed', () => {});
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: native precise-wheel standard/legacy DOM deltas on both axes and directions, one-pixel input and mouse-wheel compatibility');
  app.exit(error ? 1 : 0);
}
const events = () => window.webContents.executeJavaScript('JSON.stringify(wheelEvents)').then(JSON.parse);
async function nextEvent(count) {
  for (let turn = 0; turn < 100; ++turn) {
    const actual = await events();
    if (actual.length > count) return actual.at(-1);
    await pause(25);
  }
  throw new Error('Native AppKit wheel did not reach the DOM');
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const root = path.resolve(__dirname, '..');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'pointer.node');
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup',
    '-isysroot', sdk, `-I${path.join(root, 'third_party/libnode/src')}`, '-framework', 'AppKit',
    path.join(__dirname, 'macos-pointer-interaction-helper.mm'), '-o', helper], { stdio: 'inherit', timeout: 30000 });
  const native = require(helper);
  window = new BrowserWindow({ title, width: 800, height: 600, show: true,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><title>${title}</title>
    <style>html,body{margin:0;height:100%;overflow:hidden;background:#181818;color:white;font:16px monospace}pre{padding:24px}</style>
    <pre id="output">Native precise-wheel DOM compatibility</pre><script>
    window.wheelEvents=[];
    document.addEventListener('wheel', event => {
      wheelEvents.push({x:event.deltaX,y:event.deltaY,legacyX:event.wheelDeltaX,legacyY:event.wheelDeltaY,mode:event.deltaMode});
      document.getElementById('output').textContent=JSON.stringify(wheelEvents,null,2);
    }, {capture:true,passive:true});
    </script>`));
  await window.webContents.prepareToShow();
  for (const [deltaX, deltaY] of [[0, 120], [0, -120], [120, 0], [-120, 0], [0, 1], [1, 0], [12, -18]]) {
    const count = (await events()).length;
    native.scroll(title, 400, 300, deltaX, deltaY, true);
    const event = await nextEvent(count);
    assert.equal(event.mode, 0, 'Precise wheel deltas must stay in logical pixels');
    assert.equal(event.x, deltaX === 0 ? 0 : -deltaX);
    assert.equal(event.y, deltaY === 0 ? 0 : -deltaY);
    // xterm reads legacy deltas first; zero wheel ticks silently disable scrollback.
    assert.equal(event.legacyX, deltaX, 'Precise horizontal ticks must match logical pixels');
    assert.equal(event.legacyY, deltaY, 'Precise vertical ticks must match logical pixels');
    console.log(JSON.stringify({ input: [deltaX, deltaY], event }));
  }
  const count = (await events()).length;
  native.scroll(title, 400, 300, 0, 1, false);
  const mouse = await nextEvent(count);
  assert.equal(mouse.y, -80, 'Traditional mouse-wheel pixel distance must remain unchanged');
  assert.ok(mouse.legacyY > 0, 'Traditional mouse-wheel ticks must remain positive when scrolling up');
  finish();
}).catch(finish);
