'use strict';

// Driver: node test/broker-storage-persistence-smoke.js --runtime <mini-electron.exe>
// Optional: --output-dir <artifact-directory>. Every invocation creates its own
// application package/name before native startup, and its own fresh userData.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const http = require('node:http');
const { spawn } = require('node:child_process');

const secondaryPartition = 'persist:storage-smoke-secondary';
const ephemeralPartition = 'storage-smoke-memory-only';
const pause = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));

async function driver() {
  const option = name => {
    const index = process.argv.indexOf(name);
    return index < 0 ? undefined : process.argv[index + 1];
  };
  const runtime = option('--runtime');
  assert.ok(runtime && fs.statSync(runtime).isFile(), '--runtime must name the real mini-electron executable');
  const output = path.resolve(option('--output-dir') || os.tmpdir());
  fs.mkdirSync(output, { recursive: true });
  const root = fs.mkdtempSync(path.join(output, 'mini-broker-storage-'));
  const application = path.join(root, 'application');
  const profile = path.join(root, 'user-data');
  const name = `mini-broker-storage-${path.basename(root)}-${process.pid}`;
  fs.mkdirSync(application);
  fs.mkdirSync(profile);
  // The legacy Windows migrator scans executable-adjacent profiles even for a
  // newly named application. Mark only our fresh fixtures as already migrated
  // before any Session exists, so this smoke never copies a user's old profile.
  for (const directory of [profile,
    path.join(profile, 'Partitions', secondaryPartition.slice('persist:'.length)),
    path.join(profile, 'Partitions', 'storage-smoke-shutdown-error')]) {
    fs.mkdirSync(directory, { recursive: true });
    fs.writeFileSync(path.join(directory, '.mini-electron-profile-migrated'), 'isolated-native-smoke\n');
  }
  fs.writeFileSync(path.join(root, 'isolated-smoke.json'), JSON.stringify({ name, profile }));
  fs.writeFileSync(path.join(application, 'package.json'), JSON.stringify({ name, version: '1.0.0', main: path.resolve(__filename) }));
  const servers = [];
  try {
    const origins = [];
    for (let index = 0; index < 2; ++index) {
      const server = http.createServer((_request, response) => {
        response.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
        response.end('<!doctype html><title>isolated native storage smoke</title><body>storage</body>');
      });
      servers.push(server);
      await new Promise((resolve, reject) => {
        server.once('error', reject);
        server.listen(0, '127.0.0.1', resolve);
      });
      origins.push(`http://127.0.0.1:${server.address().port}`);
    }
    const results = [];
    for (const phase of ['write', 'restart', 'exit-write', 'exit-read', 'quit-read', 'exit-error']) {
      const childEnvironment = { ...process.env, NODE_OPTIONS: '',
        MINI_STORAGE_SMOKE_CHILD: '1', MINI_STORAGE_SMOKE_ROOT: root,
        MINI_STORAGE_SMOKE_PHASE: phase, MINI_STORAGE_SMOKE_ORIGINS: JSON.stringify(origins) };
      // The native host treats presence, including an empty value, as Node-only mode.
      delete childEnvironment.ELECTRON_RUN_AS_NODE;
      const result = await new Promise((resolve, reject) => {
        const child = spawn(path.resolve(runtime), [`--app=${application}`], {
          env: childEnvironment,
          windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'],
        });
        let stdout = '', stderr = '';
        const watchdog = setTimeout(() => {
          child.kill();
          reject(new Error(`${phase} exceeded 90s watchdog\n${stdout}\n${stderr}`));
        }, 90000);
        child.stdout.on('data', data => { stdout += data; });
        child.stderr.on('data', data => { stderr += data; });
        child.once('error', error => { clearTimeout(watchdog); reject(error); });
        child.once('close', (code, signal) => {
          clearTimeout(watchdog);
          resolve({ phase, code, signal, stdout, stderr });
        });
      });
      fs.writeFileSync(path.join(root, `${phase}.json`), JSON.stringify(result, null, 2));
      assert.equal(result.signal, null, JSON.stringify(result));
      assert.equal(result.code, phase === 'exit-error' ? 1 : 0, JSON.stringify(result));
      assert.match(result.stdout, new RegExp(`PASS: broker storage ${phase}`), JSON.stringify(result));
      if (phase === 'exit-error')
        assert.match(result.stderr, /storage shutdown failed:.*Unable to commit broker storage/, JSON.stringify(result));
      results.push({ phase, code: result.code });
    }
    const evidence = { runtime: path.resolve(runtime), root, origins, results };
    fs.writeFileSync(path.join(root, 'result.json'), JSON.stringify(evidence, null, 2));
    console.log(`PASS: broker storage persistence, ordering, no-op suppression, flush/quit/exit durability and explicit errors\n${JSON.stringify(evidence)}`);
  } finally {
    await Promise.all(servers.map(server => new Promise(resolve => server.close(resolve))));
  }
}

async function nativeChild() {
  // The generated package has already supplied a unique name to native startup.
  // Refuse standalone child invocations rather than touching a default profile.
  const root = path.resolve(process.env.MINI_STORAGE_SMOKE_ROOT || '');
  const isolated = JSON.parse(fs.readFileSync(path.join(root, 'isolated-smoke.json'), 'utf8'));
  assert.equal(isolated.profile, path.join(root, 'user-data'));
  assert.match(isolated.name, /^mini-broker-storage-mini-broker-storage-/);
  const { app, BrowserWindow, session } = require('electron');
  app.setName(isolated.name);
  app.setPath('userData', isolated.profile);
  const origins = JSON.parse(process.env.MINI_STORAGE_SMOKE_ORIGINS);
  const phase = process.env.MINI_STORAGE_SMOKE_PHASE;
  const windows = [];
  app.on('window-all-closed', () => {});
  const watchdog = setTimeout(() => fail(new Error(`${phase} native phase exceeded 75s`)), 75000);
  function fail(error) {
    clearTimeout(watchdog);
    fs.writeSync(2, `${error.stack || error}\n`);
    app.exit(2);
  }
  const evaluate = async (window, body) => JSON.parse(await window.webContents.executeJavaScript(
    `JSON.stringify((() => { ${body}\n})())`));
  const snapshot = window => evaluate(window, 'return Object.fromEntries(Object.keys(localStorage).sort().map(key => [key, localStorage.getItem(key)]));');
  const storageFile = selected => path.join(selected.getStoragePath(), 'BrokerStorage.json');
  const readDisk = selected => JSON.parse(fs.readFileSync(storageFile(selected), 'utf8'));
  const flush = selected => selected.flushStorageData();
  async function open(partition = '', origin = origins[0]) {
    const window = new BrowserWindow({ show: false, webPreferences: {
      partition, sandbox: true, contextIsolation: true, nodeIntegration: false,
    } });
    windows.push(window);
    window.webContents.on('render-process-gone', (_event, details) => fail(new Error(`Renderer failed: ${JSON.stringify(details)}`)));
    await window.loadURL(`${origin}/`);
    return window;
  }
  async function leavePending(selected, window, mutation) {
    const file = storageFile(selected), backup = `${file}.exit-backup`;
    // Loading the HTTP page does not touch DOMStorage. Initialize this
    // session's broker from its valid file before replacing the target;
    // otherwise its first load fails and no accepted mutation/writer exists.
    const before = await snapshot(window);
    assert.deepEqual(readDisk(selected)[origins[0]], before,
      `${phase}: ${selected.getPartition()} baseline was not loaded intact`);
    fs.renameSync(file, backup);
    fs.mkdirSync(file);
    await evaluate(window, mutation);
    assert.throws(() => flush(selected), /Unable to commit broker storage/,
      `${phase}: ${selected.getPartition()} first blocked flush`);
    // Drain the failed scheduled attempt while the target is still unwritable.
    // Restoring the path below does not enqueue a retry: shutdown must do that.
    await pause(100);
    assert.throws(() => flush(selected), /Unable to commit broker storage/,
      `${phase}: ${selected.getPartition()} drained blocked flush`);
    return () => {
      fs.rmdirSync(file);
      fs.unlinkSync(backup);
    };
  }
  function finish(kind = 'exit') {
    clearTimeout(watchdog);
    fs.writeSync(1, `PASS: broker storage ${phase}\n`);
    if (kind === 'quit') app.quit();
    else app.exit(0);
  }
  try {
    await app.whenReady();
    assert.equal(process.platform, 'win32', 'this smoke qualifies the Windows native broker');
    assert.equal(path.resolve(app.getPath('userData')), isolated.profile);
    const primary = session.defaultSession;
    const secondary = session.fromPartition(secondaryPartition);
    assert.equal(typeof primary.flushStorageData, 'function');
    assert.ok(path.resolve(primary.getStoragePath()).startsWith(`${root}${path.sep}`));
    assert.ok(path.resolve(secondary.getStoragePath()).startsWith(`${root}${path.sep}`));

    if (phase === 'write') {
      // This is the existing on-disk format, not a new persistence schema.
      fs.writeFileSync(storageFile(primary), JSON.stringify({ [origins[0]]: { legacy: 'compatible' } }));
      const first = await open();
      assert.deepEqual(await snapshot(first), { legacy: 'compatible' });
      assert.deepEqual(await evaluate(first, `
        localStorage.setItem('visible', 'first');
        if (localStorage.getItem('visible') !== 'first') throw new Error('set was not immediately visible');
        localStorage.removeItem('visible');
        if (localStorage.getItem('visible') !== null) throw new Error('remove was not immediately visible');
        localStorage.setItem('discard', 'before-clear');
        localStorage.clear();
        if (localStorage.length !== 0) throw new Error('clear was not immediately visible');
        localStorage.setItem('stable', 'same');
        localStorage.setItem('preserved', '中文😀');
        localStorage.setItem('empty', '');
        localStorage.setItem('ordered', 'first');
        localStorage.removeItem('ordered');
        localStorage.setItem('ordered', 'last');
        sessionStorage.setItem('memory', 'session-only');
        return { value: localStorage.getItem('ordered'), session: sessionStorage.getItem('memory') };
      `), { value: 'last', session: 'session-only' });
      const primaryExpected = { empty: '', ordered: 'last', preserved: '中文😀', stable: 'same' };
      assert.deepEqual(await snapshot(first), primaryExpected);
      flush(primary);
      assert.deepEqual(readDisk(primary)[origins[0]], primaryExpected);

      const emptyOrigin = await open('', origins[1]);
      const file = storageFile(primary);
      const oldTime = new Date('2001-01-01T00:00:00.000Z');
      fs.utimesSync(file, oldTime, oldTime);
      const before = fs.statSync(file, { bigint: true });
      const beforeBytes = fs.readFileSync(file, 'utf8');
      await evaluate(first, `for (let i = 0; i < 32; ++i) { localStorage.setItem('stable', 'same'); localStorage.removeItem('missing'); } return true;`);
      await evaluate(emptyOrigin, 'localStorage.clear(); localStorage.removeItem("missing"); return localStorage.length;');
      flush(primary);
      await pause(100); // Also cross the writer's coalescing interval after the barrier.
      flush(primary);
      assert.equal(fs.statSync(file, { bigint: true }).mtimeNs, before.mtimeNs, 'no-op mutations replaced the file');
      assert.equal(fs.readFileSync(file, 'utf8'), beforeBytes);

      const quota = await evaluate(first, `
        let name = '';
        try { localStorage.setItem('oversize', 'x'.repeat(11 * 1024 * 1024)); } catch (error) { name = error.name; }
        return { name, value: localStorage.getItem('oversize'), stable: localStorage.getItem('stable') };
      `);
      assert.equal(quota.name, 'QuotaExceededError');
      assert.equal(quota.value, null);
      assert.equal(quota.stable, 'same');
      await evaluate(emptyOrigin, 'localStorage.setItem("other-origin", "isolated"); return true;');
      const partitionWindow = await open(secondaryPartition);
      assert.deepEqual(await snapshot(partitionWindow), {});
      await evaluate(partitionWindow, 'localStorage.setItem("partition", "secondary"); return true;');
      const memory = session.fromPartition(ephemeralPartition);
      const memoryWindow = await open(ephemeralPartition);
      assert.equal(memory.getStoragePath(), null);
      await evaluate(memoryWindow, 'localStorage.setItem("ephemeral", "not-on-disk"); return localStorage.getItem("ephemeral");');
      flush(memory);
      flush(primary);
      flush(secondary);
      assert.deepEqual(readDisk(primary), { [origins[0]]: primaryExpected, [origins[1]]: { 'other-origin': 'isolated' } });
      assert.deepEqual(readDisk(secondary), { [origins[0]]: { partition: 'secondary' } });
      finish();
    } else if (phase === 'restart') {
      const first = await open();
      assert.deepEqual(await snapshot(first), { empty: '', ordered: 'last', preserved: '中文😀', stable: 'same' });
      assert.equal(await evaluate(first, 'return sessionStorage.getItem("memory");'), null);
      assert.deepEqual(await snapshot(await open('', origins[1])), { 'other-origin': 'isolated' });
      assert.deepEqual(await snapshot(await open(secondaryPartition)), { partition: 'secondary' });
      assert.deepEqual(await snapshot(await open(ephemeralPartition)), {});

      // A directory at the target path is a deterministic Windows replacement
      // failure, unlike chmod/ACL checks that can pass under elevated accounts.
      const file = storageFile(primary), backup = `${file}.smoke-backup`;
      fs.renameSync(file, backup);
      fs.mkdirSync(file);
      await evaluate(first, 'localStorage.setItem("pending", "retained-after-error"); return localStorage.getItem("pending");');
      assert.throws(() => flush(primary), /Unable to commit broker storage/);
      // Persistence failure must not reject the next mutation while Blink
      // optimistically accepts it. Prove both the existing renderer's cache and
      // a fresh renderer see the same accepted set/remove/clear sequence.
      const afterFailure = { empty: '', later: 'also-accepted', ordered: 'last',
        pending: 'accepted-after-error', preserved: '中文😀' };
      await evaluate(first, `
        localStorage.setItem('pending', 'accepted-after-error');
        localStorage.setItem('later', 'also-accepted');
        localStorage.removeItem('stable');
        localStorage.setItem('transient', 'remove-me');
        localStorage.removeItem('transient');
        return localStorage.getItem('pending');
      `);
      const otherOrigin = await open('', origins[1]);
      await evaluate(otherOrigin, 'localStorage.clear(); return localStorage.length;');
      assert.deepEqual(await snapshot(first), afterFailure);
      assert.deepEqual(await snapshot(await open()), afterFailure,
        'accepted writes diverged from authoritative memory after persistence failure');
      assert.deepEqual(await snapshot(await open('', origins[1])), {});
      assert.throws(() => flush(primary), /Unable to commit broker storage/);
      fs.rmdirSync(file);
      // Do not restore the stale backup over a concurrently recovered writer.
      fs.unlinkSync(backup);
      flush(primary); // Retries every accepted delta, including writes after failure.
      assert.deepEqual(readDisk(primary)[origins[0]], afterFailure);
      assert.deepEqual(readDisk(primary)[origins[1]], {});
      assert.deepEqual(await snapshot(await open()), afterFailure,
        'recovery flush/reload lost an accepted mutation');

      // A failed clear still reports its persistence error and retains the
      // accepted clear for recovery, rather than resurrecting older data.
      fs.renameSync(file, backup);
      fs.mkdirSync(file);
      await assert.rejects(primary.clearStorageData({ storages: ['localstorage'] }), /Unable to commit broker storage/);
      assert.deepEqual(await snapshot(await open()), {});
      fs.rmdirSync(file);
      fs.unlinkSync(backup);
      flush(primary);
      assert.deepEqual(readDisk(primary), {});

      const recovered = await open();
      await evaluate(recovered, `for (let i = 0; i < 64; ++i) localStorage.setItem('doomed-' + i, 'old-' + i); localStorage.removeItem('doomed-1'); return true;`);
      await primary.clearStorageData({ storages: ['localstorage'] });
      assert.deepEqual(readDisk(primary), {});
      first.destroy();
      const cleared = await open();
      assert.deepEqual(await snapshot(cleared), {});
      await pause(100);
      flush(primary);
      assert.deepEqual(readDisk(primary), {}, 'an older queued write resurrected cleared data');
      await evaluate(cleared, 'localStorage.setItem("after-clear", "durable"); return true;');
      flush(primary);
      assert.deepEqual(readDisk(primary), { [origins[0]]: { 'after-clear': 'durable' } });
      finish();
    } else if (phase === 'exit-write') {
      const first = await open();
      assert.deepEqual(await snapshot(first), { 'after-clear': 'durable' });
      const restorePrimary = await leavePending(primary, first,
        'localStorage.setItem("exit", "latest-primary"); return localStorage.getItem("exit");');
      const partitionWindow = await open(secondaryPartition);
      const restoreSecondary = await leavePending(secondary, partitionWindow,
        'localStorage.setItem("exit", "latest-secondary"); return true;');
      restorePrimary();
      restoreSecondary();
      // No successful flush or retry delay after restoring the targets.
      // app.exit must drain both sessions' latest accepted in-memory writes.
      finish();
    } else if (phase === 'exit-read') {
      const first = await open();
      const partitionWindow = await open(secondaryPartition);
      assert.deepEqual(await snapshot(first), { 'after-clear': 'durable', exit: 'latest-primary' });
      assert.deepEqual(await snapshot(partitionWindow), { exit: 'latest-secondary', partition: 'secondary' });
      const restore = await leavePending(primary, first,
        'localStorage.setItem("quit", "normal-quit"); return true;');
      restore();
      finish('quit');
    } else if (phase === 'quit-read') {
      const first = await open();
      assert.deepEqual(await snapshot(first), { 'after-clear': 'durable', exit: 'latest-primary', quit: 'normal-quit' });
      finish();
    } else if (phase === 'exit-error') {
      const partition = 'persist:storage-smoke-shutdown-error';
      const selected = session.fromPartition(partition);
      const first = await open(partition);
      await evaluate(first, 'return localStorage.length;');
      fs.mkdirSync(storageFile(selected));
      await evaluate(first, 'localStorage.setItem("uncommitted", "must-report-error"); return true;');
      // Exit must surface the flush error as stderr plus nonzero status.
      finish();
    } else {
      throw new Error(`Unknown native smoke phase: ${phase}`);
    }
  } catch (error) { fail(error); }
}

if (process.env.MINI_STORAGE_SMOKE_CHILD === '1') nativeChild().catch(error => {
  fs.writeSync(2, `${error.stack || error}\n`);
  process.exit(2);
});
else driver().catch(error => { console.error(error.stack || error); process.exitCode = 1; });
