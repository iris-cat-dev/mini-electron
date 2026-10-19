'use strict';

// Run on an interactive Windows desktop. Every wheel action is native SendInput
// in physical screen pixels; this test does not synthesize DOM/webContents input
// and never changes the user's wheel settings.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserView, BrowserWindow } = require('electron');
const { startNativeInput } = require('./windows-native-input-driver');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-windows-wheel-'));
app.setPath('userData', path.join(temporary, 'user-data'));
const title = `mini-windows-wheel-${process.pid}`;
const artifacts = process.env.MINI_NATIVE_INPUT_ARTIFACTS;
const artifactStem = `windows-wheel-${process.pid}`;
const pause = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
const evidence = { title, settings: null, main: {}, browserView: {}, pageMode: null };
let window, view, driver, finished = false;
const watchdog = setTimeout(() => finish(new Error('Windows wheel smoke exceeded 60s')), 60000);
app.on('window-all-closed', () => {});

function page(titleText, targetId = 'target') {
  return `<!doctype html><meta charset="utf-8"><title>${titleText}</title><style>
    html,body{margin:0;min-height:1500px;background:#e8edf3;font:15px sans-serif}
    .pane{position:absolute;top:70px;width:310px;height:330px;overflow:auto;
      box-sizing:border-box;border:4px solid #334155;background:white;overscroll-behavior:contain}
    #${targetId}{left:28px;border-color:#2563eb}#sibling{left:370px;border-color:#64748b}
    .content{width:1900px;height:2100px;background:repeating-linear-gradient(0deg,#fff 0 38px,#dbeafe 38px 40px)}
    .label{position:sticky;left:0;top:0;width:180px;padding:8px;background:#0f172a;color:white}
  </style><div id="${targetId}" class="pane"><div class="content"><div class="label">wheel target</div></div></div>
  <div id="sibling" class="pane"><div class="content"><div class="label">sibling</div></div></div><script>
    const target=document.getElementById(${JSON.stringify(targetId)}),sibling=document.getElementById('sibling');
    let events=[];
    document.addEventListener('wheel',event=>events.push({target:event.target.closest('.pane')?.id||event.target.id,
      x:event.deltaX,y:event.deltaY,mode:event.deltaMode,shift:event.shiftKey}),{passive:true});
    window.resetWheelProbe=()=>{target.scrollTop=1200;target.scrollLeft=1200;sibling.scrollTop=1200;
      sibling.scrollLeft=1200;scrollTo(0,0);events=[];
      return new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))};
    window.wheelProbe=()=>{const rect=target.getBoundingClientRect();return{dpr:devicePixelRatio,
      point:{x:rect.x+rect.width/2,y:rect.y+rect.height/2},events:events.slice(),
      target:{top:target.scrollTop,left:target.scrollLeft},sibling:{top:sibling.scrollTop,left:sibling.scrollLeft},
      page:{x:scrollX,y:scrollY}}};
  </script>`;
}

async function evaluate(contents, source) {
  return contents.executeJavaScript(source);
}
async function state(contents = window.webContents) {
  return JSON.parse(await evaluate(contents, 'JSON.stringify(window.wheelProbe())'));
}
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
function physicalPoint(geometry, dpr, point, offset = { x: 0, y: 0 }) {
  return {
    x: Math.round(geometry.clientX + offset.x * dpr + point.x * dpr),
    y: Math.round(geometry.clientY + offset.y * dpr + point.y * dpr),
  };
}
function near(actual, expected, label, tolerance = Math.max(0.02, Math.abs(expected) * 0.025)) {
  assert.ok(Math.abs(actual - expected) <= tolerance,
    `${label}: expected ${expected} ± ${tolerance}, got ${actual}`);
}
function assertAxis(event, axis, raw, reference, label) {
  const property = axis === 'horizontal' ? 'x' : 'y';
  const other = axis === 'horizontal' ? 'y' : 'x';
  assert.equal(event.mode, reference.mode, `${label}: deltaMode`);
  near(event[property], reference[property] * raw / (axis === 'horizontal' ? 120 : -120), `${label}: proportional DOM delta`);
  near(event[other], 0, `${label}: perpendicular DOM delta`);
}

async function sampleMain(axis, raw, geometry) {
  await evaluate(window.webContents, 'resetWheelProbe()');
  await pause(80);
  const before = await state();
  const point = physicalPoint(geometry, before.dpr, before.point);
  await driver.request('wheel', { ...point, delta: raw, horizontal: axis === 'horizontal' });
  const delivered = await eventually(`${axis} ${raw} native wheel delivery`, state,
    value => value.events.length === 1);
  await pause(240);
  const after = await state();
  assert.equal(after.events.length, 1, `${axis} ${raw}: one native message must produce one DOM wheel event`);
  assert.equal(after.events[0].target, 'target', `${axis} ${raw}: nested pane hit target`);
  assert.deepEqual(after.sibling, before.sibling, `${axis} ${raw}: sibling pane must remain unchanged`);
  assert.deepEqual(after.page, before.page, `${axis} ${raw}: document must remain unchanged`);
  return { event: delivered.events[0], before, after };
}

async function exerciseAxis(axis, settings, geometry) {
  const notch = axis === 'horizontal' ? 120 : -120;
  const one = await sampleMain(axis, notch, geometry);
  const multi = await sampleMain(axis, notch * 2, geometry);
  const fractional = await sampleMain(axis, notch / 2, geometry);
  const reverse = await sampleMain(axis, -notch, geometry);
  const property = axis === 'horizontal' ? 'x' : 'y';
  const scrollProperty = axis === 'horizontal' ? 'left' : 'top';
  const expectedMode = axis === 'vertical' && settings.lines === settings.pageScroll ? 2 : 0;
  assert.equal(one.event.mode, expectedMode, `${axis}: native setting must select the correct DOM delta mode`);
  assert.ok(one.event[property] > 0, `${axis}: native forward notch must preserve the Win32 direction`);
  assert.ok(one.after.target[scrollProperty] > one.before.target[scrollProperty],
    `${axis}: the nested pane must visibly consume its native forward notch`);
  assertAxis(multi.event, axis, notch * 2, one.event, `${axis} two notches`);
  assertAxis(fractional.event, axis, notch / 2, one.event, `${axis} half notch`);
  assertAxis(reverse.event, axis, -notch, one.event, `${axis} reverse notch`);

  const amount = axis === 'horizontal' ? settings.chars : settings.lines;
  if (expectedMode === 0) {
    const expected = amount * (100 / 3) / one.before.dpr;
    near(one.event[property], expected, `${axis}: SPI-scaled one-notch DOM delta`, Math.max(0.08, expected * 0.04));
  }
  return {
    spiAmount: amount,
    dpr: one.before.dpr,
    one: one.event,
    multi: multi.event,
    fractional: fractional.event,
    reverse: reverse.event,
    oneScroll: { before: one.before.target, after: one.after.target },
  };
}

async function exerciseBrowserView(settings, geometry) {
  const bounds = { x: 700, y: 70, width: 270, height: 420 };
  view = new BrowserView({ webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  window.addBrowserView(view);
  view.setBounds(bounds);
  await view.webContents.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(page('wheel-browser-view', 'viewTarget')));
  await evaluate(view.webContents, 'resetWheelProbe()');
  const beforeView = await state(view.webContents);
  const beforeMain = await state();
  const point = physicalPoint(geometry, beforeView.dpr, beforeView.point, bounds);
  await driver.request('wheel', { ...point, delta: -120 });
  const vertical = await eventually('BrowserView vertical native wheel', () => state(view.webContents),
    value => value.events.length === 1 && value.target.top > beforeView.target.top);
  assert.equal(vertical.events[0].target, 'viewTarget');
  assert.equal((await state()).events.length, beforeMain.events.length,
    'BrowserView vertical wheel must not leak to the sibling main WebContents');

  await evaluate(view.webContents, 'resetWheelProbe()');
  await driver.request('wheel', { ...point, delta: 120, horizontal: true });
  const horizontal = await eventually('BrowserView horizontal native wheel', () => state(view.webContents),
    value => value.events.length === 1 && value.target.left > beforeView.target.left);
  assert.equal(horizontal.events[0].target, 'viewTarget');
  assert.equal((await state()).events.length, beforeMain.events.length,
    'BrowserView horizontal wheel must not leak to the sibling main WebContents');
  assert.equal(horizontal.events[0].mode, 0);
  if (settings.lines === settings.pageScroll) assert.equal(vertical.events[0].mode, 2);
  return { bounds, vertical: vertical.events[0], horizontal: horizontal.events[0] };
}

async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  evidence.passed = !error;
  evidence.error = error ? String(error.stack || error) : null;
  if (artifacts) {
    try {
      fs.mkdirSync(artifacts, { recursive: true });
      if (error && driver && window && !window.isDestroyed()) {
        await driver.request('capture', { path: path.join(artifacts, `${artifactStem}-failure.png`) });
      }
      fs.writeFileSync(path.join(artifacts, `${artifactStem}.json`), JSON.stringify(evidence, null, 2));
    } catch (artifactError) { error = error || artifactError; }
  }
  if (driver) {
    try { await driver.close(); } catch (cleanupError) { error = error || cleanupError; }
  }
  if (view && window && !window.isDestroyed()) window.removeBrowserView(view);
  if (view && !view.webContents.isDestroyed()) view.webContents.destroy();
  if (window && !window.isDestroyed()) window.destroy();
  try { fs.rmSync(temporary, { recursive: true, force: true }); }
  catch (cleanupError) { error = error || cleanupError; }
  if (error) console.error(error.stack || error);
  else console.log('PASS: Windows native proportional vertical/horizontal wheel input, nested targeting and BrowserView routing');
  app.exit(error ? 1 : 0);
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  assert.equal(typeof BrowserView, 'function', 'BrowserView is required for native wheel routing coverage');
  driver = await startNativeInput(temporary, title);
  window = new BrowserWindow({
    x: 90, y: 50, width: 1000, height: 600, title, frame: false,
    show: false, alwaysOnTop: true,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(page(title)));
  window.show();
  app.focus();
  window.focus();
  await driver.request('focus');
  await eventually('native wheel window focused', () => Promise.resolve(window.isFocused()), Boolean);

  const settings = await driver.request('wheelSettings');
  evidence.settings = settings;
  if (settings.lines === 0)
    throw new Error('INCOMPLETE: SPI_GETWHEELSCROLLLINES disables vertical scrolling, so proportional native output is not observable');
  if (settings.chars === 0)
    throw new Error('INCOMPLETE: SPI_GETWHEELSCROLLCHARS disables horizontal scrolling, so proportional native output is not observable');

  const geometry = await driver.request('geometry');
  evidence.main.vertical = await exerciseAxis('vertical', settings, geometry);
  evidence.main.horizontal = await exerciseAxis('horizontal', settings, geometry);
  if (settings.lines === settings.pageScroll) {
    evidence.pageMode = { executed: true, mode: evidence.main.vertical.one.mode,
      note: 'Current read-only SPI_GETWHEELSCROLLLINES value requested page scrolling.' };
    assert.equal(evidence.main.vertical.one.mode, 2, 'WHEEL_PAGESCROLL must map to DOM_DELTA_PAGE');
  } else {
    evidence.pageMode = { executed: false,
      reason: `Current SPI_GETWHEELSCROLLLINES is ${settings.lines}, not WHEEL_PAGESCROLL (${settings.pageScroll}); the smoke does not mutate user preferences.` };
    console.log(`INFO: page-mode native branch not executed: ${evidence.pageMode.reason}`);
  }
  evidence.browserView = await exerciseBrowserView(settings, geometry);
}).then(() => finish(), finish);
