'use strict';
// Real AppKit events and native window/view geometry, not synthetic DOM events.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, BrowserView } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-mac-screen-'));
app.setPath('userData', path.join(temporary, 'profile'));
app.on('window-all-closed', () => {});
const title = `mini-mac-screen-${process.pid}`;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let window, view, native, finished = false;
const deadline = setTimeout(() => finish(new Error('Native screen coordinate smoke timed out')), 60000);
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (view && !view.webContents.isDestroyed()) view.webContents.destroy();
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: native mouse/wheel desktop DIPs, move-only rectangles, BrowserView offsets and stationary captured pointer');
  app.exit(error ? 1 : 0);
}
const page = `<!doctype html><style>html,body{margin:0;height:100%;background:#183449;color:#fff;font:16px sans-serif}#target{height:100%;padding:12px;box-sizing:border-box}</style><div id="target">Native desktop coordinate audit</div><script>
window.samples=[];
for(const type of ['mousemove','mousedown','mouseup','pointermove','pointerdown','pointerup','wheel']) document.addEventListener(type,event=>{
  samples.push({type,x:event.clientX,y:event.clientY,sx:event.screenX,sy:event.screenY,buttons:event.buttons});
  if(type==='pointerdown')event.target.setPointerCapture(event.pointerId);
  if(type==='wheel')event.preventDefault();
  document.getElementById('target').textContent=JSON.stringify(samples.slice(-4));
},{passive:false});
</script>`;
const load = contents => contents.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(page));
const evaluate = (contents, expression) => contents.executeJavaScript(expression);
async function next(contents, type, action) {
  await evaluate(contents, 'samples=[]; true');
  action();
  for (let i = 0; i < 100; ++i) {
    const samples = JSON.parse(await evaluate(contents, 'JSON.stringify(samples)'));
    const result = samples.find(sample => sample.type === type);
    if (result) return result;
    await pause(20);
  }
  throw new Error(`Native ${type} was not delivered`);
}
function near(actual, expected, label) {
  assert.ok(Math.abs(actual - expected) <= 1, `${label}: ${actual} != ${expected}`);
}
async function check(contents, index, precise = false) {
  const geometry = native.geometry(title, index);
  const x = 56, y = 48;
  const move = await next(contents, 'mousemove', () => native.mouse(title, 'move', x, y, index, false));
  near(move.x, x, 'native clientX'); near(move.y, y, 'native clientY');
  near(move.sx, geometry.widgetX + x, 'native desktop screenX');
  near(move.sy, geometry.widgetY + y, 'native desktop screenY');
  const wheel = await next(contents, 'wheel', () => native.scroll(title, x, y, 0, -1, precise, index));
  near(wheel.sx, geometry.widgetX + x, 'wheel desktop screenX');
  near(wheel.sy, geometry.widgetY + y, 'wheel desktop screenY');
  return { geometry, move, wheel };
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const root = path.resolve(__dirname, '..');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  const addon = path.join(temporary, 'screen.node');
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup',
    '-isysroot', sdk, `-I${path.join(root, 'third_party/libnode/src')}`, '-framework', 'AppKit',
    path.join(__dirname, 'macos-pointer-screen-coordinates-helper.mm'), '-o', addon], { stdio: 'inherit', timeout: 30000 });
  native = require(addon);
  window = new BrowserWindow({ title, x: 260, y: 180, width: 600, height: 420, show: false,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await load(window.webContents);
  window.showInactive();
  const observations = [];
  for (const [x, y] of [[260, 180], [-520, 120], [480, 250]]) {
    window.setPosition(x, y);
    await pause(100);
    const actual = await check(window.webContents, 0);
    const position = JSON.parse(await evaluate(window.webContents, 'JSON.stringify([screenX,screenY])'));
    near(position[0], actual.geometry.windowX, 'Blink window screenX follows native move');
    near(position[1], actual.geometry.windowY, 'Blink window screenY follows native move');
    observations.push(actual);
  }
  view = new BrowserView({ webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  view.setBounds({ x: 260, y: 120, width: 220, height: 160 });
  window.setBrowserView(view);
  await load(view.webContents);
  await pause(100);
  observations.push(await check(view.webContents, 1, true));
  window.setPosition(300, 200);
  await pause(100);
  observations.push(await check(view.webContents, 1));
  const geometry = native.geometry(title, 0);
  const sx = geometry.widgetX + 80, sy = geometry.widgetY + 70;
  await next(window.webContents, 'pointerdown', () => native.mouse(title, 'down', sx, sy, 0, true));
  for (const [x, y] of [[330, 215], [350, 230], [310, 205]]) {
    window.setPosition(x, y);
    await pause(80);
    const held = await next(window.webContents, 'pointermove', () => native.mouse(title, 'drag', sx, sy, 0, true));
    near(held.sx, sx, 'stationary captured screenX');
    near(held.sy, sy, 'stationary captured screenY');
    assert.equal(held.buttons, 1);
    observations.push({ held });
  }
  await next(window.webContents, 'pointerup', () => native.mouse(title, 'up', sx, sy, 0, true));
  if (process.env.MINI_FIX_RESULT_DIR) {
    fs.mkdirSync(process.env.MINI_FIX_RESULT_DIR, { recursive: true });
    fs.writeFileSync(path.join(process.env.MINI_FIX_RESULT_DIR, 'mac-screen-coordinates.json'), JSON.stringify(observations, null, 2));
    const image = await window.webContents.capturePage();
    fs.writeFileSync(path.join(process.env.MINI_FIX_RESULT_DIR, 'mac-screen-coordinates.png'), image.toPNG());
  }
  console.log(JSON.stringify(observations));
  finish();
}).catch(finish);
