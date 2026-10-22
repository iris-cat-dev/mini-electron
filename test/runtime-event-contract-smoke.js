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
  else console.log('PASS: generic native events have preventDefault but no IPC sendReply');
  app.exit(error ? 1 : 0);
}

app.whenReady().then(async () => {
  window = new BrowserWindow({ show: false, webPreferences: { sandbox: true } });
  await window.loadURL('data:text/html,<title>native event contract</title>');
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
