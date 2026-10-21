'use strict';

// Interactive macOS + Xcode required. Run mini-electron --app=test/macos-idle-visibility-smoke.js.
// Reads real shared-frame generations with the existing native compositor helper;
// no source-text checks, synthetic visibility events, or protocol wiring mocks.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const { once } = require('node:events');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, BrowserView, screen } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-mac-idle-visibility-'));
app.setPath('userData', path.join(temporary, 'profile'));
const windows = new Set();
const views = new Set();
const mappings = new Map();
const evidence = { phases: [] };
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let native, finished = false;
const watchdog = setTimeout(() => finish(new Error('macOS visibility smoke exceeded 120s')), 120000);
app.on('window-all-closed', () => {});

async function bounded(operation, label, milliseconds = 8000) {
  let timer;
  try {
    return await Promise.race([operation, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(`${label}: timed out`)), milliseconds);
    })]);
  } finally { clearTimeout(timer); }
}
async function eventually(read, predicate, label) {
  const until = Date.now() + 8000;
  let value;
  do {
    value = await bounded(Promise.resolve().then(read), label);
    if (predicate(value)) return value;
    await pause(50);
  } while (Date.now() < until);
  assert.fail(`${label}: ${JSON.stringify(value)}`);
}
const evaluate = (contents, source) => bounded(contents.executeJavaScript(source), 'renderer evaluation');
const dataURL = html => 'data:text/html;charset=utf-8,' + encodeURIComponent(html);
function page() {
  return dataURL(`<!doctype html><style>
    html,body{margin:0;width:100%;height:100%;overflow:hidden;background:rgb(23,91,149)}
    button{position:absolute;left:10px;top:10px;width:100px;height:40px}
    #marker{position:absolute;left:60px;top:90px;width:24px;height:24px;background:white;
      animation:spin 1s linear infinite}@keyframes spin{to{transform:rotate(360deg)}}
    </style><button id="button">Interact</button><div id="marker"></div><script>
    window.framesSeen=0;window.clicksSeen=0;
    document.getElementById('button').onclick=()=>window.clicksSeen++;
    function tick(){window.framesSeen++;requestAnimationFrame(tick)};requestAnimationFrame(tick);
    </script>`);
}
async function identify(contents, before, label) {
  const added = await eventually(() => native.frames().filter(frame => !before.has(frame.fd)),
    frames => frames.length === 1 && frames[0].generation > 0 && frames[0].generation % 2 === 0,
    `${label}: one real published renderer mapping`);
  mappings.set(contents, added[0].fd);
  contents.on('render-process-gone', (_, details) => finish(new Error(`unexpected renderer exit: ${JSON.stringify(details)}`)));
}
function frame(contents) {
  const value = native.frames().find(item => item.fd === mappings.get(contents));
  assert.ok(value, 'owned renderer mapping must remain live');
  return value;
}
async function state(contents) {
  return { page: await evaluate(contents, '({hidden:document.hidden,frames:window.framesSeen,clicks:window.clicksSeen})'),
    frame: frame(contents) };
}
async function visible(contents, label) {
  await eventually(() => evaluate(contents, 'document.hidden'), hidden => hidden === false, `${label}: visible page`);
  const before = await state(contents);
  await eventually(() => state(contents), after => after.page.frames > before.page.frames + 2 &&
    after.frame.generation > before.frame.generation, `${label}: rAF and compositor frames resume`);
}
async function hidden(contents, label) {
  await eventually(() => evaluate(contents, 'document.hidden'), actual => actual === true, `${label}: hidden page`);
  // Drain already-submitted animation/IPC work before measuring strict stability.
  let previous = await state(contents), stable = 0;
  await eventually(async () => {
    const next = await state(contents);
    stable = next.page.frames === previous.page.frames && next.frame.generation === previous.frame.generation ? stable + 1 : 0;
    previous = next;
    return next;
  }, () => stable >= 4, `${label}: pending frames settle`);
  const before = previous;
  await pause(550);
  const after = await state(contents);
  assert.equal(after.page.frames, before.page.frames, `${label}: hidden rAF must suspend`);
  assert.equal(after.frame.generation, before.frame.generation, `${label}: CSS animation must not demand paints`);
  evidence.phases.push({ label, before, after });
}
async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  try {
    app.show();
    for (const view of views) if (!view.webContents.isDestroyed()) view.webContents.destroy();
    for (const window of windows) if (!window.isDestroyed()) window.destroy();
    if (native) await eventually(() => native.frames().filter(item => [...mappings.values()].includes(item.fd)),
      remaining => remaining.length === 0, 'normal shutdown releases owned renderer frames');
  } catch (cleanupError) { error ||= cleanupError; }
  evidence.passed = !error;
  if (error) evidence.error = error.stack || String(error);
  if (process.env.MINI_IDLE_VISIBILITY_ARTIFACTS) {
    fs.mkdirSync(process.env.MINI_IDLE_VISIBILITY_ARTIFACTS, { recursive: true });
    fs.writeFileSync(path.join(process.env.MINI_IDLE_VISIBILITY_ARTIFACTS, `macos-idle-visibility-${process.pid}.json`), JSON.stringify(evidence, null, 2));
  }
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: macOS native visibility pauses page/widget/Viz work, preserves readiness/preparation, restores interaction, propagates guests/BrowserViews and honors throttling opt-out\n' + JSON.stringify(evidence));
  app.exit(error ? 1 : 0);
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin', 'PREREQUISITE: macOS required');
  const root = path.resolve(__dirname, '..');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'gpu-compositing.node');
  const includes = ['', 'gen', 'third_party/abseil-cpp', 'base/allocator/partition_allocator/src',
    'gen/base/allocator/partition_allocator/src', 'third_party/libnode/src'];
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup', '-isysroot', sdk,
    ...includes.map(include => `-I${path.join(root, include)}`), '-framework', 'AppKit', '-framework', 'CoreGraphics',
    '-framework', 'ScreenCaptureKit', '-framework', 'ImageIO',
    path.join(__dirname, 'macos-owned-window-capture.mm'), path.join(__dirname, 'macos-gpu-compositing-helper.mm'), '-o', helper],
  { stdio: 'inherit', timeout: 30000 });
  native = require(helper);
  const area = screen.getPrimaryDisplay().workArea;
  assert.ok(area.width >= 700 && area.height >= 540, 'PREREQUISITE: display must fit owned windows');
  const beforeRoot = new Set(native.frames().map(item => item.fd));
  const window = new BrowserWindow({ x: area.x + 30, y: area.y + 30, width: 640, height: 480,
    show: false, title: `mini-mac-idle-${process.pid}`, webPreferences: { webviewTag: true } });
  windows.add(window);
  const ready = once(window, 'ready-to-show');
  await window.loadURL(page());
  await bounded(ready, 'initial show:false ready-to-show');
  assert.equal(window.isVisible(), false, 'startup paint must not reveal the native window');
  await identify(window.webContents, beforeRoot, 'root');
  await hidden(window.webContents, 'initial hidden root');
  await bounded(window.webContents.prepareToShow(), 'explicit hidden native frame preparation');
  assert.equal(window.isVisible(), false, 'preparation must not reveal the native window');
  await hidden(window.webContents, 'preparation releases its temporary visibility lease');

  const beforeView = new Set(native.frames().map(item => item.fd));
  const view = new BrowserView();
  views.add(view);
  view.setBounds({ x: 340, y: 100, width: 180, height: 150 });
  window.addBrowserView(view);
  await view.webContents.loadURL(page());
  await identify(view.webContents, beforeView, 'BrowserView');
  await hidden(view.webContents, 'initial hidden BrowserView');
  window.show();
  await visible(window.webContents, 'shown root');
  await visible(view.webContents, 'shown BrowserView');

  const beforeGuest = new Set(native.frames().map(item => item.fd));
  const attached = once(window.webContents, 'did-attach-webview');
  await evaluate(window.webContents, `(() => {const guest=document.createElement('webview');guest.id='guest';
    guest.style.cssText='position:absolute;left:140px;top:260px;width:160px;height:100px';
    guest.setAttribute('src',${JSON.stringify(page())});document.body.appendChild(guest);return true})()`);
  const [, guest] = await bounded(attached, 'real webview attachment');
  await identify(guest, beforeGuest, 'embedded guest');
  const guestLoaded = await eventually(() => evaluate(guest,
    '({url:location.href.slice(0,160),readyState:document.readyState,hidden:document.hidden,framesType:typeof window.framesSeen})'),
  status => status.framesType === 'number' && status.readyState === 'complete', 'guest page loads');
  evidence.phases.push({ label: 'embedded guest loaded', page: guestLoaded });
  await visible(guest, 'shown guest');
  const all = [['root', window.webContents], ['BrowserView', view.webContents], ['guest', guest]];
  for (const [transition, hide, show] of [
    ['hidden', () => window.hide(), () => window.show()],
    ['minimized', () => window.minimize(), () => window.restore()],
    ['application hidden', () => app.hide(), () => app.show()],
  ]) {
    hide();
    for (const [name, contents] of all) await hidden(contents, `${transition} ${name}`);
    show();
    for (const [name, contents] of all) await visible(contents, `${transition} restored ${name}`);
  }

  const bounds = window.getBounds();
  const nativeExposure = native.windowState(`mini-mac-idle-${process.pid}`);
  assert.equal(nativeExposure.nativeVisible, 1, 'the owned occlusion target must be ordered onscreen');
  const cover = new BrowserWindow({ x: bounds.x - 10, y: bounds.y - 10, width: bounds.width + 20,
    height: bounds.height + 20, show: true, alwaysOnTop: true, title: `mini-mac-idle-cover-${process.pid}` });
  windows.add(cover);
  await cover.loadURL('data:text/html,<body style="background:black">occlusion cover');
  cover.show();
  const occlusionDeadline = Date.now() + 2000;
  let coveredNative;
  do {
    coveredNative = native.windowState(`mini-mac-idle-${process.pid}`);
    if (!coveredNative.exposed) break;
    await pause(50);
  } while (Date.now() < occlusionDeadline);
  if (nativeExposure.exposed && !coveredNative.exposed) {
    for (const [name, contents] of all) await hidden(contents, `fully occluded ${name}`);
    evidence.phases.push({ label: 'native occlusion', status: 'passed', nativeExposure, coveredNative });
  } else {
    evidence.phases.push({ label: 'native occlusion', status: 'unavailable',
      reason: 'AppKit did not report an exposed-to-occluded transition for the owned target',
      nativeExposure, coveredNative });
  }
  cover.destroy();
  for (const [name, contents] of all) await visible(contents, `unoccluded ${name}`);

  window.webContents.sendInputEvent({ type: 'mouseDown', x: 30, y: 30, button: 'left', clickCount: 1 });
  window.webContents.sendInputEvent({ type: 'mouseUp', x: 30, y: 30, button: 'left', clickCount: 1 });
  await eventually(() => evaluate(window.webContents, 'window.clicksSeen'), clicks => clicks === 1, 'restored renderer interaction');
  const initialPixel = [...(await window.webContents.capturePage()).toBitmap().subarray(0, 4)];
  window.hide();
  await hidden(window.webContents, 'before hidden mutation');
  await evaluate(window.webContents, 'document.documentElement.style.background="rgb(91,37,153)";document.body.style.background="rgb(91,37,153)";true');
  window.show();
  await visible(window.webContents, 'hidden mutation restored');
  await eventually(async () => [...(await window.webContents.capturePage()).toBitmap().subarray(0, 4)],
    pixel => pixel.length === 4 && pixel.some((channel, index) => channel !== initialPixel[index]),
    'restored publication contains changed pixels without assuming capture channel order');

  window.removeBrowserView(view);
  await hidden(view.webContents, 'detached BrowserView');
  window.addBrowserView(view);
  await visible(view.webContents, 'reattached BrowserView');
  window.hide();
  await hidden(window.webContents, 'before runtime throttling opt-out');
  assert.equal(window.webContents.setBackgroundThrottling(false), true, 'existing setter must be supported');
  await visible(window.webContents, 'runtime throttling disabled while physically hidden');
  assert.equal(window.isVisible(), false, 'throttling override cannot reveal native window');
  await hidden(guest, 'default guest remains physically hidden despite host override');
  assert.equal(window.webContents.setBackgroundThrottling(true), true);
  await hidden(window.webContents, 'runtime throttling re-enabled');

  const beforeUnthrottled = new Set(native.frames().map(item => item.fd));
  const unthrottled = new BrowserWindow({ width: 300, height: 200, show: false,
    webPreferences: { backgroundThrottling: false } });
  windows.add(unthrottled);
  await unthrottled.loadURL(page());
  await identify(unthrottled.webContents, beforeUnthrottled, 'constructor opt-out');
  assert.equal(unthrottled.webContents.getWebPreferences().backgroundThrottling, false);
  await visible(unthrottled.webContents, 'constructor throttling disabled while hidden');
  assert.equal(unthrottled.isVisible(), false);
  unthrottled.webContents.setBackgroundThrottling(true);
  await hidden(unthrottled.webContents, 'constructor opt-out switched back to default');
}).then(() => finish(), finish);
