// Run in application mode (--app=...) and ELECTRON_RUN_AS_NODE=1.
// Exercises the delivered engine, including its checked-in Windows snapshot.
const assert = require('node:assert/strict');
const { AsyncLocalStorage, AsyncResource, createHook } = require('node:async_hooks');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const { Worker, isMainThread, parentPort, workerData } = require('node:worker_threads');
const vm = require('node:vm');

async function probe() {
  const storage = new AsyncLocalStorage();
  const foreign = vm.runInNewContext('(async function(read) { await Promise.resolve(); return read(); })');
  const read = () => storage.getStore();
  const result = await Promise.all(['alpha', 'beta'].map(scope => storage.run(scope, async () => {
    assert.equal(read(), scope);
    await Promise.resolve();
    assert.equal(read(), scope, 'await retains its own scope');
    assert.equal(await foreign(read), scope, 'foreign Promise realm retains scope');
    await new Promise(resolve => queueMicrotask(resolve));
    assert.equal(read(), scope, 'microtask retains scope');
    await new Promise(resolve => setTimeout(resolve, 1));
    assert.equal(read(), scope, 'timer retains scope');
    const bound = AsyncResource.bind(read);
    assert.equal(storage.run('nested', bound), scope, 'bound callback retains captured scope');
    await storage.run('nested', async () => {
      await Promise.resolve();
      assert.equal(read(), 'nested');
    });
    assert.equal(read(), scope, 'nested run restores parent scope');
    await Promise.reject(new Error('expected')).catch(() => assert.equal(read(), scope));
    return read();
  })));
  assert.deepEqual(result, ['alpha', 'beta']);
  assert.equal(read(), undefined, 'concurrent scopes never leak to caller');
  storage.disable();

  const ids = new WeakMap();
  const events = new Map();
  const hook = createHook({
    init(id, type, _trigger, resource) {
      if (type === 'PROMISE') { ids.set(resource, id); events.set(id, ['init']); }
    },
    before(id) { events.get(id)?.push('before'); },
    after(id) { events.get(id)?.push('after'); },
    promiseResolve(id) { events.get(id)?.push('resolve'); },
  }).enable();
  try {
    const continuation = Promise.resolve('value').then(value => {
      assert.equal(value, 'value');
      assert.deepEqual(events.get(ids.get(continuation)), ['init', 'before']);
      return 'settled';
    });
    assert.equal(await continuation, 'settled');
    assert.deepEqual(events.get(ids.get(continuation)), ['init', 'before', 'resolve', 'after'],
      'Promise hooks fire once, in order, around the actual continuation');
  } finally { hook.disable(); }
  return { scopes: result, promiseHooks: 'ordered-once', node: process.version };
}

async function main() {
  const mainResult = await probe();
  const worker = new Worker(__filename, { workerData: { asyncContextProbe: true } });
  const workerResult = await new Promise((resolve, reject) => {
    let message;
    const timer = setTimeout(() => { worker.terminate(); reject(new Error('async context worker timed out')); }, 15000);
    worker.once('message', result => { message = result; });
    worker.once('error', error => { clearTimeout(timer); reject(error); });
    worker.once('exit', code => {
      clearTimeout(timer);
      if (code !== 0) reject(new Error(`worker exited ${code}`));
      else resolve(message);
    });
  });
  if (!process.argv.includes('--node-child')) {
    for (const flags of [[], ['--async-context-frame']]) {
      const child = spawnSync(process.execPath, [...flags, __filename, '--node-child'], {
        env: { ...process.env, ELECTRON_RUN_AS_NODE: '1', NODE_OPTIONS: '' },
        encoding: 'utf8', timeout: 30000, windowsHide: true,
      });
      assert.ifError(child.error);
      assert.equal(child.status, 0, child.stderr || child.stdout);
      assert.equal(JSON.parse(child.stdout).promiseHooks, 'ordered-once');
    }
  }
  fs.writeSync(1, `${JSON.stringify({ ...mainResult, worker: workerResult })}\n`);
}

if (!isMainThread && workerData?.asyncContextProbe) {
  probe().then(result => { parentPort.postMessage(result); parentPort.close(); }).catch(error => { throw error; });
} else if (process.env.ELECTRON_RUN_AS_NODE) {
  main().catch(error => { fs.writeSync(2, `${error.stack}\n`); process.exitCode = 1; });
} else {
  const { app } = require('electron');
  app.whenReady().then(main).then(() => app.exit(0), error => { fs.writeSync(2, `${error.stack}\n`); app.exit(1); });
}
