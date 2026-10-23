'use strict';

// Run on an unlocked Windows desktop. This smoke posts native mouse messages only
// to PID-verified owned HWNDs. It never moves the global cursor, calls SendInput,
// or modifies user input/display settings. Maximize and BrowserView coverage may
// briefly activate only the owned fixture window.
//
// MINI_POINTER_SCREEN_COORDINATES_RESULT=<path> out/windows-x64/runtime-dist/electron.exe test/windows-pointer-screen-coordinates-smoke.js
const assert = require('node:assert/strict');
const { execFile, execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { promisify } = require('node:util');
const { app, BrowserView, BrowserWindow, screen } = require('electron');

const run = promisify(execFile);
const pause = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-pointer-screen-coordinates-'));
const resultPath = process.env.MINI_POINTER_SCREEN_COORDINATES_RESULT
  ? path.resolve(process.env.MINI_POINTER_SCREEN_COORDINATES_RESULT) : null;
const title = `mini-pointer-screen-coordinates-${process.pid}`;
const helper = path.join(temporary, 'pointer-screen-coordinates-helper.exe');
const evidence = {
  runtimeVersion: process.versions.miniElectron || process.versions.electron,
  scope: 'PID/HWND-guarded PostMessage input into owned test windows; maximize/BrowserView may activate only that owned host; trusted DOM events, not physical SendInput',
  resultPath,
  displays: [],
  samples: [],
  interaction: null,
  browserView: null,
  assertions: null,
};
let target;
let view;
let finished = false;
const watchdog = setTimeout(() => finish(new Error('Windows pointer screen-coordinate smoke exceeded 60s')), 60000);
app.on('window-all-closed', () => {});
app.setPath('userData', path.join(temporary, 'user-data'));
app.setPath('sessionData', path.join(temporary, 'session-data'));

function page(label) {
  return `<!doctype html><meta charset="utf-8"><title>${label}</title><style>
    html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#17324a;color:white;font:16px sans-serif}
    #target{position:absolute;inset:0;touch-action:none;padding:20px;box-sizing:border-box}
  </style><div id="target">${label}</div><script>
    (()=>{
      const target=document.getElementById('target');let events=[];
      const record=event=>events.push({type:event.type,constructor:event.constructor.name,
        clientX:event.clientX,clientY:event.clientY,screenX:event.screenX,screenY:event.screenY,
        button:event.button,buttons:event.buttons,pointerType:event.pointerType||null,pointerId:event.pointerId||null,
        deltaX:Number.isFinite(event.deltaX)?event.deltaX:null,deltaY:Number.isFinite(event.deltaY)?event.deltaY:null,
        isTrusted:event.isTrusted,target:event.target.id});
      for(const type of ['pointermove','mousemove','pointerdown','mousedown','pointerup','mouseup',
        'gotpointercapture','lostpointercapture','wheel']) target.addEventListener(type,record);
      target.addEventListener('pointerdown',event=>target.setPointerCapture(event.pointerId));
      target.addEventListener('wheel',event=>event.preventDefault(),{passive:false});
      window.resetCoordinateProbe=()=>{events=[];return true};
      window.readCoordinateProbe=()=>({screenX:window.screenX,screenY:window.screenY,
        screenLeft:window.screenLeft,screenTop:window.screenTop,innerWidth,innerHeight,
        outerWidth,outerHeight,dpr:devicePixelRatio,events:events.slice()});
    })();
  </script>`;
}

function hwndOf(window) {
  const handle = window.getNativeWindowHandle();
  return (handle.length === 8 ? handle.readBigUInt64LE() : BigInt(handle.readUInt32LE())).toString();
}

async function native(action, values = {}) {
  const args = [action, String(process.pid), hwndOf(target)];
  if (action !== 'geometry') args.push(String(values.x), String(values.y));
  if (action === 'wheel') args.push(String(values.delta));
  const { stdout } = await run(helper, args, { windowsHide: true, timeout: 5000 });
  return JSON.parse(stdout.trim());
}

async function evaluate(contents, source) {
  return contents.executeJavaScript(source);
}
async function reset(contents) {
  await evaluate(contents, 'resetCoordinateProbe()');
}
async function state(contents) {
  return JSON.parse(await evaluate(contents, 'JSON.stringify(readCoordinateProbe())'));
}
async function eventually(label, contents, predicate, timeout = 5000) {
  const deadline = Date.now() + timeout;
  let actual;
  do {
    actual = await state(contents);
    if (predicate(actual)) return actual;
    await pause(30);
  } while (Date.now() < deadline);
  throw new Error(`${label}: ${JSON.stringify(actual)}`);
}
function displayRecord(display) {
  return { id: display.id, bounds: display.bounds, workArea: display.workArea,
    scaleFactor: display.scaleFactor, rotation: display.rotation };
}
function eventOf(record, type) {
  return [...record.renderer.events].reverse().find(event => event.type === type
    && Math.abs(event.clientX - record.cssTarget.x) <= 1.1
    && Math.abs(event.clientY - record.cssTarget.y) <= 1.1);
}

async function collectMove(label, requested, displayKind) {
  if (requested) {
    target.setBounds(requested, false);
    await pause(220);
  }
  await reset(target.webContents);
  const before = await state(target.webContents);
  const physical = { x: Math.round(140 * before.dpr), y: Math.round(90 * before.dpr) };
  const geometry = await native('geometry');
  const message = await native('move', physical);
  const renderer = await eventually(`${label} pointer/mouse move delivery`, target.webContents,
    value => ['pointermove', 'mousemove'].every(type => value.events.some(event => event.type === type
      && Math.abs(event.clientX - 140) <= 1.1 && Math.abs(event.clientY - 90) <= 1.1)));
  const bounds = target.getBounds();
  const display = screen.getDisplayNearestPoint({ x: bounds.x + Math.floor(bounds.width / 2),
    y: bounds.y + Math.floor(bounds.height / 2) });
  const record = { label, displayKind, requested, bounds, display: displayRecord(display),
    windowState: { maximized: target.isMaximized(), focused: target.isFocused() },
    cssTarget: { x: 140, y: 90 }, physicalTarget: physical, geometry, message, renderer };
  evidence.samples.push(record);
  return record;
}
async function collectPositionMove(label, position) {
  target.setPosition(position.x, position.y, false);
  await pause(220);
  const record = await collectMove(label, null, 'set-position');
  record.requested = position;
  record.position = target.getPosition();
  return record;
}

async function collectInteraction() {
  const contents = target.webContents;
  await reset(contents);
  const before = await state(contents);
  const start = { x: Math.round(180 * before.dpr), y: Math.round(130 * before.dpr) };
  const held = { x: Math.round(230 * before.dpr), y: Math.round(165 * before.dpr) };
  const messages = [];
  messages.push(await native('move', start));
  messages.push(await native('down', start));
  messages.push(await native('heldMove', held));
  messages.push(await native('up', held));
  messages.push(await native('wheel', { x: held.x, y: held.y, delta: -120 }));
  const renderer = await eventually('pointer press/capture/release and wheel delivery', contents, value => {
    const delivered = (type, point) => value.events.some(event => event.type === type
      && Math.abs(event.clientX - point.x / before.dpr) <= 1.1
      && Math.abs(event.clientY - point.y / before.dpr) <= 1.1);
    return ['pointerdown', 'mousedown'].every(type => delivered(type, start))
      && ['pointerup', 'mouseup', 'wheel'].every(type => delivered(type, held))
      && ['gotpointercapture', 'lostpointercapture'].every(type =>
        value.events.some(event => event.type === type))
      && value.events.some(event => event.type === 'pointermove' && event.buttons === 1);
  });
  evidence.interaction = { bounds: target.getBounds(), start, held, messages, renderer,
    finalGeometry: await native('geometry') };
}
async function collectBrowserViewHost(display) {
  if (target && !target.isDestroyed()) target.destroy();
  const area = display.workArea;
  const bounds = { x: area.x + 70, y: area.y + 70,
    width: Math.min(820, area.width - 140), height: Math.min(560, area.height - 140) };
  const viewBounds = { x: 330, y: 120, width: 300, height: 230 };
  target = new BrowserWindow({ ...bounds, title: `${title}-browser-view`, frame: false, show: false,
    minWidth: 0, minHeight: 0, skipTaskbar: true,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false,
      backgroundThrottling: false } });
  await target.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(page('BrowserView host probe')));
  target.show();
  target.focus();
  view = new BrowserView({ webPreferences: { sandbox: true, contextIsolation: true,
    nodeIntegration: false } });
  target.addBrowserView(view);
  view.setBounds({ ...viewBounds, width: 1, height: 1 });
  await view.webContents.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(page('BrowserView coordinate probe')));
  view.setBounds(viewBounds);
  await eventually('BrowserView viewport readiness', view.webContents,
    value => value.innerWidth === viewBounds.width && value.innerHeight === viewBounds.height);
  await Promise.all([reset(target.webContents), reset(view.webContents)]);
  const before = await state(view.webContents);
  const point = { x: Math.round((viewBounds.x + 70) * before.dpr),
    y: Math.round((viewBounds.y + 65) * before.dpr) };
  const move = await native('move', point);
  const wheel = await native('wheel', { ...point, delta: -120 });
  const renderer = await eventually('BrowserView move and wheel delivery', view.webContents, value =>
    value.events.some(event => event.type === 'pointermove'
      && Math.abs(event.clientX - 70) <= 1.1 && Math.abs(event.clientY - 65) <= 1.1)
      && value.events.some(event => event.type === 'mousemove'
        && Math.abs(event.clientX - 70) <= 1.1 && Math.abs(event.clientY - 65) <= 1.1)
      && value.events.some(event => event.type === 'wheel'
        && Math.abs(event.clientX - 70) <= 1.1 && Math.abs(event.clientY - 65) <= 1.1));
  evidence.browserView = { executed: true, bounds: viewBounds, windowBounds: target.getBounds(),
    cssTarget: { x: 70, y: 65 }, physicalHostTarget: point, move, wheel, renderer,
    rootRenderer: await state(target.webContents), focused: target.isFocused() };
}

async function collectFramedHost(display) {
  if (view && target && !target.isDestroyed()) target.removeBrowserView(view);
  if (view && !view.webContents.isDestroyed()) view.webContents.destroy();
  view = null;
  if (target && !target.isDestroyed()) target.destroy();
  const area = display.workArea;
  const bounds = { x: area.x + 90, y: area.y + 80,
    width: Math.min(720, area.width - 180), height: Math.min(500, area.height - 160) };
  target = new BrowserWindow({ ...bounds, title: `${title}-framed`, frame: true, show: false,
    focusable: false, skipTaskbar: true, minWidth: 0, minHeight: 0,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false,
      backgroundThrottling: false } });
  await target.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(page('Framed host coordinate probe')));
  target.showInactive();
  await pause(180);
  await collectMove('framed-host', bounds, 'framed');
}

function runAssertions() {
  const failures = [];
  let passed = 0;
  const check = (label, callback) => {
    try { callback(); passed++; }
    catch (error) { failures.push({ label, message: error.message }); }
  };
  const near = (actual, expected, label, tolerance = 1.1) => assert.ok(
    Number.isFinite(actual) && Math.abs(actual - expected) <= tolerance,
    `${label}: expected ${expected} +/- ${tolerance}, got ${actual}`);
  const nativeDipPoint = (message, windowBounds) => {
    const scale = message.dpi / 96;
    return {
      x: windowBounds.x + (message.point.screenX - message.window.left) / scale,
      y: windowBounds.y + (message.point.screenY - message.window.top) / scale,
    };
  };
  const absoluteEvent = (event, expectedScreen, client, label) => {
    assert.ok(event, `${label}: event missing`);
    assert.equal(event.isTrusted, true, `${label}: native HWND message must produce a trusted DOM event`);
    near(event.clientX, client.x, `${label}.clientX`);
    near(event.clientY, client.y, `${label}.clientY`);
    near(event.screenX, expectedScreen.x, `${label}.screenX`);
    near(event.screenY, expectedScreen.y, `${label}.screenY`);
  };
  const noForegroundAcquisition = (message, label) => {
    assert.notEqual(message.foregroundBefore, message.hwnd, `${label}: target was foreground before injection`);
    assert.notEqual(message.foregroundAfter, message.hwnd, `${label}: target acquired foreground`);
  };

  for (const sample of evidence.samples) {
    check(`${sample.label}: window global origin`, () => {
      near(sample.renderer.screenX, sample.bounds.x, 'window.screenX');
      near(sample.renderer.screenY, sample.bounds.y, 'window.screenY');
      near(sample.renderer.screenLeft, sample.bounds.x, 'window.screenLeft');
      near(sample.renderer.screenTop, sample.bounds.y, 'window.screenTop');
    });
    check(`${sample.label}: pointer global coordinates`, () => absoluteEvent(eventOf(sample, 'pointermove'),
      nativeDipPoint(sample.message, sample.bounds), sample.cssTarget, 'pointermove'));
    check(`${sample.label}: mouse global coordinates`, () => absoluteEvent(eventOf(sample, 'mousemove'),
      nativeDipPoint(sample.message, sample.bounds), sample.cssTarget, 'mousemove'));
    check(`${sample.label}: native coordinate oracle`, () => {
      assert.equal(sample.message.point.screenX,
        sample.message.client.origin.x + sample.message.point.clientX, 'ClientToScreen x');
      assert.equal(sample.message.point.screenY,
        sample.message.client.origin.y + sample.message.point.clientY, 'ClientToScreen y');
      assert.deepEqual(sample.message.window, sample.geometry.window, 'stable WindowRect around message');
      assert.equal(sample.message.dpi, sample.geometry.dpi, 'stable GetDpiForWindow around message');
      if (sample.displayKind !== 'owned-activated') {
        noForegroundAcquisition(sample.message, sample.label);
        assert.equal(sample.windowState.focused, false, `${sample.label}: nonactivating test window gained focus`);
      }
    });
    check(`${sample.label}: native DPI matches renderer`, () =>
      near(sample.renderer.dpr, sample.geometry.dpi / 96, 'devicePixelRatio versus GetDpiForWindow', 0.02));
    if (sample.displayKind === 'actual-display') check(`${sample.label}: actual display scale`, () =>
      near(sample.display.scaleFactor, sample.geometry.dpi / 96, 'screen display scale versus native DPI', 0.02));
    if (sample.displayKind === 'framed') check(`${sample.label}: outer and client origins are distinct`, () => {
      assert.ok(sample.geometry.client.origin.x !== sample.geometry.window.left
        || sample.geometry.client.origin.y !== sample.geometry.window.top,
      'framed fixture must retain a real non-client offset');
    });
  }

  const negative = evidence.samples.find(sample => sample.label === 'negative-offscreen');
  const positive = evidence.samples.find(sample => sample.label === 'positive-offscreen');
  const resized = evidence.samples.find(sample => sample.label === 'negative-resized');
  const positioned = evidence.samples.find(sample => sample.label === 'set-position-only');
  check('negative and positive offscreen origins are real', () => {
    assert.ok(negative.bounds.x < 0 && negative.bounds.y < 0, `negative bounds: ${JSON.stringify(negative.bounds)}`);
    assert.ok(positive.bounds.x > 0 && positive.bounds.y > 0, `positive bounds: ${JSON.stringify(positive.bounds)}`);
    assert.notEqual(eventOf(negative, 'pointermove').screenX, eventOf(positive, 'pointermove').screenX,
      'same client point must have different screenX after movement');
    assert.notEqual(eventOf(negative, 'pointermove').screenY, eventOf(positive, 'pointermove').screenY,
      'same client point must have different screenY after movement');
  });
  check('setPosition-only preserves viewport and updates global origin', () => {
    near(positioned.bounds.x, positioned.requested.x, 'setPosition requested DIP x');
    near(positioned.bounds.y, positioned.requested.y, 'setPosition requested DIP y');
    near(positioned.position[0], positioned.bounds.x, 'getPosition DIP x');
    near(positioned.position[1], positioned.bounds.y, 'getPosition DIP y');
    near(positioned.bounds.width, negative.bounds.width, 'setPosition width');
    near(positioned.bounds.height, negative.bounds.height, 'setPosition height');
    near(positioned.renderer.innerWidth, negative.renderer.innerWidth, 'setPosition innerWidth');
    near(positioned.renderer.innerHeight, negative.renderer.innerHeight, 'setPosition innerHeight');
    assert.notEqual(positioned.bounds.x, negative.bounds.x);
    assert.notEqual(positioned.bounds.y, negative.bounds.y);
    assert.notEqual(eventOf(positioned, 'pointermove').screenX, eventOf(negative, 'pointermove').screenX);
    assert.notEqual(eventOf(positioned, 'pointermove').screenY, eventOf(negative, 'pointermove').screenY);
  });
  check('resize preserves origin and updates viewport', () => {
    near(resized.bounds.x, negative.bounds.x + 260, 'resized x');
    near(resized.bounds.y, negative.bounds.y + 140, 'resized y');
    assert.notEqual(resized.renderer.innerWidth, negative.renderer.innerWidth);
    assert.notEqual(resized.renderer.innerHeight, negative.renderer.innerHeight);
  });
  const sizeRestored = evidence.samples.find(sample => sample.label === 'size-restored');
  check('restoring size preserves updated origin', () => {
    near(sizeRestored.bounds.width, negative.bounds.width, 'restored width');
    near(sizeRestored.bounds.height, negative.bounds.height, 'restored height');
    assert.notEqual(sizeRestored.bounds.x, negative.bounds.x);
    assert.notEqual(sizeRestored.bounds.y, negative.bounds.y);
  });
  const maximized = evidence.samples.find(sample => sample.label === 'maximized');
  const restored = evidence.samples.find(sample => sample.label === 'restored');
  check('maximize and restore update global coordinates on the owned host', () => {
    assert.equal(maximized.windowState.maximized, true, 'maximized sample state');
    assert.equal(restored.windowState.maximized, false, 'restored sample state');
    assert.notEqual(eventOf(maximized, 'pointermove').screenX,
      eventOf(restored, 'pointermove').screenX, 'maximize/restore global screenX');
  });

  const interaction = evidence.interaction;
  check('pointer/mouse held-button semantics', () => {
    for (const type of ['pointerdown', 'mousedown', 'pointerup', 'mouseup'])
      assert.equal(interaction.renderer.events.find(event => event.type === type).isTrusted, true, `${type}.isTrusted`);
    const heldMove = interaction.renderer.events.find(event => event.type === 'pointermove' && event.buttons === 1);
    assert.ok(heldMove, 'held pointermove with buttons=1');
    assert.equal(interaction.renderer.events.find(event => event.type === 'pointerdown').buttons, 1);
    assert.equal(interaction.renderer.events.find(event => event.type === 'pointerup').buttons, 0);
    assert.ok(interaction.renderer.events.some(event => event.type === 'gotpointercapture'));
    assert.ok(interaction.renderer.events.some(event => event.type === 'lostpointercapture'));
  });
  check('pointer/mouse/wheel interaction global coordinates', () => {
    const messageByType = {
      pointerdown: interaction.messages[1], mousedown: interaction.messages[1],
      pointerup: interaction.messages[3], mouseup: interaction.messages[3], wheel: interaction.messages[4],
    };
    for (const type of ['pointerdown', 'mousedown', 'pointerup', 'mouseup', 'wheel']) {
      const message = messageByType[type];
      const client = { x: message.point.clientX / interaction.renderer.dpr,
        y: message.point.clientY / interaction.renderer.dpr };
      const event = interaction.renderer.events.find(candidate => candidate.type === type
        && Math.abs(candidate.clientX - client.x) <= 1.1 && Math.abs(candidate.clientY - client.y) <= 1.1);
      absoluteEvent(event, nativeDipPoint(message, interaction.bounds), client, type);
    }
    assert.notEqual(interaction.renderer.events.find(event => event.type === 'wheel').deltaY, 0);
    for (const message of interaction.messages) noForegroundAcquisition(message, `interaction ${message.action}`);
    noForegroundAcquisition(interaction.finalGeometry, 'interaction final state');
  });
  const browserView = evidence.browserView;
  check('BrowserView preserves host window origin and child input offset', () => {
    near(browserView.renderer.screenX, browserView.windowBounds.x, 'BrowserView window.screenX');
    near(browserView.renderer.screenY, browserView.windowBounds.y, 'BrowserView window.screenY');
    for (const [type, message] of [['pointermove', browserView.move],
      ['mousemove', browserView.move], ['wheel', browserView.wheel]]) {
      const event = [...browserView.renderer.events].reverse().find(candidate => candidate.type === type
        && Math.abs(candidate.clientX - browserView.cssTarget.x) <= 1.1
        && Math.abs(candidate.clientY - browserView.cssTarget.y) <= 1.1);
      absoluteEvent(event, nativeDipPoint(message, browserView.windowBounds),
        browserView.cssTarget, `BrowserView ${type}`);
    }
    assert.equal(browserView.rootRenderer.events.some(event =>
      ['pointermove', 'mousemove', 'wheel'].includes(event.type)
      && Math.abs(event.clientX - (browserView.bounds.x + browserView.cssTarget.x)) <= 1.1
      && Math.abs(event.clientY - (browserView.bounds.y + browserView.cssTarget.y)) <= 1.1), false,
    'BrowserView input must not leak to root WebContents');
  });


  evidence.assertions = { passed, failed: failures.length, failures };
  if (failures.length) {
    const summary = failures.map(failure => `${failure.label}: ${failure.message}`).join('\n');
    throw new Error(`${failures.length} pointer screen-coordinate assertion(s) failed after all samples were collected:\n${summary}`);
  }
}

function writeEvidence() {
  if (!resultPath) return;
  fs.mkdirSync(path.dirname(resultPath), { recursive: true });
  fs.writeFileSync(resultPath, JSON.stringify(evidence, null, 2));
}

async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  evidence.passed = !error;
  evidence.error = error ? String(error.stack || error) : null;
  try { writeEvidence(); } catch (writeError) { error = error || writeError; }
  if (view && target && !target.isDestroyed()) target.removeBrowserView(view);
  if (view && !view.webContents.isDestroyed()) view.webContents.destroy();
  if (target && !target.isDestroyed()) target.destroy();
  try { fs.rmSync(temporary, { recursive: true, force: true }); }
  catch (cleanupError) { error = error || cleanupError; }
  if (error) console.error(error.stack || error);
  else console.log(`PASS: trusted targeted Windows pointer/mouse/wheel screen coordinates and window origins (${evidence.assertions.passed} assertions)`);
  app.exit(error ? 1 : 0);
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32', 'Windows is required');
  const compiler = path.join(process.env.WINDIR || 'C:\\Windows', 'Microsoft.NET', 'Framework64', 'v4.0.30319', 'csc.exe');
  execFileSync(compiler, ['/nologo', '/platform:x64', '/target:exe', `/out:${helper}`,
    path.join(__dirname, 'windows-pointer-screen-coordinates-helper.cs')],
  { stdio: 'inherit', timeout: 30000, windowsHide: true });

  const displays = screen.getAllDisplays();
  evidence.displays = displays.map(displayRecord);
  const minX = Math.min(...displays.map(display => display.bounds.x));
  const minY = Math.min(...displays.map(display => display.bounds.y));
  const maxX = Math.max(...displays.map(display => display.bounds.x + display.bounds.width));
  const maxY = Math.max(...displays.map(display => display.bounds.y + display.bounds.height));
  const negative = { x: minX - 1800, y: minY - 1400, width: 900, height: 600 };

  target = new BrowserWindow({ ...negative, title, frame: false, show: false, focusable: false,
    skipTaskbar: true, minWidth: 0, minHeight: 0, backgroundColor: '#17324a',
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false,
      backgroundThrottling: false } });
  await target.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(page('Root coordinate probe')));
  target.showInactive();
  await pause(180);

  await collectMove('negative-offscreen', negative, 'offscreen');
  await collectPositionMove('set-position-only', { x: negative.x + 520, y: negative.y + 310 });
  await collectMove('positive-offscreen', { x: maxX + 1200, y: maxY + 900,
    width: 900, height: 600 }, 'offscreen');
  await collectMove('negative-resized', { x: negative.x + 260, y: negative.y + 140,
    width: 1180, height: 760 }, 'offscreen');
  await collectMove('size-restored', { x: negative.x + 340, y: negative.y + 180,
    width: negative.width, height: negative.height }, 'resize-restore');

  for (const [index, display] of displays.entries()) {
    const area = display.workArea;
    const width = Math.max(360, Math.min(760, area.width - 80));
    const height = Math.max(280, Math.min(540, area.height - 80));
    await collectMove(`display-${index}`, { x: area.x + 40, y: area.y + 40, width, height },
      'actual-display');
  }

  await collectInteraction();
  target.maximize();
  for (let attempts = 0; !target.isMaximized() && attempts < 100; attempts++) await pause(30);
  assert.equal(target.isMaximized(), true, 'owned test window did not maximize');
  await collectMove('maximized', null, 'owned-activated');
  target.unmaximize();
  for (let attempts = 0; target.isMaximized() && attempts < 100; attempts++) await pause(30);
  assert.equal(target.isMaximized(), false, 'owned test window did not restore');
  await collectMove('restored', null, 'owned-activated');
  await collectBrowserViewHost(screen.getPrimaryDisplay());
  await collectFramedHost(screen.getPrimaryDisplay());
  runAssertions();
}).then(() => finish(), finish);
