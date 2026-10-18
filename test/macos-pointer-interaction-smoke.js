'use strict';

// Invoke: /path/to/mini-electron --app=test/macos-pointer-interaction-smoke.js
// Real AppKit motion/drag events exercise Blink media queries, hover, pointer
// capture and button transitions. No synthetic DOM events or input emulation.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-pointer-interaction-'));
const title = `mini-pointer-interaction-${process.pid}`;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let window, native, finished = false;
const watchdog = setTimeout(() => finish(new Error('Native pointer smoke exceeded 45s')), 45000);
app.on('window-all-closed', () => {});
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(path.join(temporary, 'pointer.node'), { force: true });
  console.log(`Native pointer PNG evidence: ${temporary}`);
  if (error) console.error(error.stack || error);
  else console.log('PASS: desktop fine/hover media queries, full-height divider hover, captured drag and released-button state');
  app.exit(error ? 1 : 0);
}
const evaluate = async source => (await window.webContents.executeJavaScript(source)).value;
const state = () => evaluate('JSON.stringify(window.pointerState())').then(JSON.parse);
async function eventually(label, predicate) {
  let actual;
  for (let turn = 0; turn < 100; ++turn) {
    actual = await predicate();
    if (actual === true) return;
    await pause(25);
  }
  throw new Error(`${label}: ${JSON.stringify(actual)}`);
}
async function capture(name) {
  await window.webContents.prepareToShow();
  const file = path.join(temporary, `${name}.png`);
  native.snapshot(title, file);
  console.log(JSON.stringify({ screenshot: file, state: await state() }));
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
    <style>html,body{margin:0;height:100%;background:#181818;color:white;font:16px sans-serif}
    #sidebar{position:absolute;inset:0 auto 0 0;width:240px;background:#111;border-right:1px solid #555;box-sizing:border-box}
    #resize{position:absolute;right:0;top:calc(50% - 44px);width:24px;height:88px;touch-action:none}
    #highlight{position:absolute;top:0;bottom:0;left:4px;width:3px;background:#268bff;opacity:0;pointer-events:none}
    #resize.hot #highlight{opacity:1}#content{margin-left:270px;padding-top:32px}
    @media (hover:hover) and (pointer:fine){#resize{right:-5px;top:0;bottom:0;width:10px;height:auto;cursor:col-resize}}
    </style><aside id="sidebar"><p>Native sidebar divider</p><div id="resize"><div id="highlight"></div></div></aside>
    <main id="content">Hover near the top or bottom; drag across the content pane.</main><script>
    const sidebar=document.getElementById('sidebar'), handle=document.getElementById('resize');
    const desktop=matchMedia('(hover:hover) and (pointer:fine)');
    let timer, dragging=false, startX=0, startWidth=0;
    window.pointerEvents=[];
    for(const type of ['pointerenter','pointerleave','pointerdown','pointermove','pointerup','gotpointercapture','lostpointercapture'])
      handle.addEventListener(type,event=>pointerEvents.push({type,buttons:event.buttons,pointerType:event.pointerType,x:event.clientX}));
    handle.addEventListener('pointerenter',()=>{if(desktop.matches)timer=setTimeout(()=>handle.classList.add('hot'),150)});
    handle.addEventListener('pointerleave',()=>{clearTimeout(timer);handle.classList.remove('hot')});
    handle.addEventListener('pointerdown',event=>{
      if(event.button!==0)return;dragging=true;startX=event.clientX;startWidth=sidebar.getBoundingClientRect().width;
      handle.setPointerCapture(event.pointerId);
    });
    handle.addEventListener('pointermove',event=>{
      if(dragging&&(event.buttons&1))sidebar.style.width=(startWidth+event.clientX-startX)+'px';
    });
    handle.addEventListener('pointerup',()=>{dragging=false});
    window.pointerState=()=>{const rect=handle.getBoundingClientRect();return {
      fine:desktop.matches,anyFine:matchMedia('(any-pointer:fine)').matches,anyHover:matchMedia('(any-hover:hover)').matches,
      width:sidebar.getBoundingClientRect().width,handle:[rect.x,rect.y,rect.width,rect.height],height:innerHeight,
      highlighted:getComputedStyle(document.getElementById('highlight')).opacity==='1',dragging,events:pointerEvents.slice()}};
    </script>`));
  app.focus();
  window.focus();
  await eventually('Native pointer window focused', () => window.isFocused());
  const initial = await state();
  assert.equal(initial.fine, true, 'Desktop must select the mouse/trackpad divider, not its touch-only target');
  assert.equal(initial.anyFine, true);
  assert.equal(initial.anyHover, true);
  assert.equal(initial.handle[3], initial.height, 'Fine-pointer hit area must span the complete sidebar');

  for (const y of [40, Math.floor(initial.height / 2), initial.height - 40]) {
    native.mouse(title, 'move', 500, y);
    await eventually('Moving away clears hover highlight', async () => !(await state()).highlighted);
    native.mouse(title, 'move', initial.width, y);
    await eventually(`Hover at sidebar height ${y} highlights the divider`, async () => (await state()).highlighted);
  }
  await capture('divider-hover');
  const y = initial.height - 40;
  native.mouse(title, 'down', initial.width, y);
  await eventually('Native press starts the divider drag', async () => (await state()).dragging);
  native.mouse(title, 'drag', initial.width + 90, y);
  await eventually('Captured drag widens sidebar across the content pane', async () => {
    const actual = await state();
    return actual.width === initial.width + 90 || actual;
  });
  native.mouse(title, 'drag', initial.width + 45, y);
  await eventually('Dragging back follows the held mouse', async () => (await state()).width === initial.width + 45);
  native.mouse(title, 'up', initial.width + 45, y);
  await eventually('Release ends the resize gesture', async () => !(await state()).dragging);
  native.mouse(title, 'move', 500, y);
  await eventually('Release clears pointer capture', async () => {
    const actual = await state();
    return actual.events.some(event => event.type === 'lostpointercapture') || actual;
  });
  await pause(150);
  const final = await state();
  assert.equal(final.width, initial.width + 45, 'An unpressed move must not keep resizing');
  assert.ok(final.events.some(event => event.type === 'pointermove' && event.buttons === 1 && event.pointerType === 'mouse'));
  assert.ok(final.events.some(event => event.type === 'pointerup' && event.buttons === 0));
  await capture('divider-resized');
  finish();
}).catch(finish);
