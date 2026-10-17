'use strict';

// Invoke: /path/to/mini-electron --app=test/macos-initial-background-smoke.js
// Requires macOS graphical session, Screen Recording permission for that binary,
// Xcode command-line tools and this checkout's generated headers. Keep the test
// windows unobstructed: these are composed DISPLAY pixels, not isolated windows.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, screen } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-initial-background-'));
const windows = [];
const sockets = new Set();
const pending = new Map();
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const dark = [24, 24, 24, 255];
const changed = [64, 64, 64, 255];
const white = [255, 255, 255, 255];
// Neutral colors isolate background presentation from existing chromatic
// renderer/display ICC conversion; the actual composed pixels still must match.
const underlayColor = [160, 160, 160, 255];
const pageColor = [224, 224, 224, 255];
let native, server, origin, finished = false;
let serial = 0;
app.on('window-all-closed', () => {});
const watchdog = setTimeout(() => finish(new Error('Initial background smoke exceeded 90s watchdog')), 90000);
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  for (const response of pending.values()) response.destroy();
  for (const socket of sockets) socket.destroy();
  if (server) server.close();
  for (const window of windows.reverse()) if (!window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: actual composed display first-frame, pending navigation, native titlebar, default white and transparent pet backgrounds');
  app.exit(error ? 1 : 0);
}
async function poll(label, predicate, bound = 5000) {
  const start = Date.now();
  let last;
  do {
    last = await predicate();
    if (last === true) return;
    await pause(16);
  } while (Date.now() - start < bound);
  throw new Error(`${label} did not become ready within ${bound}ms: ${JSON.stringify(last)}`);
}
function hasColor(snapshot, color) {
  return Object.values(snapshot.samples).every(pixel =>
    pixel.length === 4 && pixel.every((value, channel) => Math.abs(value - color[channel]) <= 4));
}
async function capture(window, label) {
  const result = await native.snapshot(window.getTitle());
  console.log(JSON.stringify({ label, ...result }));
  assert.equal(result.visible, true, `${label}: native window must be visible`);
  assert.equal(result.onScreen, true, `${label}: WindowServer must include the window`);
  assert.equal(result.occlusionVisible, true,
    `${label}: capture fixture was occluded; leave the desktop untouched and rerun in an unobstructed graphical session`);
  return result;
}
async function exact(window, color, label) {
  const result = await capture(window, label);
  assert.ok(hasColor(result, color), `${label}: FIRST successful display capture must already have requested color; no retry of wrong pixels: ${JSON.stringify(result)}`);
  return result;
}
async function repeated(window, color, label) {
  for (let turn = 0; turn < 6; ++turn) {
    await pause(16);
    await exact(window, color, `${label}:turn-${turn}`);
  }
}
async function present(window) {
  // Poll only WindowServer presence, never discard a successfully captured
  // wrong initial color while waiting for the requested color to appear.
  await poll('native visible/on-screen', () => {
    const state = native.inspect(window.getTitle());
    return (state.visible && state.onScreen) || state;
  });
}
function create(options) {
  const area = screen.getPrimaryDisplay().workArea;
  assert.ok(area.width >= 640 && area.height >= 520, 'An unobstructed display work area of at least 640x520 is required');
  const title = `mini-initial-background-${process.pid}-${++serial}`;
  const window = new BrowserWindow({ x: area.x + 60, y: area.y + 60, width: 480, height: 360,
    title, show: true, alwaysOnTop: true,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false }, ...options });
  windows.push(window);
  return window;
}
async function heldNavigation(window, initial, background, label) {
  const route = `/held-${++serial}`;
  const loading = window.loadURL(origin + route);
  // Attach rejection immediately, including failures before the held response.
  let loadError;
  const loaded = Promise.resolve(loading).catch(error => { loadError = error; });
  await poll(`${label}: HTTP request observed`, () => {
    if (loadError) throw loadError;
    return pending.has(route);
  });
  assert.equal(pending.get(route).writableEnded, false, 'Document response must remain pending during initial sampling');
  await exact(window, initial, `${label}:request-observed-response-pending`);
  await repeated(window, initial, `${label}:held-document`);
  const response = pending.get(route);
  pending.delete(route);
  response.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
  response.end(`<!doctype html><title>${window.getTitle()}</title><style>html,body{margin:0;width:100%;height:100%;background:${background};overflow:hidden}</style>`);
  await loaded;
  if (loadError) throw loadError;
  // Polling color is allowed only for a released real-document transition.
  const target = background === 'transparent' ? initial : pageColor;
  await poll(`${label}: real page composed pixels`, async () => {
    const result = await capture(window, `${label}:released-document`);
    return hasColor(result, target) || result;
  });
  await repeated(window, target, `${label}:document-presented`);
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin', 'This acceptance test requires macOS');
  const root = path.resolve(__dirname, '..');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'initial-background.node');
  const includes = ['', 'gen', 'third_party/abseil-cpp', 'base/allocator/partition_allocator/src', 'gen/base/allocator/partition_allocator/src', 'third_party/libnode/src'];
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup', '-isysroot', sdk,
    ...includes.map(include => `-I${path.join(root, include)}`), '-framework', 'AppKit', '-framework', 'CoreGraphics',
    '-framework', 'ScreenCaptureKit', path.join(__dirname, 'macos-initial-background-helper.mm'), '-o', helper],
  { stdio: 'inherit', timeout: 30000 });
  native = require(helper);
  server = http.createServer((request, response) => {
    if (!request.url.startsWith('/held-')) { response.writeHead(404); response.end(); return; }
    pending.set(request.url, response);
  });
  server.on('connection', socket => {
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  origin = `http://127.0.0.1:${server.address().port}`;

  const opaque = create({ backgroundColor: '#181818' });
  await present(opaque);
  const first = await exact(opaque, dark, 'opaque:no-navigation:first');
  assert.equal(first.opaque, true);
  assert.equal(first.titled, true, 'Default native titlebar must remain intact');
  assert.equal(first.closeButtonVisible, true, 'Default native titlebar controls must remain intact');
  assert.ok(first.backgroundRGBA.every((channel, index) => Math.abs(channel - dark[index]) <= 1));
  await repeated(opaque, dark, 'opaque:no-navigation');
  await heldNavigation(opaque, dark, '#e0e0e0', 'opaque');
  opaque.destroy();

  const mutable = create({ backgroundColor: '#181818' });
  await present(mutable);
  await exact(mutable, dark, 'mutable:initial');
  mutable.setBackgroundColor('#404040');
  // Setter IPC may present on a later turn; unlike constructor acceptance,
  // this is an explicit transition from an already verified initial frame.
  await poll('mutable:set-before-navigation transition', async () => {
    const result = await capture(mutable, 'mutable:set-before-navigation:transition');
    return hasColor(result, changed) || result;
  });
  const update = await exact(mutable, changed, 'mutable:set-before-navigation:settled');
  assert.ok(update.backgroundRGBA.every((channel, index) => Math.abs(channel - changed[index]) <= 1));
  await repeated(mutable, changed, 'mutable:no-navigation');
  await heldNavigation(mutable, changed, '#e0e0e0', 'mutable');
  mutable.destroy();

  const defaultWindow = create({});
  await present(defaultWindow);
  const defaultSnapshot = await exact(defaultWindow, white, 'default-white:no-navigation:first');
  assert.equal(defaultSnapshot.opaque, true);
  await repeated(defaultWindow, white, 'default-white:no-navigation');
  defaultWindow.destroy();

  const underlay = create({ backgroundColor: '#a0a0a0', frame: false, width: 600, height: 460, alwaysOnTop: false });
  await present(underlay);
  await exact(underlay, underlayColor, 'underlay:first');
  // Load a real solid underlay before the pet exists, so transparent renderer
  // samples cannot be mistaken for an isolated window's alpha channel.
  await underlay.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><title>${underlay.getTitle()}</title><style>html,body{margin:0;width:100%;height:100%;background:#a0a0a0}</style>`));
  await poll('solid underlay real document', async () => {
    const result = await capture(underlay, 'underlay:document');
    return hasColor(result, underlayColor) || result;
  });
  const pet = create({ transparent: true, backgroundColor: '#181818', frame: false, focusable: false, alwaysOnTop: true });
  await present(pet);
  const petFirst = await exact(pet, underlayColor, 'pet:no-navigation:first-composed');
  assert.equal(petFirst.opaque, false);
  assert.equal(petFirst.titled, false);
  assert.equal(petFirst.canBecomeKey, false);
  assert.ok(petFirst.level > native.inspect(underlay.getTitle()).level, 'Pet must really be above the contrasting underlay');
  assert.equal(petFirst.backgroundRGBA[3], 0);
  await repeated(pet, underlayColor, 'pet:no-navigation');
  await heldNavigation(pet, underlayColor, 'transparent', 'pet');
  finish();
}).catch(finish);
