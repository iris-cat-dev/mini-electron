'use strict';

// Run using the packaged mini-electron executable, with this source checkout's
// generated headers present. Only this test's direct-child renderer is stopped.
// Samples are the actual CALayer tree after AppKit display/CA flush, not drawRect.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-resize-presentation-'));
const title = `mini-resize-presentation-${process.pid}-${Date.now()}`;
const initialColor = [37, 118, 162, 255];
const laterColor = [196, 75, 25, 255];
const settleBound = 1800;
const start = Date.now();
let window;
let native;
let finished = false;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const deadline = setTimeout(() => finish(new Error('Resize presentation smoke exceeded 60s watchdog')), 60000);
app.on('window-all-closed', () => {});

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  try {
    if (native) native.resumeRenderer();
    if (window && !window.isDestroyed()) window.destroy();
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
    if (error) console.error(error.stack || error);
    else console.log('PASS: committed native layers retain complete color during continuous paused resize and present later real frames within 1800ms');
    app.exit(error ? 1 : 0);
  }
}
async function evaluate(source) {
  return window.webContents.executeJavaScript(source);
}
function snapshot(label) {
  const result = native.snapshot(title);
  console.log(JSON.stringify({ elapsedMs: Date.now() - start, label, ...result }));
  return result;
}
function hasColor(result, color) {
  return Object.values(result.samples).every(pixel =>
    pixel.length === 4 && pixel.every((value, channel) => Math.abs(value - color[channel]) <= 3));
}
function complete(result, color, label) {
  const pixel = result.samples.center;
  assert.ok(pixel.every((value, channel) => Math.abs(value - color[channel]) <= 3),
    `${label}: existing content must retain its complete color while newly exposed space awaits layout: ${JSON.stringify(result)}`);
  assert.equal(result.layer.width, result.view.width, `${label}: native layer must cover resized view width`);
  assert.equal(result.layer.height, result.view.height, `${label}: native layer must cover resized view height`);
}
async function poll(label, predicate, bound = settleBound) {
  const began = Date.now();
  let last;
  do {
    let timer;
    try {
      last = await Promise.race([
        Promise.resolve().then(predicate),
        new Promise((_, reject) => {
          timer = setTimeout(() => reject(new Error(`${label} exceeded ${bound}ms while awaiting renderer/native samples`)),
            Math.max(1, bound - (Date.now() - began)));
        }),
      ]);
    } finally {
      clearTimeout(timer);
    }
    if (last === true) {
      console.log(JSON.stringify({ label, latencyMs: Date.now() - began, boundMs: bound }));
      return;
    }
    await pause(Math.min(16, Math.max(1, bound - (Date.now() - began))));
  } while (Date.now() - began < bound);
  throw new Error(`${label} did not settle within ${bound}ms; last=${JSON.stringify(last)}`);
}
async function gesture(axis, color, label) {
  const before = snapshot(`${label}:before`).frame;
  const x = axis === 'right' ? before.x + before.width - 1 : before.x + before.width / 2;
  const y = axis === 'bottom' ? before.y + 1 : before.y + before.height / 2;
  let endX = x, endY = y;
  native.event(title, 0, x, y);
  try {
    // One native event per timer turn: expand, shrink, expand again. No geometry
    // setters, mocked native layers or synchronous queued drag gestures.
    const deltas = Array.from({ length: 4 }, () => [40, 80, 120, 160, 120, 80, 40, -40, -80, -40, 40, 80]).flat();
    for (const [index, delta] of deltas.entries()) {
      endX = x + (axis === 'right' ? delta : 0);
      endY = y - (axis === 'bottom' ? delta : 0);
      const frame = native.event(title, 1, endX, endY);
      assert.equal(frame.width, before.width + (axis === 'right' ? delta : 0), `${label}: actual native drag width`);
      assert.equal(frame.height, before.height + (axis === 'bottom' ? delta : 0), `${label}: actual native drag height`);
      complete(snapshot(`${label}:${index}:dispatch`), color, label);
      await pause(8);
      complete(snapshot(`${label}:${index}:timer`), color, label);
    }
  } finally {
    native.event(title, 2, endX, endY);
  }
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const root = path.resolve(__dirname, '..');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'resize-presentation.node');
  const includes = ['', 'gen', 'third_party/abseil-cpp', 'base/allocator/partition_allocator/src', 'gen/base/allocator/partition_allocator/src', 'third_party/libnode/src'];
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup', '-isysroot', sdk,
    ...includes.map(include => `-I${path.join(root, include)}`), '-framework', 'AppKit', '-framework', 'QuartzCore',
    path.join(__dirname, 'macos-resize-presentation-helper.mm'), '-o', helper], { stdio: 'inherit', timeout: 30000 });
  native = require(helper);
  window = new BrowserWindow({ width: 1200, height: 800, title, titleBarStyle: 'hidden',
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><title>${title}</title>
    <style>html,body{margin:0;width:100%;height:100%;background:#2576a2;overflow:hidden}
    #marker{position:absolute;left:24px;top:32px;width:12px;height:12px;background:#111;pointer-events:none}
    #layout-marker{position:absolute;left:200px;top:200px;width:16px;height:16px;background:#19e08a}</style>
    <div id="marker"></div><div id="layout-marker"></div><script>
    window.presentationPhase=0;window.resizeMarker=[innerWidth,innerHeight];
    addEventListener('resize',()=>window.resizeMarker=[innerWidth,innerHeight]);
    function animate(){++window.presentationPhase;document.getElementById('marker').style.transform='translateX('+(window.presentationPhase%64)+'px)';requestAnimationFrame(animate)}
    requestAnimationFrame(animate);
    window.laterPresentation=()=>{document.documentElement.style.background='#c44b19';document.body.style.background='#c44b19'};
    </script>`));
  await poll('initial committed complete color (unsupported CALayer extraction fails here, never falls back to drawRect)', () => {
    const result = snapshot('initial');
    return hasColor(result, initialColor) || result;
  }, 4000);
  const pid = window.webContents.getProcessId();
  const initialPhase = Number(await evaluate('presentationPhase'));
  const initialGeneration = native.pauseRenderer(pid);
  try {
    complete(snapshot('paused-initial'), initialColor, 'paused initial');
    await gesture('right', initialColor, 'paused right');
    await gesture('bottom', initialColor, 'paused bottom');
  } finally {
    native.resumeRenderer();
  }
  const finalSize = snapshot('resumed').view;
  await poll('final real renderer viewport and resize marker', async () => {
    const viewport = JSON.parse(await evaluate('JSON.stringify([innerWidth,innerHeight,...resizeMarker])'));
    const expected = [finalSize.width, finalSize.height, finalSize.width, finalSize.height];
    return JSON.stringify(viewport) === JSON.stringify(expected) || viewport;
  });
  await evaluate('laterPresentation(); true');
  await poll('later complete orange layer and advancing rAF', async () => {
    const result = snapshot('later');
    const phase = Number(await evaluate('presentationPhase'));
    return (hasColor(result, laterColor) && phase > initialPhase) || { phase, result };
  });
  await gesture('right', laterColor, 'live right');
  await gesture('bottom', laterColor, 'live bottom');
  const liveFinal = snapshot('live-final').view;
  await poll('live final viewport and retained later complete color', async () => {
    const viewport = JSON.parse(await evaluate('JSON.stringify([innerWidth,innerHeight,...resizeMarker])'));
    return (JSON.stringify(viewport) === JSON.stringify([liveFinal.width, liveFinal.height, liveFinal.width, liveFinal.height]) &&
      hasColor(snapshot('live-settled'), laterColor)) || viewport;
  });
  await poll('replacement child layout is actually visible, not only the retained old surface', () => {
    const pixels = [native.sample(title, 208, 208), native.sample(title, 208, liveFinal.height - 208)];
    return pixels.some(pixel => pixel.every((value, channel) => Math.abs(value - [25, 224, 138, 255][channel]) <= 3)) || pixels;
  });
  try {
    const laterGeneration = native.pauseRenderer(pid);
    assert.ok(laterGeneration > initialGeneration, 'A later complete shared publication must really be produced');
    complete(snapshot('later-paused'), laterColor, 'later complete publication');
  } finally {
    native.resumeRenderer();
  }
  finish();
}).catch(finish);
