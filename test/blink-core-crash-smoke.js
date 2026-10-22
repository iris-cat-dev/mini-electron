'use strict';

// Run with the rebuilt native runtime: out/mac-arm64/mini-electron test/blink-core-crash-smoke.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const http = require('node:http');
const { app, BrowserWindow } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-blink-core-'));
app.setPath('userData', path.join(temporary, 'user-data'));

let window;
let finished = false;
const server = http.createServer((_request, response) => {
  response.writeHead(200, {
    'Content-Type': 'text/html',
    'Permissions-Policy': 'geolocation=(), camera=(), microphone=(), fullscreen=(self)',
    'Document-Policy': 'force-load-at-top=?1',
    'Speculation-Rules': '"/missing-rules.json"',
  });
  response.end('<!doctype html><title>Blink core crash regression</title><canvas id="canvas" width="2" height="2"></canvas>');
});
const watchdog = setTimeout(() => finish(new Error('Blink core smoke timed out')), 30000);
app.on('window-all-closed', () => {});

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (window && !window.isDestroyed()) window.destroy();
  server.close();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: URLPattern matching/errors, ImageBitmap pixels and unsupported media/storage safety');
  app.exit(error ? 1 : 0);
}

app.whenReady().then(async () => {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  window = new BrowserWindow({ show: false, webPreferences: { contextIsolation: true } });
  window.webContents.on('render-process-gone', (_event, details) => finish(new Error(JSON.stringify(details))));
  await window.loadURL(`http://127.0.0.1:${server.address().port}/`);
  const result = await window.webContents.executeJavaScript(`(async () => {
    const pattern = new URLPattern({protocol: 'https', hostname: 'example.test', pathname: '/users/:id'});
    const match = pattern.exec('https://example.test/users/42');
    if (match?.pathname.groups.id !== '42' || pattern.test('https://other.test/users/42'))
      throw new Error('URLPattern component matching or captures failed');
    const relativePattern = new URLPattern('/users/:id', 'https://example.test');
    if (!relativePattern.test('https://example.test/users/42'))
      throw new Error('URLPattern string constructor lost the base URL');
    let invalidPatternError;
    try { new URLPattern({pathname: '/bad/('}); } catch (error) { invalidPatternError = error.name; }
    if (invalidPatternError !== 'TypeError') throw new Error('Malformed URLPattern did not throw TypeError');
    const canvas = document.getElementById('canvas');
    const context = canvas.getContext('2d');
    context.fillStyle = 'rgb(23, 67, 191)';
    context.fillRect(0, 0, 2, 2);
    const bitmap = await createImageBitmap(canvas);
    const channel = new MessageChannel();
    const received = new Promise((resolve, reject) => {
      channel.port2.onmessageerror = () => reject(new Error('ImageBitmap deserialization failed'));
      channel.port2.onmessage = event => resolve(event.data);
    });
    channel.port1.postMessage(bitmap, [bitmap]);
    const transferred = await received;
    context.clearRect(0, 0, 2, 2);
    context.drawImage(transferred, 0, 0);
    const pixel = [...context.getImageData(0, 0, 1, 1).data];
    transferred.close();
    channel.port1.close();
    channel.port2.close();
    const audio = document.createElement('audio');
    function captureFailure(element) {
      try {
        element.captureStream();
        return { returnedStream: true };
      } catch (error) {
        return { name: error.name, isDOMException: error instanceof DOMException };
      }
    }
    async function errorName(operation) {
      try { await operation(); return 'NO_ERROR'; } catch (error) { return error.name; }
    }
    const directoryError = await errorName(() => navigator.storage.getDirectory());
    const brandErrors = [];
    for (const value of [{}, Object.create(null), { [Symbol.toStringTag]: 'MediaStream' },
      document.createElement('div'), new URL(location.href), new DOMException('wrong brand'),
      new Proxy(document.createElement('div'), {})]) {
      let sourceError;
      try { audio.srcObject = value; sourceError = 'NO_ERROR'; } catch (error) { sourceError = error.name; }
      brandErrors.push([sourceError, await errorName(() => audio.setMediaKeys(value))]);
    }
    const unavailableSink = await errorName(() => audio.setSinkId('unavailable-device'));
    const emptyAudioCapture = captureFailure(audio);
    const emptyVideoCapture = captureFailure(document.createElement('video'));
    const failed = new Promise(resolve => audio.addEventListener('error', () => resolve(audio.error.code), { once: true }));
    audio.src = 'data:audio/wav;base64,UklGRiQAAABXQVZFZm10IBAAAAABAAEAQB8AAIA+AAACABAAZGF0YQAAAAA=';
    audio.load();
    const errorCode = await failed;
    const failedAudioCapture = captureFailure(audio);
    audio.removeAttribute('src');
    audio.load();
    return { pixel, errorCode, emptyAudioCapture, emptyVideoCapture,
      failedAudioCapture, brandErrors, unavailableSink, directoryError };
  })().catch(error => ({ rendererFailure: { name: error.name, message: error.message, stack: error.stack } }))`);
  if (result.rendererFailure)
    throw new Error(JSON.stringify(result.rendererFailure));
  assert.deepEqual(result.pixel, [23, 67, 191, 255], 'ImageBitmap transfer must retain actual pixels');
  assert.equal(result.errorCode, 4, 'The selected lite media factory must report MEDIA_ERR_SRC_NOT_SUPPORTED');
  for (const errors of result.brandErrors) assert.deepEqual(errors, ['TypeError', 'TypeError']);
  assert.equal(result.unavailableSink, 'NotSupportedError');
  assert.ok(['NotSupportedError', 'SecurityError'].includes(result.directoryError),
    'Missing directory backend must reject, not resolve undefined or hang');
  for (const key of ['emptyAudioCapture', 'emptyVideoCapture', 'failedAudioCapture']) {
    assert.equal(result[key].name, 'NotSupportedError', key + ' must reject the unavailable lite backend instead of returning a fake stream');
    assert.equal(result[key].isDOMException, true, key + ' must throw a standard DOMException');
  }
  finish();
}).catch(finish);
