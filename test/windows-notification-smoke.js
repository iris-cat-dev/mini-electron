'use strict';

// Run with the freshly rebuilt native Windows runtime, not Node.
// Terminal callbacks are synthetic VERSION_4 messages sent only to this PID.
// --natural-timeout also waits for an actual Shell timeout (unlocked desktop,
// notifications enabled); it never substitutes a synthetic timeout on failure.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, Notification, Tray, nativeImage } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-notification-'));
app.setPath('userData', path.join(temporary, 'profile'));
app.on('window-all-closed', () => {});
const retained = [];
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const observations = [];
let helper, tray, finished = false;
const deadline = setTimeout(() => finish(new Error('Windows notification smoke timed out')), 180000);
const image = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==';

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  for (const entry of retained) entry.notification.close();
  if (tray) tray.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: Windows notification VERSION_4 timeout/hide/click, explicit close, duplicate callbacks, hidden temporary icons and independent resident Tray');
  console.log(JSON.stringify({ observations }));
  app.exit(error ? 1 : 0);
}

function native(operation, ...args) {
  return execFileSync(helper, [operation, String(process.pid), ...args.map(String)],
    { encoding: 'utf8', timeout: 10000, windowsHide: true }).trim();
}
function inspect(ids, operation = 'inspect') {
  return JSON.parse(native(operation, ids.join(',')));
}
function show(id, icon = false) {
  const notification = new Notification({ title: `mini notification ${id}`, body: 'Native lifecycle regression',
    silent: true, ...(icon ? { icon: nativeImage.createFromDataURL(image).resize({ width: 32, height: 32 }) } : {}) });
  const entry = { id, notification, events: [] };
  retained.push(entry);
  for (const event of ['show', 'click', 'close', 'failed']) notification.on(event, () => entry.events.push(event));
  notification.on('failed', (_event, error) => { entry.failure = error; });
  notification.show();
  assert.deepEqual(entry.events, ['show'], entry.failure);
  return entry;
}
async function until(predicate, label, timeout = 6000) {
  const end = Date.now() + timeout;
  while (!predicate()) {
    assert.ok(Date.now() < end, label);
    await pause(20);
  }
}
async function post(id, event, wparam = 0) {
  assert.equal(native('post', id, event, wparam), 'POSTED');
  await pause(50);
}
function checkResidentTray() {
  assert.equal(inspect([130], 'tray')[0].registered, true, 'notification cleanup must preserve the application Tray');
}
function checkEmpty(ids) {
  const state = inspect(ids);
  assert.ok(state.every(item => !item.registered), `temporary icons must be deleted: ${JSON.stringify(state)}`);
  checkResidentTray();
  return state;
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  assert.equal(Notification.isSupported(), true);
  const compiler = path.join(process.env.WINDIR || 'C:\\Windows', 'Microsoft.NET', 'Framework64', 'v4.0.30319', 'csc.exe');
  helper = path.join(temporary, 'notification-helper.exe');
  execFileSync(compiler, ['/nologo', '/platform:x64', '/target:exe', `/out:${helper}`,
    path.join(__dirname, 'windows-notification-helper.cs')], { stdio: 'inherit', timeout: 30000, windowsHide: true });
  tray = new Tray(nativeImage.createFromDataURL(image).resize({ width: 32, height: 32 }));
  tray.setToolTip(`mini notification resident tray ${process.pid}`);
  checkResidentTray();

  const entries = [show(1), show(2, true), show(3), show(4)];
  const active = inspect([1, 2, 3, 4]);
  assert.ok(active.every(item => item.registered), 'each active balloon has an owned Shell registration');
  assert.ok(active.every(item => !item.positioned), 'NIS_HIDDEN temporary icons must not occupy tray positions');
  observations.push({ phase: 'active', state: active });

  await post(1, 0x400); // NIN_BALLOONSHOW is not a terminal event.
  assert.deepEqual(entries[0].events, ['show']);
  await post(65000, 0x404, 2); // Unknown packed ID must not fall back to wParam.
  assert.deepEqual(entries[1].events, ['show']);
  await post(1, 0x404, 2); // NIN_BALLOONTIMEOUT; wrong wParam names another live ID.
  await until(() => entries[0].events.includes('close'), 'VERSION_4 timeout must close ID 1');
  assert.deepEqual(entries[0].events, ['show', 'close']);
  assert.deepEqual(entries[1].events, ['show']);
  assert.equal(inspect([1])[0].registered, false);

  await post(2, 0x403); // NIN_BALLOONHIDE.
  await until(() => entries[1].events.includes('close'), 'hide must close ID 2');
  await post(3, 0x405, 0xfeedbeef); // NIN_BALLOONUSERCLICK; wParam is undefined.
  await until(() => entries[2].events.includes('close'), 'click must close ID 3');
  assert.deepEqual(entries[1].events, ['show', 'close']);
  assert.deepEqual(entries[2].events, ['show', 'click', 'close']);
  entries[3].notification.close();
  assert.deepEqual(entries[3].events, ['show', 'close']);
  for (const entry of entries) {
    await post(entry.id, 0x405);
    entry.notification.close();
  }
  assert.deepEqual(entries.map(entry => entry.events), [
    ['show', 'close'], ['show', 'close'], ['show', 'click', 'close'], ['show', 'close']
  ], 'late/duplicate callbacks and repeated close must not emit events twice');
  observations.push({ phase: 'terminal', state: checkEmpty([1, 2, 3, 4]), events: entries.map(entry => entry.events.slice()) });

  // Re-showing a closed object creates a distinct registration; an old callback
  // must not close it, and close listeners may release/create notifications.
  entries[0].notification.show();
  const next = show(6);
  let replacement;
  next.notification.once('close', () => { replacement = show(7); });
  await post(1, 0x404);
  assert.deepEqual(entries[0].events, ['show', 'close', 'show']);
  await post(5, 0x404);
  await until(() => entries[0].events.length === 4, 're-shown notification must close its new ID');
  await post(6, 0x403);
  await until(() => Boolean(replacement), 'close listener must be able to create another notification');
  assert.equal(inspect([7])[0].registered, true);
  replacement.notification.close();
  observations.push({ phase: 'reentrant', state: checkEmpty([1, 2, 3, 4, 5, 6, 7]) });

  if (process.argv.includes('--natural-timeout')) {
    const natural = [show(8, true), show(9), show(10, true)];
    const start = Date.now();
    console.log(JSON.stringify({ phase: 'natural-start', ids: [8, 9, 10] }));
    await until(() => natural.every(entry => entry.events.includes('close')),
      'actual Shell balloons did not expire within 120 seconds', 120000);
    assert.deepEqual(natural.map(entry => entry.events), [
      ['show', 'close'], ['show', 'close'], ['show', 'close']
    ]);
    observations.push({ phase: 'natural-timeout', milliseconds: Date.now() - start,
      state: checkEmpty([8, 9, 10]), events: natural.map(entry => entry.events) });
  }
  finish();
}).catch(finish);
