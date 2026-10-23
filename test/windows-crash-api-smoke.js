'use strict';

// Run on an unlocked Windows desktop:
// out/windows-x64/stage/mini-electron.exe --app=test/windows-crash-api-smoke.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, BrowserView, Menu, shell } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-crash-api-'));
app.setPath('userData', path.join(temporary, 'user-data'));
const windows = [];
let finished = false;
const timeout = setTimeout(() => finish(new Error('Windows crash API smoke timed out')), 60000);
app.on('window-all-closed', () => {});

function create(options) {
  const window = new BrowserWindow({ show: false, minWidth: 1, minHeight: 1, ...options });
  windows.push(window);
  return window;
}
function near(actual, expected, label) {
  assert.ok(Math.abs(actual - expected) <= 2, `${label}: ${actual} != ${expected}`);
}
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(timeout);
  for (const window of windows.reverse())
    if (!window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: Windows geometry, unsupported window operations, logging and recycling data preservation');
  app.exit(error ? 1 : 0);
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  const parent = create({ title: `mini-crash-api-${process.pid}`, x: 100, y: 100, width: 480, height: 380 });
  await parent.loadURL(`data:text/html,<title>mini-crash-api-${process.pid}</title>`);
  const boundsBefore = parent.getBounds();
  const windowsBefore = BrowserWindow.getAllWindows().map(window => window.id);
  for (const operation of [
    () => new BrowserWindow({ parent, show: false }),
    () => new BrowserWindow({ modal: true, show: false }),
    () => new BrowserWindow({ kiosk: true, show: false }),
    () => parent.setParentWindow(null),
    () => parent.setAspectRatio(2),
    () => parent.setKiosk(true),
  ]) {
    assert.throws(operation, { name: 'Error' });
  }
  assert.deepEqual(BrowserWindow.getAllWindows().map(window => window.id), windowsBefore,
    'rejected constructors must not leave registered windows');
  assert.deepEqual(parent.getBounds(), boundsBefore, 'unsupported operations must not resize the live window');
  assert.equal(await parent.webContents.executeJavaScript('document.title'), `mini-crash-api-${process.pid}`);
  assert.throws(() => new BrowserWindow({ parent: {}, show: false }), TypeError);
  assert.throws(() => new BrowserWindow(null), TypeError);
  for (const webContents of [{}, null, parent.webContents]) {
    assert.throws(() => new BrowserWindow({ webContents, show: false }), TypeError);
  }
  assert.deepEqual(BrowserWindow.getAllWindows().map(window => window.id), windowsBefore,
    'invalid options and rejected WebContents adoption must not leave registered windows');
  const defaultWindow = new BrowserWindow(undefined);
  defaultWindow.destroy();
  assert.throws(() => parent.setParentWindow({}), TypeError);

  for (const frame of [true, false]) {
    const window = create({ frame });
    const wanted = { x: 150, y: 140, width: 320, height: 220 };
    window.setContentBounds(wanted);
    const actual = window.getContentBounds();
    for (const key of Object.keys(wanted)) near(actual[key], wanted[key], `frame=${frame} content.${key}`);
    window.setContentBounds({ width: 340 });
    const changed = window.getContentBounds();
    near(changed.width, 340, 'partial width');
    near(changed.x, actual.x, 'partial x preservation');
    near(changed.height, actual.height, 'partial height preservation');
    window.destroy();
  }


  const view = new BrowserView(undefined);
  parent.addBrowserView(view);
  assert.throws(() => new BrowserView({ webContents: {} }), TypeError);
  assert.throws(() => new BrowserView({ webContents: parent.webContents }), TypeError);
  assert.throws(() => new BrowserView(null), TypeError);
  parent.removeBrowserView(view);
  // Close this unattached view's renderer explicitly.
  view.webContents.destroy();
  const menu = new Menu();
  assert.throws(() => menu._sendActionToFirstResponder('copy:'), { name: 'Error' });
  const { Protocol } = process._linkedBinding('electron_browser_protocol');
  assert.throws(() => Protocol(() => {}), TypeError);
  assert.throws(() => new Protocol(null), TypeError);

  process.log('windows-crash-api-log', 42);
  const conversionError = new Error('log-conversion');
  assert.throws(() => process.log({ toString() { throw conversionError; } }), error => error === conversionError);
  const trashFile = path.join(temporary, `recycle-${process.pid}.txt`);
  fs.writeFileSync(trashFile, 'recycle, do not permanently delete');
  assert.equal(shell.moveItemToTrash(trashFile), false, 'unsupported recycling must report failure');
  assert.equal(fs.readFileSync(trashFile, 'utf8'), 'recycle, do not permanently delete');
  assert.equal(shell.moveItemToTrash(path.join(temporary, 'missing.txt')), false);
  assert.throws(() => shell.moveItemToTrash(), 'argument-count validation remains active');
  assert.throws(() => shell.moveItemToTrash({}), 'argument-type validation remains active');
  const trashDirectory = path.join(temporary, `recycle-directory-${process.pid}`);
  fs.mkdirSync(trashDirectory);
  fs.writeFileSync(path.join(trashDirectory, 'child.txt'), 'nested recycle');
  assert.equal(shell.moveItemToTrash(trashDirectory), false);
  assert.equal(fs.readFileSync(path.join(trashDirectory, 'child.txt'), 'utf8'), 'nested recycle');

  assert.throws(() => parent.setAspectRatio(NaN), TypeError);
  assert.throws(() => parent.setAspectRatio(-1), TypeError);
  finish();
}).catch(finish);
