'use strict';

// Run in a fresh native mini-electron process. This intentionally exhausts only
// the disposable sandboxed renderer owned by this smoke.
const assert = require('node:assert/strict');
const { once } = require('node:events');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-renderer-memory-'));
app.setPath('userData', path.join(temporary, 'profile'));
const expectDumpError = process.argv.includes('--unwritable-dump-directory');
if (expectDumpError) {
  const unavailable = path.join(temporary, 'unavailable-dumps');
  fs.mkdirSync(unavailable);
  app.setPath('crashDumps', unavailable);
  fs.rmdirSync(unavailable);
  fs.writeFileSync(unavailable, 'A file replaced the configured dump directory.');
}

let window;
let finished = false;
const deadline = setTimeout(() => finish(new Error('Renderer memory smoke timed out')), 180000);
app.on('window-all-closed', () => {});

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  try {
    if (window && !window.isDestroyed()) window.destroy();
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
    if (error) console.error(error.stack || error);
    else console.log('PASS: Windows renderer OOM produces a symbol-identifiable exception dump and truthful memory diagnostics');
    app.exit(error ? 1 : 0);
  }
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  window = new BrowserWindow({
    width: 480, height: 320, show: false,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  const contents = window.webContents;
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(
    '<!doctype html><title>disposable renderer memory smoke</title>'));
  const rendererPid = contents.getProcessId();
  assert.ok(Number.isInteger(rendererPid) && rendererPid > 0 && rendererPid !== process.pid);

  const gone = once(contents, 'render-process-gone');
  const pressure = contents.executeJavaScript(`(() => {
    globalThis.__rendererMemorySmoke = [];
    // 160 compressed-slot Smi arrays request 5 GiB, exceeding the restored
    // machine-default ~4-GiB heap rather than only the former custom 2-GiB cap.
    for (let index = 0; index < 160; index++)
      globalThis.__rendererMemorySmoke.push(new Array(8 * 1024 * 1024).fill(37));
    throw new Error('Expected bounded heap workload to exhaust the renderer V8 heap');
  })()`);
  pressure.catch(() => {});
  const [, details] = await gone;

  assert.equal(details.reason, 'oom');
  assert.equal(details.exitCode, 0xe0000008);
  assert.ok(details.sandboxMemoryLimitExceeded === true || details.sandboxMemoryLimitExceeded === null,
    'missing Job evidence must stay unknown, not false');
  assert.equal(details.crashCaptureStatus, 'captured');
  assert.equal(details.exceptionCode, 0xe0000008);
  assert.equal(details.rendererPid, rendererPid);
  const GiB = 1024 ** 3;
  assert.ok(details.memory.processMemoryLimit >= (process.arch === 'x64' ? 2 : 1) * GiB);
  if (process.arch === 'x64' && os.totalmem() >= 16 * GiB)
    assert.equal(details.memory.processMemoryLimit, 8 * GiB,
      'a modern x64 renderer must retain the full native/V8 Job headroom');
  assert.ok(details.memory.peakCommitBytes > 0 && details.memory.peakCommitBytes <= details.memory.processMemoryLimit);
  assert.ok(['last-main-isolate', 'unavailable'].includes(details.v8.sampleKind));
  assert.notEqual(details.oomSource, 'unknown',
    'the matching fatal V8 callback must remain attributed');
  assert.equal(details.v8.failureThreadId, details.exceptionThreadId,
    'the classified V8 failure must belong to the captured exception thread');
  assert.equal(details.v8.isHeapOOM, true);
  assert.match(details.image.pdbGuid, /^[0-9A-F]{32}$/);
  assert.ok(details.image.pdbAge > 0);
  assert.equal(contents.isCrashed(), true);
  assert.equal(contents.getProcessId(), 0);
  assert.equal(window.isDestroyed(), false);
  if (expectDumpError) {
    assert.equal(details.crashDumpError, 80,
      'the original ERROR_FILE_EXISTS must survive when a file replaces the dump directory');
    assert.equal(details.crashDumpPath, undefined);
    assert.equal(details.crashDiagnosticsPath, undefined);
    finish();
    return;
  }
  assert.equal(details.crashDumpError, undefined);
  assert.equal(details.crashDiagnosticsError, undefined);
  const metadata = JSON.parse(fs.readFileSync(details.crashDiagnosticsPath, 'utf8'));
  assert.equal(metadata.exceptionCode, 0xe0000008);
  assert.deepEqual(metadata.image, details.image);
  assert.deepEqual(metadata.v8, details.v8);
  assert.equal(metadata.oomSource, details.oomSource);
  assert.deepEqual(metadata.memory, details.memory);
  const dump = fs.readFileSync(details.crashDumpPath);
  assert.equal(dump.toString('ascii', 0, 4), 'MDMP');
  const streams = new Map();
  for (let index = 0, entry = dump.readUInt32LE(12); index < dump.readUInt32LE(8); index++, entry += 12)
    streams.set(dump.readUInt32LE(entry), dump.readUInt32LE(entry + 8));
  const exception = streams.get(6);
  assert.notEqual(exception, undefined, 'the dump must preserve the failing thread exception context');
  assert.equal(dump.readUInt32LE(exception), metadata.exceptionThreadId);
  assert.equal(dump.readUInt32LE(exception + 8), 0xe0000008);
  const contextSize = dump.readUInt32LE(exception + 160);
  const context = dump.readUInt32LE(exception + 164);
  assert.ok(contextSize >= 256 && context + contextSize <= dump.length,
    'the exception stream must contain the original AMD64 context');
  assert.equal(dump.readUInt32LE(context + 48) & 0x100001, 0x100001,
    'the exception context must include AMD64 control registers');
  assert.ok(dump.readBigUInt64LE(context + 248) > 0n, 'the original instruction pointer must be present');
  assert.ok(dump.readBigUInt64LE(context + 152) > 0n, 'the original stack pointer must be present');
  const requested = dump.readUInt32LE(exception + 32) > 0
    ? dump.readBigUInt64LE(exception + 40) : 0n;
  assert.equal(details.failedAllocationBytes, requested ? Number(requested) : null,
    'an unspecified native allocation size must remain unknown');
  const diagnosticStream = streams.get(0x4d450001);
  assert.notEqual(diagnosticStream, undefined, 'the dump must embed its diagnostic evidence');
  const diagnosticEntry = [...Array(dump.readUInt32LE(8)).keys()]
    .map(index => dump.readUInt32LE(12) + index * 12)
    .find(entry => dump.readUInt32LE(entry) === 0x4d450001);
  const embedded = JSON.parse(dump.toString('utf8', diagnosticStream,
    diagnosticStream + dump.readUInt32LE(diagnosticEntry + 4)));
  assert.equal(embedded.exceptionThreadId, details.exceptionThreadId);
  assert.deepEqual(embedded.image, details.image);
  assert.deepEqual(embedded.v8, details.v8);
  assert.equal(embedded.failedAllocationBytes, details.failedAllocationBytes);
  const modules = streams.get(4);
  assert.notEqual(modules, undefined);
  let matchedSymbols = false;
  for (let index = 0; index < dump.readUInt32LE(modules); index++) {
    const module = modules + 4 + index * 108;
    const cvSize = dump.readUInt32LE(module + 76);
    const cv = dump.readUInt32LE(module + 80);
    if (cvSize < 24 || dump.toString('ascii', cv, cv + 4) !== 'RSDS') continue;
    const guid = dump.readUInt32LE(cv + 4).toString(16).padStart(8, '0')
      + dump.readUInt16LE(cv + 8).toString(16).padStart(4, '0')
      + dump.readUInt16LE(cv + 10).toString(16).padStart(4, '0')
      + dump.subarray(cv + 12, cv + 20).toString('hex');
    if (guid.toUpperCase() === metadata.image.pdbGuid) {
      assert.equal(dump.readUInt32LE(cv + 20), metadata.image.pdbAge);
      matchedSymbols = true;
    }
  }
  assert.ok(matchedSymbols, 'diagnostic symbol identity must match the module recorded in the actual dump');
  finish();
}).catch(finish);
