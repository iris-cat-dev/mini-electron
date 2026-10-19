'use strict';

// /path/to/mini-electron --app=test/macos-gpu-guests-smoke.js
// Requires Xcode tools, generated headers, real Metal hardware, and an unlocked
// macOS graphical session. Native proof uses composed owned-window screenshots,
// never producer readback. Intentional CPU cases are not genuine GPU coverage.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, screen } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-gpu-guests-'));
const windows = new Set();
const originalGpuSetting = process.env.MINI_ELECTRON_DISABLE_GPU;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const base = [32, 64, 96, 255];
const magenta = [208, 32, 160, 255];
const cyan = [16, 208, 224, 255];
const green = [48, 192, 80, 255];
const blend = [136, 72, 64, 255];
const marker = [249, 205, 37, 255];
const width = 600, height = 400;
let native, finished = false;
const watchdog = setTimeout(() => finish(new Error('Embedded GPU guest smoke exceeded 120s watchdog')), 120000);
app.on('window-all-closed', () => {});

function restoreGpuSetting(value) {
  if (value === undefined) delete process.env.MINI_ELECTRON_DISABLE_GPU;
  else process.env.MINI_ELECTRON_DISABLE_GPU = value;
}
async function creatingWithMode(cpu, create) {
  const previous = process.env.MINI_ELECTRON_DISABLE_GPU;
  if (cpu) process.env.MINI_ELECTRON_DISABLE_GPU = '1';
  else delete process.env.MINI_ELECTRON_DISABLE_GPU;
  try { return await create(); }
  finally { restoreGpuSetting(previous); }
}
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  restoreGpuSetting(originalGpuSetting);
  try {
    if (native) native.resumeRenderer();
    for (const window of windows) if (!window.isDestroyed()) window.destroy();
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
    if (error) console.error(error.stack || error);
    else console.log('PASS: embedded webview GPU/GPU, GPU/CPU, CPU/GPU and CPU/CPU lifecycle, owned-window pixels, independent GPU guest publication, alpha-correct root captures, physical crops, move and detach');
    app.exit(error ? (/INCOMPLETE:/.test(error.message) ? 2 : 1) : 0);
  }
}
function bounded(operation, label, milliseconds = 5000) {
  let timer;
  return Promise.race([Promise.resolve(operation), new Promise((resolve, reject) => {
    timer = setTimeout(() => reject(new Error(`${label}: exceeded ${milliseconds}ms`)), milliseconds);
  })]).finally(() => clearTimeout(timer));
}
async function poll(label, read, milliseconds = 5000) {
  const deadline = Date.now() + milliseconds;
  let last;
  do {
    last = await bounded(read(), label, Math.max(1, deadline - Date.now())).catch(error => {
      error.message += `; last=${JSON.stringify(last)}`;
      throw error;
    });
    if (last === true) return;
    await pause(25);
  } while (Date.now() < deadline);
  throw new Error(`${label}: no result within ${milliseconds}ms; last=${JSON.stringify(last)}`);
}
const evaluate = (contents, source, label = 'renderer evaluation') =>
  bounded(contents.executeJavaScript(source), label);
const dataURL = html => 'data:text/html;charset=utf-8,' + encodeURIComponent(html);
function closeColor(actual, expected, tolerance = 10) {
  return actual.length === 4 && actual.every((channel, index) => Math.abs(channel - expected[index]) <= tolerance);
}
function metadata(fd, cpu, label) {
  const frame = native.frames().find(item => item.fd === fd);
  assert.ok(frame, `${label}: owned frame mapping must still exist`);
  assert.equal(frame.outputFormat, cpu ? 0 : 1, cpu
    ? `${label}: requested CPU fallback must really publish CPU pixels`
    : `INCOMPLETE: ${label} selected CPU fallback; actual Metal output is required`);
  assert.ok(frame.generation > 0 && frame.generation % 2 === 0, `${label}: complete publication`);
  assert.ok(frame.width > 0 && frame.height > 0, `${label}: nonempty frame`);
  if (cpu) {
    assert.equal(frame.nativeContextId, 0, `${label}: CPU must not advertise a GPU context`);
    assert.equal(frame.dataSize, frame.width * frame.height * 4, `${label}: actual CPU frame bytes`);
  } else {
    assert.ok(frame.nativeContextId > 0, `${label}: actual remote CAContext`);
    assert.ok(frame.presentedGeneration > 0 && frame.presentedGeneration % 2 === 0,
      `${label}: complete native presentation`);
    assert.ok(frame.generation >= frame.presentedGeneration);
    assert.ok(Number.isFinite(frame.nativeScaleFactor) && frame.nativeScaleFactor > 0);
  }
  return frame;
}
async function addedFrame(before, label) {
  let added;
  await poll(label, () => {
    added = native.frames().filter(item => !before.has(item.fd));
    return (added.length === 1 && added[0].generation > 0 && added[0].generation % 2 === 0) || added;
  });
  assert.equal(added.length, 1, `${label}: one isolated renderer, not a guessed fd`);
  return added[0].fd;
}
function generation(frame) {
  return frame.outputFormat === 1 ? frame.presentedGeneration : frame.generation;
}
async function nativeColors(window, probes, expected, label) {
  let result;
  await poll(label, async () => {
    result = await native.snapshot(window.getTitle(), probes);
    return (result.samples.length === expected.length &&
      result.samples.every((pixel, index) => closeColor(pixel, expected[index]))) || result;
  });
  assert.match(result.source, /WindowServer.*owned.window/i,
    `${label}: native proof must come from the composed owned window`);
  console.log(JSON.stringify({ label, ...result }));
}
function imageColors(image, logicalWidth, logicalHeight, probes, expected, label) {
  assert.equal(image.isEmpty(), false, `${label}: real root capture required`);
  const size = image.getSize();
  assert.ok(size.width > 0 && size.height > 0);
  assert.ok(Math.abs(size.width / logicalWidth - size.height / logicalHeight) < 0.02,
    `${label}: capture aspect ratio`);
  const decoded = native.imageSamples(image.toPNG(), probes.map(point => ({
    x: Math.floor(point.x * size.width / logicalWidth),
    y: Math.floor(point.y * size.height / logicalHeight),
  })));
  assert.deepEqual({ width: decoded.width, height: decoded.height }, size, `${label}: encoded dimensions`);
  decoded.samples.forEach((rgba, index) =>
    assert.ok(closeColor(rgba, expected[index], 3), `${label}: point ${index} actual=${rgba} expected=${expected[index]}`));
  return size;
}
async function rootCaptures(window, origin, guestColor, label) {
  const probes = [{ x: 24, y: 24 }, { x: origin.x + 20, y: origin.y + 30 },
    { x: origin.x + 120, y: origin.y + 40 }, { x: 510, y: 330 }];
  const expected = [base, guestColor, blend, marker];
  const full = await bounded(window.webContents.capturePage(), `${label}: full capture`);
  const size = imageColors(full, width, height, probes, expected, `${label}: current full capture`);
  // capturePage(rect) takes source physical pixels, NOT CSS points. Derive the
  // actual source scale from the returned full image, including CPU hosts.
  const scaleX = size.width / width, scaleY = size.height / height;
  const crop = { x: Math.round(origin.x * scaleX), y: Math.round(origin.y * scaleY),
    width: Math.round(180 * scaleX), height: Math.round(110 * scaleY) };
  const image = await bounded(window.webContents.capturePage(crop), `${label}: physical crop`);
  assert.deepEqual(image.getSize(), { width: crop.width, height: crop.height },
    `${label}: crop preserves physical source dimensions`);
  imageColors(image, crop.width, crop.height,
    probes.slice(1, 3).map(point => ({ x: point.x * scaleX - crop.x, y: point.y * scaleY - crop.y })),
    [guestColor, blend], `${label}: current cropped guest and alpha`);
}

async function runCase(hostCpu, guestCpu, index) {
  const label = `${hostCpu ? 'CPU fallback' : 'GPU'} host / ${guestCpu ? 'CPU fallback' : 'GPU'} embedded guest`;
  const area = screen.getPrimaryDisplay().workArea;
  assert.ok(area.width >= width + 40 && area.height >= height + 40,
    'INCOMPLETE: display work area must fit the 600x400 owned window');
  const title = `mini-gpu-webview-${process.pid}-${index}`;
  const beforeRoot = new Set(native.frames().map(frame => frame.fd));
  let window, guest;
  try {
    await creatingWithMode(hostCpu, async () => {
      window = new BrowserWindow({ x: area.x + 20, y: area.y + 20, width, height,
        title, frame: false, show: true, alwaysOnTop: true, backgroundColor: '#204060',
        webPreferences: { webviewTag: true, sandbox: true, contextIsolation: true, nodeIntegration: false } });
      windows.add(window);
      await bounded(window.loadURL(dataURL(`<!doctype html><title>${title}</title>
        <style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#204060}
        webview{position:absolute;display:block;border:0;padding:0;margin:0}
        #marker{position:absolute;left:500px;top:320px;width:30px;height:30px;background:#f9cd25}</style>
        <div id="marker"></div>`)), `${label}: load root`);
      await bounded(window.webContents.prepareToShow(), `${label}: root ready`);
    });
    const rootFd = await addedFrame(beforeRoot, `${label}: identify root`);
    const initialRoot = metadata(rootFd, hostCpu, `${label}: root`);
    if (!hostCpu) assert.equal(initialRoot.dataSize, 0, `${label}: initial GPU host needs no pixel mirror`);
    await nativeColors(window, [{ x: 68, y: 130 }, { x: 510, y: 330 }], [base, marker], `${label}: bare root`);
    assert.deepEqual(await evaluate(window.webContents, '[typeof require,typeof process]'), ['undefined', 'undefined'],
      `${label}: root page remains isolated from Node`);

    const beforeGuest = new Set(native.frames().map(frame => frame.fd));
    let attachments = 0;
    const attached = (event, contents) => { guest = contents; ++attachments; };
    window.webContents.on('did-attach-webview', attached);
    window.webContents.on('will-attach-webview', (event, preferences) => {
      preferences.sandbox = true;
      preferences.contextIsolation = true;
      preferences.nodeIntegration = false;
      preferences.transparent = true;
    });
    const guestURL = dataURL(`<!doctype html><style>
      html,body{margin:0;width:100%;height:100%;overflow:hidden;background:transparent}
      #solid{position:absolute;left:0;top:0;width:80px;height:100%;background:#d020a0}
      #alpha{position:absolute;left:100px;top:20px;width:60px;height:60px;background:rgba(240,80,32,.5)}
      </style><div id="solid"></div><div id="alpha"></div>`);
    try {
      await creatingWithMode(guestCpu, async () => {
        await evaluate(window.webContents, `(() => {
          const view=document.createElement('webview');view.id='guest';
          view.style.cssText='left:48px;top:100px;width:180px;height:120px';
          view.setAttribute('src',${JSON.stringify(guestURL)});document.body.appendChild(view);return true;
        })()`, `${label}: insert public webview`);
        await poll(`${label}: did-attach-webview`, () => !!guest);
      });
    } finally { window.webContents.removeListener('did-attach-webview', attached); }
    assert.equal(attachments, 1, `${label}: one real guest attachment`);
    assert.equal(guest.getType(), 'webview', `${label}: embedded webview, not BrowserView`);
    assert.notEqual(guest.getProcessId(), window.webContents.getProcessId(), `${label}: separate sandbox renderer`);
    const guestFd = await addedFrame(beforeGuest, `${label}: identify guest`);
    await nativeColors(window, [{ x: 24, y: 24 }, { x: 68, y: 130 }, { x: 168, y: 140 }, { x: 232, y: 130 }],
      [base, magenta, blend, base], `${label}: attach at CSS bounds and blend`);
    const firstGuest = metadata(guestFd, guestCpu, `${label}: guest`);
    metadata(rootFd, hostCpu, `${label}: root after attach`);
    if (!hostCpu && !guestCpu) assert.equal(firstGuest.dataSize, 0, `${label}: all-GPU guest normally publishes metadata only`);
    assert.deepEqual(await evaluate(guest, '[typeof require,typeof process]'), ['undefined', 'undefined'],
      `${label}: guest page remains isolated from Node`);
    await rootCaptures(window, { x: 48, y: 100 }, magenta, `${label}: initial`);

    await evaluate(window.webContents,
      `document.getElementById('guest').executeJavaScript("document.getElementById('solid').style.background='#10d0e0';true");true`,
      `${label}: public webview executeJavaScript`);
    await nativeColors(window, [{ x: 68, y: 130 }, { x: 168, y: 140 }], [cyan, blend], `${label}: live guest update`);
    const updatedGuest = metadata(guestFd, guestCpu, `${label}: updated guest`);
    assert.ok(generation(updatedGuest) > generation(firstGuest), `${label}: updated pixels require a later source publication`);
    assert.equal(updatedGuest.nativeContextId, firstGuest.nativeContextId, `${label}: no context substitution on update`);
    await rootCaptures(window, { x: 48, y: 100 }, cyan, `${label}: updated`);

    if (!hostCpu && !guestCpu) {
      // Stop the host before commanding the guest's public WebContents. A static
      // host cannot echo changed pixels; the remote guest must present itself.
      native.pauseRenderer(window.webContents.getProcessId(), rootFd);
      try {
        const stopped = metadata(rootFd, false, `${label}: paused root`);
        const independentStart = metadata(guestFd, false, `${label}: guest before independent update`);
        await evaluate(guest, `document.getElementById('solid').style.background='#30c050';true`,
          `${label}: update independent guest with host stopped`);
        await nativeColors(window, [{ x: 68, y: 130 }, { x: 168, y: 140 }], [green, blend],
          `${label}: guest changes while host renderer is stopped`);
        assert.equal(metadata(rootFd, false, `${label}: still paused root`).generation, stopped.generation,
          `${label}: stopped host must not republish the guest`);
        assert.ok(generation(metadata(guestFd, false, `${label}: independent guest`)) > generation(independentStart));
      } finally { native.resumeRenderer(); }
      await rootCaptures(window, { x: 48, y: 100 }, green, `${label}: independent guest current capture`);
    }
    const current = !hostCpu && !guestCpu ? green : cyan;
    const beforeMove = metadata(guestFd, guestCpu, `${label}: before move`);
    // Change size as well as position: public ResizeObserver drives guest bounds.
    await evaluate(window.webContents,
      `document.getElementById('guest').style.cssText='left:270px;top:190px;width:200px;height:140px';true`,
      `${label}: move and resize public webview`);
    await nativeColors(window, [{ x: 68, y: 130 }, { x: 290, y: 220 }, { x: 390, y: 230 },
      { x: 268, y: 220 }, { x: 352, y: 220 }, { x: 472, y: 220 }, { x: 510, y: 330 }],
    [base, current, blend, base, base, base, marker], `${label}: moved CSS bounds, no stale old guest or stretched pixels`);
    await poll(`${label}: resized guest source publication`, () => {
      const state = metadata(guestFd, guestCpu, `${label}: resized guest`);
      return generation(state) > generation(beforeMove) || state;
    });
    await rootCaptures(window, { x: 270, y: 190 }, current, `${label}: moved`);
    const finalRoot = metadata(rootFd, hostCpu, `${label}: root before detach`);
    assert.equal(finalRoot.nativeContextId, initialRoot.nativeContextId, `${label}: host output mode/context retained`);
    if (hostCpu) assert.ok(finalRoot.generation > initialRoot.generation, `${label}: CPU host incorporates GPU guest through real publication`);
    await evaluate(window.webContents, `document.getElementById('guest').remove();true`, `${label}: detach public webview`);
    await poll(`${label}: detached guest destroyed`, () => guest.isDestroyed());
    await nativeColors(window, [{ x: 68, y: 130 }, { x: 290, y: 220 }, { x: 390, y: 230 }, { x: 510, y: 330 }],
      [base, base, base, marker], `${label}: detach removes real native guest pixels`);
    imageColors(await bounded(window.webContents.capturePage(), `${label}: detached capture`), width, height,
      [{ x: 290, y: 220 }, { x: 390, y: 230 }], [base, base], `${label}: detached root capture`);
    await poll(`${label}: guest frame resource released`, () => !native.frames().some(frame => frame.fd === guestFd));
    console.log(JSON.stringify({ label, initialRoot, firstGuest, updatedGuest, finalRoot,
      coverage: 'public embedded webview lifecycle; actual composed owned-window pixels; explicitly identified CPU fallback' }));
  } finally {
    native.resumeRenderer();
    if (window && !window.isDestroyed()) window.destroy();
    if (window) windows.delete(window);
    if (guest && !guest.isDestroyed()) guest.destroy();
  }
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
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
  await runCase(false, false, 1);
  await runCase(false, true, 2);
  await runCase(true, false, 3);
  await runCase(true, true, 4);
  finish();
}).catch(finish);
