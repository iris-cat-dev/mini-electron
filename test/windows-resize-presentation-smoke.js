'use strict';

// Requires an unlocked Windows desktop. Uses actual mouse-driven Win32 sizing loops.
// out/windows-x64/mini-electron.exe --app=test/windows-resize-presentation-smoke.js
// CPU renderer with native presentation: MINI_ELECTRON_DISABLE_GPU=1 <command above>
// Fully software/GDI: MINI_ELECTRON_DISABLE_GPU=1 <exe> --disable-gpu --app=<this file>
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, screen } = require('electron');
const { startNativeInput } = require('./windows-native-input-driver');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-windows-resize-'));
app.setPath('userData', path.join(temporary, 'user-data'));
const title = `mini-windows-resize-${process.pid}`;
const artifacts = process.env.MINI_NATIVE_INPUT_ARTIFACTS;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const evidence = { batches: [] };
let window, driver, finished = false;
const watchdog = setTimeout(() => finish(new Error('Native resize regression exceeded 90s')), 90000);
app.on('window-all-closed', () => {});

async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (error) {
    if (driver) {
      try { evidence.nativeFailure = await driver.request('pointerProbe', { title, x: 0, y: 0 }); }
      catch (failure) { evidence.nativeFailure = String(failure); }
    }
    evidence.error = error.stack || String(error);
    if (window && !window.isDestroyed()) {
      try {
        evidence.failureInfo = await Promise.race([
          window.webContents._getCompositorInfo(),
          pause(2000).then(() => 'compositor diagnostics timed out'),
        ]);
      } catch (failure) { evidence.failureInfo = String(failure); }
    }
  }
  try {
    if (driver && artifacts) {
      fs.mkdirSync(artifacts, { recursive: true });
      await driver.request('capture', { title, path: path.resolve(artifacts, `resize-${process.pid}.png`) });
    }
  } catch (failure) { evidence.captureError = String(failure); }
  try { if (driver) await driver.close(); } catch (failure) { error = error || failure; }
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (artifacts) {
    fs.mkdirSync(artifacts, { recursive: true });
    fs.writeFileSync(path.join(artifacts, `resize-${process.pid}.json`), JSON.stringify(evidence, null, 2));
  }
  if (error) console.error(error.stack || error, JSON.stringify(evidence));
  else console.log('PASS: repeated native edge resizes retain live rendering and subsequent desktop updates\n' + JSON.stringify(evidence));
  app.exit(error ? 1 : 0);
}

async function eventually(read, predicate, label) {
  const deadline = Date.now() + 6000;
  let value;
  do {
    value = await read();
    if (predicate(value)) return value;
    await pause(30);
  } while (Date.now() < deadline);
  assert.fail(`${label}: ${JSON.stringify(value)}`);
}

const native = (action, values = {}) => driver.request(action, { title, ...values });
const evaluate = source => window.webContents.executeJavaScript(source);
async function expectPixel(rgb, label) {
  const geometry = await native('geometry');
  const x = geometry.clientX + Math.round(60 * geometry.dpi / 96);
  const y = geometry.clientY + Math.round(120 * geometry.dpi / 96);
  return eventually(async () => (await native('sample', { x, y, width: 1, height: 1 }))[0].rgb,
    actual => actual.every((channel, index) => Math.abs(channel - rgb[index]) <= 4), label);
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32', 'PREREQUISITE: Windows interactive desktop required');
  const area = screen.getPrimaryDisplay().workArea;
  const width = Math.min(1400, area.width - 100);
  const height = Math.min(900, area.height - 100);
  assert.ok(width >= 800 && height >= 600, 'PREREQUISITE: at least 900x700 DIP work area required');
  driver = await startNativeInput(temporary, title);
  window = new BrowserWindow({
    title, x: area.x + Math.round((area.width - width) / 2), y: area.y + Math.round((area.height - height) / 2),
    width, height, frame: false, show: false, minWidth: 0, minHeight: 0,
    titleBarStyle: 'hidden', titleBarOverlay: { color: '#183048', symbolColor: '#ffffff', height: 40 },
    backgroundColor: '#214365', webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  window.webContents.on('render-process-gone', (_, details) => finish(new Error(`renderer exited: ${JSON.stringify(details)}`)));
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><title>${title}</title>
    <style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:rgb(33,67,101)}
    #animation{position:fixed;left:180px;top:80px;width:90px;height:90px;background:#ef6c40}
    #cards{position:fixed;left:300px;top:80px;right:20px;bottom:20px;display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:8px}
    .card{background:linear-gradient(120deg,#315784aa,#ae684baa);border-radius:12px;backdrop-filter:blur(12px)}</style>
    <div id="animation"></div><div id="cards">${'<div class="card"></div>'.repeat(48)}</div><script>
    window.fixture={frames:0,resizes:0};addEventListener('resize',()=>fixture.resizes++);
    function tick(){fixture.frames++;document.querySelector('#animation').style.transform='translateX('+(fixture.frames%80)+'px)';requestAnimationFrame(tick)};
    requestAnimationFrame(tick);
    window.setColor=color=>{document.documentElement.style.background=color;document.body.style.background=color;return true};
    </script>`));
  window.show();
  window.focus();
  await native('focus');
  await expectPixel([33, 67, 101], 'initial page not visible on the native desktop');
  evidence.initial = await window.webContents._getCompositorInfo();
  assert.equal(evidence.initial.backend, process.env.MINI_ELECTRON_DISABLE_GPU === '1' ? 'software' : 'd3d12');
  if (artifacts) fs.mkdirSync(artifacts, { recursive: true });
  let currentRgb = [33, 67, 101];
  for (let batch = 0; batch < 3; batch++) {
    const before = await evaluate('({frames:fixture.frames,resizes:fixture.resizes})');
    const { resizing, presentation } = await native('resizeCycles', {
      cycles: 20, distance: 640, steps: 32, interval: 1,
      samples: [{ x: 60, y: 120, rgb: currentRgb }, { x: 60, y: 24, bottom: true, rgb: currentRgb }],
      ...(artifacts ? { failurePath: path.resolve(artifacts, `resize-${process.pid}-batch-${batch}-flicker.png`) } : {}),
    });
    assert.equal(resizing.entered, 20, 'all edge drags must enter real Win32 sizing loops');
    const rgb = [[44, 156, 92], [164, 52, 116], [36, 116, 196]][batch];
    await evaluate(`setColor('rgb(${rgb.join(',')})')`);
    const pixel = await expectPixel(rgb, `page stopped presenting after batch ${batch + 1}`);
    const after = await eventually(() => evaluate('({frames:fixture.frames,resizes:fixture.resizes,width:innerWidth,height:innerHeight})'),
      value => value.frames > before.frames && value.resizes > before.resizes,
      'animation and resize events must continue after native sizing');
    evidence.batches.push({ resizing, presentation, before, after, pixel, info: await window.webContents._getCompositorInfo() });
    assert.equal(presentation.whiteFrames, 0, 'native resizing must not expose white backing frames');
    assert.equal(presentation.missingFrames, 0, 'native resizing must retain page pixels, including the moving bottom edge');
    currentRgb = rgb;
  }
}).then(() => finish(), finish);
