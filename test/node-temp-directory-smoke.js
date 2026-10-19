'use strict';

// Run in both application mode and ELECTRON_RUN_AS_NODE=1.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

function fromCallback(prefix, options) {
  return new Promise((resolve, reject) => {
    fs.mkdtemp(prefix, options, (error, directory) => {
      if (error) reject(error);
      else resolve(directory);
    });
  });
}

async function run() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-temp-directory-'));
  const prefix = path.join(root, '临时-images-');
  const created = new Set();
  function verify(directory) {
    const name = Buffer.isBuffer(directory) ? directory.toString() : directory;
    assert.equal(typeof name, 'string');
    assert.equal(path.dirname(name), root);
    assert.equal(name.slice(0, prefix.length), prefix);
    assert.equal(name.length, prefix.length + 6);
    assert.equal(created.has(name), false, 'each call must create a distinct directory');
    assert.equal(fs.statSync(name).isDirectory(), true);
    assert.deepEqual(fs.readdirSync(name), []);
    fs.writeFileSync(path.join(name, 'owner.txt'), String(created.size));
    created.add(name);
  }
  try {
    // Repeated synchronous calls exercise collision retries on the same stack.
    for (let index = 0; index < 32; index++) verify(fs.mkdtempSync(prefix));
    for (let index = 0; index < 32; index++) verify(await fromCallback(prefix));
    for (let index = 0; index < 32; index++) verify(await fsp.mkdtemp(prefix));
    const parallel = await Promise.all(Array.from({ length: 32 }, () => fsp.mkdtemp(prefix)));
    parallel.forEach(verify);
    verify(fs.mkdtempSync(Buffer.from(prefix), { encoding: 'buffer' }));
    verify(await fromCallback(Buffer.from(prefix), { encoding: 'buffer' }));
    verify(await fsp.mkdtemp(Buffer.from(prefix), { encoding: 'buffer' }));
    let index = 0;
    for (const directory of created) {
      assert.equal(fs.readFileSync(path.join(directory, 'owner.txt'), 'utf8'), String(index++),
        'later calls must not reuse or overwrite a previous directory');
    }
    const missing = path.join(root, 'missing', 'temp-');
    assert.throws(() => fs.mkdtempSync(missing), { code: 'ENOENT' });
    await assert.rejects(fromCallback(missing), { code: 'ENOENT' });
    await assert.rejects(fsp.mkdtemp(missing), { code: 'ENOENT' });
    console.log(`PASS: ${created.size} distinct temporary directories across sync, callback, promises, concurrent, UTF-8, and Buffer paths; missing parents reject with ENOENT`);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
}

function reportFailure(error) {
  console.error(error.stack || error);
  return 1;
}

const deadline = setTimeout(() => {
  console.error('Temporary directory creation timed out');
  process.exit(1);
}, 20000);

if (process.env.ELECTRON_RUN_AS_NODE === '1') {
  run().then(() => clearTimeout(deadline), error => {
    clearTimeout(deadline);
    process.exitCode = reportFailure(error);
  });
} else {
  const { app } = require('electron');
  app.on('window-all-closed', () => {});
  app.whenReady().then(run).then(
    () => { clearTimeout(deadline); app.exit(0); },
    error => { clearTimeout(deadline); app.exit(reportFailure(error)); },
  );
}
