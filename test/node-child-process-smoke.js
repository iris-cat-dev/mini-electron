'use strict';

// Normal application mode:
// out/windows-x64/runtime-dist/electron.exe test/node-child-process-smoke.js
// out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron test/node-child-process-smoke.js
// Node mode: prefix either command with ELECTRON_RUN_AS_NODE=1.
const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const { once } = require('node:events');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { promisify } = require('node:util');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-child-process-'));
const childScript = path.join(temporary, 'child.js');
const childEnvironment = { ...process.env, ELECTRON_RUN_AS_NODE: '1' };

function shellQuote(value) {
  if (process.platform === 'win32') return `"${value.replace(/"/g, '""')}"`;
  return `'${value.replace(/'/g, `'\\''`)}'`;
}

function writeUInt32(value) {
  const buffer = Buffer.alloc(4);
  buffer.writeUInt32LE(value, 0);
  return buffer;
}

function createAsar(archive, name, contents) {
  const parts = name.split('/');
  const leaf = parts.pop();
  const file = { size: contents.length, offset: '0', executable: true };
  let node = { files: { [leaf]: file } };
  while (parts.length) node = { files: { [parts.pop()]: node } };

  const headerString = Buffer.from(JSON.stringify(node));
  const stringPadding = Buffer.alloc((4 - (headerString.length % 4)) % 4);
  const headerPayload = Buffer.concat([writeUInt32(headerString.length), headerString, stringPadding]);
  const headerPickle = Buffer.concat([writeUInt32(headerPayload.length), headerPayload]);
  const sizePickle = Buffer.concat([writeUInt32(4), writeUInt32(headerPickle.length)]);
  const noAsar = process.noAsar;
  process.noAsar = true;
  try {
    fs.writeFileSync(archive, Buffer.concat([sizePickle, headerPickle, contents]));
  } finally {
    process.noAsar = noAsar;
  }
}

function capture(stream) {
  const chunks = [];
  stream.on('data', chunk => chunks.push(Buffer.from(chunk)));
  return () => Buffer.concat(chunks);
}

function assertChild(pending, label) {
  assert.ok(pending.child instanceof childProcess.ChildProcess, `${label} exposes its ChildProcess`);
  assert.ok(Number.isInteger(pending.child.pid) && pending.child.pid > 0, `${label} child has a pid`);
  return pending.child;
}

async function rejection(pending, label) {
  try {
    await pending;
  } catch (error) {
    return error;
  }
  assert.fail(`${label} unexpectedly resolved`);
}

async function verifyApi(api) {
  assert.equal(typeof api.callback[promisify.custom], 'function', `${api.name} custom promisifier`);

  let pending = api.invoke('success');
  let child = assertChild(pending, `${api.name} success`);
  let stdout = capture(child.stdout);
  let stderr = capture(child.stderr);
  let closed = once(child, 'close');
  let result = await pending;
  let [code, signal] = await closed;
  assert.deepEqual(result, { stdout: 'success-stdout', stderr: 'success-stderr' });
  assert.equal(stdout().toString(), result.stdout);
  assert.equal(stderr().toString(), result.stderr);
  assert.equal(code, 0);
  assert.equal(signal, null);
  assert.equal(child.exitCode, 0, `${api.name} promise.child is the child that completed`);

  pending = api.invoke('failure');
  child = assertChild(pending, `${api.name} failure`);
  closed = once(child, 'close');
  const failure = await rejection(pending, `${api.name} failure`);
  await closed;
  assert.equal(failure.code, 7);
  assert.equal(failure.stdout, 'failure-stdout');
  assert.equal(failure.stderr, 'failure-stderr');
  assert.equal(child.exitCode, 7);

  pending = api.invoke('timeout', { timeout: 2000, killSignal: 'SIGTERM' });
  child = assertChild(pending, `${api.name} timeout`);
  const timeout = await rejection(pending, `${api.name} timeout`);
  assert.equal(timeout.killed, true);
  assert.equal(timeout.signal, 'SIGTERM');
  assert.equal(timeout.stdout, 'timeout-stdout');
  assert.equal(timeout.stderr, 'timeout-stderr');
  assert.equal(child.killed, true, `${api.name} timeout exposes the killed child`);

  const controller = new AbortController();
  pending = api.invoke('abort', { signal: controller.signal });
  child = assertChild(pending, `${api.name} abort`);
  await Promise.all([once(child.stdout, 'data'), once(child.stderr, 'data')]);
  controller.abort();
  const aborted = await rejection(pending, `${api.name} abort`);
  assert.equal(aborted.name, 'AbortError');
  assert.equal(aborted.code, 'ABORT_ERR');
  assert.equal(aborted.stdout, 'abort-stdout');
  assert.equal(aborted.stderr, 'abort-stderr');
  assert.equal(child.killed, true, `${api.name} cancellation exposes the aborted child`);

  pending = api.invoke('buffer', { encoding: 'buffer' });
  assertChild(pending, `${api.name} buffer encoding`);
  result = await pending;
  assert.ok(Buffer.isBuffer(result.stdout));
  assert.ok(Buffer.isBuffer(result.stderr));
  assert.equal(result.stdout.toString(), 'buffer-stdout');
  assert.equal(result.stderr.toString(), 'buffer-stderr');
}

async function verifyAsarExecutable(execFileAsync, archive) {
  const entry = process.platform === 'win32' ? 'bin/fixture.exe' : 'bin/fixture';
  const args = process.platform === 'win32'
    ? ['/d', '/c', 'echo asar-stdout&& echo asar-stderr 1>&2']
    : ['-c', 'printf asar-stdout; printf asar-stderr >&2'];
  const pending = execFileAsync(path.join(archive, ...entry.split('/')), args, { encoding: 'utf8' });
  const child = assertChild(pending, 'execFile ASAR executable');
  const result = await pending;
  assert.equal(result.stdout.trim(), 'asar-stdout');
  assert.equal(result.stderr.trim(), 'asar-stderr');
  assert.equal(child.exitCode, 0, 'ASAR rewrite promise.child is the extracted executable process');
}

async function main() {
  const asarArgument = process.argv.indexOf('--verify-asar');
  if (asarArgument !== -1) {
    await verifyAsarExecutable(promisify(childProcess.execFile), process.argv[asarArgument + 1]);
    return;
  }
  fs.writeFileSync(childScript, `
    'use strict';
    const mode = process.argv[2];
    process.stdout.write(mode + '-stdout');
    process.stderr.write(mode + '-stderr');
    if (mode === 'failure') process.exitCode = 7;
    if (mode === 'timeout' || mode === 'abort') setInterval(() => {}, 1000);
  `);

  const execFileAsync = promisify(childProcess.execFile);
  const execAsync = promisify(childProcess.exec);
  const noAsarBeforeInvalidArguments = process.noAsar;
  assert.throws(() => execAsync(null), { code: 'ERR_INVALID_ARG_TYPE' });
  assert.equal(process.noAsar, noAsarBeforeInvalidArguments);
  assert.throws(() => execFileAsync(null), { code: 'ERR_INVALID_ARG_TYPE' });
  const apis = [
    {
      name: 'execFile',
      callback: childProcess.execFile,
      invoke(mode, options = {}) {
        return execFileAsync(process.execPath, [childScript, mode], {
          ...options,
          env: childEnvironment,
        });
      },
    },
    {
      name: 'exec',
      callback: childProcess.exec,
      invoke(mode, options = {}) {
        const command = mode === 'timeout' || mode === 'abort'
          ? process.platform === 'win32'
            ? `<nul set /p \"=${mode}-stdout\" & >&2 <nul set /p \"=${mode}-stderr\" & set /p hang=`
            : `printf ${mode}-stdout; printf ${mode}-stderr >&2; read hang`
          : `${shellQuote(process.execPath)} ${shellQuote(childScript)} ${mode}`;
        return execAsync(command, {
          ...options,
          env: childEnvironment,
        });
      },
    },
  ];

  for (const api of apis) await verifyApi(api);
  const archive = path.join(temporary, 'fixture.asar');
  const executable = process.platform === 'win32'
    ? process.env.ComSpec || process.env.COMSPEC || path.join(process.env.SystemRoot, 'System32', 'cmd.exe')
    : '/bin/sh';
  const entry = process.platform === 'win32' ? 'bin/fixture.exe' : 'bin/fixture';
  createAsar(archive, entry, fs.readFileSync(executable));
  // The native archive cache holds its file open until process exit on Windows.
  // Exercise rewriting in a child, then remove its archive after those handles close.
  const entryArgument = Object.hasOwn(process.env, 'ELECTRON_RUN_AS_NODE') ? __filename : `--app=${__filename}`;
  await execFileAsync(process.execPath, [entryArgument, '--verify-asar', archive], { env: process.env });
  console.log('PASS: child_process exec/execFile promisification, child identity, failures, timeout, cancellation, buffers, and ASAR executable rewriting');
}

async function runAndCleanup() {
  try {
    await main();
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

if (Object.hasOwn(process.env, 'ELECTRON_RUN_AS_NODE')) {
  runAndCleanup().catch(error => {
    console.error(error.stack || error);
    process.exitCode = 1;
  });
} else {
  const { app } = require('electron');
  app.whenReady().then(runAndCleanup).then(
    () => app.exit(0),
    error => { console.error(error.stack || error); app.exit(1); },
  );
}
