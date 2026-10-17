'use strict';

// Run with the native runtime, not Node: mini-electron test/ipc-frame-smoke.js.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, ipcMain, protocol } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-ipc-frame-smoke-'));
const frames = new Map();
const reports = new Set();
const deliveries = new Map();
let deliveryStarted = false;
let window;
let finished = false;

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: IPC frame authorization, identity, detachment, and target-only delivery');
  app.exit(error ? 1 : 0);
}
const deadline = setTimeout(() => finish(new Error('Native IPC frame smoke timed out')), 20000);

protocol.registerSchemesAsPrivileged([
  { scheme: 'mini-ipc-frame-smoke', privileges: { standard: true, secure: true, supportFetchAPI: true } },
]);

app.whenReady().then(async () => {
  const preload = path.join(temporary, 'preload.js');
  fs.writeFileSync(preload, `const {contextBridge,ipcRenderer}=require('electron');
    const deliveries = [];
    ipcRenderer.on('smoke:deliver', (_event, label) => {
      deliveries.push(label);
      if (label === 'all') {
        const role = new URL(location.href).pathname.slice(1) || 'main';
        ipcRenderer.send('smoke:delivery', role, deliveries);
      }
    });
    contextBridge.exposeInMainWorld('frameSmoke', {
      inspect: role => ipcRenderer.invoke('smoke:inspect', role),
      report: (role, error) => ipcRenderer.send('smoke:report', role, error)
    });`);
  ipcMain.handle('smoke:inspect', (event, role) => {
    try {
      assert.equal(event.sender, window.webContents);
      assert.ok(event.senderFrame, 'IPC senderFrame must identify the native source frame');
      assert.equal(event.senderFrame.detached, false);
      assert.equal(event.senderFrame === event.sender.mainFrame, role !== 'child', 'a subframe must never acquire main-frame authority');
      assert.equal(event.sender.mainFrame, event.sender.mainFrame, 'mainFrame identity must be stable');
      if (frames.has(role)) assert.equal(event.senderFrame, frames.get(role), 'send/invoke must share the same frame identity');
      else frames.set(role, event.senderFrame);
      if (role === 'second') {
        assert.notEqual(event.senderFrame, frames.get('main'));
        assert.equal(frames.get('main').detached, true, 'replaced main document must be detached');
        assert.equal(frames.get('child').detached, true, 'old subframe must be detached after navigation');
        assert.throws(() => frames.get('main').send('smoke:deliver', 'stale'), /detached/);
        assert.equal(window.webContents.sendToFrame(
          frames.get('main').routingId, 'smoke:deliver', 'stale'), false);
      }
      return { authorized: event.senderFrame === event.sender.mainFrame };
    } catch (error) { finish(error); throw error; }
  });
  ipcMain.on('smoke:report', (event, role, error) => {
    try {
      assert.equal(error, null);
      assert.equal(event.senderFrame, frames.get(role), 'send and invoke must preserve frame identity');
      reports.add(role);
      if (role === 'second') return finish();
      if (!deliveryStarted && reports.has('main') && reports.has('child')) {
        deliveryStarted = true;
        assert.equal(window.webContents.sendToFrame(Number.NaN, 'smoke:deliver', 'invalid'), false);
        assert.equal(window.webContents.send('smoke:deliver', 'main'), true);
        assert.equal(window.webContents.sendToFrame(
          frames.get('child').routingId, 'smoke:deliver', 'public-child'), true);
        assert.equal(frames.get('child').send('smoke:deliver', 'sender-child'), true);
        assert.equal(window.webContents.sendToAll('smoke:deliver', 'all'), true);
      }
    } catch (error) { finish(error); }
  });
  ipcMain.on('smoke:delivery', (_event, role, received) => {
    try {
      deliveries.set(role, received);
      if (deliveries.has('main') && deliveries.has('child')) {
        assert.deepEqual(deliveries.get('main'), ['main', 'all']);
        assert.deepEqual(deliveries.get('child'), ['public-child', 'sender-child', 'all']);
        void window.loadURL('mini-ipc-frame-smoke://app/second').catch(finish);
      }
    } catch (error) { finish(error); }
  });
  protocol.handle('mini-ipc-frame-smoke', request => {
    const role = new URL(request.url).pathname.slice(1) || 'main';
    return new Response(`<!doctype html><title>IPC frame smoke</title><script>
      (async () => {
        const role = ${JSON.stringify(role)};
        try {
          for (let i = 0; i < 2; ++i) {
            const result = await frameSmoke.inspect(role);
            if (result.authorized !== (role !== 'child')) throw new Error('Incorrect main-frame authorization');
          }
          if (role === 'main') {
            const child = document.createElement('iframe');
            child.src = 'mini-ipc-frame-smoke://app/child';
            document.body.append(child);
          }
          frameSmoke.report(role, null);
        } catch (error) { frameSmoke.report(role, String(error.stack || error)); }
      })();
    </script>`, { headers: { 'content-type': 'text/html' } });
  });
  window = new BrowserWindow({ width: 320, height: 200, webPreferences: { preload, sandbox: true, contextIsolation: true, nodeIntegration: false, nodeIntegrationInSubFrames: true } });
  await window.loadURL('mini-ipc-frame-smoke://app/');
}).catch(finish);
