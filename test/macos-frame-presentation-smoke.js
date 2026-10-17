'use strict';

// Requires a source checkout and Xcode; exercises real AppKit drawing, not capturePage().
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, ipcMain } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-frame-presentation-'));
let finished = false;
let window;
let initial;
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: native presentation retains a complete frame during writer contention and presents later frames');
  app.exit(error ? 1 : 0);
}
const deadline = setTimeout(() => finish(new Error('Native frame presentation smoke timed out')), 40000);

app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const root = path.resolve(__dirname, '..');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'presentation.node');
  const includes = ['', 'gen', 'third_party/abseil-cpp', 'base/allocator/partition_allocator/src', 'gen/base/allocator/partition_allocator/src', 'third_party/libnode/src'];
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup', '-isysroot', sdk,
    ...includes.map(include => `-I${path.join(root, include)}`), '-framework', 'AppKit',
    path.join(__dirname, 'macos-frame-presentation-helper.mm'), '-o', helper], { stdio: 'inherit', timeout: 30000 });
  const { probe } = require(helper);
  const preload = path.join(temporary, 'preload.js');
  fs.writeFileSync(preload, "const {contextBridge,ipcRenderer}=require('electron');contextBridge.exposeInMainWorld('presentationSmoke',{report:phase=>ipcRenderer.send('smoke:presentation',phase)});");
  ipcMain.on('smoke:presentation', async (_event, phase) => {
    try {
      if (phase === 'initial') assert.equal(window.isVisible(), false);
      // Static content has no outstanding animation: preparation must force
      // its own causal compositor commit, not await an arbitrary later paint.
      await window.webContents.prepareToShow();
      if (phase === 'initial') window.show();
    } catch (error) { finish(error); return; }
    setTimeout(() => {
      try {
        const result = probe(window.webContents.getProcessId(), () => window.webContents.invalidate());
        assert.equal(result.differentBytes, 0, 'An in-progress shared frame must not replace the visible frame with the window background');
        assert.deepEqual(result.pixel, phase === 'initial' ? [37, 118, 162, 255] : [196, 75, 25, 255]);
        if (phase === 'initial') {
          initial = result;
          void window.webContents.executeJavaScript('nextPresentationFrame()').catch(finish);
        } else {
          assert.ok(result.generation > initial.generation, 'A later complete frame must still be published');
          finish();
        }
      } catch (error) { finish(error); }
    }, 100);
  });
  window = new BrowserWindow({ show: false, width: 800, height: 600, frame: false, webPreferences: { preload, sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><style>body{margin:0;background:#2576a2}</style><script>
    const report = phase => requestAnimationFrame(() => requestAnimationFrame(() => presentationSmoke.report(phase)));
    window.nextPresentationFrame = () => { document.body.style.background = '#c44b19'; report('updated'); };
    addEventListener('DOMContentLoaded', () => report('initial'));
  </script>`));
}).catch(finish);
