'use strict';

// Run in a fresh native mini-electron process on a >=15-GiB x64 Windows host.
// Cross the former 2-GiB V8 cap with live JS heap objects, not external buffers.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');

const GiB = 1024 ** 3;
const elementsPerArray = 8 * 1024 * 1024;
const arrayCount = 68;
// The native x64 build uses pointer-compressed four-byte Smi array slots.
const heldSlotBytes = arrayCount * elementsPerArray * 4; // 2.125 GiB.
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-v8-headroom-'));
app.setPath('userData', path.join(temporary, 'profile'));
app.commandLine.appendSwitch('disable-gpu');

let window;
let finished = false;
const deadline = setTimeout(() => finish(new Error('V8 headroom smoke timed out')), 180000);
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
    else console.log('PASS: renderer retained more than 2 GiB of live JS arrays and remained responsive');
    app.exit(error ? 1 : 0);
  }
}

async function bounded(promise, label, milliseconds = 15000) {
  let timer;
  try {
    return await Promise.race([
      promise,
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error(`${label} timed out`)), milliseconds);
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

function pause(milliseconds) {
  return new Promise(resolve => setTimeout(resolve, milliseconds));
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  assert.equal(process.arch, 'x64');
  assert.ok(os.totalmem() >= 15 * GiB,
    'this regression needs the modern x64 machine-default ~4-GiB V8 heap');
  window = new BrowserWindow({
    width: 480, height: 320, show: false,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  const contents = window.webContents;
  contents.on('render-process-gone', (_, details) => {
    finish(new Error(`Renderer exited during headroom regression: ${JSON.stringify(details)}`));
  });
  await bounded(window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(
    '<!doctype html><title>V8 headroom regression</title><p id="state">ready</p>')),
  'load headroom page');
  const rendererPid = contents.getProcessId();
  assert.ok(Number.isInteger(rendererPid) && rendererPid > 0 && rendererPid !== process.pid);

  for (let index = 0; index < arrayCount; ++index) {
    const count = await bounded(contents.executeJavaScript(`(() => {
      const arrays = globalThis.__headroomArrays ||= [];
      arrays.push(new Array(${elementsPerArray}).fill(${index + 1}));
      return arrays.length;
    })()`), `retain JS array ${index}`);
    assert.equal(count, index + 1);
    assert.equal(contents.getProcessId(), rendererPid);
    // Separate renderer tasks and browser yields keep this a responsiveness
    // regression and allow the real one-second monitor to run normally.
    await pause(20);
  }
  assert.ok(heldSlotBytes > 2 * GiB);
  await pause(1500);
  const retained = await bounded(contents.executeJavaScript(`(() => {
    const arrays = globalThis.__headroomArrays;
    for (let index = 0; index < arrays.length; ++index) {
      const array = arrays[index];
      if (array.length !== ${elementsPerArray}
          || array[0] !== index + 1
          || array[array.length / 2] !== index + 1
          || array[array.length - 1] !== index + 1)
        throw new Error('retained JS array was lost or corrupted');
    }
    const state = document.getElementById('state');
    state.textContent = 'responsive after retaining ' + arrays.length + ' arrays';
    // First performance.memory read: coarse values are cached for 20 minutes,
    // so querying the limit before allocation would also freeze usedJSHeapSize.
    return {
      count: arrays.length,
      state: state.textContent,
      usedJSHeapSize: performance.memory.usedJSHeapSize,
      heapSizeLimit: performance.memory.jsHeapSizeLimit,
    };
  })()`), 'validate held arrays and page responsiveness');
  assert.equal(retained.count, arrayCount);
  assert.equal(retained.state, `responsive after retaining ${arrayCount} arrays`);
  // performance.memory is intentionally coarse. Check the default heap class,
  // not exact byte equality against privacy-rounded renderer telemetry.
  assert.ok(retained.heapSizeLimit >= 3.5 * GiB && retained.heapSizeLimit <= 4.5 * GiB,
    `expected the unmodified ~4-GiB V8 default, got ${retained.heapSizeLimit}`);
  assert.ok(heldSlotBytes < 0.65 * retained.heapSizeLimit,
    'this boundary workload must remain below automatic pre-OOM snapshot pressure');
  assert.ok(retained.usedJSHeapSize > 2 * GiB,
    `renderer must report live JS heap beyond the former cap: ${JSON.stringify(retained)}`);
  assert.equal(contents.isCrashed(), false);
  assert.equal(contents.getProcessId(), rendererPid);
  const state = await bounded(contents.executeJavaScript('document.getElementById("state").textContent'),
    'read subsequent renderer response');
  assert.equal(state, retained.state);

  assert.equal(await bounded(contents.executeJavaScript(`(() => {
    globalThis.__headroomArrays = null;
    document.getElementById('state').textContent = 'released';
    return document.getElementById('state').textContent;
  })()`), 'release held arrays'), 'released');
  const report = {
    rendererPid, physicalMemory: os.totalmem(), heldSlotBytes, arrayCount,
    elementsPerArray, heapLimitSource: 'coarse renderer performance.memory', retained,
  };
  if (process.env.MINI_FIX_RESULT_DIR) {
    fs.writeFileSync(path.join(process.env.MINI_FIX_RESULT_DIR, 'v8-headroom-report.json'),
      JSON.stringify(report, null, 2) + '\n');
  }
  console.log(JSON.stringify(report));
  finish();
}).catch(finish);
