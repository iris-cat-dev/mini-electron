'use strict';

// Run in the native runtime, not Node. Exercises the embedded IndexedDB used by
// Desktop's replica cache, including the global root that previously kept every
// completed write and its cloned values alive.
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-idb-lifetime-'));
app.setName(`mini-idb-lifetime-${process.pid}`);
for (const name of ['userData', 'sessionData', 'logs', 'crashDumps']) {
  const directory = path.join(temporary, name);
  fs.mkdirSync(directory);
  app.setPath(name, directory);
}
let window;
let finished = false;
const deadline = setTimeout(() => finish(new Error('IndexedDB lifetime smoke timed out')), 90000);
app.on('window-all-closed', () => {});

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  try {
    if (window && !window.isDestroyed()) window.destroy();
  } finally {
    try { fs.rmSync(temporary, { recursive: true, force: true }); }
    catch (cleanupError) { error ||= cleanupError; }
    if (error) console.error(error.stack || error);
    else console.log('PASS: IndexedDB overwrites release completed transactions; rollback, queued writes and upgrade ordering remain correct');
    app.exit(error ? 1 : 0);
  }
}

async function exercise() {
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  const task = () => new Promise(resolve => {
    if (typeof scheduler !== 'undefined') scheduler.postTask(resolve);
    else setTimeout(resolve, 0);
  });
  const settle = async () => { await task(); await task(); };
  const result = request => new Promise((resolve, reject) => {
    request.addEventListener('success', () => resolve(request.result));
    request.addEventListener('error', () => reject(request.error));
  });
  const completed = transaction => new Promise((resolve, reject) => {
    transaction.addEventListener('complete', resolve);
    transaction.addEventListener('abort', () => reject(transaction.error || new Error('Unexpected abort')));
  });
  const aborted = transaction => new Promise((resolve, reject) => {
    transaction.addEventListener('abort', resolve);
    transaction.addEventListener('complete', () => reject(new Error('Expected rollback, not commit')));
  });
  const open = (name, version, upgrade) => {
    const request = indexedDB.open(name, version);
    if (upgrade) request.addEventListener('upgradeneeded', () => upgrade(request));
    return result(request);
  };
  const write = async (database, value) => {
    const transaction = database.transaction('values', 'readwrite');
    const done = completed(transaction);
    transaction.objectStore('values').put(value, 'single-key');
    await done;
  };
  const read = async database => {
    const transaction = database.transaction('values', 'readonly');
    const done = completed(transaction);
    const value = await result(transaction.objectStore('values').get('single-key'));
    await done;
    return value;
  };
  const count = async database => {
    const transaction = database.transaction('values', 'readonly');
    const done = completed(transaction);
    const value = await result(transaction.objectStore('values').count());
    await done;
    return value;
  };
  check(indexedDB._databases instanceof Map, 'This regression must exercise the embedded IndexedDB, not a replacement');
  const name = 'owned-transaction-lifetime';
  const census = async stage => {
    await settle();
    const raw = indexedDB._databases.get(name);
    const transactions = raw.transactions;
    const finished = transactions.filter(transaction => transaction._state === 'finished');
    check(finished.length === 0,
      `${stage}: database still roots ${finished.length} finished transactions and their historical values`);
    check(transactions.length === 0, `${stage}: a completed workload still has pending transactions`);
    return { stage, transactions: transactions.length, finished: finished.length };
  };
  const observations = [];
  let database = await open(name, 1, request => request.result.createObjectStore('values'));

  for (let batch = 0; batch < 3; ++batch) {
    for (let index = 0; index < 32; ++index) {
      const sequence = batch * 32 + index + 1;
      await write(database, JSON.stringify({ sequence, text: String.fromCharCode(65 + sequence % 26).repeat(524288) }));
    }
    const latest = JSON.parse(await read(database));
    check(latest.sequence === (batch + 1) * 32 && latest.text.length === 524288,
      'Overwriting one key lost or corrupted the newest value');
    check(await count(database) === 1, 'Overwriting one key accumulated database records');
    observations.push(await census(`overwrite-${(batch + 1) * 32}`));
  }

  await write(database, 'committed');
  {
    const transaction = database.transaction('values', 'readwrite');
    const done = aborted(transaction);
    transaction.objectStore('values').put('rolled-back', 'single-key')
      .addEventListener('success', () => transaction.abort());
    await done;
    check(await read(database) === 'committed', 'Abort did not restore the overwritten record');
    observations.push(await census('explicit-abort-after-write'));
  }
  {
    const transaction = database.transaction('values', 'readwrite');
    const done = aborted(transaction);
    const pending = result(transaction.objectStore('values').put('never-written', 'single-key'))
      .then(() => 'success', error => error.name);
    transaction.abort();
    await done;
    check(await pending === 'AbortError', 'Aborting a queued request did not cancel it');
    check(await read(database) === 'committed', 'An immediately aborted write changed the record');
    observations.push(await census('abort-before-start'));
  }
  {
    const transaction = database.transaction('values', 'readwrite');
    const done = aborted(transaction);
    transaction.objectStore('values').put('must-roll-back', 'single-key');
    const duplicate = result(transaction.objectStore('values').add('duplicate', 'single-key'))
      .then(() => 'success', error => error.name);
    await done;
    check(await duplicate === 'ConstraintError', 'Duplicate add did not abort with ConstraintError');
    check(await read(database) === 'committed', 'An automatic abort did not roll back earlier requests');
    observations.push(await census('constraint-error-abort'));
  }
  {
    const order = [];
    let thirdDone;
    const first = database.transaction('values', 'readwrite');
    const firstDone = completed(first);
    first.objectStore('values').put('first', 'single-key');
    first.addEventListener('complete', () => {
      order.push('first');
      const third = database.transaction('values', 'readwrite');
      thirdDone = completed(third);
      third.objectStore('values').put('third', 'single-key');
      third.addEventListener('complete', () => order.push('third'));
    });
    const second = database.transaction('values', 'readwrite');
    const secondDone = completed(second);
    second.objectStore('values').put('second', 'single-key');
    second.addEventListener('complete', () => order.push('second'));
    await Promise.all([firstDone, secondDone]);
    await thirdDone;
    check(order.join(',') === 'first,second,third', 'Cleanup reordered overlapping queued transactions');
    check(await read(database) === 'third', 'Reentrant completion lost the last queued write');
    observations.push(await census('queued-and-reentrant-writes'));
  }
  {
    const transaction = database.transaction('values', 'readwrite');
    const done = completed(transaction);
    transaction.objectStore('values').put('explicit-commit', 'single-key');
    transaction.commit();
    await done;
    check(await read(database) === 'explicit-commit', 'Explicit commit did not retain its value');
    observations.push(await census('explicit-commit'));
  }

  database.close();
  const upgradeEvents = [];
  {
    const request = indexedDB.open(name, 2);
    const failed = result(request).then(() => 'success', error => error.name);
    request.addEventListener('upgradeneeded', () => {
      const transaction = request.transaction;
      request.result.createObjectStore('aborted-store');
      transaction.addEventListener('abort', () => {
        check(request.transaction === transaction, 'Upgrade transaction was detached before its abort event');
        upgradeEvents.push('abort');
      });
      transaction.abort();
    });
    check(await failed === 'AbortError', 'Aborted upgrade unexpectedly succeeded');
    check(request.transaction === null, 'Open request retained an aborted upgrade transaction');
    observations.push(await census('aborted-upgrade'));
  }
  database = await open(name, 1);
  check(database.version === 1 && !database.objectStoreNames.contains('aborted-store'),
    'Aborted upgrade did not restore the version and object stores');
  check(await read(database) === 'explicit-commit', 'Aborted upgrade corrupted the existing record');
  database.close();
  {
    const request = indexedDB.open(name, 2);
    const opened = result(request);
    request.addEventListener('upgradeneeded', () => {
      const transaction = request.transaction;
      request.result.createObjectStore('committed-store');
      transaction.addEventListener('complete', () => {
        check(request.transaction === transaction, 'Upgrade transaction was detached before its complete event');
        upgradeEvents.push('complete');
      });
    });
    database = await opened;
    check(request.transaction === null, 'Open request retained a committed upgrade transaction');
    check(database.version === 2 && database.objectStoreNames.contains('committed-store'),
      'Committed upgrade lost the version or object store');
    check(upgradeEvents.join(',') === 'abort,complete', 'Upgrade terminal events changed order');
    observations.push(await census('committed-upgrade'));
  }
  database.close();
  await result(indexedDB.deleteDatabase(name));
  check(!indexedDB._databases.has(name), 'Deleting a closed database retained its database root');
  database = await open(name, 1, request => request.result.createObjectStore('values'));
  check(await count(database) === 0 && await read(database) === undefined,
    'Delete and reopen restored deleted records');
  observations.push(await census('delete-and-reopen'));
  database.close();
  await result(indexedDB.deleteDatabase(name));
  return { writes: 96, payloadCharacters: 524288, observations, upgradeEvents };
}

app.whenReady().then(async () => {
  window = new BrowserWindow({ show: false, width: 480, height: 320,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  window.webContents.on('render-process-gone', (_event, details) => finish(new Error(JSON.stringify(details))));
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent('<!doctype html><title>IndexedDB transaction lifetime</title>'));
  const rendererPid = window.webContents.getProcessId();
  const report = await window.webContents.executeJavaScript(`(${exercise.toString()})()`);
  report.rendererPid = rendererPid;
  report.mainPid = process.pid;
  report.singleProcess = process.argv.includes('--single-process');
  if (process.env.MINI_FIX_RESULT_DIR) {
    const suffix = report.singleProcess ? 'single' : 'sandbox';
    fs.writeFileSync(path.join(process.env.MINI_FIX_RESULT_DIR, `indexeddb-transaction-lifetime-${suffix}-report.json`),
      JSON.stringify(report, null, 2) + '\n');
  }
  console.log(JSON.stringify(report));
  finish();
}).catch(finish);
