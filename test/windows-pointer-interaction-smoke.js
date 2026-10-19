'use strict';

// Run with out/windows-x64/runtime-dist/electron.exe test/windows-pointer-interaction-smoke.js
// on an interactive Windows desktop. Every pointer action below is native SendInput
// in physical screen pixels; no DOM event or webContents input synthesis is used.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');
const { startNativeInput } = require('./windows-native-input-driver');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-windows-pointer-interaction-'));
app.setPath('userData', path.join(temporary, 'user-data'));
const title = `mini-windows-pointer-interaction-${process.pid}`;
const artifacts = process.env.MINI_NATIVE_INPUT_ARTIFACTS;
const artifactStem = `windows-pointer-interaction-${process.pid}`;
const evidence = { title, hover: [], drags: [] };
const pause = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
let window, driver, finished = false;
const request = (...args) => driver.request(...args);
const watchdog = setTimeout(() => finish(new Error('Windows pointer interaction smoke exceeded 60s')), 60000);
app.on('window-all-closed', () => {});

async function evaluate(source) {
  return window.webContents.executeJavaScript(source);
}

const state = () => evaluate('JSON.stringify(window.pointerState())').then(JSON.parse);

async function eventually(label, read, predicate, timeout = 6000) {
  const deadline = Date.now() + timeout;
  let actual;
  do {
    actual = await read();
    if (predicate(actual)) return actual;
    await pause(35);
  } while (Date.now() < deadline);
  assert.fail(`${label}: ${JSON.stringify(actual)}`);
}

function physicalPoint(view, geometry, x, y) {
  return {
    x: Math.round(geometry.clientX + x * view.dpr),
    y: Math.round(geometry.clientY + y * view.dpr),
  };
}

function nativeBounds(geometry) {
  return {
    left: geometry.left,
    top: geometry.top,
    right: geometry.right,
    bottom: geometry.bottom,
  };
}

function closeEnough(actual, expected, tolerance = 2) {
  return Math.abs(actual - expected) <= tolerance;
}

async function moveAt(view, geometry, x, y) {
  const expected = physicalPoint(view, geometry, x, y);
  await request('focus');
  await request('pointerMove', expected);
  const probe = await request('pointerProbe', expected);
  evidence.lastPointerProbe = probe;
  assert.ok(closeEnough(probe.cursor.x, expected.x) && closeEnough(probe.cursor.y, expected.y),
    `SendInput pointer missed its physical target: ${JSON.stringify(probe)}`);
  assert.equal(probe.foreground, probe.target,
    `native test window lost foreground during pointer movement: ${JSON.stringify(probe)}`);
  return probe;
}

async function assertHoverAt(controlName, x, y, label) {
  let view = await state();
  const geometry = await request('geometry');
  await moveAt(view, geometry, view.viewport.width / 2, view.viewport.height - 28);
  await eventually(`${label} clears away from the resize handle`, state,
    actual => !actual.controls[controlName].highlighted);
  view = await state();
  await moveAt(view, geometry, x(view), y(view));
  const hovered = await eventually(label, state, actual => actual.controls[controlName].highlighted);
  evidence.hover.push({ label, rect: hovered.controls[controlName].rect });
}

async function dragControl({ name, axis, farMovement, backMovement, farSizeDelta, backSizeDelta, cross }) {
  const before = await state();
  const beforeNative = await request('geometry');
  const rect = before.controls[name].rect;
  const origin = axis === 'x'
    ? { x: rect.x + rect.width / 2, y: cross(before) }
    : { x: cross(before), y: rect.y + rect.height / 2 };
  const at = movement => axis === 'x'
    ? { x: origin.x + movement, y: origin.y }
    : { x: origin.x, y: origin.y + movement };
  const startSize = before.controls[name].size;
  const firstEvent = before.controls[name].events.length;

  await moveAt(before, beforeNative, origin.x, origin.y);
  await request('pointerDown', physicalPoint(before, beforeNative, origin.x, origin.y));
  await eventually(`${name} native press starts resize`, state,
    actual => actual.controls[name].dragging);

  const far = at(farMovement);
  await request('pointerMove', physicalPoint(before, beforeNative, far.x, far.y));
  const enlarged = await eventually(`${name} captured drag crosses the adjacent pane`, state,
    actual => actual.controls[name].dragging
      && closeEnough(actual.controls[name].size, startSize + farSizeDelta));
  assert.ok(Math.abs(enlarged.controls[name].size - startSize) >= 60,
    `${name} drag must produce a meaningful pane-size delta`);
  assert.deepEqual(nativeBounds(await request('geometry')), nativeBounds(beforeNative),
    `${name} pane drag must not become a native window drag`);

  const back = at(backMovement);
  await request('pointerMove', physicalPoint(before, beforeNative, back.x, back.y));
  await eventually(`${name} held drag backtracks`, state,
    actual => actual.controls[name].dragging
      && closeEnough(actual.controls[name].size, startSize + backSizeDelta));
  await request('pointerUp', physicalPoint(before, beforeNative, back.x, back.y));
  const released = await eventually(`${name} mouse-up releases pointer capture`, state, actual => {
    const events = actual.controls[name].events.slice(firstEvent);
    return !actual.controls[name].dragging
      && events.some(event => event.type === 'pointerup' && event.buttons === 0)
      && events.some(event => event.type === 'lostpointercapture');
  });

  const events = released.controls[name].events.slice(firstEvent);
  assert.ok(events.some(event => event.type === 'pointerdown' && event.button === 0 && event.buttons === 1),
    `${name} must receive a genuine held primary-button press`);
  assert.ok(events.some(event => event.type === 'gotpointercapture'),
    `${name} must gain DOM pointer capture`);
  assert.ok(events.some(event => event.type === 'pointermove'
    && event.buttons === 1 && event.pointerType === 'mouse'),
  `${name} captured movement must preserve the native mouse button state`);

  const releasedSize = released.controls[name].size;
  await moveAt(released, beforeNative, released.viewport.width / 2, released.viewport.height - 28);
  await pause(180);
  const final = await state();
  assert.ok(closeEnough(final.controls[name].size, releasedSize, 0.5),
    `${name} must not resize on a subsequent unpressed move`);
  assert.ok(closeEnough(final.controls[name].size, startSize + backSizeDelta),
    `${name} must retain the backtracked size after release`);
  assert.deepEqual(nativeBounds(await request('geometry')), nativeBounds(beforeNative),
    `${name} released movement must leave native window bounds unchanged`);
  evidence.drags.push({ name, startSize, farSize: enlarged.controls[name].size,
    releasedSize: final.controls[name].size, events });
}

async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (error && driver && window && !window.isDestroyed() && artifacts) {
    try {
      fs.mkdirSync(artifacts, { recursive: true });
      await request('focus');
      await request('capture', { path: path.join(artifacts, `${artifactStem}-failure.png`) });
      evidence.failureState = await state();
    } catch (diagnosticError) {
      evidence.diagnosticError = String(diagnosticError.stack || diagnosticError);
    }
  }
  evidence.passed = !error;
  evidence.error = error ? String(error.stack || error) : null;
  if (artifacts) {
    try {
      fs.mkdirSync(artifacts, { recursive: true });
      fs.writeFileSync(path.join(artifacts, `${artifactStem}.json`), JSON.stringify(evidence, null, 2));
    } catch (artifactError) { error = error || artifactError; }
  }
  if (driver) {
    try { await driver.close(); }
    catch (cleanupError) { error = error || cleanupError; }
  }
  if (window && !window.isDestroyed()) window.destroy();
  try { fs.rmSync(temporary, { recursive: true, force: true }); }
  catch (cleanupError) { error = error || cleanupError; }
  if (error) console.error(error.stack || error);
  else console.log('PASS: Windows fine/hover media queries, full-edge hover and captured pane drags on both axes');
  app.exit(error ? 1 : 0);
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  driver = await startNativeInput(temporary, title);
  window = new BrowserWindow({
    x: 120,
    y: 60,
    width: 820,
    height: 560,
    title,
    frame: false,
    show: false,
    alwaysOnTop: true,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html>
    <meta charset="utf-8"><title>${title}</title><style>
    :root{--left-size:220px;--right-size:160px;--top-size:210px}
    html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#20242b;color:#f4f6f8;font:16px sans-serif}
    body{-webkit-app-region:drag;user-select:none}
    [data-window-chrome-interactive],[data-window-chrome-interactive] *{-webkit-app-region:no-drag!important}
    #left{position:absolute;inset:0 auto 0 0;width:var(--left-size);background:#111820;border-right:1px solid #58616d;box-sizing:border-box}
    #right{position:absolute;inset:0 0 0 auto;width:var(--right-size);background:#18202a;border-left:1px solid #58616d;box-sizing:border-box}
    #workspace{position:absolute;inset:0 var(--right-size) 0 var(--left-size);background:#28303a}
    #upper{position:absolute;inset:0 0 auto 0;height:var(--top-size);background:#303944;border-bottom:1px solid #66717e;box-sizing:border-box}
    #lower{position:absolute;inset:var(--top-size) 0 0 0;background:#232a32}
    .copy{padding:22px;pointer-events:none}.handle{position:absolute;touch-action:none;z-index:4}
    .side-handle{top:calc(50% - 44px);width:24px;height:88px;cursor:col-resize}
    #left-handle{right:0}#right-handle{left:0}
    #row-handle{left:calc(50% - 44px);top:var(--top-size);width:88px;height:24px;cursor:row-resize;transform:translateY(-50%)}
    #away-target{position:fixed;left:calc(50% - 24px);bottom:4px;width:48px;height:48px;z-index:9}
    .indicator{position:absolute;background:#268bff;opacity:0;pointer-events:none}
    .side-handle .indicator{left:calc(50% - 1px);top:0;bottom:0;width:3px}
    #row-handle .indicator{left:0;right:0;top:calc(50% - 1px);height:3px}
    .handle.hot .indicator{opacity:1}
    @media (hover:hover) and (pointer:fine){
      .side-handle{top:0;bottom:0;width:10px;height:auto}
      #left-handle{right:-5px}#right-handle{left:-5px}
      #row-handle{left:0;right:0;width:auto;height:10px;transform:translateY(-50%)}
    }
    </style>
    <aside id="left"><div class="copy">Left sidebar</div><div id="left-handle" class="handle side-handle" data-window-chrome-interactive="true"><div class="indicator"></div></div></aside>
    <main id="workspace"><section id="upper"><div class="copy">Upper pane</div></section><section id="lower"><div class="copy">Lower pane</div></section><div id="row-handle" class="handle" data-window-chrome-interactive="true"><div class="indicator"></div></div></main>
    <aside id="right"><div id="right-handle" class="handle side-handle" data-window-chrome-interactive="true"><div class="indicator"></div></div><div class="copy">Right rail</div></aside>
    <div id="away-target" data-window-chrome-interactive="true" aria-hidden="true"></div>
    <script>
    const root=document.documentElement;
    const definitions={
      left:{element:document.getElementById('left-handle'),size:()=>document.getElementById('left').getBoundingClientRect().width,axis:'x',sign:1,property:'--left-size'},
      right:{element:document.getElementById('right-handle'),size:()=>document.getElementById('right').getBoundingClientRect().width,axis:'x',sign:-1,property:'--right-size'},
      row:{element:document.getElementById('row-handle'),size:()=>document.getElementById('upper').getBoundingClientRect().height,axis:'y',sign:1,property:'--top-size'},
    };
    for(const [name,definition] of Object.entries(definitions)){
      const interaction={dragging:false,pointerId:null,startCoordinate:0,startSize:0,events:[],timer:null};
      definition.interaction=interaction;
      const record=event=>interaction.events.push({type:event.type,button:event.button,buttons:event.buttons,pointerType:event.pointerType,x:event.clientX,y:event.clientY});
      for(const type of ['pointerenter','pointerleave','pointerdown','pointermove','pointerup','pointercancel','gotpointercapture','lostpointercapture'])
        definition.element.addEventListener(type,record);
      definition.element.addEventListener('pointerenter',()=>{clearTimeout(interaction.timer);interaction.timer=setTimeout(()=>definition.element.classList.add('hot'),150)});
      definition.element.addEventListener('pointerleave',()=>{clearTimeout(interaction.timer);definition.element.classList.remove('hot')});
      definition.element.addEventListener('pointerdown',event=>{
        if(event.button!==0)return;
        interaction.dragging=true;interaction.pointerId=event.pointerId;
        interaction.startCoordinate=definition.axis==='x'?event.clientX:event.clientY;
        interaction.startSize=definition.size();
        definition.element.setPointerCapture(event.pointerId);
        event.preventDefault();event.stopPropagation();
      });
      definition.element.addEventListener('pointermove',event=>{
        if(!interaction.dragging||!(event.buttons&1))return;
        const coordinate=definition.axis==='x'?event.clientX:event.clientY;
        const size=Math.max(80,interaction.startSize+definition.sign*(coordinate-interaction.startCoordinate));
        root.style.setProperty(definition.property,size+'px');
        event.preventDefault();event.stopPropagation();
      });
      const end=event=>{if(event.pointerId===interaction.pointerId){interaction.dragging=false;interaction.pointerId=null}};
      definition.element.addEventListener('pointerup',end);
      definition.element.addEventListener('pointercancel',end);
      definition.element.addEventListener('lostpointercapture',()=>{interaction.dragging=false;interaction.pointerId=null});
    }
    window.pointerState=()=>{
      const controls={};
      for(const [name,definition] of Object.entries(definitions)){
        const rect=definition.element.getBoundingClientRect(),interaction=definition.interaction;
        controls[name]={rect:{x:rect.x,y:rect.y,width:rect.width,height:rect.height},size:definition.size(),
          highlighted:definition.element.classList.contains('hot'),dragging:interaction.dragging,events:interaction.events.slice()};
      }
      return {hover:matchMedia('(hover:hover)').matches,fine:matchMedia('(pointer:fine)').matches,
        anyHover:matchMedia('(any-hover:hover)').matches,anyFine:matchMedia('(any-pointer:fine)').matches,
        viewport:{width:innerWidth,height:innerHeight},dpr:devicePixelRatio,controls};
    };
    </script>`));
  window.show();
  app.focus();
  window.focus();
  await request('focus');
  await eventually('native pointer window focused', () => Promise.resolve(window.isFocused()), Boolean);

  const initial = await state();
  assert.equal(initial.hover, true, 'desktop primary pointer must support hover');
  assert.equal(initial.fine, true, 'desktop primary pointer must be fine');
  assert.equal(initial.anyHover, true, 'at least one desktop pointer must support hover');
  assert.equal(initial.anyFine, true, 'at least one desktop pointer must be fine');
  assert.equal(initial.controls.left.rect.height, initial.viewport.height,
    'left sidebar fine-pointer hit area must span the client height');
  assert.equal(initial.controls.right.rect.height, initial.viewport.height,
    'right sidebar fine-pointer hit area must span the client height');
  assert.equal(initial.controls.row.rect.width,
    initial.viewport.width - initial.controls.left.size - initial.controls.right.size,
    'row resize hit area must span the adjacent workspace panes');

  for (const name of ['left', 'right']) {
    for (const [position, y] of [['top', 30], ['middle', initial.viewport.height / 2], ['bottom', initial.viewport.height - 30]]) {
      await assertHoverAt(name,
        view => view.controls[name].rect.x + view.controls[name].rect.width / 2,
        () => y, `${name} full-height ${position} hover`);
    }
  }
  for (const [position, ratio] of [['left', 0.15], ['middle', 0.5], ['right', 0.85]]) {
    await assertHoverAt('row',
      view => view.controls.row.rect.x + view.controls.row.rect.width * ratio,
      view => view.controls.row.rect.y + view.controls.row.rect.height / 2,
      `row full-width ${position} hover`);
  }

  await dragControl({ name: 'left', axis: 'x', farMovement: 100, backMovement: 48,
    farSizeDelta: 100, backSizeDelta: 48, cross: view => view.viewport.height - 42 });
  await dragControl({ name: 'right', axis: 'x', farMovement: -90, backMovement: -44,
    farSizeDelta: 90, backSizeDelta: 44, cross: view => 42 });
  await dragControl({ name: 'row', axis: 'y', farMovement: 75, backMovement: 36,
    farSizeDelta: 75, backSizeDelta: 36, cross: view => view.viewport.width / 2 });

  if (artifacts) {
    fs.mkdirSync(artifacts, { recursive: true });
    await request('focus');
    const capturePath = path.join(artifacts, `${artifactStem}.png`);
    await request('capture', { path: capturePath });
    evidence.capture = capturePath;
  }
}).then(() => finish(), finish);
