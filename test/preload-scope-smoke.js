'use strict';

// Run with the native runtime: mini-electron test/preload-scope-smoke.js.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, ipcMain, session } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-preload-scope-'));
app.setPath('userData', path.join(temporary, 'user-data'));
app.on('window-all-closed', () => {});
const windows = new Set();
const sessions = [];
let finished = false;

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  for (const window of windows) if (!window.isDestroyed()) window.destroy();
  for (const current of sessions) current.setPreloads([]);
  ipcMain.removeHandler('preload-scope:inspect');
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: preload-local electron bindings, independent scopes, navigation, IPC, and unsuppressed preload errors (contextIsolation on/off)');
  app.exit(error ? 1 : 0);
}
const deadline = setTimeout(() => finish(new Error('Preload scope smoke timed out')), 30000);

app.whenReady().then(async () => {
  const preloads = ['let', 'var', 'const'].map((declaration, index) => {
    const filename = path.join(temporary, `preload-${declaration}.js`);
    const name = `preloadScope${index}`;
    // Match electron-log's top-level let/assignment; repeat private names in
    // independent preloads to catch shared lexical or global bindings too.
    fs.writeFileSync(filename, `'use strict';
      ${declaration} electron = ${declaration === 'const' ? "require('electron')" : '{}'};
      ${declaration === 'const' ? '' : "electron = require('electron');"}
      const privateValue = ${JSON.stringify(declaration)};
      const api = {
        inspect: () => electron.ipcRenderer.invoke('preload-scope:inspect', privateValue)
      };
      if (process.contextIsolated) electron.contextBridge.exposeInMainWorld(${JSON.stringify(name)}, api);
      else window[${JSON.stringify(name)}] = api;
    `);
    return filename;
  });
  const syntaxFailure = path.join(temporary, 'syntax-failure.js');
  const runtimeFailure = path.join(temporary, 'runtime-failure.js');
  fs.writeFileSync(syntaxFailure, 'const invalid = ;');
  fs.writeFileSync(runtimeFailure, "throw new Error('intentional preload failure');");
  const pages = ['first', 'second'].map(label => {
    const filename = path.join(temporary, `${label}.html`);
    fs.writeFileSync(filename, `<!doctype html><title>Preload scope ${label}</title><script>
      let electron = 'page electron';
      const privateValue = 'page private value';
      window.pageScope = [electron, privateValue];
    </script>`);
    return filename;
  });

  ipcMain.handle('preload-scope:inspect', (event, label) => {
    assert.ok([...windows].some(window => window.webContents === event.sender));
    assert.equal(event.senderFrame, event.sender.mainFrame);
    return { label, contentsId: event.sender.id };
  });

  for (const contextIsolation of [true, false]) {
    const partition = `preload-scope-${contextIsolation}`;
    const current = session.fromPartition(partition);
    sessions.push(current);
    // Failures must not stop later session/window preloads from executing.
    current.setPreloads([preloads[0], syntaxFailure, runtimeFailure, preloads[1]]);
    const window = new BrowserWindow({
      width: 320, height: 200, show: false,
      webPreferences: { partition, preload: preloads[2], sandbox: true, contextIsolation, nodeIntegration: false },
    });
    windows.add(window);
    const errors = [];
    window.webContents.on('preload-error', (_event, filename, error) => errors.push({ filename, error }));
    window.webContents.on('render-process-gone', (_event, details) =>
      finish(new Error(`Renderer exited: ${JSON.stringify(details)}`)));

    // Initial load, cross-document navigation, and same-URL reload.
    for (const filename of [pages[0], pages[1], pages[1]]) {
      errors.length = 0;
      await window.loadFile(filename);
      assert.deepEqual(errors.map(entry => entry.filename), [syntaxFailure, runtimeFailure],
        `Only intentional failures should be reported (contextIsolation=${contextIsolation}): ${errors.map(entry => entry.error.message).join('; ')}`);
      assert.match(errors[0].error.message, /SyntaxError/);
      assert.match(errors[1].error.message, /intentional preload failure/);
      assert.ok(errors[1].error.stack.includes(runtimeFailure), 'Runtime errors must retain the preload filename');
      const snapshot = await window.webContents.executeJavaScript(`(async () => ({
        page: pageScope,
        leakedElectron: Object.prototype.hasOwnProperty.call(window, 'electron'),
        leakedPrivate: Object.prototype.hasOwnProperty.call(window, 'privateValue'),
        results: await Promise.all([preloadScope0.inspect(), preloadScope1.inspect(), preloadScope2.inspect()])
      }))()`);
      assert.deepEqual(snapshot, {
        page: ['page electron', 'page private value'],
        leakedElectron: false,
        leakedPrivate: false,
        results: ['let', 'var', 'const'].map(label => ({ label, contentsId: window.webContents.id })),
      });
    }
    window.destroy();
    windows.delete(window);
  }
  finish();
}).catch(finish);
