'use strict';

// Run with the native runtime: mini-electron test/execute-javascript-smoke.js.
// No preload, IPC bridge, DevTools, or renderer-side result polling is involved.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const zlib = require('node:zlib');
const { app, BrowserWindow, nativeImage } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-execute-javascript-'));
app.setPath('userData', path.join(temporary, 'user-data'));
app.on('window-all-closed', () => {});
let window;
let finished = false;
let phase = 'startup';
const deadline = setTimeout(() => finish(new Error(`executeJavaScript smoke timed out at ${phase}`)), 30000);

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: native executeJavaScript values, Promise settlement/errors, CSP image/canvas PNG, and document/window lifecycle');
  app.exit(error ? 1 : 0);
}

// An opaque, nonuniform PNG makes the canvas result exceed 64 KiB and permits
// exact pixel comparison after independent native decoding in the main process.
function makePng(width, height) {
  function chunk(type, data) {
    const body = Buffer.concat([Buffer.from(type), data]);
    let crc = 0xffffffff;
    for (const byte of body) {
      crc ^= byte;
      for (let bit = 0; bit < 8; ++bit) crc = crc & 1 ? 0xedb88320 ^ (crc >>> 1) : crc >>> 1;
    }
    const header = Buffer.alloc(4), trailer = Buffer.alloc(4);
    header.writeUInt32BE(data.length);
    trailer.writeUInt32BE((crc ^ 0xffffffff) >>> 0);
    return Buffer.concat([header, body, trailer]);
  }
  const pixels = Buffer.alloc((width * 4 + 1) * height);
  let random = 0x12345678;
  for (let y = 0; y < height; ++y) {
    for (let x = 0; x < width; ++x) {
      const offset = y * (width * 4 + 1) + 1 + x * 4;
      for (let channel = 0; channel < 3; ++channel) {
        random ^= random << 13; random ^= random >>> 17; random ^= random << 5;
        pixels[offset + channel] = random & 255;
      }
      pixels[offset + 3] = 255;
    }
  }
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width); header.writeUInt32BE(height, 4);
  header[8] = 8; header[9] = 6;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk('IHDR', header), chunk('IDAT', zlib.deflateSync(pixels)), chunk('IEND', Buffer.alloc(0))]);
}

app.whenReady().then(async () => {
  const page = path.join(temporary, 'index.html');
  fs.writeFileSync(page, '<!doctype html><meta http-equiv="Content-Security-Policy" content="default-src \'none\'; img-src \'self\'"><title>executeJavaScript smoke</title>');
  const sourcePng = makePng(256, 192);
  fs.writeFileSync(path.join(temporary, 'source.png'), sourcePng);
  window = new BrowserWindow({ width: 360, height: 260, show: false,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadFile(page);
  const contents = window.webContents;
  const execute = (...args) => contents.executeJavaScript(...args);
  assert.deepEqual(await execute('[typeof require, typeof process]'), ['undefined', 'undefined']);

  phase = 'seven reported contract cases';
  assert.equal(await execute('"sync-value"'), 'sync-value');
  assert.equal(await execute('42'), 42);
  assert.deepEqual(await execute('({marker:"object-value",count:7})'), { marker: 'object-value', count: 7 });
  assert.equal(await execute('Promise.resolve("settled-value")'), 'settled-value');
  assert.equal(await execute('new Promise(r => setTimeout(() => r("delayed-value"), 30))'), 'delayed-value');
  await assert.rejects(execute('Promise.reject(new Error("expected renderer rejection"))'),
    error => error instanceof Error && error.message === 'expected renderer rejection');
  await assert.rejects(execute('(() => { throw new Error("expected renderer throw") })()'),
    error => error instanceof Error && error.message === 'expected renderer throw');
  await assert.rejects(execute('new Promise((resolve, reject) => setTimeout(() => reject(new RangeError("delayed rejection")), 30))'),
    error => error instanceof RangeError && error.message === 'delayed rejection');

  phase = 'structured clone boundaries and concurrent settlement';
  assert.deepEqual(await execute('({ nested: { values: [1, true, null, { text: "ok" }] }, result: "ordinary property", value: 42 })'),
    { nested: { values: [1, true, null, { text: 'ok' }] }, result: 'ordinary property', value: 42 });
  assert.equal(await execute('undefined'), undefined);
  assert.equal(await execute('null'), null);
  assert.equal(await execute('false'), false);
  assert.equal(await execute('-0'), -0);
  assert.ok(Number.isNaN(await execute('NaN')));
  assert.equal(await execute('Infinity'), Infinity);
  assert.equal(await execute('12345678901234567890n'), 12345678901234567890n);
  const structured = await execute(`(() => {
    const shared = { missing: undefined };
    const result = { shared, alias: shared, sparse: [, undefined, null],
      bytes: new Uint8Array([99, 1, 2, 3, 88]).subarray(1, 4),
      buffer: new Uint16Array([257, 65535]).buffer,
      date: new Date('2026-01-02T03:04:05Z'), regex: /native/gi,
      map: new Map([['nested', shared]]), set: new Set([1, 'two']) };
    result.self = result;
    return result;
  })()`);
  assert.equal(structured.self, structured);
  assert.equal(structured.alias, structured.shared);
  assert.ok(Object.hasOwn(structured.shared, 'missing'));
  assert.equal(structured.shared.missing, undefined);
  assert.equal(0 in structured.sparse, false);
  assert.deepEqual(structured.sparse.slice(1), [undefined, null]);
  assert.ok(structured.bytes instanceof Uint8Array);
  assert.deepEqual([...structured.bytes], [1, 2, 3]);
  assert.ok(structured.buffer instanceof ArrayBuffer);
  assert.deepEqual([...new Uint16Array(structured.buffer)], [257, 65535]);
  assert.equal(structured.date.toISOString(), '2026-01-02T03:04:05.000Z');
  assert.equal(structured.regex.source, 'native');
  assert.equal(structured.regex.flags, 'gi');
  assert.equal(structured.map.get('nested'), structured.shared);
  assert.deepEqual([...structured.set], [1, 'two']);
  assert.equal(await execute('({ then(resolve) { setTimeout(() => resolve("thenable"), 10); } })'), 'thenable');
  assert.deepEqual(await Promise.all([
    execute('new Promise(resolve => setTimeout(() => resolve("slow"), 40))'),
    execute('Promise.resolve({ fast: true })'),
    execute('7'),
  ]), ['slow', { fast: true }, 7]);
  await assert.rejects(execute('throw new TypeError("specific error type")'),
    error => error instanceof TypeError && error.message === 'specific error type');
  await assert.rejects(execute('const = invalid syntax'), SyntaxError);
  await assert.rejects(execute('(() => {})'), /could not be cloned/i);
  let callbackResult;
  assert.deepEqual(await execute('({ callback: true })', result => { callbackResult = result; }), { callback: true });
  assert.deepEqual(callbackResult, { callback: true });
  let rejectedCallbackCalled = false;
  await assert.rejects(execute('Promise.reject(new Error("callback rejection"))', false,
    () => { rejectedCallbackCalled = true; }), /callback rejection/);
  assert.equal(rejectedCallbackCalled, false);

  phase = 'CSP image decode and asynchronous canvas PNG';
  const base64 = await execute(`(async () => {
    const { promise, resolve, reject } = Promise.withResolvers();
    const image = new Image();
    image.onload = resolve;
    image.onerror = () => reject(new Error('source image did not load'));
    image.src = 'source.png';
    await promise;
    const canvas = document.createElement('canvas');
    canvas.width = image.naturalWidth; canvas.height = image.naturalHeight;
    canvas.getContext('2d').drawImage(image, 0, 0);
    return canvas.toDataURL('image/png').slice('data:image/png;base64,'.length);
  })()`);
  assert.equal(typeof base64, 'string');
  assert.ok(base64.length > 65536, 'The actual PNG return must cross the container-entry boundary');
  const output = Buffer.from(base64, 'base64');
  assert.deepEqual(output.subarray(0, 8), Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]));
  const decoded = nativeImage.createFromBuffer(output);
  assert.equal(decoded.isEmpty(), false);
  assert.deepEqual(decoded.getSize(), { width: 256, height: 192 });
  assert.deepEqual(decoded.toBitmap(), nativeImage.createFromBuffer(sourcePng).toBitmap(), 'Image/canvas conversion must preserve every source pixel');

  phase = 'navigation cancels a pending source document';
  const navigationPending = execute('window.navigationPending = true; new Promise(() => {})');
  const navigationRejected = assert.rejects(navigationPending, /interrupted|frame|destroyed/i);
  assert.equal(await execute('window.navigationPending'), true);
  await window.loadURL('data:text/html,<title>replacement document</title>');
  await navigationRejected;
  assert.equal(await execute('"new document remains usable"'), 'new document remains usable');

  phase = 'window destruction cancels a pending result';
  const destructionPending = execute('window.destructionPending = true; new Promise(() => {})');
  const destructionRejected = assert.rejects(destructionPending, Error);
  assert.equal(await execute('window.destructionPending'), true);
  window.destroy();
  await destructionRejected;
  await assert.rejects(execute('42'), /destroyed|closed|renderer/i);
  finish();
}).catch(finish);
