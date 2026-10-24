'use strict';

// Exercises the production renderer Job thresholds and cache-purge path. Run in
// a fresh native mini-electron process; it deliberately commits most of the
// disposable renderer's Job budget but stops below the enforced limit.
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');

const MiB = 1024 * 1024;
const GiB = 1024 * MiB;
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-renderer-pressure-'));
app.setPath('userData', path.join(temporary, 'profile'));
app.commandLine.appendSwitch('disable-gpu');

let window;
let finished = false;
const deadline = setTimeout(() => finish(new Error('Renderer pressure reclamation smoke timed out')), 240000);
app.on('window-all-closed', () => {});

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  try {
    if (window && !window.isDestroyed()) window.destroy();
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
    if (error) console.error(error.stack || error);
    else console.log('PASS: production renderer pressure reclaimed caches and preserved redraw');
    app.exit(error ? 1 : 0);
  }
}

function pause(milliseconds) {
  return new Promise(resolve => setTimeout(resolve, milliseconds));
}

async function bounded(promise, label, milliseconds) {
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(
      new Error(`${label} timed out after ${milliseconds}ms`)), milliseconds);
  });
  try {
    return await Promise.race([promise, timeout]);
  } finally {
    clearTimeout(timer);
  }
}

function phase(label) {
  console.log(`PHASE: ${label}`);
}

function privateBytes(pid) {
  assert.ok(Number.isInteger(pid) && pid > 0, 'renderer PID must be valid');
  const result = spawnSync('powershell.exe', [
    '-NoLogo', '-NoProfile', '-NonInteractive', '-Command',
    `[int64](Get-Process -Id ${pid} -ErrorAction Stop).PrivateMemorySize64`,
  ], { encoding: 'utf8', windowsHide: true, timeout: 15000 });
  assert.equal(result.status, 0,
    `Get-Process failed: ${(result.stderr || result.stdout || '').trim()}`);
  const value = Number(result.stdout.trim());
  assert.ok(Number.isSafeInteger(value) && value > 0,
    `invalid renderer private-byte sample: ${result.stdout}`);
  return value;
}

function capturedColor(image) {
  assert.equal(image.isEmpty(), false, 'software capture must contain a frame');
  const size = image.getSize();
  const bitmap = image.toBitmap();
  const x = Math.floor(size.width / 2);
  const y = Math.floor(size.height / 2);
  const offset = (y * size.width + x) * 4;
  return [bitmap[offset + 2], bitmap[offset + 1], bitmap[offset]];
}

function closeColor(actual, expected) {
  return actual.every((channel, index) => Math.abs(channel - expected[index]) <= 2);
}

const captures = [];
function saveCapture(image, label) {
  const directory = process.env.MINI_FIX_RESULT_DIR;
  if (!directory) return;
  fs.mkdirSync(directory, { recursive: true });
  const filename = path.join(directory, `pressure-${label}.png`);
  fs.writeFileSync(filename, image.toPNG());
  captures.push({ label, filename, size: image.getSize() });
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  phase('create renderer');
  window = new BrowserWindow({
    width: 1536, height: 1536, show: true,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  const contents = window.webContents;
  await bounded(window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(
    '<!doctype html><style>html,body{margin:0;overflow:hidden;background:black}img{width:1536px;height:1536px}</style>')),
    'initial renderer load', 30000);
  const rendererPid = contents.getProcessId();
  const baseline = privateBytes(rendererPid);
  phase('generate PNG sources');

  const colors = await bounded(contents.executeJavaScript(`(async () => {
    const side = 1536;
    const source = document.createElement('canvas');
    source.width = source.height = side;
    const context = source.getContext('2d', {alpha: false});
    const colors = Array.from({length: 9}, (_, index) => [
      (31 + index * 37) & 255, (71 + index * 53) & 255, (113 + index * 29) & 255]);
    const images = [];
    for (const color of colors) {
      context.fillStyle = 'rgb(' + color.join(',') + ')';
      context.fillRect(0, 0, side, side);
      const image = new Image();
      const loaded = new Promise((resolve, reject) => {
        image.onload = resolve;
        image.onerror = () => reject(new Error('PNG source load failed'));
      });
      image.src = source.toDataURL('image/png');
      await loaded;
      image.onload = image.onerror = null;
      images.push(image);
    }
    // Retain encoded sources, then warm decoded pixels via the visible software
    // compositor below. Source load alone does not prove that raster occurred.
    source.width = source.height = 1;
    globalThis.__pressureCache = {images, colors};
    globalThis.__showPressureImage = index => {
      document.body.replaceChildren(images[index]);
      return true;
    };
    return colors;
  })()`), 'PNG source generation', 60000);

  // Paint distinct full-frame images through the real software compositor. The
  // decoded frames become unlocked discardable-cache candidates after raster.
  // This is intentionally visible: forced redraw does not override the hidden
  // renderer's suspended compositor, so hidden capture cannot warm this cache.
  phase('warm software decode cache');
  for (let index = 0; index < colors.length; ++index) {
    await bounded(contents.executeJavaScript(`__showPressureImage(${index})`),
      `select warm image ${index}`, 10000);
    const image = await bounded(contents.capturePage(),
      `warm composed capture ${index}`, 30000);
    if (index === 0 || index === colors.length - 1)
      saveCapture(image, `before-${index}`);
    const actual = capturedColor(image);
    assert.ok(closeColor(actual, colors[index]),
      `warm cache capture ${index}: ${JSON.stringify({ actual, expected: colors[index] })}`);
  }
  const cached = privateBytes(rendererPid);
  assert.ok(cached - baseline >= 16 * MiB,
    `software decode/cache warmup must commit meaningful memory: ${JSON.stringify({ baseline, cached })}`);

  // renderer_platform_win.cc enforces physical/4 clamped to [1,4] GiB on x64.
  // Stop just inside the monitor's moderate band, leaving its normal production
  // hysteresis and rate limiting untouched.
  const jobLimit = process.arch === 'x64'
    ? Math.max(GiB, Math.min(4 * GiB, Math.floor(os.totalmem() / 4)))
    : GiB;
  const moderateThreshold = Math.floor(jobLimit * 0.70);
  const targetPrivate = Math.floor(jobLimit * 0.73);
  assert.ok(cached < Math.floor(jobLimit * 0.60),
    `cache warmup must start below the pressure hysteresis band: ${JSON.stringify({ jobLimit, cached })}`);
  let allocated = 0;
  let pressured = cached;
  let pressurePeak = cached;
  const allocationDeadline = Date.now() + 120000;
  phase('commit live renderer pressure buffers');
  for (;;) {
    assert.ok(Date.now() < allocationDeadline,
      `renderer could not sustain the moderate pressure band: ${JSON.stringify({ jobLimit, allocated, pressured, pressurePeak })}`);
    // Issue bounded synchronous batches from the browser, giving the Blink
    // monitor real task-loop turns between them. A single long JS allocation
    // loop would prevent the very monitor this test is meant to exercise.
    const requested = Math.min(64 * MiB, Math.max(0, targetPrivate - pressured));
    if (requested) {
      await bounded(contents.executeJavaScript(`(() => {
        const chunk = 8 * 1024 * 1024;
        const requested = ${requested};
        const buffers = globalThis.__pressureBuffers ||= [];
        for (let committed = 0; committed < requested; committed += chunk) {
          const bytes = Math.min(chunk, requested - committed);
          const buffer = new Uint8Array(bytes);
          for (let offset = 0; offset < bytes; offset += 4096) buffer[offset] = 0x5a;
          buffers.push(buffer);
        }
      })()`), 'live renderer pressure allocation batch', 10000);
      allocated += requested;
      pressured = privateBytes(rendererPid);
      pressurePeak = Math.max(pressurePeak, pressured);
    }
    if (pressured < moderateThreshold) continue;
    phase('wait for production pressure monitor reclamation');
    await pause(4000); // Multiple real 1-second Blink-thread monitor samples.
    pressured = privateBytes(rendererPid);
    pressurePeak = Math.max(pressurePeak, pressured);
    if (pressured >= moderateThreshold) break;
    // Reclamation during allocation can put a small Job back below 70%. Top up
    // from an actual sample instead of accepting the old test's 55% non-signal.
  }
  assert.ok(pressured >= moderateThreshold && pressured < Math.floor(jobLimit * 0.85),
    `renderer must sustain real moderate, not critical, Job pressure: ${JSON.stringify({ jobLimit, pressured, pressurePeak })}`);
  const retained = await bounded(contents.executeJavaScript(`(() => {
    let total = 0;
    for (const buffer of __pressureBuffers) {
      total += buffer.byteLength;
      for (let offset = 0; offset < buffer.byteLength; offset += 4096) {
        if (buffer[offset] !== 0x5a) throw new Error('live pressure buffer was corrupted');
      }
    }
    return total;
  })()`), 'validate held pressure buffers', 10000);
  assert.equal(retained, allocated, 'every committed live buffer survives pressure');
  const reclaimed = cached + allocated - pressured;
  assert.ok(reclaimed >= 8 * MiB,
    `held buffers plus warmed caches must show native reclamation: ${JSON.stringify({ baseline, cached, allocated, pressured, reclaimed })}`);

  // A purged discardable decode must fail its old relock and be transparently
  // decoded again. Validate actual composed pixels, not JS-only wiring.
  phase('verify post-purge composed redraw');
  for (const index of [0, colors.length - 1]) {
    await bounded(contents.executeJavaScript(`__showPressureImage(${index})`),
      `select post-pressure image ${index}`, 10000);
    const image = await bounded(contents.capturePage(),
      `post-pressure composed capture ${index}`, 30000);
    saveCapture(image, `after-${index}`);
    const actual = capturedColor(image);
    assert.ok(closeColor(actual, colors[index]),
      `post-pressure redraw ${index}: ${JSON.stringify({ actual, expected: colors[index] })}`);
  }
  assert.equal(contents.isCrashed(), false);
  assert.equal(contents.getProcessId(), rendererPid);

  await bounded(contents.executeJavaScript(`(() => {
    __pressureBuffers = null;
    __pressureCache = null;
    document.body.replaceChildren();
    return true;
  })()`), 'release pressure test resources', 10000);
  const report = {
    rendererPid, jobLimit, jobLimitSource: 'production physical-memory formula, not a queried Job limit',
    moderateThreshold, baseline, cached, allocated, pressurePeak, pressured, reclaimed, captures,
  };
  if (process.env.MINI_FIX_RESULT_DIR) {
    fs.writeFileSync(path.join(process.env.MINI_FIX_RESULT_DIR, 'pressure-reclamation-report.json'),
      JSON.stringify(report, null, 2) + '\n');
  }
  console.log(JSON.stringify(report));
  finish();
}).catch(finish);
