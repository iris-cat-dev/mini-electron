'use strict';

// Requires unmodified electron-log@5.4.4 through normal Node module resolution.
// Run in native application mode, with or without --single-process.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-electron-log-'));
const windows = new Set();
const expected = new Set();
const received = new Map();
const preloadErrors = [];
const rendererLosses = [];
let finished = false;
let documentLoads = 0;

app.setPath('userData', path.join(temporary, 'profile'));
app.on('window-all-closed', () => {});
const log = require('electron-log/main');
log.transports.console.level = 'info';
log.transports.file.resolvePathFn = () => path.join(temporary, 'renderer.log');
// Hooks run once per transport; count the user's console consumer, not each sink.
log.hooks.push((message, _transport, transportName) => {
  for (const item of message.data || []) {
    if (transportName === 'console' && typeof item === 'string'
        && item.startsWith('mini-electron-log-smoke|')) {
      received.set(item, (received.get(item) || 0) + 1);
    }
  }
  return message;
});
// This is the real issue #5 initialization, not a substitute IPC logger.
log.initialize({ spyRendererConsole: true });

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  try {
    for (const window of windows) {
      if (!window.isDestroyed()) window.destroy();
    }
    fs.rmSync(temporary, { recursive: true, force: true });
  } catch (cleanupError) {
    if (!error) error = cleanupError;
  }
  if (error) console.error(error.stack || error);
  else console.log('PASS: unmodified electron-log console spy and preload IPC, 4 windows, 12 document loads, 24 exactly-once messages, contextIsolation on/off');
  app.exit(error ? 1 : 0);
}

const deadline = setTimeout(() => finish(new Error('electron-log native smoke timed out')), 45000);

async function waitForMessages(messages) {
  const end = Date.now() + 10000;
  while (messages.some(message => !received.has(message))) {
    if (Date.now() >= end) {
      assert.fail('Missing main logger messages: ' + messages.filter(message => !received.has(message)).join(', '));
    }
    await new Promise(resolve => setTimeout(resolve, 20));
  }
}

function reload(contents) {
  return new Promise((resolve, reject) => {
    const cleanup = () => {
      contents.removeListener('did-finish-load', loaded);
      contents.removeListener('did-fail-load', failed);
    };
    const loaded = () => { cleanup(); resolve(); };
    const failed = (_event, code, description, url, isMainFrame) => {
      if (!isMainFrame) return;
      cleanup();
      reject(new Error(`Reload failed (${code} ${description}): ${url}`));
    };
    contents.once('did-finish-load', loaded);
    contents.on('did-fail-load', failed);
    contents.reload();
  });
}

app.whenReady().then(async () => {
  const pages = ['first', 'second'].map(label => {
    const filename = path.join(temporary, label + '.html');
    fs.writeFileSync(filename, `<!doctype html><title>electron-log ${label}</title>
<script>
let electron = 'page electron';
const privateValue = 'page private';
window.pageScope = [electron, privateValue];
</script>`);
    return filename;
  });

  for (const contextIsolation of [true, false]) {
    const pair = [0, 1].map(index => {
      const window = new BrowserWindow({
        show: false, width: 360, height: 240,
        webPreferences: { sandbox: true, nodeIntegration: false, contextIsolation },
      });
      windows.add(window);
      window.webContents.on('preload-error', (_event, filename, error) => {
        preloadErrors.push({ contextIsolation, index, filename, message: error.message });
      });
      window.webContents.on('render-process-gone', (_event, details) => {
        rendererLosses.push({ contextIsolation, index, details });
      });
      return window;
    });

    for (let navigation = 0; navigation < 3; ++navigation) {
      for (let index = 0; index < pair.length; ++index) {
        const window = pair[index];
        if (navigation === 2) await reload(window.webContents);
        else await window.loadFile(pages[navigation]);
        const snapshot = await window.webContents.executeJavaScript(`({
          page: pageScope,
          logging: typeof window.__electronLog?.info,
          leakedElectron: Object.prototype.hasOwnProperty.call(window, 'electron')
        })`);
        assert.deepEqual(snapshot, {
          page: ['page electron', 'page private'], logging: 'function', leakedElectron: false,
        });
        assert.deepEqual(preloadErrors, [], 'the real logging preload must execute');
        assert.deepEqual(rendererLosses, [], 'logging must preserve the renderer');

        const identity = `${contextIsolation}:${index}:${navigation}`;
        const ipc = 'mini-electron-log-smoke|ipc|' + identity;
        const consoleMessage = 'mini-electron-log-smoke|console|' + identity;
        expected.add(ipc);
        expected.add(consoleMessage);
        await window.webContents.executeJavaScript(`
          window.__electronLog.info(${JSON.stringify(ipc)});
          console.info(${JSON.stringify(consoleMessage)});
          true;
        `);
        await waitForMessages([ipc, consoleMessage]);
        ++documentLoads;
      }
    }
    for (const window of pair) window.destroy();
  }

  // Keep the real spy active briefly so duplicate subscriptions cannot win a race.
  await new Promise(resolve => setTimeout(resolve, 200));
  assert.equal(documentLoads, 12);
  assert.equal(received.size, expected.size);
  for (const message of expected) {
    assert.equal(received.get(message), 1, `${message} reaches the main logger exactly once`);
  }
  assert.deepEqual(preloadErrors, []);
  assert.deepEqual(rendererLosses, []);
  finish();
}).catch(finish);
