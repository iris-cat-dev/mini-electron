'use strict';

// Run with the built runtime: mini-electron test/generated-image-download-smoke.js.
// Exercises real DownloadItem behavior, selecting destinations through
// will-download/setSavePath rather than automating the native save panel.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const zlib = require('node:zlib');
const { app, BrowserWindow } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-generated-download-'));
app.setPath('userData', path.join(temporary, 'profile'));
app.on('window-all-closed', () => {});
const windows = [];
let server;
let phase = 'startup';
let finished = false;
const deadline = setTimeout(() => finish(new Error(`Download smoke timed out at ${phase}`)), 60000);
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  for (const window of windows) if (!window.isDestroyed()) window.destroy();
  if (server) server.close();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: original PNG/SVG bytes, data/base64/percent, fetched MIME/chunked/sliced/opaque blobs, HTTP, pause, cancellation and unauthorized/invalid download lifecycle');
  app.exit(error ? 1 : 0);
}

function png() {
  function chunk(type, bytes) {
    const data = Buffer.concat([Buffer.from(type), bytes]);
    let crc = 0xffffffff;
    for (const byte of data) {
      crc ^= byte;
      for (let bit = 0; bit < 8; bit++) crc = crc & 1 ? 0xedb88320 ^ (crc >>> 1) : crc >>> 1;
    }
    const length = Buffer.alloc(4), checksum = Buffer.alloc(4);
    length.writeUInt32BE(bytes.length); checksum.writeUInt32BE((crc ^ 0xffffffff) >>> 0);
    return Buffer.concat([length, data, checksum]);
  }
  const width = 512, height = 384;
  const pixels = Buffer.alloc((width * 4 + 1) * height);
  let random = 0x12345678;
  for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
    const offset = y * (width * 4 + 1) + 1 + x * 4;
    for (let channel = 0; channel < 3; channel++) {
      random ^= random << 13; random ^= random >>> 17; random ^= random << 5;
      pixels[offset + channel] = random & 255;
    }
    pixels[offset + 3] = 255;
  }
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width); header.writeUInt32BE(height, 4); header[8] = 8; header[9] = 6;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk('IHDR', header), chunk('IDAT', zlib.deflateSync(pixels)), chunk('IEND', Buffer.alloc(0))]);
}

function download(contents, url, label, options = {}) {
  phase = label;
  const destination = path.join(temporary, `${label}.saved`);
  if (options.sentinel) fs.writeFileSync(destination, 'unchanged destination');
  return new Promise((resolve, reject) => {
    const session = contents.session;
    session.once('will-download', (event, item, owner) => {
      try {
        assert.equal(owner, contents);
        assert.equal(item.getURL(), url);
        item.setSavePath(destination);
        item.once('done', (_event, state) => {
          try {
            assert.equal(state, options.state || 'completed', label);
            assert.equal(item.getState(), state);
            if (state === 'completed') {
              assert.deepEqual(fs.readFileSync(destination), options.bytes);
              assert.equal(item.getReceivedBytes(), options.bytes.length);
              assert.equal(item.getTotalBytes(), options.bytes.length);
              assert.equal(item.getMimeType(), options.mime);
              if (options.filename) assert.equal(item.getFilename(), options.filename);
            } else if (options.sentinel) {
              assert.equal(fs.readFileSync(destination, 'utf8'), 'unchanged destination');
            } else assert.equal(fs.existsSync(destination), false);
            resolve();
          } catch (error) { reject(error); }
        });
        if (options.cancel === 'before') item.cancel();
        if (options.cancel === 'progress') item.once('updated', () => item.cancel());
        if (options.pause) {
          item.pause();
          setTimeout(() => item.resume(), 100);
        }
      } catch (error) { item.cancel(); reject(error); }
    });
    try { contents.downloadURL(url); } catch (error) { reject(error); }
  });
}

app.whenReady().then(async () => {
  const bytes = png();
  assert.ok(bytes.length > 512 * 1024, 'PNG must span multiple transport chunks');
  const base64 = `data:image/png;base64,${bytes.toString('base64')}`;
  const small = Buffer.from('<svg xmlns="http://www.w3.org/2000/svg" width="2" height="2"><!-- + % # 雪 --><rect width="2" height="2" fill="red"/></svg>');
  const percent = `data:image/svg+xml,${[...small].map(byte => `%${byte.toString(16).padStart(2, '0')}`).join('')}`;
  const page = path.join(temporary, 'index.html');
  fs.writeFileSync(page, '<!doctype html><title>Generated image download smoke</title>');
  const window = new BrowserWindow({ show: false, webPreferences: { sandbox: true, nodeIntegration: false, contextIsolation: true } });
  windows.push(window);
  await window.loadFile(page);
  const contents = window.webContents;
  await contents.executeJavaScript(`new Promise((resolve, reject) => {
    const image = new Image(); image.onload = () => resolve(true); image.onerror = () => reject(new Error('PNG display failed'));
    image.src = ${JSON.stringify(base64)}; document.body.append(image);
  })`);
  await download(contents, base64, 'base64-png', { bytes, mime: 'image/png', filename: 'download.png' });
  await download(contents, percent, 'percent-svg', { bytes: small, mime: 'image/svg+xml' });
  const fetchedBlob = await contents.executeJavaScript(`fetch(${JSON.stringify(percent)})
    .then(response => response.blob()).then(blob => URL.createObjectURL(blob))`);
  await download(contents, fetchedBlob, 'fetched-image-blob',
    { bytes: small, mime: 'image/svg+xml', filename: 'download.svg' });
  // Percent escapes inside base64 must be decoded before base64 decoding.
  await download(contents, `data:image/png;base64,${bytes.toString('base64').replaceAll('+', '%2B').replaceAll('/', '%2F').replaceAll('=', '%3D')}`,
    'escaped-base64', { bytes, mime: 'image/png' });
  const blob = await contents.executeJavaScript(`new Promise((resolve, reject) => {
    const raw = atob(${JSON.stringify(bytes.toString('base64'))});
    const bytes = Uint8Array.from(raw, value => value.charCodeAt(0));
    globalThis.originalBlob = new Blob([bytes.subarray(0, 123456), bytes.subarray(123456)], { type: 'image/png' });
    globalThis.paddedBlob = new Blob(['prefix', originalBlob, 'suffix']);
    globalThis.slicedBlob = paddedBlob.slice(6, 6 + bytes.length, 'image/png');
    globalThis.imageBlobURL = URL.createObjectURL(slicedBlob);
    const image = new Image();
    image.onload = () => resolve(imageBlobURL);
    image.onerror = () => reject(new Error('Blob PNG display failed'));
    image.src = imageBlobURL; document.body.append(image);
    globalThis.originalBlob = globalThis.paddedBlob = globalThis.slicedBlob = null;
  })`);
  await download(contents, blob, 'sliced-blob', { bytes, mime: 'image/png', pause: true });
  await download(contents, base64, 'cancel-data', { cancel: 'before', state: 'cancelled', sentinel: true });
  await download(contents, blob, 'cancel-blob', { cancel: 'progress', state: 'cancelled', sentinel: true });
  await download(contents, blob, 'cancel-blob-before', { cancel: 'before', state: 'cancelled' });
  const emptyBlob = await contents.executeJavaScript('URL.createObjectURL(new Blob([], {type:\"application/octet-stream\"}))');
  await download(contents, emptyBlob, 'empty-blob', { bytes: Buffer.alloc(0), mime: 'application/octet-stream' });
  await download(contents, 'data:application/octet-stream,', 'empty-data', { bytes: Buffer.alloc(0), mime: 'application/octet-stream' });
  await download(contents, 'data:image/png;base64,%%%', 'invalid-data', { state: 'interrupted' });
  await download(contents, 'blob:https://not-the-owner.invalid/00000000-0000-0000-0000-000000000000', 'unauthorized-blob', { state: 'interrupted' });
  await contents.executeJavaScript(`URL.revokeObjectURL(${JSON.stringify(blob)})`);
  await download(contents, blob, 'revoked-blob', { state: 'interrupted' });
  await download(contents, 'file:///etc/passwd', 'unauthorized-file', { state: 'interrupted' });
  await download(contents, 'unknown-download:source', 'unknown-scheme', { state: 'interrupted' });

  server = http.createServer((request, response) => {
    if (request.url === '/image.png') {
      response.writeHead(200, { 'Content-Type': 'image/png', 'Content-Length': bytes.length,
        'Content-Disposition': 'attachment; filename="original-image.png"' });
      for (let offset = 0; offset < bytes.length; offset += 32768) response.write(bytes.subarray(offset, offset + 32768));
      response.end();
    } else { response.writeHead(200, { 'Content-Type': 'text/html' }); response.end('<!doctype html><title>HTTP owner</title>'); }
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const origin = `http://127.0.0.1:${server.address().port}`;
  await download(contents, `${origin}/image.png`, 'http-png', { bytes, mime: 'image/png', filename: 'original-image.png' });
  const other = new BrowserWindow({ show: false, webPreferences: { sandbox: true, nodeIntegration: false } });
  windows.push(other);
  await other.loadURL(origin);
  const otherBlob = await other.webContents.executeJavaScript('URL.createObjectURL(new Blob(["other renderer"], {type:"text/plain"}))');
  await download(contents, otherBlob, 'other-renderer-blob', { state: 'interrupted' });
  // Opaque origins must use Blink's nonce-aware blob-origin check, not "null".
  await other.loadURL('data:text/html,<title>opaque blob owner</title>');
  const opaqueBlob = await other.webContents.executeJavaScript('URL.createObjectURL(new Blob(["opaque bytes"], {type:"text/plain"}))');
  await download(other.webContents, opaqueBlob, 'opaque-blob', { bytes: Buffer.from('opaque bytes'), mime: 'text/plain' });
  finish();
}).catch(finish);
