'use strict';

// Run: /path/to/packaged/omp test/macos-resize-geometry-smoke.js
// Requires this checkout's generated headers and macOS command-line tools.
// Captures committed CALayer pixels, never drawRect or a renderer screenshot.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-resize-geometry-'));
const title = `mini-resize-geometry-${process.pid}-${Date.now()}`;
const bound = 1800;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let window, native, flipRows = false, finished = false;
const watchdog = setTimeout(() => finish(new Error('Geometry smoke exceeded 60s watchdog')), 60000);
app.on('window-all-closed', () => {});
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  try {
    if (native) native.resumeRenderer();
    if (window && !window.isDestroyed()) window.destroy();
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
    if (error) console.error(error.stack || error);
    else console.log('PASS: native geometry, responsive layout and later animated pixels agree across extreme aspect transitions');
    app.exit(error ? 1 : 0);
  }
}
async function evaluate(source) {
  return window.webContents.executeJavaScript(source);
}
async function dom() {
  return JSON.parse(await evaluate(`JSON.stringify({width:innerWidth,height:innerHeight,phase:geometryPhase,
    markers:Object.fromEntries(['fixed','responsive','circle','animated'].map(id=>{
      const r=document.getElementById(id).getBoundingClientRect();
      return [id,{x:r.x,y:r.y,width:r.width,height:r.height}]}))})`));
}
function capture() { return native.geometry(title, flipRows); }
function mismatch(pixels, layout, includeAnimated = false) {
  const errors = [];
  for (const [actual, expected, label] of [
    [pixels.bitmapWidth, layout.width, 'bitmap width'], [pixels.bitmapHeight, layout.height, 'bitmap height'],
    [pixels.view.width, layout.width, 'view width'], [pixels.view.height, layout.height, 'view height'],
    [pixels.layer.width, layout.width, 'layer width'], [pixels.layer.height, layout.height, 'layer height']]) {
    if (Math.abs(actual - expected) > 1) errors.push(`${label}: ${actual} != ${expected}`);
  }
  for (const id of includeAnimated ? ['fixed', 'responsive', 'circle', 'animated'] : ['fixed', 'responsive', 'circle']) {
    const actual = pixels.markers[id], expected = layout.markers[id];
    if (!actual.pixels) errors.push(`${id}: missing native color pixels`);
    for (const key of ['x', 'y', 'width', 'height']) {
      if (Math.abs(actual[key] - expected[key]) > 2) errors.push(`${id}.${key}: ${actual[key]} != ${expected[key]}`);
    }
    if (Math.abs(actual.width - actual.height) > 2) errors.push(`${id}: nonuniform aspect ${actual.width}/${actual.height}`);
  }
  return errors;
}
async function settle(label, predicate, timeout = bound) {
  const began = Date.now();
  let last;
  do {
    let timer;
    try {
      last = await Promise.race([predicate(), new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error(`${label}: renderer response timeout; last=${JSON.stringify(last)}`)),
          Math.max(1, timeout - (Date.now() - began)));
      })]);
    } finally { clearTimeout(timer); }
    if (last === true) {
      console.log(JSON.stringify({ label, settleLatencyMs: Date.now() - began, boundMs: timeout }));
      return;
    }
    await pause(16);
  } while (Date.now() - began < timeout);
  console.error(JSON.stringify({ label, mismatchSnapshot: last }));
  throw new Error(`${label}: native geometry failed to settle within ${timeout}ms`);
}
async function settledGeometry(label) {
  await settle(label, async () => {
    const layout = await dom(), pixels = capture();
    const errors = mismatch(pixels, layout);
    return errors.length ? { errors, layout, pixels } : true;
  });
}
async function resize(width, height, label) {
  const before = capture().frame;
  const x = before.x + before.width - 1, y = before.y + 1;
  let endX = x, endY = y;
  native.event(title, 0, x, y);
  try {
    // Real bottom-right corner drags over separate timer turns, not setBounds.
    // Each large aspect transition allows renderer work/metadata to interleave.
    for (let step = 1; step <= 12; ++step) {
      const w = Math.round(before.width + (width - before.width) * step / 12);
      const h = Math.round(before.height + (height - before.height) * step / 12);
      endX = x + w - before.width; endY = y - (h - before.height);
      const frame = native.event(title, 1, endX, endY);
      assert.ok(Math.abs(frame.width - w) <= 1 && Math.abs(frame.height - h) <= 1,
        `${label}: native corner dispatch failed: ${JSON.stringify({ frame, w, h })}`);
      await pause(8);
    }
  } finally { native.event(title, 2, endX, endY); }
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const root = path.resolve(__dirname, '..');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'resize-geometry.node');
  const includes = ['', 'gen', 'third_party/abseil-cpp', 'base/allocator/partition_allocator/src', 'gen/base/allocator/partition_allocator/src', 'third_party/libnode/src'];
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup', '-isysroot', sdk,
    ...includes.map(include => `-I${path.join(root, include)}`), '-framework', 'AppKit', '-framework', 'QuartzCore',
    path.join(__dirname, 'macos-resize-presentation-helper.mm'), '-o', helper], { stdio: 'inherit', timeout: 30000 });
  native = require(helper);
  window = new BrowserWindow({ width: 1200, height: 800, title, titleBarStyle: 'hidden',
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><title>${title}</title>
    <style>html,body{margin:0;width:100%;height:100%;background:#263545;overflow:hidden}
    .marker{position:absolute;z-index:2}
    #fixed{left:37px;top:53px;width:80px;height:80px;background:rgb(241,43,67)}
    #responsive{right:43px;bottom:61px;width:100px;height:100px;background:rgb(31,227,137)}
    #circle{left:calc(50% - 50px);top:calc(50% - 50px);width:100px;height:100px;border-radius:50%;background:rgb(47,113,251)}
    #animated{left:180px;top:170px;width:20px;height:20px;background:rgb(249,205,37)}
    #work{display:grid;grid-template-columns:repeat(auto-fit,minmax(29px,1fr));position:absolute;inset:220px 0 0;opacity:.3}
    #work div{height:13px;background:#647487;border:1px solid #394958}</style>
    <div id="work"></div><div class="marker" id="fixed"></div><div class="marker" id="responsive"></div>
    <div class="marker" id="circle"></div><div class="marker" id="animated"></div><script>
    const work=document.getElementById('work');
    for(let i=0;i<900;i++){const cell=document.createElement('div');cell.textContent=String(i%10);work.appendChild(cell)}
    window.geometryPhase=0;
    window.animationAnchor=null;
    function animate(){++geometryPhase;
      document.getElementById('animated').style.transform='translateX('+(animationAnchor===null?geometryPhase%100:animationAnchor)+'px)';
      // Bounded real responsive layout/paint work, no busy waits.
      const cell=work.children[geometryPhase%work.children.length];cell.style.height=(13+geometryPhase%3)+'px';
      requestAnimationFrame(animate)}
    window.startGeometryAnimation=()=>requestAnimationFrame(animate);
    </script>`));
  // Derive row orientation from an asymmetric fixed marker at known DOM y.
  // The initial session is completely static: no rAF, interval, resize handler
  // or marker mutation can manufacture paint retries for a dropped final size.
  await settle('initial pixel extraction and orientation calibration', async () => {
    const layout = await dom();
    for (const candidate of [false, true]) {
      const pixels = native.geometry(title, candidate);
      if (!mismatch(pixels, layout, true).length) { flipRows = candidate; return true; }
    }
    return { layout, raw: native.geometry(title, false), reversed: native.geometry(title, true) };
  }, 4000);
  const screen = capture().screen;
  // Anchor the existing native window near the visible top-left. Only position
  // is set; every accepted size change below is produced by mouse dispatch.
  const maxW = Math.floor(screen.width - 32), maxH = Math.floor(screen.height - 32);
  assert.ok(maxW >= 850 && maxH >= 650, 'Screen too small for meaningful extreme-aspect regression');
  window.setPosition(Math.round(screen.x + 16), Math.round(screen.y + 16));
  const targets = [[Math.min(700, maxW), Math.min(1050, maxH)],
    [Math.min(1500, maxW), 500], [Math.min(620, maxW), Math.min(1000, maxH)],
    [Math.min(1200, maxW), Math.min(800, maxH)]];
  const initialFixed = (await dom()).markers.fixed;
  try {
    native.pauseRenderer(window.webContents.getProcessId());
    await resize(targets[0][0], targets[0][1], 'paused portrait geometry');
    const pixels = capture();
    for (const key of ['x', 'y', 'width', 'height']) {
      assert.ok(Math.abs(pixels.markers.fixed[key] - initialFixed[key]) <= 2,
        `Retained content must preserve native pixel geometry during resize, not stretch: ${JSON.stringify(pixels)}`);
    }
  } finally { native.resumeRenderer(); }
  await settledGeometry('paused portrait resumes to correct responsive layout');
  await resize(targets[3][0], targets[3][1], 'restore after paused geometry');
  await settledGeometry('static geometry restored after pause');
  for (const [index, [width, height]] of targets.entries()) {
    await resize(width, height, `static-aspect-${index}`);
    await settledGeometry(`static-aspect-${index} released ${width}x${height}`);
    assert.equal((await dom()).phase, 0, 'Static phase must not run animation');
  }
  // Rapid transitions intentionally omit intermediate renderer settle waits.
  // Only timer-yielding mouse dispatch drives layout and paint here.
  for (const [index, [width, height]] of targets.entries()) {
    await resize(width, height, `static-rapid-${index}`);
  }
  await settledGeometry('static rapid sequence final released geometry');
  assert.equal((await dom()).phase, 0, 'Static rapid phase must not run animation');
  await evaluate('animationAnchor=null; startGeometryAnimation(); true');
  for (const [index, [width, height]] of targets.entries()) {
    const phase = (await dom()).phase;
    await resize(width, height, `aspect-${index}`);
    await settledGeometry(`aspect-${index} released ${width}x${height}`);
    // Require a subsequently painted marker at a newly commanded position;
    // old frozen/scaled images cannot satisfy both later anchors.
    for (const anchor of [11, 89]) {
      await evaluate(`animationAnchor=${anchor}; true`);
      await settle(`aspect-${index} later real paint anchor ${anchor}`, async () => {
        const layout = await dom(), pixels = capture();
        const errors = mismatch(pixels, layout, true);
        return layout.phase > phase && !errors.length && Math.abs(layout.markers.animated.x - (180 + anchor)) <= 1
          ? true : { errors, layout, pixels, phaseBefore: phase };
      });
    }
    await evaluate('animationAnchor=null; true');
  }
  // Paused geometry is diagnostic observation only: no assertion prescribing
  // whether the interim retained image should stretch, crop or letterbox.
  const pid = window.webContents.getProcessId();
  try {
    const generation = native.pauseRenderer(pid);
    console.log(JSON.stringify({ label: 'paused-final-observation-only', generation, pixels: capture() }));
  } finally { native.resumeRenderer(); }
  await settledGeometry('final resumed geometry');
  const finalPhase = (await dom()).phase;
  const finalAnimatedX = capture().markers.animated.x;
  await settle('final resumed animation produces changing native pixels', async () => {
    const layout = await dom(), pixels = capture();
    const errors = mismatch(pixels, layout);
    return !errors.length && layout.phase > finalPhase && pixels.markers.animated.pixels > 0 &&
      pixels.markers.animated.x !== finalAnimatedX ? true : { errors, layout, pixels, finalPhase, finalAnimatedX };
  });
  finish();
}).catch(finish);
