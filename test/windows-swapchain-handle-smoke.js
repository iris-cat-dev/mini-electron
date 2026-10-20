'use strict';

// Requires an unlocked Windows desktop and the native Windows runtime.
// Exercises real DirectComposition presents and rapid native sizing while
// measuring the browser process's kernel handle count.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, screen } = require('electron');
const { startNativeInput } = require('./windows-native-input-driver');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-swapchain-handles-'));
app.setPath('userData', path.join(temporary, 'user-data'));
const title = `mini-swapchain-handles-${process.pid}`;
const artifacts = process.env.MINI_NATIVE_INPUT_ARTIFACTS;
const pause = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
const limits = { maximumActiveGrowth: 96, maximumClosedGrowth: 64, framesPerBatch: 120 };
const evidence = { limits, batches: [] };
let window, driver, finished = false, closingWindow = false;
const watchdog = setTimeout(() => finish(new Error('Swap-chain handle smoke exceeded 75s')), 75000);
app.on('window-all-closed', () => {});

const native = (action, values = {}) => driver.request(action, { title, ...values });
const evaluate = source => window.webContents.executeJavaScript(source);

async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (error && window && !window.isDestroyed()) {
    try { evidence.failureInfo = await window.webContents._getCompositorInfo(); }
    catch (failure) { evidence.failureInfoError = String(failure); }
  }
  try { if (window && !window.isDestroyed()) window.destroy(); }
  catch (failure) { error ||= failure; }
  try { if (driver) await driver.close(); }
  catch (failure) { error ||= failure; }
  fs.rmSync(temporary, { recursive: true, force: true });
  evidence.error = error ? String(error.stack || error) : null;
  if (artifacts) {
    fs.mkdirSync(artifacts, { recursive: true });
    fs.writeFileSync(path.join(artifacts, `windows-swapchain-handles-${process.pid}.json`),
      JSON.stringify(evidence, null, 2));
  }
  if (error) console.error(error.stack || error, JSON.stringify(evidence));
  else console.log('PASS: animated DirectComposition presents and canceled resize swap chains keep browser handles bounded and release window handles\n' + JSON.stringify(evidence));
  app.exit(error ? 1 : 0);
}

async function eventually(read, predicate, label, timeout = 8000) {
  const deadline = Date.now() + timeout;
  let value;
  do {
    value = await read();
    if (predicate(value)) return value;
    await pause(30);
  } while (Date.now() < deadline);
  assert.fail(`${label}: ${JSON.stringify(value)}`);
}

async function processHandleCount() {
  const count = await native('processHandleCount', { processId: process.pid });
  assert.ok(Number.isSafeInteger(count) && count > 0, `invalid process handle count: ${count}`);
  return count;
}

async function settledMinimumHandleCount(samples = 16) {
  const counts = [];
  for (let index = 0; index < samples; index++) {
    counts.push(await processHandleCount());
    await pause(75);
  }
  return { minimum: Math.min(...counts), maximum: Math.max(...counts), counts };
}

function assertNativeCompositor(info, label) {
  assert.equal(info.backend, 'd3d12', `${label}: genuine D3D12 rendering is required`);
  assert.equal(info.presentation, 'directcomposition', `${label}: native DirectComposition presentation is required`);
  assert.equal(info.readbacks, 0, `${label}: ordinary presentation must not use readback`);
  assert.ok(Number.isSafeInteger(info.frames) && info.frames >= 0, `${label}: valid renderer frame count`);
  assert.ok(Number.isSafeInteger(info.importedFrames) && info.importedFrames >= 0,
    `${label}: valid imported frame count`);
}

async function expectPixel(rgb, label) {
  const geometry = await native('geometry');
  const scale = geometry.dpi / 96;
  const x = geometry.clientX + Math.round(60 * scale);
  const y = geometry.clientY + Math.round(120 * scale);
  return eventually(async () => (await native('sample', { x, y, width: 1, height: 1 }))[0].rgb,
    actual => actual.every((channel, index) => Math.abs(channel - rgb[index]) <= 4), label);
}

async function warmDirectComposition() {
  const warmup = new BrowserWindow({ width: 420, height: 320, show: false,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await warmup.loadURL('data:text/html,' + encodeURIComponent(
    '<body style="margin:0;background:#203858"><div id="p" style="width:20px;height:20px;background:#ef7044"></div><script>' +
    'const p=document.querySelector("#p");let n=0;function tick(){p.style.transform=`translateX(${++n % 100}px)`;requestAnimationFrame(tick)}tick()</script>'));
  warmup.show();
  const info = await eventually(() => warmup.webContents._getCompositorInfo(),
    value => value.presentation === 'directcomposition' && value.frames > 2, 'warm-up composition did not present');
  assertNativeCompositor(info, 'warm-up');
  warmup.destroy();
  await pause(300);
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32', 'PREREQUISITE: Windows interactive desktop required');
  const area = screen.getPrimaryDisplay().workArea;
  const width = Math.min(1200, area.width - 100);
  const height = Math.min(800, area.height - 100);
  assert.ok(width >= 800 && height >= 600, 'PREREQUISITE: at least 900x700 DIP work area required');
  driver = await startNativeInput(temporary, title);

  await warmDirectComposition();
  evidence.closedBaseline = await settledMinimumHandleCount();

  window = new BrowserWindow({
    title,
    x: area.x + Math.round((area.width - width) / 2),
    y: area.y + Math.round((area.height - height) / 2),
    width,
    height,
    minWidth: 0,
    minHeight: 0,
    frame: false,
    show: false,
    backgroundColor: '#203858',
    titleBarStyle: 'hidden',
    titleBarOverlay: { color: '#16283e', symbolColor: '#ffffff', height: 40 },
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  window.webContents.on('render-process-gone', (_, details) => {
    if (!closingWindow) finish(new Error(`renderer exited: ${JSON.stringify(details)}`));
  });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><title>${title}</title>
    <style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:rgb(32,56,88)}
    #motion{position:fixed;left:260px;top:100px;width:180px;height:180px;background:#ef7044;border-radius:24px}</style>
    <div id="motion"></div><script>
    const motion=document.querySelector('#motion');window.fixture={frames:0};
    function tick(){fixture.frames++;motion.style.transform='translateX('+(fixture.frames%240)+'px) rotate('+(fixture.frames%360)+'deg)';requestAnimationFrame(tick)}
    requestAnimationFrame(tick);
    window.setBackground=(red,green,blue)=>{document.documentElement.style.backgroundColor='rgb('+red+','+green+','+blue+')';document.body.style.backgroundColor='rgb('+red+','+green+','+blue+')';return true};
    </script>`));
  window.show();
  window.focus();
  await native('focus');
  await expectPixel([32, 56, 88], 'initial page pixels did not reach the desktop');

  const initial = await eventually(() => window.webContents._getCompositorInfo(), info => info.frames >= 60,
    'animated renderer did not publish initial frames');
  assertNativeCompositor(initial, 'initial animation');
  evidence.initial = { info: initial, handles: await processHandleCount() };

  const colors = [[36, 116, 196], [44, 156, 92], [164, 52, 116]];
  let previousInfo = initial;
  for (let batch = 0; batch < colors.length; batch++) {
    const rgb = colors[batch];
    const resize = await native('resizeCycles', {
      cycles: 12,
      distance: 520,
      steps: 24,
      interval: 1,
      samples: [
        { x: 60, y: 120, rgb: batch === 0 ? [32, 56, 88] : colors[batch - 1] },
        { x: 60, y: 24, bottom: true, rgb: batch === 0 ? [32, 56, 88] : colors[batch - 1] },
      ],
    });
    assert.equal(resize.resizing.entered, 12, `batch ${batch + 1}: all native sizing loops must run`);
    assert.equal(resize.presentation.whiteFrames, 0, `batch ${batch + 1}: resizing exposed white pixels`);
    assert.equal(resize.presentation.missingFrames, 0, `batch ${batch + 1}: resizing lost retained pixels`);

    await evaluate(`setBackground(${rgb.join(',')})`);
    const pixel = await expectPixel(rgb, `batch ${batch + 1}: latest frame did not reach the desktop`);
    const info = await eventually(() => window.webContents._getCompositorInfo(), value =>
      value.frames >= previousInfo.frames + limits.framesPerBatch,
    `batch ${batch + 1}: animation stopped publishing frames`);
    assertNativeCompositor(info, `batch ${batch + 1}`);
    const handles = await processHandleCount();
    evidence.batches.push({ resize, pixel, info, handles });
    assert.ok(handles <= evidence.initial.handles + limits.maximumActiveGrowth,
      `batch ${batch + 1}: browser handles grew from ${evidence.initial.handles} to ${handles}`);
    previousInfo = info;
  }

  assert.ok(previousInfo.frames - initial.frames >= colors.length * limits.framesPerBatch,
    'the bounded-handle interval must include sustained renderer publication');
  evidence.beforeClose = await processHandleCount();
  closingWindow = true;
  window.destroy();
  window = null;
  await pause(300);
  evidence.afterClose = await settledMinimumHandleCount(20);
  assert.ok(evidence.afterClose.minimum < evidence.beforeClose,
    `closing the test window did not release browser handles (${evidence.beforeClose} -> ${evidence.afterClose.minimum})`);
  assert.ok(evidence.afterClose.minimum <=
      evidence.closedBaseline.minimum + limits.maximumClosedGrowth,
    `closed-window handle count did not return near its warmed baseline ` +
      `(${evidence.closedBaseline.minimum} -> ${evidence.afterClose.minimum})`);
}).then(() => finish(), finish);
