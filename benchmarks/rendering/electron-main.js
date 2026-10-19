'use strict';

const { app, BrowserWindow } = require('electron');

let window;
let quitting = false;
function fail(error) {
  console.error(error.stack || error);
  app.exit(1);
}
function quit() {
  if (quitting) return;
  quitting = true;
  app.quit();
}

// The runner owns this process and its renderer; no bridge or benchmark code
// runs in Node. Both engines execute the same page served over loopback HTTP.
process.stdin.setEncoding('utf8');
let input = '';
process.stdin.on('data', chunk => {
  input += chunk;
  let end;
  while ((end = input.indexOf('\n')) !== -1) {
    const command = input.slice(0, end).trim();
    input = input.slice(end + 1);
    if (command === 'quit') quit();
  }
});
process.stdin.on('end', quit);
app.on('window-all-closed', quit);

app.whenReady().then(async () => {
  const url = new URL(process.env.MINI_RENDER_BENCH_URL);
  const width = Number(process.env.MINI_RENDER_BENCH_WIDTH);
  const height = Number(process.env.MINI_RENDER_BENCH_HEIGHT);
  if (url.protocol !== 'http:' || url.hostname !== '127.0.0.1' ||
      !Number.isInteger(width) || width < 1 || !Number.isInteger(height) || height < 1) {
    throw new Error('Launch this host through benchmarks/rendering/run.js');
  }
  window = new BrowserWindow({
    title: 'mini-electron rendering benchmark',
    width,
    height,
    x: 40,
    y: 40,
    frame: false,
    show: true,
    backgroundColor: '#111827',
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  window.webContents.on('render-process-gone', (...args) => {
    if (!quitting) fail(new Error('Benchmark renderer exited: ' + JSON.stringify(args.at(-1))));
  });
  window.show();
  window.focus();
  console.log('MINI_RENDER_HOST ' + JSON.stringify({
    versions: process.versions, executable: process.execPath, windowBounds: window.getBounds(),
  }));
  await window.loadURL(url.href);
}).catch(fail);
