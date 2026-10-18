'use strict';

// ELECTRON_RUN_AS_NODE=1 out/mac-arm64/mini-electron test/node-sqlite-smoke.js
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
    insert.run(9, 'openai-codex', 'oauth', null);
    database.exec('ROLLBACK');
    assert.deepEqual(select.all('openai-codex').map(row => row.id), [6]);

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
    console.log(`PASS: native SQLite ${process.versions.sqlite}, OAuth-style filtering, WAL, rollback, read-only access, metadata, backup, changesets, and worker binding`);
  } finally {
    for (const database of databases) database.close();
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error.stack || error);
  process.exitCode = 1;
});
