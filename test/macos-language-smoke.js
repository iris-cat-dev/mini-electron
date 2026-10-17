'use strict';

// Run with the macOS runtime; -AppleLanguages '(en-US)' changes only this process.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, ipcMain } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-language-smoke-'));
let finished = false;
let window;
function finish(error, languages) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: macOS preferred languages reach the sandboxed navigator:', JSON.stringify(languages));
  app.exit(error ? 1 : 0);
}
const deadline = setTimeout(() => finish(new Error('Native language smoke timed out')), 15000);

app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin', 'This smoke exercises macOS system language preferences');
  const languages = app.getPreferredSystemLanguages();
  assert.equal(app.getLocale(), languages[0]);
  const expected = process.argv.find(arg => arg.startsWith('--expected-language='))?.split('=')[1];
  if (expected) assert.equal(languages[0], expected);
  const preload = path.join(temporary, 'preload.js');
  fs.writeFileSync(preload, "const {contextBridge,ipcRenderer}=require('electron');contextBridge.exposeInMainWorld('languageSmoke',{report:value=>ipcRenderer.send('smoke:language',value)});");
  ipcMain.on('smoke:language', (event, result) => {
    try {
      assert.equal(event.sender, window.webContents);
      assert.deepEqual(result.languages, languages, 'navigator must preserve the actual system preference order');
      assert.equal(result.language, languages[0]);
      finish(null, languages);
    } catch (error) { finish(error); }
  });
  window = new BrowserWindow({ width: 320, height: 200, webPreferences: { preload, sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent('<!doctype html><title>System language</title><script>languageSmoke.report({language:navigator.language,languages:Array.from(navigator.languages)});</script>'));
}).catch(finish);
