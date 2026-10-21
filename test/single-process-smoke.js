'use strict';

// Native runtime, CLI mode: mini-electron --single-process test/single-process-smoke.js
// API mode: MINI_SINGLE_PROCESS_API=1 mini-electron test/single-process-smoke.js
// Default-mode control: MINI_SINGLE_PROCESS_EXPECT_DEFAULT=1 mini-electron test/single-process-smoke.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, ipcMain, protocol } = require('electron');

if (process.env.MINI_SINGLE_PROCESS_API === '1') app.commandLine.appendSwitch('single-process');
const singleProcess = process.env.MINI_SINGLE_PROCESS_EXPECT_DEFAULT !== '1';
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-single-process-'));
app.setPath('userData', path.join(temporary, 'user-data'));
app.on('window-all-closed', () => {});
const windows = [];
const identities = new Map();
const evidence = { mode: singleProcess ? 'single-process' : 'default', mainPid: process.pid, windows: [] };
let finished = false;
let phase = 'startup';
const watchdog = setTimeout(() => finish(new Error(`Single-process smoke timed out: ${phase}`)), 45000);

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  for (const window of windows) if (!window.isDestroyed()) window.destroy();
  ipcMain.removeHandler('single:identity');
  ipcMain.removeHandler('single:wait');
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error, JSON.stringify(evidence));
  else console.log(`PASS: ${evidence.mode} multiwindow identity, IPC, storage, capture, teardown and reopen\n${JSON.stringify(evidence)}`);
  app.exit(error ? 1 : 0);
}

protocol.registerSchemesAsPrivileged([
  { scheme: 'single-ui', privileges: { standard: true, secure: true, supportFetchAPI: true } },
]);

app.whenReady().then(async () => {
  const preload = path.join(temporary, 'preload.js');
  fs.writeFileSync(preload, `const { contextBridge, ipcRenderer } = require('electron');
    contextBridge.exposeInMainWorld('ui', {
      sandboxed: process.sandboxed,
      identity: () => ipcRenderer.invoke('single:identity'),
      wait: () => ipcRenderer.invoke('single:wait'),
      listen: () => new Promise(resolve => ipcRenderer.once('single:message', (_event, value) => resolve(value)))
    });`);
  ipcMain.handle('single:identity', event => {
    const identity = identities.get(event.sender.id);
    assert.ok(identity, 'IPC must route to a live owning window');
    assert.equal(event.senderFrame, event.sender.mainFrame);
    return identity;
  });
  ipcMain.handle('single:wait', () => new Promise(resolve => setTimeout(() => resolve('late'), 200)));
  protocol.handle('single-ui', () => new Response(`<!doctype html><style>
    html,body { margin:0; width:100%; height:100%; background:rgb(32,112,176); }
    button { margin:36px; padding:12px; }
    </style><title>Single process UI</title><button onclick="this.textContent='clicked'">click</button>`,
    { headers: { 'content-type': 'text/html' } }));

  function create(label) {
    const partition = `single-${label}`;
    const window = new BrowserWindow({ width: 360, height: 240, show: false,
      title: `Single process ${label}`, webPreferences: { partition, preload, contextIsolation: true } });
    windows.push(window);
    identities.set(window.webContents.id, label);
    window.webContents.on('javascript-dialog-opening', (event, details) => {
      event.returnValue = details.type === 'prompt'
        ? { accepted: true, promptText: label }
        : details.message !== 'reject';
    });
    return window;
  }

  async function exercise(window, label) {
    await window.loadURL('single-ui://app/');
    const contents = window.webContents;
    const rendererPid = contents.getProcessId();
    assert.ok(rendererPid > 0);
    if (singleProcess) assert.equal(rendererPid, process.pid, 'renderer must run in the host process');
    else assert.notEqual(rendererPid, process.pid, 'default renderer must remain out of process');
    assert.equal(await contents.executeJavaScript('ui.sandboxed'), !singleProcess);
    assert.equal(await contents.executeJavaScript('ui.identity()'), label);
    assert.deepEqual(await contents.executeJavaScript('[typeof require,typeof process]'), ['undefined', 'undefined']);
    assert.equal(await contents.executeJavaScript("document.querySelector('button').click();document.querySelector('button').textContent"), 'clicked');
    assert.deepEqual(await contents.executeJavaScript(
      '[confirm("accept"),confirm("reject"),prompt("text","default"),(alert("done"),"returned")]'),
    [true, false, label, 'returned'], 'JavaScript dialogs must route to their owning window without blocking the host');
    await contents.executeJavaScript(`localStorage.setItem('owner', ${JSON.stringify(label)})`);
    assert.equal(await contents.executeJavaScript("localStorage.getItem('owner')"), label);
    const received = contents.executeJavaScript('ui.listen()');
    await contents.executeJavaScript('0');
    contents.send('single:message', label);
    assert.equal(await received, label);
    evidence.windows.push({ label, rendererPid, sandboxed: !singleProcess });
  }

  phase = 'first window';
  const first = create('first');
  await exercise(first, 'first');
  phase = 'second window';
  const second = create('second');
  await exercise(second, 'second');
  if (!singleProcess) assert.notEqual(first.webContents.getProcessId(), second.webContents.getProcessId());
  phase = 'concurrent routing';
  const evaluate = (window, source) => window.webContents.executeJavaScript(source);
  assert.deepEqual(await Promise.all([evaluate(first, 'ui.identity()'), evaluate(second, 'ui.identity()')]), ['first', 'second']);
  assert.deepEqual(await Promise.all([evaluate(first, "localStorage.getItem('owner')"), evaluate(second, "localStorage.getItem('owner')")]), ['first', 'second']);
  await first.webContents.prepareToShow();
  first.show();
  phase = 'presentation and capture';
  const image = await first.webContents.capturePage();
  const size = image.getSize();
  assert.ok(size.width >= 200 && size.height >= 100, 'capture must contain the real window content');
  const pixels = await first.webContents.executeJavaScript(`new Promise((resolve, reject) => {
    const image = new Image();
    image.onload = () => {
      const canvas = document.createElement('canvas');
      canvas.width = image.width;
      canvas.height = image.height;
      const context = canvas.getContext('2d');
      context.drawImage(image, 0, 0);
      resolve([...context.getImageData(image.width - 8, image.height - 8, 1, 1).data]);
    };
    image.onerror = () => reject(new Error('Captured PNG must decode'));
    image.src = ${JSON.stringify(image.toDataURL())};
  })`);
  assert.deepEqual(pixels, [32, 112, 176, 255], 'capture must contain rendered page pixels');
  evidence.capture = size;

  phase = 'pending work and first-window teardown';
  const pending = evaluate(first, 'ui.wait()');
  const rejected = assert.rejects(pending, /destroy|closed|context|renderer|cancel/i);
  first.destroy();
  await rejected;
  assert.equal(await evaluate(second, 'ui.identity()'), 'second');
  assert.equal(await evaluate(second, "localStorage.getItem('owner')"), 'second');

  phase = 'all windows closed and reopened';
  second.destroy();
  const third = create('third');
  await exercise(third, 'third');
  finish();
}).catch(finish);
