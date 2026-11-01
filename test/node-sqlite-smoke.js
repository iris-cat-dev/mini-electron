'use strict';

// Run with ELECTRON_RUN_AS_NODE=1 and either packaged runtime:
// out/windows-x64/runtime-dist/electron.exe test/node-sqlite-smoke.js
// out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron test/node-sqlite-smoke.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { DatabaseSync, backup } = require('node:sqlite');
const { Worker } = require('node:worker_threads');

async function main() {
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-node-sqlite-'));
  const databases = new Set();
  const open = (location, options = {}) => {
    const database = new DatabaseSync(location, options);
    databases.add(database);
    return database;
  };
  try {
    const file = path.join(temporary, 'agent.db');
    const database = open(file);
    assert.equal(database.isOpen, true);
    assert.equal(database.isTransaction, false);
    assert.throws(() => { database.isTransaction = true; }, TypeError);
    const unopened = open(':memory:', { open: false });
    assert.equal(unopened.isOpen, false);
    assert.throws(() => unopened.isTransaction, { code: 'ERR_INVALID_STATE' });
    unopened.open();
    assert.equal(unopened.isTransaction, false);
    unopened.close();
    assert.equal(unopened.isOpen, false);
    assert.throws(() => unopened.isTransaction, { code: 'ERR_INVALID_STATE' });
    databases.delete(unopened);
    const numbered = database.prepare('SELECT ?3 AS third, :name AS named, ?1 AS first, ?3 AS repeated, ? AS last');
    assert.deepEqual({ ...numbered.get({ name: 'named' }, 'first', 'unused', 'third', 'last') }, {
      third: 'third', named: 'named', first: 'first', repeated: 'third', last: 'last',
    });
    assert.throws(() => database.enableLoadExtension(true), { code: 'ERR_INVALID_STATE' });
    assert.throws(() => database.loadExtension(path.join(temporary, 'not-authorized-extension')), {
      code: 'ERR_INVALID_STATE',
    });
    database.exec(`
      PRAGMA journal_mode = WAL;
      CREATE TABLE auth_credentials (
        id INTEGER PRIMARY KEY,
        provider TEXT NOT NULL,
        credential_type TEXT NOT NULL,
        disabled_cause TEXT
      );
    `);
    const insert = database.prepare('INSERT INTO auth_credentials VALUES (?, ?, ?, ?)');
    insert.run(6, 'openai-codex', 'oauth', null);
    insert.run(7, 'openai-codex', 'oauth', 'disabled');
    insert.run(8, 'openai', 'api_key', null);
    const select = database.prepare(`
      SELECT id, provider FROM auth_credentials
      WHERE provider = ? AND credential_type = 'oauth' AND disabled_cause IS NULL
      ORDER BY id
    `);
    assert.deepEqual(select.all('openai-codex').map(row => ({ ...row })), [
      { id: 6, provider: 'openai-codex' },
    ]);
    assert.equal(select.columns()[0].table, 'auth_credentials');
    database.exec('BEGIN IMMEDIATE');
    assert.equal(database.isTransaction, true);
    insert.run(9, 'openai-codex', 'oauth', null);
    database.exec('SAVEPOINT nested');
    assert.equal(database.isTransaction, true);
    insert.run(10, 'openai-codex', 'oauth', null);
    database.exec('ROLLBACK TO nested');
    assert.equal(database.isTransaction, true);
    assert.equal(database.prepare('SELECT COUNT(*) AS count FROM auth_credentials WHERE id = 10').get().count, 0);
    database.exec('RELEASE nested');
    assert.equal(database.isTransaction, true);
    database.exec('ROLLBACK');
    assert.equal(database.isTransaction, false);
    assert.deepEqual(select.all('openai-codex').map(row => row.id), [6]);

    database.exec('SAVEPOINT outermost');
    assert.equal(database.isTransaction, true);
    database.exec('RELEASE outermost');
    assert.equal(database.isTransaction, false);
    database.exec('BEGIN');
    assert.equal(database.isTransaction, true);
    insert.run(9, 'openai-codex', 'oauth', null);
    database.exec('COMMIT');
    assert.equal(database.isTransaction, false);
    assert.equal(database.prepare('SELECT COUNT(*) AS count FROM auth_credentials WHERE id = 9').get().count, 1);
    database.exec('DELETE FROM auth_credentials WHERE id = 9');

    // Statement errors leave a transaction open; state-driven cleanup must release its writer lock.
    const contender = open(file);
    database.exec('BEGIN IMMEDIATE');
    assert.throws(() => insert.run(6, 'duplicate', 'oauth', null), { code: 'ERR_SQLITE_ERROR' });
    assert.equal(database.isTransaction, true);
    assert.throws(() => contender.exec('BEGIN IMMEDIATE'), /locked/i);
    assert.equal(contender.isTransaction, false);
    if (database.isTransaction) database.exec('ROLLBACK');
    assert.equal(database.isTransaction, false);
    contender.exec('BEGIN IMMEDIATE');
    assert.equal(contender.isTransaction, true);

    // A failed BEGIN must not claim a transaction or disturb another connection's writer lock.
    assert.throws(() => database.exec('BEGIN IMMEDIATE'), /locked/i);
    assert.equal(database.isTransaction, false);
    if (database.isTransaction) database.exec('ROLLBACK');
    assert.equal(contender.isTransaction, true);
    assert.throws(() => database.exec('BEGIN IMMEDIATE'), /locked/i);
    contender.exec('INSERT INTO auth_credentials VALUES (9, \'lock-owner\', \'oauth\', NULL)');
    contender.exec('COMMIT');
    assert.equal(contender.isTransaction, false);
    assert.equal(database.prepare('SELECT provider FROM auth_credentials WHERE id = 9').get().provider, 'lock-owner');
    database.exec('DELETE FROM auth_credentials WHERE id = 9');

    // SQLite can also roll back automatically: report engine state, not a cached BEGIN flag.
    database.exec('BEGIN IMMEDIATE');
    assert.equal(database.isTransaction, true);
    assert.throws(() => database.exec("INSERT OR ROLLBACK INTO auth_credentials VALUES (6, 'duplicate', 'oauth', NULL)"), {
      code: 'ERR_SQLITE_ERROR',
    });
    assert.equal(database.isTransaction, false);
    if (database.isTransaction) database.exec('ROLLBACK');
    database.exec('BEGIN IMMEDIATE');
    assert.equal(database.isTransaction, true);
    database.exec('COMMIT');
    assert.equal(database.isTransaction, false);

    const reader = open(file, { readOnly: true });
    assert.equal(reader.prepare('SELECT COUNT(*) AS count FROM auth_credentials').get().count, 3);
    assert.throws(() => reader.exec('DELETE FROM auth_credentials'), /readonly/i);
    const copy = path.join(temporary, 'backup.db');
    await backup(database, copy);
    const restored = open(copy, { readOnly: true });
    assert.equal(restored.prepare('SELECT provider FROM auth_credentials WHERE id = ?').get(6).provider, 'openai-codex');
    assert.equal(database.prepare('SELECT sqlite_version() AS version').get().version, process.versions.sqlite);

    const source = open(':memory:');
    const target = open(':memory:');
    source.exec('CREATE TABLE changes (id INTEGER PRIMARY KEY, value TEXT)');
    target.exec('CREATE TABLE changes (id INTEGER PRIMARY KEY, value TEXT)');
    const session = source.createSession();
    try {
      source.prepare('INSERT INTO changes VALUES (?, ?)').run(1, '额度');
      assert.equal(target.applyChangeset(session.changeset()), true);
      assert.equal(target.prepare('SELECT value FROM changes WHERE id = 1').get().value, '额度');
    } finally {
      session.close();
    }

    await new Promise((resolve, reject) => {
      let result;
      const worker = new Worker(`
        const { parentPort } = require('node:worker_threads');
        const { DatabaseSync } = require('node:sqlite');
        const db = new DatabaseSync(':memory:');
        parentPort.postMessage(db.prepare('SELECT 42 AS value').get().value);
        db.close();
      `, { eval: true });
      worker.on('message', value => { result = value; });
      worker.on('error', reject);
      worker.on('exit', code => {
        try {
          assert.equal(code, 0);
          assert.equal(result, 42);
          resolve();
        } catch (error) { reject(error); }
      });
    });
    console.log(`PASS: native SQLite ${process.versions.sqlite}, OAuth-style filtering, WAL, transaction state, numbered bindings, savepoints, rollback recovery, writer-lock isolation, read-only access, metadata, backup, changesets, worker binding, and extension-loading restrictions`);
  } finally {
    for (const database of databases) database.close();
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error.stack || error);
  process.exitCode = 1;
});
