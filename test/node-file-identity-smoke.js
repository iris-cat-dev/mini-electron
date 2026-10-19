'use strict';

// Run both ways on Windows:
// out/windows-x64/runtime-dist/electron.exe test/node-file-identity-smoke.js
// cmd /d /s /c "set ELECTRON_RUN_AS_NODE=1&& out\windows-x64\runtime-dist\electron.exe test\node-file-identity-smoke.js"
const assert = require('node:assert/strict');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

function fromCallback(start) {
  return new Promise((resolve, reject) => {
    start((error, value) => error ? reject(error) : resolve(value));
  });
}

function readSync(file, handle, options) {
  return {
    stat: fs.statSync(file, options),
    lstat: fs.lstatSync(file, options),
    fstat: fs.fstatSync(handle.fd, options),
  };
}

async function readCallback(file, handle, options) {
  const [stat, lstat, fstat] = await Promise.all([
    fromCallback(done => fs.stat(file, options, done)),
    fromCallback(done => fs.lstat(file, options, done)),
    fromCallback(done => fs.fstat(handle.fd, options, done)),
  ]);
  return { stat, lstat, fstat };
}

async function readPromises(file, handle, options) {
  const [stat, lstat, fstat] = await Promise.all([
    fsp.stat(file, options),
    fsp.lstat(file, options),
    handle.stat(options),
  ]);
  return { stat, lstat, fstat };
}

const readers = [
  ['sync', readSync],
  ['callback', readCallback],
  ['promises', readPromises],
];
const modes = [
  ['number', { bigint: false }, 'number'],
  ['bigint', { bigint: true }, 'bigint'],
];

function assertStatShape(label, value, expectedType) {
  assert.equal(typeof value.dev, expectedType, `${label} dev type`);
  assert.equal(typeof value.ino, expectedType, `${label} ino type`);
  assert.equal(typeof value.size, expectedType, `${label} size type`);
}

function assertSameFile(label, pathname, opened, expectedType) {
  assertStatShape(`${label} pathname`, pathname, expectedType);
  assertStatShape(`${label} handle`, opened, expectedType);
  assert.equal(pathname.dev, opened.dev, `${label} device identity`);
  assert.equal(pathname.ino, opened.ino, `${label} inode identity`);
  assert.equal(pathname.size, opened.size, `${label} size`);
}

function assertReplacement(label, pathname, opened, expectedType) {
  assertStatShape(`${label} pathname`, pathname, expectedType);
  assertStatShape(`${label} handle`, opened, expectedType);
  assert.equal(pathname.dev, opened.dev, `${label} files remain on the same volume`);
  assert.notEqual(pathname.ino, opened.ino, `${label} replacement must have a distinct inode`);
  assert.equal(pathname.size, opened.size,
    `${label} fixture sizes must match so size cannot distinguish replacement`);
}

async function verifyUnchanged(file, handle) {
  for (const [mode, options, expectedType] of modes) {
    for (const [api, read] of readers) {
      const values = await read(file, handle, options);
      assertSameFile(`${api} ${mode} stat/fstat`, values.stat, values.fstat, expectedType);
      assertSameFile(`${api} ${mode} lstat/fstat`, values.lstat, values.fstat, expectedType);
    }
  }
}

async function verifyReplacement(file, retained, handle) {
  for (const [mode, options, expectedType] of modes) {
    for (const [api, read] of readers) {
      const retainedValues = await read(retained, handle, options);
      assertSameFile(`${api} ${mode} retained stat/fstat`,
        retainedValues.stat, retainedValues.fstat, expectedType);
      assertSameFile(`${api} ${mode} retained lstat/fstat`,
        retainedValues.lstat, retainedValues.fstat, expectedType);

      const replacementValues = await read(file, handle, options);
      assertReplacement(`${api} ${mode} replacement stat/fstat`,
        replacementValues.stat, replacementValues.fstat, expectedType);
      assertReplacement(`${api} ${mode} replacement lstat/fstat`,
        replacementValues.lstat, replacementValues.fstat, expectedType);
    }
  }
}

async function run() {
  assert.equal(process.platform, 'win32', 'This regression exercises Windows file identity');
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-file-identity-'));
  const file = path.join(temporary, 'identity.txt');
  const retained = path.join(temporary, 'identity-retained.txt');
  const originalContent = Buffer.from('original-identity');
  const replacementContent = Buffer.from('replaced-identity');
  assert.equal(originalContent.length, replacementContent.length);

  let handle;
  try {
    fs.writeFileSync(file, originalContent);
    handle = await fsp.open(file, 'r');
    await verifyUnchanged(file, handle);

    // libuv opens regular files with FILE_SHARE_DELETE. Renaming keeps the open
    // handle bound to the original while allowing a new file at the old path.
    fs.renameSync(file, retained);
    fs.writeFileSync(file, replacementContent);
    await verifyReplacement(file, retained, handle);
    assert.deepEqual(fs.readFileSync(handle.fd), originalContent,
      'the open handle must continue to read the retained original');
    assert.deepEqual(fs.readFileSync(file), replacementContent,
      'the original pathname must resolve to the replacement');

    console.log('PASS: Windows stat, lstat, and fstat preserve file identity across sync, callback, promises, number, bigint, and real replacement checks');
  } finally {
    if (handle) await handle.close();
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

function reportFailure(error) {
  console.error(error.stack || error);
  return 1;
}

if (process.env.ELECTRON_RUN_AS_NODE === '1') {
  run().catch(error => { process.exitCode = reportFailure(error); });
} else {
  const { app } = require('electron');
  app.on('window-all-closed', () => {});
  app.whenReady().then(run).then(
    () => app.exit(0),
    error => app.exit(reportFailure(error)),
  );
}
