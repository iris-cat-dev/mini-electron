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
async function eventually(predicate, label) {
  for (let attempt = 0; attempt < 80; ++attempt) {
    const result = predicate();
    if (result) return result;
    await new Promise(resolve => setTimeout(resolve, 25));
  }
  throw new Error(`Timed out waiting for ${label}`);
}


protocol.registerSchemesAsPrivileged([
  { scheme: 'mini-ipc-frame-smoke', privileges: { standard: true, secure: true, supportFetchAPI: true } },
]);
async function exerciseFocusedFrameDetach() {
  const blocker = new BrowserWindow({ width: 120, height: 90 });
  try {
    await blocker.loadURL('data:text/html,<title>focus blocker</title>');
    blocker.focus();
    await eventually(() => blocker.isFocused(), 'focus blocker');

    await window.webContents.executeJavaScript(`new Promise(resolve => {
      const frame = document.createElement('iframe');
      frame.name = 'lifetime-detach';
      frame.src = 'mini-ipc-frame-smoke://app/lifetime-detach';
      frame.style.cssText = 'position:fixed;left:0;top:0;width:100px;height:100px;border:0;z-index:2147483647';
      frame.onload = () => resolve(true);
      document.body.append(frame);
    })`, true);
    const detachedFrame = await eventually(() => reports.has('lifetime-detach') && frames.get('lifetime-detach'),
      'transient child frame IPC registration');

    // A trusted child-frame input gives top.focus() the interaction token that
    // makes Blink enqueue FocusPage and DidCallFocus. The same handler removes
    // the frame before the native replies return.
    window.webContents.sendInputEvent(
      { type: 'mouseDown', x: 20, y: 20, button: 'left', clickCount: 1 });
    window.webContents.sendInputEvent(
      { type: 'mouseUp', x: 20, y: 20, button: 'left', clickCount: 1 });
    await eventually(() => detachedFrame.detached,
      'focused child frame detach');
    blocker.focus();
    await eventually(() => blocker.isFocused(),
      'focus blocker after child-frame detach');
    await window.webContents.executeJavaScript(`(() => {
      const target = document.createElement('div');
      target.id = 'native-focus-target';
      target.style.cssText = 'position:fixed;left:0;top:0;width:100px;height:100px;z-index:2147483647';
      document.body.append(target);
      return true;
    })()`);

    window.webContents.sendInputEvent(
      { type: 'mouseDown', x: 20, y: 20, button: 'left', clickCount: 1 });
    window.webContents.sendInputEvent(
      { type: 'mouseUp', x: 20, y: 20, button: 'left', clickCount: 1 });
    const result = await window.webContents.executeJavaScript(`(async () => {
      const inspection = await frameSmoke.inspect('main');
      return {
        active: document.activeElement && document.activeElement.id,
        authorized: inspection.authorized
      };
    })()`);
    assert.equal(detachedFrame.detached, true);
    assert.equal(result.active, 'live-after-detach');
    assert.equal(result.authorized, true);
    await eventually(() => window.isFocused(),
      'live main-frame native focus after focused iframe detach');
  } finally {
    if (!blocker.isDestroyed()) blocker.close();
  }
}


app.whenReady().then(async () => {
  const preload = path.join(temporary, 'preload.js');
  fs.writeFileSync(preload, `const {contextBridge,ipcRenderer}=require('electron');
    const role = new URL(location.href).pathname.slice(1) || 'main';
    const deliveries = [];
    ipcRenderer.on('smoke:deliver', (_event, label) => {
      deliveries.push(label);
      if (label === 'all') ipcRenderer.send('smoke:delivery', role, deliveries);
    });
    if (role === 'lifetime-detach') {
      window.addEventListener('mousedown', () => {
        window.focus();
        top.focus();
        const input = document.createElement('input');
        input.id = 'detached-focus';
        document.body.append(input);
        input.focus();
        frameElement.remove();
      }, { once: true });
    } else if (role === 'main') {
      window.addEventListener('mousedown', event => {
        event.preventDefault();
        const input = document.createElement('input');
        input.id = 'live-after-detach';
        document.body.append(input);
        input.focus();
        window.focus();
      }, { once: true });
    }
    contextBridge.exposeInMainWorld('frameSmoke', {
      inspect: role => ipcRenderer.invoke('smoke:inspect', role),
      report: (role, error) => ipcRenderer.send('smoke:report', role, error)
    });`);
  ipcMain.handle('smoke:inspect', (event, role) => {
    try {
      assert.equal(event.sender, window.webContents);
      assert.ok(event.senderFrame, 'IPC senderFrame must identify the native source frame');
      assert.equal(event.senderFrame.detached, false);
      const authorized = event.senderFrame === event.sender.mainFrame;
      assert.equal(authorized, role === 'main' || role === 'second',
        'a subframe must never acquire main-frame authority');
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
      return { authorized };
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
        void exerciseFocusedFrameDetach()
          .then(() => window.loadURL('mini-ipc-frame-smoke://app/second'))
          .catch(finish);
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
            if (result.authorized !== (role === 'main' || role === 'second')) throw new Error('Incorrect main-frame authorization');
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
