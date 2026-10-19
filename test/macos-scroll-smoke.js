'use strict';

// Invoke: out/mac-arm64/mini-electron --app=test/macos-scroll-smoke.js
// Native AppKit pixel/line events exercise the renderer's actual scroll surface.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-scroll-'));
const title = `mini-scroll-${process.pid}`;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let window, native, finished = false;
const watchdog = setTimeout(() => finish(new Error('Native scroll smoke exceeded 45s')), 45000);
app.on('window-all-closed', () => {});
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(path.join(temporary, 'pointer.node'), { force: true });
  console.log(`Native scroll PNG evidence: ${temporary}`);
  if (error) console.error(error.stack || error);
  else console.log('PASS: responsive proportional mouse-wheel scrolling, pixel-proportional trackpad motion, fractional deltas and nested hit testing');
  app.exit(error ? 1 : 0);
}
const evaluate = source => window.webContents.executeJavaScript(source);
const state = () => evaluate('JSON.stringify(scrollState())').then(JSON.parse);
async function eventually(label, predicate) {
  let actual;
  for (let turn = 0; turn < 100; ++turn) {
    actual = await predicate();
    if (actual === true) return;
    await pause(25);
  }
  throw new Error(`${label}: ${JSON.stringify(actual)}`);
}
async function reset() {
  await evaluate('scroller.scrollTop=1000;scroller.scrollLeft=1000;wheels=[]');
  await pause(100);
}
async function checkPosition(top, left, label) {
  await eventually(label, async () => {
    const actual = await state();
    return (Math.abs(actual.top - top) <= 0.5 && Math.abs(actual.left - left) <= 0.5) || actual;
  });
  await pause(150);
  const actual = await state();
  assert.ok(Math.abs(actual.top - top) <= 0.5 && Math.abs(actual.left - left) <= 0.5,
    `${label} must not drift after input: ${JSON.stringify(actual)}`);
  assert.equal(actual.otherTop, 0, 'Scroll must target the nested element under the pointer');
  assert.equal(actual.pageTop, 0, 'Nested scrolling must not move the document');
  console.log(JSON.stringify({ scenario: label, ...actual }));
  return actual;
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
  native = require(helper);
  window = new BrowserWindow({ title, width: 800, height: 600, show: false,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><title>${title}</title>
    <style>html,body{margin:0;background:#181818;color:white;font:16px sans-serif}
    h1{font-size:20px;margin:16px 30px}.pane{position:absolute;top:60px;width:350px;height:400px;overflow:scroll;border:1px solid #888}
    #scroller{left:30px}#other{left:410px}.content{width:6000px;height:6000px;background:repeating-linear-gradient(#233345 0 39px,#92b3d5 39px 40px)}
    .row{position:sticky;left:0;width:350px;padding:12px;height:40px;box-sizing:border-box}</style>
    <h1>Native trackpad pixel scrolling / mouse wheel comparison</h1>
    <div id="scroller" class="pane"><div class="content">${Array.from({ length: 150 }, (_, i) => `<div class="row">Row ${i} — ${i * 40} px</div>`).join('')}</div></div>
    <div id="other" class="pane"><div class="content">Unscrolled control pane</div></div><script>
    const scroller=document.getElementById('scroller'),other=document.getElementById('other');
    let wheels=[];scroller.addEventListener('wheel',event=>wheels.push({x:event.deltaX,y:event.deltaY,mode:event.deltaMode}),{passive:true});
    window.scrollState=()=>({top:scroller.scrollTop,left:scroller.scrollLeft,otherTop:other.scrollTop,pageTop:scrollY,scale:devicePixelRatio,wheels});
    </script>`));
  await window.webContents.prepareToShow();
  const scroll = (x, y, precise = true) => native.scroll(title, 160, 200, x, y, precise);
  const snapshot = async filename => {
    const image = await window.webContents.capturePage();
    fs.writeFileSync(path.join(temporary, filename), image.toPNG());
  };

  await reset();
  scroll(0, -1, false);
  await pause(500);
  const mouse = await state();
  const step = mouse.top - 1000;
  assert.equal(step, 80, 'One mouse-wheel notch must move 80 logical pixels regardless of display scale');
  assert.equal(mouse.left, 1000, 'Vertical wheel input must not move the horizontal axis');
  assert.equal(mouse.otherTop, 0, 'Wheel input must target the pane under the pointer');
  assert.equal(mouse.pageTop, 0, 'Nested wheel input must not scroll the document');
  console.log(JSON.stringify({ scenario: 'single mouse-wheel notch', ...mouse }));

  await reset();
  scroll(0, -3, false);
  await checkPosition(1000 + 3 * step, 1000, 'three mouse-wheel notches in one native event');

  await reset();
  scroll(0, 1, false);
  await checkPosition(1000 - step, 1000, 'upward mouse-wheel notch');

  await reset();
  await snapshot('before.png');
  scroll(0, -2);
  const small = await checkPosition(1002, 1000, '2-point trackpad motion');
  assert.equal(small.wheels.at(-1).y, 2, 'DOM wheel deltas must use CSS pixels');
  assert.equal(small.wheels.at(-1).mode, 0);

  await reset();
  scroll(0, 8);
  await checkPosition(992, 1000, 'upward trackpad motion');

  await reset();
  for (let i = 0; i < 4; ++i) scroll(0, -2);
  const burst = await checkPosition(1008, 1000, 'high-frequency 4 × 2-point motion');
  assert.equal(burst.wheels.reduce((sum, event) => sum + event.y, 0), 8);
  await window.webContents.prepareToShow();
  await snapshot('trackpad-burst.png');

  await reset();
  scroll(12, -18);
  await checkPosition(1018, 988, 'diagonal trackpad motion');
  await window.webContents.prepareToShow();
  await snapshot('trackpad-diagonal.png');

  await reset();
  for (let i = 0; i < 4; ++i) {
    window.webContents.sendInputEvent({ type: 'mouseWheel', x: 160, y: 200,
      precise: true, deltaX: 0, deltaY: -0.25 });
  }
  await checkPosition(1001, 1000, 'fractional pixel accumulation through renderer IPC');

  await reset();
  scroll(0, -1, false);
  await checkPosition(1000 + step, 1000, 'mouse wheel after trackpad input');
  await window.webContents.prepareToShow();
  await snapshot('after.png');
  finish();
}).catch(finish);
