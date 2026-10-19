'use strict';
// Requires an unlocked Windows desktop and genuine D3D12/DirectComposition.
const assert = require('node:assert/strict');
const { app, BrowserWindow, screen } = require('electron');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { startNativeInput } = require('./windows-native-input-driver');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-windows-gpu-guests-'));
app.setPath('userData', path.join(temporary, 'profile'));
app.on('window-all-closed', () => {});
const originalGpu = process.env.MINI_ELECTRON_DISABLE_GPU;
const title = `mini-windows-gpu-guests-${process.pid}`;
const colors = { base: [32, 64, 96], first: [208, 32, 160], next: [16, 208, 224], blend: [136, 72, 64] };
const windows = new Set();
const evidence = [];
let driver, finished = false;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const data = html => 'data:text/html;charset=utf-8,' + encodeURIComponent(html);
const watchdog = setTimeout(() => finish(new Error('Windows GPU guest smoke timed out')), 180000);
function restoreGpu(value) {
  if (value === undefined) delete process.env.MINI_ELECTRON_DISABLE_GPU;
  else process.env.MINI_ELECTRON_DISABLE_GPU = value;
}
async function mode(cpu, operation) {
  const previous = process.env.MINI_ELECTRON_DISABLE_GPU;
  if (cpu) process.env.MINI_ELECTRON_DISABLE_GPU = '1';
  else delete process.env.MINI_ELECTRON_DISABLE_GPU;
  try { return await operation(); } finally { restoreGpu(previous); }
}
async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  restoreGpu(originalGpu);
  if (driver) { try { await driver.close(); } catch (failure) { error ||= failure; } }
  for (const window of windows) if (!window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error, JSON.stringify(evidence));
  else console.log('PASS: Windows real webview GPU/GPU, GPU/CPU, CPU/GPU and CPU/CPU native composition, alpha, captures, moves, and detach\n' + JSON.stringify(evidence));
  app.exit(error ? 1 : 0);
}
async function poll(read, accepts, label) {
  const until = Date.now() + 8000;
  let last;
  do {
    last = await read();
    if (accepts(last)) return last;
    await pause(30);
  } while (Date.now() < until);
  assert.fail(label + ': ' + JSON.stringify(last));
}
const close = (actual, expected) => actual.every((value, index) => Math.abs(value - expected[index]) <= 6);
async function nativeColor(window, point, expected, label) {
  const targetTitle = window.getTitle();
  return poll(async () => {
    const geometry = await driver.request('geometry', { title: targetTitle });
    const scale = geometry.dpi / 96;
    const result = await driver.request('sample', { title: targetTitle,
      x: geometry.clientX + Math.round(point.x * scale), y: geometry.clientY + Math.round(point.y * scale), width: 1, height: 1 });
    return result[0].rgb;
  }, value => close(value, expected), label);
}
async function captured(window, origin, solid, label) {
  const image = await window.webContents.capturePage();
  assert.equal(image.isEmpty(), false, label + ': actual root capture');
  const size = image.getSize();
  const bitmap = image.toBitmap();
  for (const [point, expected] of [[{ x: 24, y: 24 }, colors.base],
    [{ x: origin.x + 20, y: origin.y + 30 }, solid],
    [{ x: origin.x + 120, y: origin.y + 40 }, colors.blend]]) {
    const offset = (Math.floor(point.y * size.height / 400) * size.width + Math.floor(point.x * size.width / 600)) * 4;
    const actual = [bitmap[offset + 2], bitmap[offset + 1], bitmap[offset], bitmap[offset + 3]];
    assert.ok(close(actual, [...expected, 255]), label + ': ' + JSON.stringify({ point, actual, expected }));
  }
  return size;
}
async function concurrentCrops(window, origin, solid, label, sourceSize) {
  const cases = [
    { rect: { x: 12, y: 12, width: 24, height: 24 }, expected: colors.base },
    { rect: { x: origin.x + 8, y: origin.y + 20, width: 24, height: 24 }, expected: solid },
    { rect: { x: origin.x + 108, y: origin.y + 28, width: 24, height: 24 }, expected: colors.blend },
  ];
  const scaleX = sourceSize.width / 600, scaleY = sourceSize.height / 400;
  const physical = cases.map(({ rect, expected }) => ({
    expected, rect: { x: Math.round(rect.x * scaleX), y: Math.round(rect.y * scaleY),
      width: Math.round(rect.width * scaleX), height: Math.round(rect.height * scaleY) },
  }));
  const images = await Promise.all(physical.map(({ rect }) => window.webContents.capturePage(rect)));
  for (const [index, image] of images.entries()) {
    const { rect, expected } = physical[index];
    const size = image.getSize();
    assert.deepEqual(size, { width: rect.width, height: rect.height },
      label + ': each concurrent capture retains its requested physical crop');
    const bitmap = image.toBitmap();
    const offset = (Math.floor(size.height / 2) * size.width + Math.floor(size.width / 2)) * 4;
    const actual = [bitmap[offset + 2], bitmap[offset + 1], bitmap[offset], bitmap[offset + 3]];
    assert.ok(close(actual, [...expected, 255]),
      label + ': concurrent crop ' + JSON.stringify({ rect, actual, expected }));
  }
}
async function runCase(hostCpu, guestCpu, index) {
  const label = `${hostCpu ? 'CPU' : 'GPU'} host / ${guestCpu ? 'CPU' : 'GPU'} guest`;
  const area = screen.getPrimaryDisplay().workArea;
  let window, guest;
  try {
    await mode(hostCpu, async () => {
      window = new BrowserWindow({ title: `${title}-${index}`, frame: false, show: true, alwaysOnTop: true,
        x: area.x + 50, y: area.y + 50, width: 600, height: 400, minWidth: 0, minHeight: 0,
        backgroundColor: '#204060', webPreferences: { webviewTag: true, sandbox: true, contextIsolation: true, nodeIntegration: false } });
      windows.add(window);
      await window.loadURL(data(`<!doctype html><title>${title}-${index}</title><style>
        html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#204060}
        webview{position:absolute;display:block;border:0;padding:0;margin:0}</style>`));
    });
    await driver.request('focus', { title: window.getTitle() });
    await nativeColor(window, { x: 24, y: 24 }, colors.base, label + ' root');
    window.webContents.on('will-attach-webview', (_, preferences) => {
      preferences.transparent = true;
      preferences.sandbox = true;
      preferences.contextIsolation = true;
      preferences.nodeIntegration = false;
    });
    window.webContents.once('did-attach-webview', (_, attached) => { guest = attached; });
    const guestURL = data(`<!doctype html><style>
      html,body{margin:0;width:100%;height:100%;overflow:hidden;background:transparent}
      #solid{position:absolute;left:0;top:0;width:80px;height:100%;background:#d020a0}
      #alpha{position:absolute;left:100px;top:20px;width:60px;height:60px;background:rgba(240,80,32,.5)}
      </style><div id="solid"></div><div id="alpha"></div>`);
    await mode(guestCpu, async () => {
      await window.webContents.executeJavaScript(`(()=>{const view=document.createElement('webview');view.id='guest';
        view.style.cssText='left:48px;top:100px;width:180px;height:120px';
        view.setAttribute('src',${JSON.stringify(guestURL)});document.body.appendChild(view);return true})()`);
      await poll(() => !!guest, Boolean, label + ' public webview attachment');
    });
    assert.equal(guest.getType(), 'webview');
    assert.notEqual(guest.getProcessId(), window.webContents.getProcessId(), label + ': separate renderer sandbox');
    await nativeColor(window, { x: 68, y: 130 }, colors.first, label + ' real guest desktop pixels');
    await nativeColor(window, { x: 168, y: 140 }, colors.blend, label + ' real alpha composition');
    const initialHost = await window.webContents._getCompositorInfo();
    const initialGuest = await guest._getCompositorInfo();
    assert.equal(initialHost.backend, hostCpu ? 'software' : 'd3d12', label + ' root backend');
    assert.equal(initialGuest.backend, guestCpu ? 'software' : 'd3d12', label + ' guest backend');
    assert.equal(initialHost.presentation, 'directcomposition', label + ' actual native presenter, including renderer CPU fallback');
    assert.deepEqual(await guest.executeJavaScript('[typeof require,typeof process]'), ['undefined', 'undefined']);
    await guest.executeJavaScript("document.getElementById('solid').style.background='#10d0e0';true");
    await nativeColor(window, { x: 68, y: 130 }, colors.next, label + ' independent live update');
    const liveHost = await window.webContents._getCompositorInfo();
    const liveGuest = await guest._getCompositorInfo();
    assert.equal(liveHost.readbacks, initialHost.readbacks, label + ' ordinary host update must not read back');
    assert.equal(liveGuest.readbacks, initialGuest.readbacks, label + ' ordinary guest update must not read back');
    const captureSize = await captured(window, { x: 48, y: 100 }, colors.next, label + ' current capture');
    await concurrentCrops(window, { x: 48, y: 100 }, colors.next, label, captureSize);
    await nativeColor(window, { x: 168, y: 140 }, colors.blend,
      label + ' capture must not replace the ordinary root with an already-composited guest image');
    await window.webContents.executeJavaScript("document.getElementById('guest').style.cssText='left:270px;top:190px;width:200px;height:140px';true");
    await nativeColor(window, { x: 68, y: 130 }, colors.base, label + ' cleared old bounds');
    await nativeColor(window, { x: 290, y: 220 }, colors.next, label + ' moved source');
    await nativeColor(window, { x: 390, y: 230 }, colors.blend, label + ' moved alpha');
    await nativeColor(window, { x: 472, y: 220 }, colors.base, label + ' clipping');
    await captured(window, { x: 270, y: 190 }, colors.next, label + ' resized capture');
    await window.webContents.executeJavaScript("document.getElementById('guest').remove();true");
    await poll(() => guest.isDestroyed(), Boolean, label + ' guest teardown');
    await nativeColor(window, { x: 290, y: 220 }, colors.base, label + ' detached native layer');
    evidence.push({ label, initialHost, initialGuest, liveHost, liveGuest });
  } catch (error) {
    const diagnostic = { label };
    try {
      diagnostic.info = await window.webContents._getCompositorInfo();
      diagnostic.dom = await window.webContents.executeJavaScript(
        '[innerWidth,innerHeight,devicePixelRatio,getComputedStyle(document.body).backgroundColor]');
      console.error('Guest failure state:', JSON.stringify(diagnostic));
      const image = await Promise.race([window.webContents.capturePage(),
        pause(8000).then(() => { throw new Error('failure capture timed out'); })]);
      const size = image.getSize(), bitmap = image.toBitmap();
      const offset = (Math.floor(24 * size.height / 400) * size.width + Math.floor(24 * size.width / 600)) * 4;
      diagnostic.capturedRoot = [bitmap[offset + 2], bitmap[offset + 1], bitmap[offset], bitmap[offset + 3]];
      if (process.env.MINI_NATIVE_INPUT_ARTIFACTS)
        fs.writeFileSync(path.join(process.env.MINI_NATIVE_INPUT_ARTIFACTS, `guest-failure-${index}.png`), image.toPNG());
    } catch (failure) { diagnostic.error = String(failure); }
    evidence.push(diagnostic);
    throw error;
  } finally {
    if (window && !window.isDestroyed()) window.destroy();
    if (window) windows.delete(window);
  }
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32', 'Windows desktop required');
  driver = await startNativeInput(temporary, title);
  for (const [index, modes] of [[false, false], [false, true], [true, false], [true, true]].entries())
    await runCase(modes[0], modes[1], index);
}).then(() => finish(), finish);
