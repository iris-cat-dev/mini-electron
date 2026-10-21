'use strict';

// Run with a bundled macOS runtime in a graphical login session. The native
// helper targets only this process's renderer and observes WindowServer's hang
// flag across its timeout; JavaScript liveness alone cannot detect this bug.
const assert = require('node:assert/strict');
const { execFile, execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { promisify } = require('node:util');
const { app, BrowserWindow } = require('electron');
const run = promisify(execFile);
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-renderer-responsive-'));
app.setPath('userData', path.join(temporary, 'user-data'));
app.on('window-all-closed', () => {});
let window;
let finished = false;
const watchdog = setTimeout(() => finish(new Error('Renderer responsiveness smoke timed out')), 60000);

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: native renderer application events, page interaction, sandbox isolation and clean shutdown');
  app.exit(error ? 1 : 0);
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin', 'This regression requires macOS');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'renderer-responsiveness');
  execFileSync(compiler, ['-std=c++17', '-fobjc-arc', '-Wno-deprecated-declarations', '-isysroot', sdk,
    '-framework', 'AppKit', '-framework', 'Carbon', path.join(__dirname, 'macos-renderer-responsiveness-helper.mm'), '-o', helper],
  { stdio: 'inherit', timeout: 30000 });

  window = new BrowserWindow({ width: 420, height: 240, show: false,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  let rendererGone;
  window.webContents.on('render-process-gone', (_event, details) => { rendererGone = details; });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(
    '<!doctype html><title>renderer responsiveness</title><button onclick="document.querySelector(\'output\').value=\'clicked\'">Click</button><output>waiting</output>'));
  const renderer = window.webContents.getProcessId();
  assert.notEqual(renderer, process.pid, 'Exercise the sandboxed renderer child, not single-process mode');
  const result = await run(helper, [String(process.pid), String(renderer)], { timeout: 25000 });
  console.log(result.stdout.trim());
  assert.equal(rendererGone, undefined, 'Handling native application events must not terminate the renderer');
  assert.deepEqual(await window.webContents.executeJavaScript('[typeof require, typeof process]'), ['undefined', 'undefined']);
  assert.equal(await window.webContents.executeJavaScript(
    'document.querySelector("button").click(); document.querySelector("output").value'), 'clicked');

  window.destroy();
  const exitDeadline = Date.now() + 5000;
  while (Date.now() < exitDeadline) {
    try { process.kill(renderer, 0); }
    catch (error) {
      if (error.code === 'ESRCH') return finish();
      throw error;
    }
    await pause(25);
  }
  throw new Error('Closing the owned window left its native renderer event loop running');
}).catch(finish);
