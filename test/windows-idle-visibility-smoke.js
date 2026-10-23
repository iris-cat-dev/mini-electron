'use strict';

// Requires an interactive Windows desktop and the native compositor diagnostics.
// out/windows-x64/mini-electron.exe --app=test/windows-idle-visibility-smoke.js
const assert = require('node:assert/strict');
const { once } = require('node:events');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, BrowserView } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-idle-visibility-'));
app.setPath('userData', path.join(temporary, 'profile'));
const artifacts = process.env.MINI_NATIVE_INPUT_ARTIFACTS;
const evidence = { phases: [] };
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let window, view, finished = false;
const deadline = setTimeout(() => finish(new Error('Idle visibility regression timed out')), 90000);
app.on('window-all-closed', () => {});

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  evidence.passed = !error;
  if (error) evidence.error = error.stack || String(error);
  if (artifacts) {
    fs.mkdirSync(artifacts, { recursive: true });
    fs.writeFileSync(path.join(artifacts, `idle-visibility-${process.pid}.json`), JSON.stringify(evidence, null, 2));
  }
  if (view && !view.webContents.isDestroyed()) view.webContents.destroy();
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: static root idles; hidden/minimized renderers pause and resume; startup painting and restored pixels remain correct\n' + JSON.stringify(evidence));
  app.exit(error ? 1 : 0);
}

async function bounded(promise, label, ms = 8000) {
  let timeout;
  try {
    return await Promise.race([promise, new Promise((_, reject) => {
      timeout = setTimeout(() => reject(new Error(`${label} timed out`)), ms);
    })]);
  } finally { clearTimeout(timeout); }
}

async function eventually(read, predicate, label) {
  const until = Date.now() + 6000;
  let value;
  do {
    value = await read();
    if (predicate(value)) return value;
    await pause(40);
  } while (Date.now() < until);
  assert.fail(`${label}: ${JSON.stringify(value)}`);
}

const evaluate = (contents, source) => contents.executeJavaScript(source);
async function visibility(contents, hidden, label) {
  await eventually(() => evaluate(contents, 'document.hidden'), actual => actual === hidden, label);
}

async function idle(contents, label, monitorIdle = true) {
  let previous = await contents._getCompositorInfo();
  let stable = 0;
  await eventually(async () => {
    const next = await contents._getCompositorInfo();
    stable = next.frames === previous.frames && next.importedFrames === previous.importedFrames ? stable + 1 : 0;
    previous = next;
    return next;
  }, () => stable >= 5, `${label} did not settle`);
  const before = previous;
  await pause(650);
  const after = await contents._getCompositorInfo();
  assert.equal(after.frames, before.frames, `${label}: idle renderer must not generate new frames`);
  assert.equal(after.importedFrames, before.importedFrames, `${label}: idle browser must not import duplicate frames`);
  if (monitorIdle)
    assert.equal(after.vsync.active, false, `${label}: idle monitor must release its high-rate clock`);
  evidence.phases.push({ label, before, after });
}

async function animating(contents, label) {
  const before = await contents._getCompositorInfo();
  await pause(400);
  const after = await contents._getCompositorInfo();
  assert.ok(after.frames >= before.frames + 2, `${label}: visible animation must produce subsequent frames`);
  evidence.phases.push({ label, before, after });
}

function page(color) {
  return 'data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><style>
    html,body{margin:0;width:100%;height:100%;background:${color};overflow:hidden}
    #marker{position:absolute;left:60px;top:60px;width:30px;height:30px;background:white}
    .animate{animation:move 1s linear infinite alternate}@keyframes move{to{transform:translateX(80px)}}
    </style><div id="marker"></div>`);
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32', 'PREREQUISITE: Windows required');
  window = new BrowserWindow({ width: 640, height: 480, show: false, title: `mini-idle-visibility-${process.pid}` });
  view = new BrowserView();
  for (const contents of [window.webContents, view.webContents]) {
    contents.on('render-process-gone', (_, details) => finish(new Error(`unexpected renderer exit: ${JSON.stringify(details)}`)));
  }
  const initialFrame = once(window, 'ready-to-show');
  await window.loadURL(page('rgb(23,91,149)'));
  await bounded(initialFrame, 'initial hidden painting');
  assert.equal(window.isVisible(), false, 'initial painting must not reveal the native window');
  await idle(view.webContents, 'unattached blank startup');
  view.setBounds({ x: 300, y: 100, width: 180, height: 130 });
  window.addBrowserView(view);
  await view.webContents.loadURL(page('rgb(30,130,70)'));
  window.showInactive();
  for (const [name, contents] of [['root', window.webContents], ['view', view.webContents]]) {
    await visibility(contents, false, `${name} initially visible`);
    await idle(contents, `${name} static-visible`);
  }
  for (const contents of [window.webContents, view.webContents]) {
    await evaluate(contents, 'document.getElementById("marker").className="animate";true');
  }
  await animating(window.webContents, 'root visible animation');
  await animating(view.webContents, 'BrowserView visible animation');
  window.minimize();
  for (const [name, contents] of [['root', window.webContents], ['view', view.webContents]]) {
    await visibility(contents, true, `${name} minimized`);
    await idle(contents, `${name} animated-minimized`);
  }
  window.restore();
  for (const [name, contents] of [['root', window.webContents], ['view', view.webContents]]) {
    await visibility(contents, false, `${name} restored`);
    await animating(contents, `${name} restored animation`);
  }
  window.hide();
  for (const [name, contents] of [['root', window.webContents], ['view', view.webContents]]) {
    await visibility(contents, true, `${name} hidden`);
    await idle(contents, `${name} animated-hidden`);
  }
  await evaluate(window.webContents, 'document.documentElement.style.background="rgb(91,37,153)";document.body.style.background="rgb(91,37,153)";true');
  window.showInactive();
  await visibility(window.webContents, false, 'root shown again');
  await visibility(view.webContents, false, 'view shown again');
  await eventually(async () => [...(await window.webContents.capturePage()).toBitmap().subarray(0, 4)],
    pixel => pixel.every((channel, index) => channel === [153, 37, 91, 255][index]),
    'restoring a hidden window must publish changed document pixels');
  await animating(window.webContents, 'root shown-again animation');
  window.removeBrowserView(view);
  await visibility(view.webContents, true, 'detached BrowserView hidden');
  // The root still animates; its monitor-wide clock must remain available.
  await idle(view.webContents, 'detached BrowserView', false);
  await animating(window.webContents, 'root animation with detached BrowserView');
  window.addBrowserView(view);
  await visibility(view.webContents, false, 'reattached BrowserView visible');
  await animating(view.webContents, 'reattached BrowserView animation');
  for (const contents of [window.webContents, view.webContents]) {
    await evaluate(contents, 'document.getAnimations().forEach(animation=>animation.pause());true');
  }
  await idle(window.webContents, 'root paused-visible');
  await idle(view.webContents, 'BrowserView paused-visible');
}).then(() => finish(), finish);
