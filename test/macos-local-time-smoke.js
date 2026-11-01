'use strict';

// Run with a macOS native runtime, optionally setting TZ to an IANA zone.
// Regression: a sandboxed renderer displayed UTC instead of local completion time.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-local-time-'));
app.setPath('userData', path.join(temporary, 'user-data'));
app.on('window-all-closed', () => {});
let window;
let finished = false;
const keepAlive = setInterval(() => {}, 250);
const deadline = setTimeout(() => finish(new Error('renderer local-time smoke timed out')), 30000);

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  clearInterval(keepAlive);
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  app.exit(error ? 1 : 0);
}

app.whenReady().then(async () => {
  const instants = [
    '2026-10-31T13:09:14.106Z', // Reported completion time.
    '2026-01-15T02:05:00.000Z', // Local date boundary and winter offset.
    '2026-07-15T02:05:00.000Z', // Summer offset differs in DST-observing zones.
  ];
  const snapshot = (instant) => {
    const date = new Date(instant);
    return {
      instant: date.toISOString(),
      local: [date.getFullYear(), date.getMonth(), date.getDate(), date.getHours(), date.getMinutes()],
      offset: date.getTimezoneOffset(),
      zone: new Intl.DateTimeFormat().resolvedOptions().timeZone,
      label: new Intl.DateTimeFormat('zh-CN', {
        year: 'numeric', month: '2-digit', day: '2-digit',
        hour: '2-digit', minute: '2-digit', hourCycle: 'h23',
      }).format(date),
    };
  };
  const expected = instants.map(snapshot);
  const source = `const snapshots = ${JSON.stringify(instants)}.map(${snapshot.toString()});
    document.body.textContent = snapshots.map(row => '完成于 ' + row.label).join('\\n');
    snapshots;`;
  const file = path.join(temporary, 'clock.html');
  fs.writeFileSync(file, '<!doctype html><meta charset="utf-8"><title>Local completion time</title>');
  window = new BrowserWindow({ width: 620, height: 220, show: false,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadFile(file);
  const actual = await window.webContents.executeJavaScript(source);
  assert.deepEqual(actual, expected, 'sandboxed Date fields and Intl completion labels must match system local time');
  const displayed = await window.webContents.executeJavaScript('document.body.textContent');
  if (process.env.MINI_LOCAL_TIME_SCREENSHOT) {
    window.showInactive();
    await window.webContents.executeJavaScript(`(() => {
      const frame = Promise.withResolvers();
      requestAnimationFrame(() => requestAnimationFrame(() => frame.resolve()));
      return frame.promise;
    })()`);
    const image = await window.webContents.capturePage();
    fs.writeFileSync(process.env.MINI_LOCAL_TIME_SCREENSHOT, image.toPNG());
  }
  console.log(JSON.stringify({ result: 'PASS', timezone: expected[0].zone, snapshots: actual, displayed }));
  finish();
}).catch(finish);
