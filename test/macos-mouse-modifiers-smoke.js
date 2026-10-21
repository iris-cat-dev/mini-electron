'use strict';

// Invoke: /path/to/mini-electron --app=test/macos-mouse-modifiers-smoke.js
// AppKit NSEvents exercise the native host, IPC and Blink input conversion.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-mouse-modifiers-'));
const title = `mini-mouse-modifiers-${process.pid}`;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const shift = 1 << 17, control = 1 << 18, option = 1 << 19, command = 1 << 20;
let window, native, finished = false;
const watchdog = setTimeout(() => finish(new Error('Native mouse modifier smoke exceeded 45s')), 45000);
app.on('window-all-closed', () => {});
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(path.join(temporary, 'pointer.node'), { force: true });
  console.log(`Native mouse modifier evidence: ${temporary}`);
  if (error) console.error(error.stack || error);
  else console.log('PASS: native Shift/Control/Command/Option flags survive pointer, mouse and activation events');
  app.exit(error ? 1 : 0);
}
const evaluate = source => window.webContents.executeJavaScript(source);
const state = () => evaluate('JSON.stringify(window.modifierState())').then(JSON.parse);
async function eventually(label, predicate) {
  let actual;
  for (let turn = 0; turn < 100; ++turn) {
    actual = await predicate();
    if (actual === true) return;
    await pause(25);
  }
  throw new Error(`${label}: ${JSON.stringify(actual)}`);
}
async function click(x, y, modifiers = 0) {
  native.mouse(title, 'move', x, y, modifiers);
  await pause(30);
  native.mouse(title, 'down', x, y, modifiers);
  await pause(30);
  native.mouse(title, 'up', x, y, modifiers);
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
  window = new BrowserWindow({ title, width: 800, height: 600, show: true,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><title>${title}</title>
    <style>html,body{margin:0;background:white;color:#111;font:18px sans-serif}
    #probe{margin:20px;padding:20px;background:#ddeaff;height:100px}</style>
    <div id="probe">Native mouse modifiers</div><pre id="status"></pre><script>
    window.mouseEvents=[];
    for(const type of ['pointermove','pointerdown','mousedown','pointerup','mouseup','click','contextmenu'])
      document.getElementById('probe').addEventListener(type,event=>{
        mouseEvents.push({type:event.type,ctrl:event.ctrlKey,shift:event.shiftKey,meta:event.metaKey,alt:event.altKey,trusted:event.isTrusted});
        document.getElementById('status').textContent=JSON.stringify(mouseEvents,null,2);
        if(type==='contextmenu')event.preventDefault();
      });
    window.modifierState=()=>({events:mouseEvents.slice()});
    </script>`));
  app.focus();
  window.focus();
  await eventually('Native window focused', () => window.isFocused());

  for (const [name, modifiers, expected] of [
    ['plain', 0, [false, false, false, false]],
    ['Shift', shift, [false, true, false, false]],
    ['Control', control, [true, false, false, false]],
    ['Command', command, [false, false, true, false]],
    ['Option', option, [false, false, false, true]],
    ['Control+Shift', control | shift, [true, true, false, false]],
    ['Command+Shift', command | shift, [false, true, true, false]],
    ['all', control | shift | command | option, [true, true, true, true]],
  ]) {
    await evaluate('window.mouseEvents=[]');
    await click(150, 70, modifiers);
    await eventually(`${name} native mouse-up delivered`, async () => (await state()).events.some(event => event.type === 'mouseup'));
    const actual = (await state()).events;
    console.log(`${name}: ${JSON.stringify(actual)}`);
    for (const type of ['pointerdown', 'mousedown', 'pointerup', 'mouseup']) {
      assert.ok(actual.some(event => event.type === type), `${name}: missing ${type}`);
    }
    assert.ok(actual.some(event => event.type === 'click' || event.type === 'contextmenu'), `${name}: missing activation`);
    for (const event of actual) {
      assert.equal(event.trusted, true, `${name}: ${event.type} must originate in the native input path`);
      assert.deepEqual([event.ctrl, event.shift, event.meta, event.alt], expected, `${name}: ${event.type} loses native modifier flags`);
    }
  }

  await window.webContents.prepareToShow();
  native.snapshot(title, path.join(temporary, 'mouse-modifiers.png'));
  console.log(JSON.stringify(await state()));
  finish();
}).catch(finish);
