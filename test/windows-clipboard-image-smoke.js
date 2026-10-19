'use strict';
// Run on an interactive Windows desktop. Native clipboard formats are written by
// the shared Win32 helper, not through Electron's clipboard implementation.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const zlib = require('node:zlib');
const { app, clipboard, nativeImage } = require('electron');
const { startNativeInput } = require('./windows-native-input-driver');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-windows-clipboard-image-'));
app.setPath('userData', path.join(temporary, 'user-data'));
const title = `mini-windows-clipboard-image-${process.pid}`;
let phase = 'startup';
let driver;
let clipboardSaved = false;
let finished = false;
const watchdog = setTimeout(() => void finish(new Error(`Windows clipboard image smoke timed out at ${phase}`)), 60000);
app.on('window-all-closed', () => {});

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function pngWithDimensions(width, height) {
  phase = `generating valid ${width}x${height} PNG`;
  return new Promise((resolve, reject) => {
    const deflater = zlib.createDeflate();
    const compressed = [];
    deflater.on('data', chunk => compressed.push(chunk));
    deflater.once('error', reject);
    deflater.once('end', () => {
      function chunk(type, data) {
        const output = Buffer.alloc(data.length + 12);
        output.writeUInt32BE(data.length);
        output.write(type, 4, 'ascii');
        data.copy(output, 8);
        output.writeUInt32BE(crc32(output.subarray(4, output.length - 4)), output.length - 4);
        return output;
      }
      const header = Buffer.alloc(13);
      header.writeUInt32BE(width);
      header.writeUInt32BE(height, 4);
      header[8] = 8;
      header[9] = 6;
      resolve(Buffer.concat([
        Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
        chunk('IHDR', header), chunk('IDAT', Buffer.concat(compressed)),
        chunk('IEND', Buffer.alloc(0)),
      ]));
    });
    // Fully valid, highly compressible pixels test policy limits rather than
    // accidentally testing a truncated stream with a forged large header.
    const rowBytes = width * 4 + 1;
    const rowsPerChunk = Math.min(height, 64);
    const block = Buffer.alloc(rowBytes * rowsPerChunk);
    for (let y = 0; y < height; y += rowsPerChunk)
      deflater.write(y + rowsPerChunk <= height ? block : block.subarray(0, (height - y) * rowBytes));
    deflater.end();
  });
}

function dibWithDimensions(dib, width, height) {
  const headerSize = dib.readUInt32LE(0);
  const result = Buffer.alloc(headerSize + width * height * 4);
  dib.copy(result, 0, 0, headerSize);
  result.writeUInt32LE(width * height * 4, 20);
  result.writeInt32LE(width, 4);
  result.writeInt32LE(height, 8);
  return result;
}

function makeDibV5(dib) {
  assert.equal(dib.readUInt32LE(0), 40);
  assert.equal(dib.readUInt16LE(14), 32);
  const result = Buffer.alloc(124 + dib.length - 40);
  dib.copy(result, 0, 0, 40);
  dib.copy(result, 124, 40);
  result.writeUInt32LE(124, 0);
  result.writeUInt32LE(3, 16);
  for (const [offset, value] of [
    [40, 0x00ff0000], [44, 0x0000ff00], [48, 0x000000ff],
    [52, 0xff000000], [56, 0x73524742],
  ]) result.writeUInt32LE(value, offset);
  return result;
}

function assertSolidFixture(label) {
  const image = clipboard.readImage();
  assert.equal(image.isEmpty(), false, `${label}: readImage returned empty`);
  assert.deepEqual(image.getSize(), { width: 16, height: 16 }, `${label}: dimensions`);

  const bitmap = image.toBitmap();
  assert.equal(bitmap.length, 16 * 16 * 4, `${label}: bitmap byte count`);
  for (let offset = 0; offset < bitmap.length; offset += 4) {
    assert.deepEqual([...bitmap.subarray(offset, offset + 4)], [55, 44, 203, 255],
      `${label}: BGRA pixel ${offset / 4}`);
  }

  const encoded = image.toPNG();
  assert.deepEqual([...encoded.subarray(0, 8)], [137, 80, 78, 71, 13, 10, 26, 10],
    `${label}: PNG signature`);
  const roundTrip = nativeImage.createFromBuffer(encoded);
  assert.equal(roundTrip.isEmpty(), false, `${label}: PNG output decodes`);
  assert.deepEqual(roundTrip.toBitmap(), bitmap, `${label}: PNG output preserves pixels`);
}

function assertEmpty(label) {
  const image = clipboard.readImage();
  assert.equal(image.isEmpty(), true, `${label}: readImage must be empty`);
  assert.deepEqual(image.getSize(), { width: 0, height: 0 }, `${label}: empty dimensions`);
  assert.equal(image.toBitmap().length, 0, `${label}: empty bitmap`);
  assert.equal(image.toPNG().length, 0, `${label}: empty PNG`);
}

async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (driver && clipboardSaved) {
    try {
      await driver.request('clipboardRestore');
      clipboardSaved = false;
    } catch (restoreError) {
      error = error || restoreError;
    }
  }
  if (driver) {
    try { await driver.close(); }
    catch (closeError) { error = error || closeError; }
  }
  try { fs.rmSync(temporary, { recursive: true, force: true }); }
  catch (cleanupError) { error = error || cleanupError; }
  if (error) console.error(error.stack || error);
  else console.log('PASS: Windows public clipboard.readImage PNG/DIB/DIBV5, fallback, pixels, PNG output and rejection bounds');
  app.exit(error ? 1 : 0);
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  driver = await startNativeInput(temporary, title);
  await driver.request('clipboardSave');
  clipboardSaved = true;

  const images = await driver.request('images');
  const dib = Buffer.from(images.dib, 'base64');
  const dibV5 = makeDibV5(dib);
  for (const [label, formats] of [
    ['PNG only', [{ name: 'PNG', data: images.png }]],
    ['DIB only', [{ name: 'DIB', data: images.dib }]],
    ['DIBV5 only', [{ name: 'DIBV5', data: dibV5.toString('base64') }]],
    ['invalid PNG with valid DIB fallback', [
      { name: 'PNG', data: Buffer.from('invalid PNG').toString('base64') },
      { name: 'DIB', data: images.dib },
    ]],
  ]) {
    phase = label;
    await driver.request('clipboardWrite', { formats });
    assertSolidFixture(label);
  }

  for (const [label, name, bytes] of [
    ['malformed PNG only', 'PNG', Buffer.from('malformed PNG')],
    ['malformed DIB only', 'DIB', Buffer.alloc(20)],
    ['PNG dimension limit', 'PNG', await pngWithDimensions(8193, 16)],
    ['PNG pixel-count limit', 'PNG', await pngWithDimensions(8192, 5121)],
    ['DIB dimension limit', 'DIB', dibWithDimensions(dib, 8193, 16)],
  ]) {
    phase = label;
    await driver.request('clipboardWrite', {
      formats: [{ name, data: bytes.toString('base64') }],
    });
    assertEmpty(label);
  }
}).then(() => finish(), finish);
