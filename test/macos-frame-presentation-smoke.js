'use strict';

// CPU mode probes writer contention; Metal mode pauses the actual owned producer
// and observes WindowServer pixels, never drawRect/cacheDisplay remote layers.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, ipcMain } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-frame-presentation-'));
let finished = false;
let window;
let initial;
let native;
const title = `mini-frame-presentation-${process.pid}`;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
async function gpuProbe() {
  const frames = native.frames();
  assert.equal(frames.length, 1, 'An isolated owned frame mapping is required');
  const frame = frames[0];
  assert.equal(frame.outputFormat, 1);
  assert.ok(frame.nativeContextId > 0 && frame.presentedGeneration > 0);
  const points = [80, 400, 720].flatMap(x => [60, 300, 540].map(y => ({ x, y })));
  const before = await native.snapshot(title, points);
  native.pauseRenderer(window.webContents.getProcessId(), frame.fd);
  try {
    const paused = native.frames()[0];
    window.webContents.invalidate();
    await pause(100);
    const during = await native.snapshot(title, points);
    assert.equal(native.frames()[0].presentedGeneration, paused.presentedGeneration,
      'Pausing the actual GPU producer must retain its last published native generation');
    assert.deepEqual(during.samples, before.samples,
      'Invalidation while the GPU producer is stopped must retain actual visible pixels');
    console.log(JSON.stringify({ mode: 'native-metal-producer-paused', frame: paused, before, during }));
    return { differentBytes: 0, pixel: before.samples[0], generation: paused.presentedGeneration, gpu: true,
      samples: before.samples };
  } finally { native.resumeRenderer(); }
}
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (native) native.resumeRenderer();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: native presentation retains a complete frame during writer contention and presents later frames');
  app.exit(error ? (/INCOMPLETE:/.test(error.message) ? 2 : 1) : 0);
}
const deadline = setTimeout(() => finish(new Error('Native frame presentation smoke timed out')), 40000);

app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const root = path.resolve(__dirname, '..');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'presentation.node');
  const includes = ['', 'gen', 'third_party/abseil-cpp', 'base/allocator/partition_allocator/src', 'gen/base/allocator/partition_allocator/src', 'third_party/libnode/src'];
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup', '-isysroot', sdk,
    ...includes.map(include => `-I${path.join(root, include)}`), '-framework', 'AppKit',
    path.join(__dirname, 'macos-frame-presentation-helper.mm'), '-o', helper], { stdio: 'inherit', timeout: 30000 });
  const { probe } = require(helper);
  const gpuHelper = path.join(temporary, 'gpu-presentation.node');
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup', '-isysroot', sdk,
    ...includes.map(include => `-I${path.join(root, include)}`), '-framework', 'AppKit', '-framework', 'CoreGraphics',
    '-framework', 'ScreenCaptureKit', '-framework', 'ImageIO',
    path.join(__dirname, 'macos-owned-window-capture.mm'), path.join(__dirname, 'macos-gpu-compositing-helper.mm'), '-o', gpuHelper],
  { stdio: 'inherit', timeout: 30000 });
  native = require(gpuHelper);
  const preload = path.join(temporary, 'preload.js');
  fs.writeFileSync(preload, "const {contextBridge,ipcRenderer}=require('electron');contextBridge.exposeInMainWorld('presentationSmoke',{report:phase=>ipcRenderer.send('smoke:presentation',phase)});");
  ipcMain.on('smoke:presentation', async (_event, phase) => {
    try {
      if (phase === 'initial') assert.equal(window.isVisible(), false);
      // Static content has no outstanding animation: preparation must force
      // its own causal compositor commit, not await an arbitrary later paint.
      await window.webContents.prepareToShow();
      if (phase === 'initial') window.show();
    } catch (error) { finish(error); return; }
    setTimeout(async () => {
      try {
        const frames = native.frames();
        assert.equal(frames.length, 1);
        const result = frames[0].outputFormat === 1 ? await gpuProbe() :
          probe(window.webContents.getProcessId(), () => window.webContents.invalidate());
        assert.equal(result.differentBytes, 0, 'An unavailable/in-progress producer must not replace visible content with the background');
        const expected = phase === 'initial' ? [37, 118, 162, 255] : [196, 75, 25, 255];
        if (result.gpu) {
          assert.ok(result.samples.every(pixel => pixel.every((value, channel) => Math.abs(value - expected[channel]) <= 10)),
            `Actual GPU display pixels must contain the requested complete color: ${JSON.stringify(result)}`);
        } else assert.deepEqual(result.pixel, expected);
        if (phase === 'initial') {
          initial = result;
          void window.webContents.executeJavaScript('nextPresentationFrame()').catch(finish);
        } else {
          assert.ok(result.generation > initial.generation, 'A later complete frame must still be published');
          finish();
        }
      } catch (error) { finish(error); }
    }, 100);
  });
  window = new BrowserWindow({ title, show: false, width: 800, height: 600, frame: false, webPreferences: { preload, sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><title>${title}</title><style>body{margin:0;background:#2576a2}</style><script>
    const report = phase => requestAnimationFrame(() => requestAnimationFrame(() => presentationSmoke.report(phase)));
    window.nextPresentationFrame = () => { document.body.style.background = '#c44b19'; report('updated'); };
    addEventListener('DOMContentLoaded', () => report('initial'));
  </script>`));
}).catch(finish);
