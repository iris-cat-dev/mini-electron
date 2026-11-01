'use strict';
// Native pointer/cursor observations across renderer IPC, not computed CSS assertions.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const { once } = require('node:events');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, BrowserView } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-mac-cursors-'));
app.setPath('userData', path.join(temporary, 'profile'));
app.on('window-all-closed', () => {});
const title = `mini-mac-cursors-${process.pid}`;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let window, other, view, native, finished = false;
const deadline = setTimeout(() => finish(new Error('Native cursor smoke timed out')), 60000);
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (native) native.restore();
  if (view && !view.webContents.isDestroyed()) view.webContents.destroy();
  for (const item of [other, window]) if (item && !item.isDestroyed()) item.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: native editing/link/CSS cursors, stationary updates, grab transition, hidden/custom restoration, BrowserView and resize precedence');
  app.exit(error ? 1 : 0);
}
const shapes = ['crosshair', 'grab', 'grabbing', 'move', 'not-allowed', 'copy', 'alias',
  'zoom-in', 'zoom-out', 'col-resize', 'row-resize', 'nwse-resize', 'nesw-resize',
  'wait', 'help', 'cell', 'context-menu', 'vertical-text', 'none'];
const page = `<!doctype html><style>
html,body{margin:0;height:100%;background:#183449;color:white;font:15px sans-serif;user-select:none}
main{padding:20px}h1{font-size:20px;margin:0 0 14px}.row{display:flex;gap:14px;margin-bottom:14px}
input,a,[contenteditable]{box-sizing:border-box;width:180px;min-height:42px;padding:10px;background:#fff;color:#152a3b}
#grid{display:grid;grid-template-columns:repeat(5,1fr);gap:8px}.tile{height:46px;border:1px solid #7598ad;display:grid;place-items:center}
#dynamic{cursor:pointer}#drag{cursor:grab}#drag.held{cursor:grabbing}
</style><main><h1>Native cursor semantics</h1><div class="row"><input id="edit" value="Hover to edit"><a id="link" href="#target">Hover a link</a><div id="editable" contenteditable>Editable text</div></div>
<div id="grid">${shapes.map(shape => `<div class="tile" id="${shape}" style="cursor:${shape}">${shape}</div>`).join('')}<div class="tile" id="dynamic">Stationary change</div><div class="tile" id="drag">Hold to drag</div><div class="tile" id="custom">RGBA cursor</div></div></main><script>
window.lastTarget='';window.trace=[];for(const type of ['mousemove','mousedown','mouseup'])document.addEventListener(type,e=>{trace.push({type,x:e.clientX,buttons:e.buttons});if(type==='mousemove')lastTarget=e.target.id});
const drag=document.getElementById('drag');drag.addEventListener('pointerdown',e=>{drag.setPointerCapture(e.pointerId);drag.classList.add('held')});drag.addEventListener('pointerup',()=>drag.classList.remove('held'));
const canvas=document.createElement('canvas');canvas.width=canvas.height=16;const context=canvas.getContext('2d');context.fillStyle='rgba(40,180,90,0.5)';context.fillRect(0,0,16,16);document.getElementById('custom').style.cursor='url('+canvas.toDataURL()+') 5 7, crosshair';
</script>`;
const load = contents => contents.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(page));
const evaluate = (contents, expression) => contents.executeJavaScript(expression);
async function center(contents, id) {
  return JSON.parse(await evaluate(contents, `JSON.stringify((()=>{const r=document.getElementById(${JSON.stringify(id)}).getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})())`));
}
async function cursor(expected, label) {
  let state;
  for (let i = 0; i < 100; ++i) {
    state = native.cursor();
    if (state.kind === expected) return state;
    await pause(20);
  }
  assert.equal(state.kind, expected, `${label}: native cursor ${JSON.stringify(state)}`);
}
async function mouseTransition(contents, command, point, index, type, buttons) {
  const start = await evaluate(contents, 'trace.length');
  native.mouse(title, command, point.x, point.y, index);
  let event;
  for (let i = 0; i < 100; ++i) {
    event = JSON.parse(await evaluate(contents,
      `JSON.stringify(trace.slice(${start}).find(e=>e.type===${JSON.stringify(type)})||null)`));
    if (event) break;
    await pause(20);
  }
  assert.ok(event, `${command} reaches the DOM: ` + await evaluate(contents, 'JSON.stringify(trace)'));
  assert.equal(event.buttons, buttons, `${command} retains the correct held buttons: ${JSON.stringify(event)}`);
  return event;
}
async function hover(contents, id, expected, index = 0, nativeTitle = title, offset = { x: 0, y: 0 }) {
  const point = await center(contents, id);
  const x = point.x + offset.x, y = point.y + offset.y;
  const positioned = native.mouse(nativeTitle, 'position', x, y, index);
  assert.equal(positioned.warpError, 0, 'native pointer positioning succeeds');
  let position;
  for (let i = 0; i < 100; ++i) {
    position = native.ready(nativeTitle, x, y, index);
    if (position.ready) break;
    await pause(20);
  }
  assert.ok(position.ready, 'WindowServer acknowledges the owned pointer position: ' + JSON.stringify(position));
  const receipt = native.mouse(nativeTitle, 'move', x, y, index);
  assert.equal(receipt.pointerWindow, receipt.windowNumber, 'native pointer is over the owned window');
  let target;
  for (let i = 0; i < 100; ++i) {
    target = await evaluate(contents, 'window.lastTarget');
    if (target === id) break;
    await pause(20);
  }
  assert.equal(target, id, `native move reaches the intended DOM target: ${JSON.stringify(receipt)}`);
  const state = await cursor(expected, id);
  return { id, ...state, nativeWindowKey: receipt.windowKey };
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const root = path.resolve(__dirname, '..');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  const addon = path.join(temporary, 'cursors.node');
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup',
    '-isysroot', sdk, `-I${path.join(root, 'third_party/libnode/src')}`, '-framework', 'AppKit',
    path.join(__dirname, 'macos-cursor-helper.mm'), '-o', addon], { stdio: 'inherit', timeout: 30000 });
  native = require(addon);
  assert.ok(native.consoleUnlocked(), 'Unlock the macOS console before physical cursor verification');
  window = new BrowserWindow({ title, x: 180, y: 140, width: 820, height: 560, frame: false, show: false,
    alwaysOnTop: true,
    webPreferences: { webviewTag: true, sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await load(window.webContents);
  window.show();
  const observations = [];
  observations.push(await hover(window.webContents, 'edit', 'text'));
  observations.push(await hover(window.webContents, 'editable', 'text'));
  observations.push(await hover(window.webContents, 'link', 'pointer'));
  for (const shape of shapes) observations.push(await hover(window.webContents, shape, shape));
  observations.push(await hover(window.webContents, 'dynamic', 'pointer'));
  await evaluate(window.webContents, 'document.getElementById("dynamic").style.cursor="text"; true');
  observations.push({ id: 'stationary-style-update', ...await cursor('text', 'stationary style change') });
  observations.push(await hover(window.webContents, 'drag', 'grab'));
  const drag = await center(window.webContents, 'drag');
  await mouseTransition(window.webContents, 'down', drag, 0, 'mousedown', 1);
  observations.push({ id: 'pointerdown', ...await cursor('grabbing', 'pressed drag handle') });
  await mouseTransition(window.webContents, 'up', drag, 0, 'mouseup', 0);
  observations.push({ id: 'pointerup', ...await cursor('grab', 'released drag handle') });
  const custom = await hover(window.webContents, 'custom', 'custom');
  assert.equal(custom.width, 16); assert.equal(custom.height, 16);
  assert.equal(custom.hotspotX, 5); assert.equal(custom.hotspotY, 7);
  for (const [component, expected] of [['red', 40 / 255], ['green', 180 / 255], ['blue', 90 / 255], ['alpha', 0.5]])
    assert.ok(Math.abs(custom[component] - expected) < 0.025, `${component}: ${JSON.stringify(custom)}`);
  observations.push(custom);
  observations.push(await hover(window.webContents, 'link', 'pointer'));
  if (process.env.MINI_FIX_RESULT_DIR) {
    fs.mkdirSync(process.env.MINI_FIX_RESULT_DIR, { recursive: true });
    const image = await window.webContents.capturePage();
    fs.writeFileSync(path.join(process.env.MINI_FIX_RESULT_DIR, 'mac-cursors.png'), image.toPNG());
  }
  view = new BrowserView({ webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  view.setBounds({ x: 280, y: 110, width: 500, height: 400 });
  window.setBrowserView(view);
  await load(view.webContents);
  observations.push(await hover(view.webContents, 'edit', 'text', 1));
  observations.push(await hover(window.webContents, 'edit', 'text'));
  observations.push(await hover(view.webContents, 'link', 'pointer', 1));
  observations.push(await hover(view.webContents, 'drag', 'grab', 1));
  const viewDrag = await center(view.webContents, 'drag');
  await mouseTransition(view.webContents, 'down', viewDrag, 1, 'mousedown', 1);
  await cursor('grabbing', 'captured BrowserView drag handle');
  const hostEdit = await center(window.webContents, 'edit');
  const outsideView = { x: hostEdit.x - 280, y: hostEdit.y - 110 };
  const viewMove = await mouseTransition(view.webContents, 'drag', outsideView, 1, 'mousemove', 1);
  assert.ok(Math.abs(viewMove.x - outsideView.x) <= 1, 'captured BrowserView receives out-of-bounds coordinates');
  await evaluate(view.webContents, 'document.getElementById("drag").style.cursor="copy"; true');
  observations.push({ id: 'captured-view-stationary-update', ...await cursor('copy', 'captured view owns cursor outside its bounds') });
  await mouseTransition(view.webContents, 'up', outsideView, 1, 'mouseup', 0);
  observations.push(await hover(window.webContents, 'edit', 'text'));
  window.removeBrowserView(view);
  observations.push(await hover(window.webContents, 'copy', 'copy'));
  native.mouse(title, 'move', 2, 280, 0);
  observations.push({ id: 'native-resize-border', ...await cursor('ew-resize', 'native border overrides page cursor') });
  observations.push(await hover(window.webContents, 'link', 'pointer'));
  const guestURL = 'data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><style>
    html,body{margin:0;height:100%;background:#406080;color:white;user-select:none}
    #guestTarget{height:100%;cursor:crosshair;display:grid;place-items:center}
    </style><div id="guestTarget">Guest pointer capture</div><script>
    window.lastTarget='';window.lastX=null;window.trace=[];for(const type of ['mousemove','mousedown','mouseup','pointerdown','pointerup'])document.addEventListener(type,e=>{trace.push({type,x:e.clientX,buttons:e.buttons});if(type==='mousemove'){lastTarget=e.target.id;lastX=e.clientX}});
    document.addEventListener('pointerdown',e=>e.target.setPointerCapture(e.pointerId));document.addEventListener('contextmenu',e=>e.preventDefault());
    </script>`);
  const attached = once(window.webContents, 'did-attach-webview');
  await evaluate(window.webContents, `(()=>{const guest=document.createElement('webview');guest.id='guest';
    guest.style.cssText='position:absolute;left:460px;top:150px;width:260px;height:180px';
    guest.setAttribute('src',${JSON.stringify(guestURL)});document.body.appendChild(guest);return true})()`);
  const [, guest] = await attached;
  for (let i = 0; i < 100; ++i) {
    if (await evaluate(guest, 'document.readyState==="complete" && typeof window.lastTarget==="string"')) break;
    await pause(20);
  }
  const offset = JSON.parse(await evaluate(window.webContents,
    'JSON.stringify((()=>{const r=document.getElementById("guest").getBoundingClientRect();return {x:r.x,y:r.y}})())'));
  observations.push(await hover(guest, 'guestTarget', 'crosshair', 0, title, offset));
  const guestPoint = await center(guest, 'guestTarget');
  const nativeGuestPoint = { x: guestPoint.x + offset.x, y: guestPoint.y + offset.y };
  await mouseTransition(guest, 'down', nativeGuestPoint, 0, 'mousedown', 1);
  await mouseTransition(guest, 'rightDown', nativeGuestPoint, 0, 'mousedown', 3);
  await mouseTransition(guest, 'up', nativeGuestPoint, 0, 'mouseup', 2);
  const outside = await center(window.webContents, 'link');
  const guestMove = await mouseTransition(guest, 'rightDrag', outside, 0, 'mousemove', 2);
  assert.ok(Math.abs(guestMove.x - (outside.x - offset.x)) <= 1,
    'guest retains capture until every held button is released: ' + await evaluate(guest, 'JSON.stringify(trace)'));
  observations.push({ id: 'guest-multi-button-capture', ...await cursor('crosshair', 'captured guest keeps cursor outside its bounds') });
  await mouseTransition(guest, 'rightUp', outside, 0, 'mouseup', 0);
  observations.push(await hover(window.webContents, 'link', 'pointer'));
  await evaluate(window.webContents, 'document.getElementById("guest").remove(); true');
  // Late updates from a different native surface must not overwrite the hovered view.
  other = new BrowserWindow({ title: title + '-other', x: 1020, y: 140, width: 400, height: 250,
    alwaysOnTop: true,
    show: false, webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await load(other.webContents); other.showInactive();
  const unfocused = await hover(other.webContents, 'edit', 'text', 0, title + '-other');
  assert.equal(unfocused.nativeWindowKey, 0, 'hover semantics must not require a key window');
  observations.push(unfocused);
  observations.push(await hover(window.webContents, 'link', 'pointer'));
  await evaluate(other.webContents, 'document.getElementById("edit").style.cursor="not-allowed"; true');
  await pause(250);
  assert.equal(native.cursor().kind, 'pointer', 'non-hovered window must not steal cursor');
  await window.webContents.loadURL('data:text/html,<style>html,body{margin:0;height:100%;user-select:none}</style><div>Navigation reset</div>');
  observations.push({ id: 'navigation-reset', ...await cursor('default', 'navigation releases stale cursor') });
  if (process.env.MINI_FIX_RESULT_DIR)
    fs.writeFileSync(path.join(process.env.MINI_FIX_RESULT_DIR, 'mac-cursors.json'), JSON.stringify(observations, null, 2));
  console.log(JSON.stringify(observations));
  finish();
}).catch(finish);
