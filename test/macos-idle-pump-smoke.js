'use strict';

// Run from a graphical macOS login session with the bundled runtime:
// ELECTRON_RUN_AS_NODE=1 Electron.app/Contents/MacOS/Electron test/macos-idle-pump-smoke.js
// The Node-only supervisor owns the watchdog, so a stalled GUI/libuv pump cannot
// also stall the test's timeout. No CPU thresholds or periodic application ticks.
const assert = require('node:assert/strict');
const { execFile, execFileSync, spawn } = require('node:child_process');
const { once } = require('node:events');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const { promisify } = require('node:util');
const run = promisify(execFile);
const childMode = process.argv.includes('--idle-pump-browser');

async function supervise() {
  assert.equal(process.platform, 'darwin', 'This regression requires macOS');
  assert.equal(process.env.ELECTRON_RUN_AS_NODE, '1', 'Run the supervisor in Node-only mode');
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-idle-pump-'));
  let child;
  let socket;
  let watchdog;
  try {
    const root = path.resolve(__dirname, '..');
    const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
    const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
    const helper = path.join(temporary, 'window-buttons.node');
    const includes = ['', 'gen', 'third_party/abseil-cpp', 'base/allocator/partition_allocator/src',
      'gen/base/allocator/partition_allocator/src', 'third_party/libnode/src'];
    execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup',
      '-isysroot', sdk, ...includes.map(include => `-I${path.join(root, include)}`), '-framework', 'AppKit',
      path.join(__dirname, 'macos-window-buttons-helper.mm'), '-o', helper],
    { stdio: 'inherit', timeout: 30000 });

    const environment = { ...process.env, MINI_IDLE_PUMP_TEMP: temporary, MINI_IDLE_PUMP_HELPER: helper };
    delete environment.ELECTRON_RUN_AS_NODE;
    child = spawn(process.execPath, [`--app=${__filename}`, '--idle-pump-browser'], {
      env: environment, stdio: ['ignore', 'pipe', 'pipe'],
    });
    let timedOut = false;
    watchdog = setTimeout(() => {
      timedOut = true;
      if (socket) socket.destroy();
      child.kill('SIGKILL');
    }, 30000);
    let stdout = '';
    let stderr = '';
    let pending = '';
    let sawPort = false;
    let socketResult = Promise.resolve({ response: '', error: new Error('No passive socket listener announced') });
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', chunk => { stderr += chunk; });
    child.stdout.setEncoding('utf8');
    child.stdout.on('data', chunk => {
      stdout += chunk;
      pending += chunk;
      const lines = pending.split('\n');
      pending = lines.pop();
      for (const line of lines) {
        const match = /^MINI_IDLE_PUMP_PORT:(\d+)$/.exec(line.trim());
        if (!match || sawPort) continue;
        sawPort = true;
        socketResult = new Promise(resolve => {
          let response = '';
          let failure;
          socket = net.connect({ port: Number(match[1]), host: '127.0.0.1' });
          socket.setEncoding('utf8');
          socket.on('connect', () => socket.write('external-wakeup'));
          socket.on('data', data => { response += data; });
          socket.on('error', error => { failure = error; });
          socket.on('close', () => resolve({ response, error: failure }));
        });
      }
    });
    const [code, signal] = await once(child, 'close');
    const result = await socketResult;
    assert.equal(timedOut, false, `Event-driven pump stalled\n${stdout}\n${stderr}`);
    assert.equal(signal, null, stderr);
    assert.equal(code, 0, `${stdout}\n${stderr}`);
    assert.equal(sawPort, true, 'The child reached its genuinely passive I/O wait');
    assert.equal(result.error, undefined, String(result.error));
    assert.equal(result.response, 'external-wakeup:ok', 'External socket activity woke libuv');
    assert.match(stdout, /PASS: native idle pump child/);
    console.log('PASS: event-driven AppKit/libuv timers, passive I/O, renderer IPC, native tracking/close, Node-only children and orderly quit');
  } finally {
    clearTimeout(watchdog);
    if (socket) socket.destroy();
    if (child && child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

async function exerciseBrowser() {
  const { app, BrowserWindow, ipcMain } = require('electron');
  const temporary = process.env.MINI_IDLE_PUMP_TEMP;
  app.setPath('userData', path.join(temporary, 'user-data'));
  app.on('window-all-closed', () => {});
  let window;
  try {
    await app.whenReady();
    // A shorter timer must replace a previously distant libuv sleep deadline.
    const distant = setTimeout(() => { throw new Error('A shorter timer never woke'); }, 60000);
    await new Promise(resolve => setTimeout(() => queueMicrotask(resolve), 15));
    clearTimeout(distant);

    // There are deliberately no browser-process watchdog/interval timers while
    // waiting here: only this socket's backend readiness can finish the wait.
    const server = net.createServer();
    await new Promise((resolve, reject) => {
      server.once('error', reject);
      server.listen(0, '127.0.0.1', resolve);
    });
    const received = new Promise((resolve, reject) => {
      server.once('error', reject);
      server.once('connection', socket => {
        socket.on('error', reject);
        let input = '';
        const receive = data => {
          input += data.toString();
          if (input.length < 'external-wakeup'.length) return;
          socket.off('data', receive);
          try { assert.equal(input, 'external-wakeup'); }
          catch (error) { reject(error); return; }
          setTimeout(() => { socket.end('external-wakeup:ok'); resolve(); }, 5);
        };
        socket.on('data', receive);
      });
    });
    console.log(`MINI_IDLE_PUMP_PORT:${server.address().port}`);
    await received;
    await new Promise(resolve => server.close(resolve));

    const file = path.join(temporary, 'worker-completion.txt');
    await fs.promises.writeFile(file, 'worker-wakeup');
    assert.equal(await fs.promises.readFile(file, 'utf8'), 'worker-wakeup');
    const nodeChild = await run(process.execPath, ['-e',
      'setTimeout(() => process.stdout.write("node-only-child"), 15)'], {
      env: { ...process.env, ELECTRON_RUN_AS_NODE: '1' },
    });
    assert.equal(nodeChild.stdout, 'node-only-child');

    const preload = path.join(temporary, 'preload.js');
    fs.writeFileSync(preload, `
      const { contextBridge, ipcRenderer } = require('electron');
      contextBridge.exposeInMainWorld('idlePump', {
        async exchange() {
          const value = await ipcRenderer.invoke('idle-pump:request', 'ping');
          await Promise.resolve();
          ipcRenderer.send('idle-pump:result', value);
        },
      });
    `);
    ipcMain.handle('idle-pump:request', async (_event, value) => {
      assert.equal(value, 'ping');
      return 'pong:' + await fs.promises.readFile(file, 'utf8');
    });
    const rendererReply = once(ipcMain, 'idle-pump:result');
    const title = `mini-idle-pump-${process.pid}`;
    window = new BrowserWindow({ width: 480, height: 320, title,
      webPreferences: { preload, sandbox: true, contextIsolation: true, nodeIntegration: false } });
    await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(
      `<!doctype html><title>${title}</title><p>Idle native pump</p>
       <script>setTimeout(() => idlePump.exchange(), 30)</script>`));
    const [event, reply] = await rendererReply;
    assert.equal(event.sender, window.webContents);
    assert.equal(reply, 'pong:worker-wakeup');
    assert.notEqual(window.webContents.getProcessId(), process.pid, 'Use the sandboxed renderer child');
    assert.equal(await window.webContents.executeJavaScript(
      'new Promise(resolve => setTimeout(() => resolve("renderer-delay"), 20))'), 'renderer-delay');
    assert.deepEqual(await window.webContents.executeJavaScript('[typeof require, typeof process]'),
      ['undefined', 'undefined']);

    const native = require(process.env.MINI_IDLE_PUMP_HELPER);
    // The existing helper posts real AppKit mouse down/up events. A native resize
    // callback then creates a near timer while a distant deadline was armed.
    await new Promise((resolve, reject) => {
      const distant = setTimeout(() => reject(new Error('Native resize never woke')), 60000);
      window.once('resize', () => {
        Promise.resolve().then(() => setTimeout(() => {
          clearTimeout(distant);
          resolve();
        }, 10)).catch(reject);
      });
      if (!native.click(title, 'zoom')) reject(new Error('Could not post native zoom events'));
    });
    const closed = once(window, 'closed');
    let closeEvents = 0;
    window.on('close', () => { ++closeEvents; });
    assert.equal(native.click(title, 'close'), true);
    await closed;
    window = undefined;
    assert.equal(closeEvents, 1, 'The native close button emits close exactly once');
    await new Promise(resolve => setImmediate(resolve));
    ipcMain.removeHandler('idle-pump:request');
    let beforeQuit = 0;
    let willQuit = 0;
    app.on('before-quit', () => { ++beforeQuit; });
    app.on('will-quit', () => { ++willQuit; });
    app.once('quit', () => {
      assert.equal(beforeQuit, 1);
      assert.equal(willQuit, 1);
      console.log('PASS: native idle pump child');
    });
    app.quit();
  } catch (error) {
    console.error(error.stack || error);
    if (window && !window.isDestroyed()) window.destroy();
    app.exit(1);
  }
}

(childMode ? exerciseBrowser() : supervise()).catch(error => {
  console.error(error.stack || error);
  process.exitCode = 1;
});
