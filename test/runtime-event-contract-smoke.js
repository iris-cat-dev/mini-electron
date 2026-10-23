'use strict';

// Run with the native runtime, not Node or the npm installer.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-runtime-event-'));
app.setPath('userData', path.join(temporary, 'user-data'));
app.on('window-all-closed', () => {});
let window;
let finished = false;
const deadline = setTimeout(() => finish(new Error('Native event contract smoke timed out')), 15000);

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: generic native events reject IPC replies; ready-to-show destruction is reentrant-safe');
  app.exit(error ? 1 : 0);
}

app.whenReady().then(async () => {
  window = new BrowserWindow({ show: false, webPreferences: { sandbox: true } });
  const closingContents = window.webContents;
  const closingId = window.id;
  let inReady = false;
  let closedDuringReady = false;
  window.once('closed', () => { closedDuringReady = inReady; });
  const readyClosed = new Promise(resolve => {
    window.once('ready-to-show', () => {
      inReady = true;
      window.destroy();
      inReady = false;
      // The native observer dispatch must unwind before the next window opens.
      setTimeout(resolve, 0);
    });
  });
  // Closing during first paint can cancel navigation; its settlement is not the
  // assertion. The synchronous closed event and subsequent live window are.
  await Promise.allSettled([
    window.loadURL('data:text/html,<title>close inside ready-to-show</title>'),
    readyClosed,
  ]);
  assert.equal(closedDuringReady, true, 'destroy must close synchronously inside ready-to-show');
  assert.equal(closingContents.isDestroyed(), true);
  assert.equal(BrowserWindow.getAllWindows().some(candidate => candidate.id === closingId), false);

  window = new BrowserWindow({ show: false, webPreferences: { sandbox: true } });
  await window.loadURL('data:text/html,<title>native event contract</title>');
  assert.equal(await window.webContents.executeJavaScript('document.title'), 'native event contract',
    'a fresh renderer must remain usable after reentrant observer removal');
  window.once('close', event => event.preventDefault());
  window.close();
  assert.equal(window.isDestroyed(), false, 'preventDefault must cancel closing');

  let destroyedEvent;
  window.webContents.once('destroyed', event => { destroyedEvent = event; });
  window.destroy();
  assert.ok(destroyedEvent, 'native destroyed event must be delivered');
  assert.throws(() => destroyedEvent.sendReply('not-an-IPC-event'), TypeError);
  finish();
}).catch(finish);
