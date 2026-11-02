'use strict';

// Run: out/windows-x64/runtime-dist/electron.exe test/windows-cursor-smoke.js
// Requires an unlocked, unobscured interactive Windows desktop and .NET Framework csc.exe.
// Native SendInput + GetCursorInfo/GetIconInfo/GetDIBits, never computed CSS assertions.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { once } = require('node:events');
const { app, BrowserWindow, BrowserView, screen } = require('electron');
const { startNativeInput } = require('./windows-native-input-driver');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-windows-cursors-'));
app.setPath('userData', path.join(temporary, 'profile'));
app.on('window-all-closed', () => {});
const title = `mini-windows-cursors-${process.pid}`;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const artifacts = process.env.MINI_NATIVE_INPUT_ARTIFACTS;
const evidence = { observations: [], transitions: [] };
let window, other, view, guest, driver, finished = false;
const deadline = setTimeout(() => finish(new Error('Windows native cursor smoke exceeded 120s')), 120000);
const request = (action, values = {}) => driver.request(action, { executable: process.execPath, pid: process.pid, ...values });
const evaluate = (contents, source) => contents.executeJavaScript(source);
const json = async (contents, source) => JSON.parse(await evaluate(contents, `JSON.stringify(${source})`));
async function eventually(label, read, predicate) {
  let state;
  for (let i = 0; i < 100; ++i) {
    state = await read();
    if (predicate(state)) return state;
    await pause(25);
  }
  assert.fail(`${label}: ${JSON.stringify(state)}`);
}
const shapes = ['crosshair', 'grab', 'grabbing', 'move', 'not-allowed', 'no-drop', 'copy', 'alias',
  'zoom-in', 'zoom-out', 'col-resize', 'row-resize', 'nwse-resize', 'nesw-resize', 'ew-resize',
  'ns-resize', 'wait', 'progress', 'help', 'cell', 'context-menu', 'vertical-text', 'none'];
const page = `<!doctype html><meta charset="utf-8"><style>
html,body{margin:0;height:100%;background:#183449;color:white;font:14px sans-serif;user-select:none;overflow:hidden}
main{padding:18px}.row{display:flex;gap:10px;margin-bottom:12px}
input,a,[contenteditable]{box-sizing:border-box;width:140px;height:38px;padding:8px;background:white;color:#152a3b}
#grid{display:grid;grid-template-columns:repeat(5,1fr);gap:6px}.tile{height:38px;border:1px solid #7598ad;display:grid;place-items:center}
#dynamic{cursor:pointer}#drag{cursor:grab}#drag.held{cursor:grabbing}
#chrome{position:absolute;left:18px;bottom:12px;width:180px;height:28px;-webkit-app-region:drag;cursor:copy;background:#547080}
</style><main><div class="row"><input id="edit" value="Native text"><a id="link" href="#target">Native link</a><div id="editable" contenteditable>Editable</div></div>
<div id="grid">${shapes.map(shape => `<div class="tile" id="${shape}" style="cursor:${shape}">${shape}</div>`).join('')}<div class="tile" id="dynamic">Stationary</div><div class="tile" id="drag">Capture</div><div class="tile" id="custom">RGBA</div></div></main><div id="chrome">Native draggable chrome</div><script>
window.lastTarget='';window.lastPointerId=0;window.trace=[];
for(const type of ['mousemove','mousedown','mouseup','pointerdown','pointerup','gotpointercapture','lostpointercapture'])document.addEventListener(type,e=>{if(type==='pointerdown')lastPointerId=e.pointerId;trace.push({type,target:e.target.id,x:e.clientX,y:e.clientY,buttons:e.buttons,button:e.button,pointerId:e.pointerId,time:e.timeStamp});if(type==='mousemove')lastTarget=e.target.id});
document.addEventListener('contextmenu',e=>e.preventDefault());
const drag=document.getElementById('drag');drag.addEventListener('pointerdown',e=>{drag.setPointerCapture(e.pointerId);drag.classList.add('held')});drag.addEventListener('pointerup',e=>{if(!e.buttons)drag.classList.remove('held')});
window.customCursor=(size=16)=>{const canvas=document.createElement('canvas');canvas.width=canvas.height=size;const context=canvas.getContext('2d');context.fillStyle='rgba(40,180,90,0.5)';context.fillRect(0,0,size,size);document.getElementById('custom').style.cursor='url('+canvas.toDataURL()+') '+(size===16?'5 7':'10 14')+', crosshair';return true};customCursor();
</script>`;
const load = contents => contents.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(page));
async function center(contents, id) {
  return json(contents, `(()=>{const r=document.getElementById(${JSON.stringify(id)}).getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()`);
}
async function point(contents, position, offset = { x: 0, y: 0 }, nativeTitle = title) {
  const geometry = await request('geometry', { title: nativeTitle });
  const dpr = await evaluate(contents, 'devicePixelRatio');
  const scale = geometry.dpi / 96;
  // Embedded guests currently use a 1x backing store; embedding positions
  // still use their native host window's DIP-to-pixel transform.
  if (contents.getType() !== 'webview')
    assert.ok(Math.abs(dpr - scale) < 0.01, `renderer and native DPI agree: ${JSON.stringify({ dpr, geometry })}`);
  return { x: Math.round(geometry.clientX + (position.x + offset.x) * scale),
    y: Math.round(geometry.clientY + (position.y + offset.y) * scale) };
}
async function cursor(expected, label, nativeTitle = title) {
  const state = await eventually(label, () => request('cursor', { title: nativeTitle }), value => value.matches.includes(expected));
  evidence.observations.push({ label, ...state });
  return state;
}
async function hover(contents, id, expected, options = {}) {
  const nativeTitle = options.title || title;
  const position = await point(contents, await center(contents, id), options.offset, nativeTitle);
  await request('move', { title: nativeTitle, ...position, ...(options.foregroundTitle ? { foregroundTitle: options.foregroundTitle } : {}) });
  await eventually(`${id} receives native mouse movement`, () => evaluate(contents, 'lastTarget'), value => value === id);
  const state = await cursor(expected, id, nativeTitle);
  assert.ok(Math.abs(state.x - position.x) <= 2 && Math.abs(state.y - position.y) <= 2,
    `physical pointer reached ${id}: ${JSON.stringify({ state, position })}`);
  return state;
}
async function transition(contents, action, position, type, buttons, button = 'left') {
  const start = await evaluate(contents, 'trace.length');
  const record = { action, button, position, type, buttons, start };
  evidence.transitions.push(record);
  await request(action, { ...position, button });
  const event = await eventually(`${action}/${button} reaches DOM`, () => json(contents,
    `trace.slice(${start}).find(event=>event.type===${JSON.stringify(type)})||null`), value => value !== null);
  record.event = event;
  record.native = await request('state');
  assert.ok(Math.abs(record.native.x - position.x) <= 2 && Math.abs(record.native.y - position.y) <= 2,
    `native ${action}/${button} reaches its physical point: ${JSON.stringify(record)}`);
  assert.equal(record.native.buttons, buttons, `native ${action}/${button} preserves physical held buttons`);
  assert.equal(record.native.capture, buttons ? record.native.window : 0,
    `native ${action}/${button} keeps BrowserWindow capture until every button is released`);
  assert.equal(event.buttons, buttons, `native ${action}/${button} preserves held buttons`);
  return event;
}
function customImage(state, size, hotspot) {
  const scale = state.dpi / 96;
  assert.equal(state.width, Math.round(size * scale), 'native RGBA width scales with window DPI');
  assert.equal(state.height, Math.round(size * scale), 'native RGBA height scales with window DPI');
  assert.equal(state.hotspotX, Math.round(hotspot[0] * scale), 'native hotspot X scales with window DPI');
  assert.equal(state.hotspotY, Math.round(hotspot[1] * scale), 'native hotspot Y scales with window DPI');
  assert.ok(state.centerBGRA, 'custom cursor has an actual color bitmap');
  for (const [index, expected] of [45, 90, 20, 128].entries())
    assert.ok(Math.abs(state.centerBGRA[index] - expected) <= 2,
      `native premultiplied BGRA/alpha ${index}: ${JSON.stringify(state)}`);
}
async function failureState() {
  const state = {};
  for (const [name, contents] of [['host', window && window.webContents], ['view', view && view.webContents], ['guest', guest]]) {
    if (!contents || contents.isDestroyed()) continue;
    try {
      state[name] = await json(contents, `({focused:document.hasFocus(),active:document.activeElement&&document.activeElement.id,
        lastTarget:window.lastTarget,trace:window.trace&&trace.slice(-30),drag:(()=>{const drag=document.getElementById('drag')||document.getElementById('guestTarget');
        return drag&&{bounds:drag.getBoundingClientRect().toJSON(),inlineCursor:drag.style.cursor,capture:drag.hasPointerCapture(window.lastPointerId)}})()})`);
    } catch (error) { state[name] = { error: String(error) }; }
  }
  if (driver) {
    for (const action of ['state', 'cursor']) {
      try { state[action] = await request(action); } catch (error) { state[action] = { error: String(error) }; }
    }
  }
  return state;
}
async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (error) evidence.failure = await failureState();
  if (driver) { try { await driver.close(); } catch (cleanupError) { error = error || cleanupError; } }
  if (view && !view.webContents.isDestroyed()) view.webContents.destroy();
  for (const item of [other, window]) if (item && !item.isDestroyed()) item.destroy();
  evidence.passed = !error;
  evidence.error = error ? String(error.stack || error) : null;
  if (artifacts) {
    try {
      fs.mkdirSync(artifacts, { recursive: true });
      fs.writeFileSync(path.join(artifacts, `windows-cursors-${process.pid}.json`), JSON.stringify(evidence, null, 2));
    } catch (artifactError) { error = error || artifactError; }
  }
  try { fs.rmSync(temporary, { recursive: true, force: true }); } catch (cleanupError) { error = error || cleanupError; }
  if (error) console.error(error.stack || error);
  else console.log('PASS: native Windows cursor resources, RGBA/DPI/hotspots, stationary updates, chrome, inactive windows, BrowserView and chorded webview capture');
  app.exit(error ? 1 : 0);
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  const work = screen.getPrimaryDisplay().workArea;
  assert.ok(work.width >= 840 && work.height >= 620, 'Native cursor fixture needs an 840x620 DIP unobscured desktop');
  const bounds = { x: work.x + 30, y: work.y + 30, width: Math.min(900, work.width - 60), height: Math.min(680, work.height - 60) };
  window = new BrowserWindow({ ...bounds, title, frame: false, alwaysOnTop: true, show: false,
    webPreferences: { webviewTag: true, sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await load(window.webContents);
  window.show(); window.focus();
  driver = await startNativeInput(temporary, title, 'windows-cursor-helper.cs');
  await request('focus');
  for (const id of ['edit', 'editable']) await hover(window.webContents, id, 'text');
  await hover(window.webContents, 'link', 'pointer');
  for (const shape of shapes) await hover(window.webContents, shape,
    shape === 'context-menu' ? 'default' : shape === 'no-drop' ? 'not-allowed' : shape);
  await hover(window.webContents, 'dynamic', 'pointer');
  await evaluate(window.webContents, 'document.getElementById("dynamic").style.cursor="text"; true');
  await cursor('text', 'stationary renderer style update');
  await hover(window.webContents, 'drag', 'grab');
  const drag = await point(window.webContents, await center(window.webContents, 'drag'));
  await transition(window.webContents, 'down', drag, 'mousedown', 1);
  await cursor('grabbing', 'pressed drag handle');
  await transition(window.webContents, 'up', drag, 'mouseup', 0);
  await cursor('grab', 'released drag handle');
  const custom = await hover(window.webContents, 'custom', 'custom');
  customImage(custom, 16, [5, 7]);
  await evaluate(window.webContents, 'customCursor(32)');
  await cursor('custom', 'stationary custom image replacement');
  // Wait for the new bitmap, not merely the previous custom cursor kind.
  const replacement = await eventually('new custom bitmap reaches Win32', () => request('cursor'), state => state.width === Math.round(32 * state.dpi / 96));
  assert.notEqual(replacement.handle, custom.handle, 'replacing a live owned HCURSOR allocates a new native image');
  customImage(replacement, 32, [10, 14]);
  evidence.observations.push({ label: 'replacement-bitmap', ...replacement });
  await hover(window.webContents, 'link', 'pointer');

  view = new BrowserView({ webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  const viewOffset = { x: 240, y: 110 };
  view.setBounds({ ...viewOffset, width: 500, height: 400 });
  window.setBrowserView(view);
  await load(view.webContents);
  await hover(view.webContents, 'edit', 'text', { offset: viewOffset });
  await hover(window.webContents, 'edit', 'text');
  await hover(view.webContents, 'link', 'pointer', { offset: viewOffset });
  await hover(view.webContents, 'drag', 'grab', { offset: viewOffset });
  const viewDrag = await point(view.webContents, await center(view.webContents, 'drag'), viewOffset);
  await transition(view.webContents, 'down', viewDrag, 'mousedown', 1);
  await cursor('grabbing', 'captured BrowserView');
  const outside = await point(window.webContents, await center(window.webContents, 'link'));
  const captured = await transition(view.webContents, 'move', outside, 'mousemove', 1);
  assert.ok(captured.y < 0, 'captured BrowserView receives native movement outside its bounds');
  await evaluate(view.webContents, 'document.getElementById("drag").style.cursor="copy"; true');
  await cursor('copy', 'captured BrowserView stationary update outside view bounds');
  await transition(view.webContents, 'up', outside, 'mouseup', 0);
  await hover(window.webContents, 'link', 'pointer');
  window.removeBrowserView(view);
  view.webContents.destroy(); view = null;

  // Repro for queued HTCLIENT wakeups overriding native HTCAPTION: retain the
  // renderer's copy cursor, enter chrome, then attach a view away from the pointer.
  await hover(window.webContents, 'copy', 'copy');
  const chromePoint = await point(window.webContents, await center(window.webContents, 'chrome'));
  await request('move', chromePoint);
  const chrome = await cursor('default', 'native draggable caption before wakeup');
  assert.equal(chrome.hit, 2, 'native WM_NCHITTEST identifies HTCAPTION');
  view = new BrowserView({ webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  view.setBounds({ x: 600, y: 100, width: 100, height: 100 });
  window.setBrowserView(view);
  await pause(150);
  await cursor('default', 'queued cursor wakeup preserves native caption');
  window.removeBrowserView(view); view.webContents.destroy(); view = null;
  await cursor('default', 'detached-view wakeup preserves native caption');
  const geometry = await request('geometry');
  await request('move', { x: geometry.clientX + 2, y: geometry.clientY + Math.round(geometry.clientHeight / 2) });
  const edge = await cursor('ew-resize', 'native border overrides page cursor');
  assert.equal(edge.hit, 10, 'native WM_NCHITTEST identifies HTLEFT');
  await hover(window.webContents, 'link', 'pointer');

  const guestURL = 'data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><style>
    html,body{margin:0;height:100%;background:#406080;user-select:none}#guestTarget{height:100%;cursor:crosshair}
    </style><div id="guestTarget">Guest capture</div><script>
    window.lastTarget='';window.lastPointerId=0;window.trace=[];for(const type of ['mousemove','mousedown','mouseup','pointerdown','pointerup','gotpointercapture','lostpointercapture'])document.addEventListener(type,e=>{if(type==='pointerdown')lastPointerId=e.pointerId;trace.push({type,target:e.target.id,x:e.clientX,y:e.clientY,buttons:e.buttons,button:e.button,pointerId:e.pointerId,time:e.timeStamp});if(type==='mousemove')lastTarget=e.target.id});
    document.addEventListener('pointerdown',e=>e.target.setPointerCapture(e.pointerId));document.addEventListener('contextmenu',e=>e.preventDefault());
    </script>`);
  const attached = new Promise(resolve => window.webContents.once('did-attach-webview', (_event, attachedGuest) => {
    guest = attachedGuest;
    resolve(once(guest, 'did-finish-load'));
  }));
  const guestOffset = { x: 460, y: 150 };
  await evaluate(window.webContents, `(()=>{const guest=document.createElement('webview');guest.id='guest';guest.style.cssText='position:absolute;left:460px;top:150px;width:260px;height:180px';guest.setAttribute('src',${JSON.stringify(guestURL)});document.body.appendChild(guest);return true})()`);
  await attached;
  evidence.guestViewport = await eventually('guest renderer and viewport ready', () => json(guest,
    '({ready:document.readyState==="complete"&&typeof window.trace==="object",width:innerWidth,height:innerHeight,dpr:devicePixelRatio})'),
  state => state.ready && state.width === 260 && state.height === 180);
  await hover(guest, 'guestTarget', 'crosshair', { offset: guestOffset });
  const guestPoint = await point(guest, await center(guest, 'guestTarget'), guestOffset);
  await transition(guest, 'down', guestPoint, 'mousedown', 1);
  await transition(guest, 'down', guestPoint, 'mousedown', 3, 'right');
  await transition(guest, 'up', guestPoint, 'mouseup', 2);
  const guestMove = await transition(guest, 'move', outside, 'mousemove', 2);
  assert.ok(guestMove.x < 0 && guestMove.y < 0, 'guest retains out-of-bounds capture until all held buttons are released');
  await cursor('crosshair', 'multi-button captured guest owns native cursor');
  await evaluate(guest, 'document.getElementById("guestTarget").style.cursor="copy"; true');
  await cursor('copy', 'captured guest stationary cursor update outside guest bounds');
  await transition(guest, 'up', outside, 'mouseup', 0, 'right');
  await hover(window.webContents, 'link', 'pointer');
  await evaluate(window.webContents, 'document.getElementById("guest").remove(); true');

  other = new BrowserWindow({ title: title + '-other', x: bounds.x + bounds.width - 350, y: bounds.y + bounds.height - 300,
    width: 340, height: 280, frame: false, show: false, alwaysOnTop: true,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await load(other.webContents); other.showInactive();
  const inactive = await hover(other.webContents, 'edit', 'text', { title: title + '-other', foregroundTitle: title });
  assert.notEqual(inactive.foreground, inactive.window, 'native cursor updates do not require an active window');
  await hover(window.webContents, 'link', 'pointer');
  await evaluate(other.webContents, 'document.getElementById("edit").style.cursor="not-allowed"; true');
  await pause(200);
  await cursor('pointer', 'non-hovered window cannot steal native cursor');
  await request('wakeup', { targetTitle: title + '-other' });
  await cursor('pointer', 'non-hovered queued cursor wakeup is consumed');
  other.hide();
  await request('wakeup', { targetTitle: title + '-other' });
  await cursor('pointer', 'hidden queued cursor wakeup is consumed');
  other.showInactive();
  other.setIgnoreMouseEvents(true);
  await request('wakeup', { targetTitle: title + '-other' });
  await cursor('pointer', 'mouse-ignored queued cursor wakeup is consumed');
  other.setIgnoreMouseEvents(false);
  other.destroy(); other = null;

  // Exercise a real DPI transition if a second differently scaled monitor exists.
  evidence.displays = screen.getAllDisplays().map(display => ({ id: display.id, scaleFactor: display.scaleFactor, workArea: display.workArea }));
  for (const display of screen.getAllDisplays()) {
    if (display.scaleFactor === screen.getPrimaryDisplay().scaleFactor || display.workArea.width < bounds.width + 20 || display.workArea.height < bounds.height + 20) continue;
    window.setPosition(display.workArea.x + 10, display.workArea.y + 10);
    await eventually('native monitor DPI transition', () => request('geometry'), state => Math.abs(state.dpi / 96 - display.scaleFactor) < 0.01);
    customImage(await hover(window.webContents, 'custom', 'custom'), 32, [10, 14]);
  }
  await hover(window.webContents, 'link', 'pointer');
  await window.webContents.loadURL('data:text/html,<style>html,body{margin:0;height:100%;user-select:none}</style>Navigation reset');
  await cursor('default', 'navigation releases stale cursor');
  console.log(JSON.stringify(evidence));
}).then(() => finish(), finish);
