'use strict';
// Driver: node test/windows-owned-dialog-smoke.js --runtime <mini-electron.exe>
// Interactive Windows desktop required. Optional: --case <name>, --output-dir <directory>.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

function launch() {
  assert.ok(!process.versions.miniElectron && !process.versions.electron, 'Use the isolated Node launcher');
  const option = name => {
    const index = process.argv.indexOf(name);
    return index < 0 ? undefined : process.argv[index + 1];
  };
  const runtime = option('--runtime');
  assert.ok(runtime && fs.statSync(runtime).isFile(), '--runtime must name the real native executable');
  const output = path.resolve(option('--output-dir') || os.tmpdir());
  fs.mkdirSync(output, { recursive: true });
  const modes = option('--case') || process.env.MINI_OWNED_DIALOG_CASE;
  for (const mode of modes ? [modes] : ['matrix', 'exit-pending', 'quit-pending', 'exit-queued', 'quit-queued']) {
    const root = fs.mkdtempSync(path.join(output, 'mini-owned-dialog-'));
    const application = path.join(root, 'application');
    const name = `mini-owned-dialog-${path.basename(root)}-${process.pid}`;
    const paths = Object.fromEntries(['appData', 'userData', 'sessionData', 'temp', 'logs', 'crashDumps', 'cache']
      .map(key => [key, path.join(root, key)]));
    paths.sessionData = paths.userData;
    for (const directory of new Set(Object.values(paths))) fs.mkdirSync(directory);
    fs.mkdirSync(application);
    fs.writeFileSync(path.join(paths.userData, '.mini-electron-profile-migrated'), 'isolated-native-smoke\n');
    fs.writeFileSync(path.join(root, 'isolated-smoke.json'), JSON.stringify({ name, paths }));
    fs.writeFileSync(path.join(application, 'package.json'), JSON.stringify({
      name, version: '1.0.0', main: path.resolve(__filename),
    }));
    const environment = { ...process.env, NODE_OPTIONS: '', MINI_OWNED_DIALOG_CHILD: '1',
      MINI_OWNED_DIALOG_ROOT: root, MINI_OWNED_DIALOG_CASE: mode,
      TEMP: paths.temp, TMP: paths.temp, TMPDIR: paths.temp + path.sep };
    // Keep Windows known-folder variables valid until main redirects app paths.
    // Presence, even when empty, selects the native host's Node-only mode.
    delete environment.ELECTRON_RUN_AS_NODE;
    const result = spawnSync(path.resolve(runtime), [`--app=${application}`], {
      env: environment, windowsHide: true, encoding: 'utf8', timeout: 110000, maxBuffer: 16 * 1024 * 1024,
    });
    fs.writeFileSync(path.join(root, 'native.stdout.log'), result.stdout || '');
    fs.writeFileSync(path.join(root, 'native.stderr.log'), result.stderr || '');
    const evidence = { mode, root, runtime: path.resolve(runtime),
      status: result.status, signal: result.signal, error: result.error?.message };
    fs.writeFileSync(path.join(root, 'result.json'), JSON.stringify(evidence, null, 2));
    if (result.stdout) process.stdout.write(result.stdout);
    if (result.stderr) process.stderr.write(result.stderr);
    console.log(`Owned dialog fixture: ${root}`);
    assert.ifError(result.error); assert.equal(result.signal, null); assert.equal(result.status, 0, JSON.stringify(evidence));
    assert.match(result.stdout, /PASS: Windows owned/);
  }
}
if (process.env.MINI_OWNED_DIALOG_CHILD !== '1') {
  try { launch(); } catch (error) { console.error(error.stack || error); process.exit(1); }
  process.exit(0);
}
const { app, BrowserWindow, dialog } = require('electron');

const { startNativeInput } = require('./windows-native-input-driver');
const temporary = process.env.MINI_OWNED_DIALOG_ROOT;
assert.ok(temporary && path.isAbsolute(temporary), 'Use the isolated launcher');
const isolated = JSON.parse(fs.readFileSync(path.join(temporary, 'isolated-smoke.json'), 'utf8'));
assert.equal(app.getName(), isolated.name, 'Unique application namespace must precede native startup');
const early = app.getPath('userData');
assert.equal(path.basename(early), isolated.name);
for (const [name, directory] of Object.entries(isolated.paths)) {
  assert.ok(directory.startsWith(temporary + path.sep)); app.setPath(name, directory);
}
if (!early.startsWith(temporary + path.sep)) {
  try { fs.rmdirSync(early); } catch (error) {
    if (error.code !== 'ENOENT' && error.code !== 'ENOTEMPTY') throw error;
  }
}
const callbacks = new Map();
for (const method of ['_showOpenDialog', '_showSaveDialog']) {
  const original = dialog[method];
  dialog[method] = function(ownerId, options, callback) {
    callbacks.set(options.title, 0);
    return original.call(this, ownerId, options, (...args) => {
      callbacks.set(options.title, callbacks.get(options.title) + 1);
      return callback(...args);
    });
  };
}
app.on('window-all-closed', () => {});
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let driver, sequence = 0, finished = false;
const windows = new Set();
const mode = process.env.MINI_OWNED_DIALOG_CASE;
const selected = mode === 'matrix' ? undefined : mode;
const deadline = setTimeout(() => finish(new Error('Owned file dialog smoke timed out')), 90000);
const title = label => `mini-owned-dialog-${process.pid}-${++sequence}-${label}`;
async function eventually(read, accept, label) {
  const until = Date.now() + 7000;
  let value;
  do {
    value = await read();
    if (accept(value)) return value;
    await pause(25);
  } while (Date.now() < until);
  assert.fail(`${label}: ${JSON.stringify(value)}`);
}
const native = (action, values = {}) => driver.request(action, { pid: process.pid, ...values });
const dialogs = () => native('dialogs');
const visible = name => eventually(dialogs, items => items.some(item => item.title === name), `native dialog ${name}`)
  .then(items => items.find(item => item.title === name));
function owner(label) {
  const window = new BrowserWindow({ title: title(label), width: 420, height: 280, show: true });
  windows.add(window); window.once('closed', () => windows.delete(window));
  return window;
}
async function canceled(promise, label) {
  const result = await Promise.race([promise, pause(7000).then(() => { throw new Error(`${label} did not settle`); })]);
  assert.equal(result.canceled, true, label);
  if ('filePaths' in result) assert.deepEqual(result.filePaths, []);
  else assert.equal(result.filePath, undefined);
}
async function absent(name) {
  await eventually(dialogs, items => !items.some(item => item.title === name), `dismissed ${name}`);
  assert.equal(callbacks.get(name), 1, 'native completion must be delivered exactly once');
}
async function finish(error) {
  if (finished) return;
  finished = true; clearTimeout(deadline);
  if (driver) {
    try { for (const item of await dialogs()) await native('cancel', item); } catch (_) {}
  }
  for (const window of windows) if (!window.isDestroyed()) window.destroy();
  if (driver) { try { await driver.close(); } catch (cleanup) { error = error || cleanup; } }
  if (error) console.error(error.stack || error);
  else console.log('PASS: Windows owned async open/save lifecycle, exact HWND, Node progress and native acceptance/cancel');
  app.exit(error ? 1 : 0);
}
async function run(name, body) {
  if (selected && selected !== name) return;
  await body(); console.log('PASS: ' + name);
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  driver = await startNativeInput(temporary, '', 'windows-owned-dialog-helper.cs');
  if (['exit-pending', 'quit-pending', 'exit-queued', 'quit-queued'].includes(mode)) {
    const window = owner('shutdown-owner');
    const names = [title('shutdown-owned'), title('shutdown-ownerless')];
    dialog.showOpenDialog(window, { title: names[0] });
    dialog.showSaveDialog({ title: names[1] });
    if (mode.endsWith('-pending')) {
      await visible(names[0]); await visible(names[1]);
      await driver.close(); driver = null;
    }
    // Also cover dialogs opened by the last JavaScript exit listener, after
    // the main native message loop has already stopped.
    process.once('exit', () => dialog.showOpenDialog({ title: title('exit-listener') }));
    clearTimeout(deadline);
    finished = true;
    console.log(`PASS: Windows owned ${mode} shutdown requested`);
    if (mode.startsWith('quit-')) app.quit(); else app.exit(0);
    return;
  }
  await run('binding', async () => {
    const first = owner('actual-owner'), decoy = owner('focused-decoy'); decoy.focus();
    for (const save of [false, true]) {
      const name = title(save ? 'save-binding' : 'open-binding');
      const promise = save ? dialog.showSaveDialog(first, { title: name }) : dialog.showOpenDialog(first, { title: name });
      const item = await visible(name);
      const handle = first.getNativeWindowHandle();
      const hwnd = (handle.length === 8 ? handle.readBigUInt64LE() : BigInt(handle.readUInt32LE())).toString();
      assert.equal(item.owner, hwnd, 'dialog must bind the explicit HWND, not the focused window');
      let ticks = 0; const timer = setInterval(() => ticks++, 10);
      await pause(150); clearInterval(timer);
      assert.ok(ticks >= 3, 'Node timers must progress during native modal presentation');
      await native('cancel', item); await canceled(promise, 'native cancel'); await absent(name);
    }
    first.destroy(); decoy.destroy();
  });
  await run('destroyed-owner', async () => {
    const window = owner('destroyed'); window.destroy();
    for (const save of [false, true]) {
      const name = title('invalid-owner');
      const promise = save ? dialog.showSaveDialog(window, { title: name }) : dialog.showOpenDialog(window, { title: name });
      await Promise.race([
        assert.rejects(promise, /owner.*(destroyed|unknown|invalid)/i),
        pause(2000).then(() => { throw new Error('Destroyed explicit owner did not reject promptly'); }),
      ]);
      assert.equal((await dialogs()).some(item => item.title === name), false);
    }
  });
  for (const action of ['close', 'destroy']) {
    await run(action, async () => {
      for (const save of [false, true]) {
        const window = owner(action), name = title(action);
        const promise = save ? dialog.showSaveDialog(window, { title: name }) : dialog.showOpenDialog(window, { title: name });
        await visible(name); window[action]();
        await canceled(promise, `owner ${action}`); await absent(name);
        await eventually(() => window.isDestroyed(), Boolean, `owner ${action} destruction`);
      }
    });
    await run('queued-' + action, async () => {
      const window = owner('queued-' + action), names = [title('queued-open'), title('queued-save')];
      const promises = [dialog.showOpenDialog(window, { title: names[0] }), dialog.showSaveDialog(window, { title: names[1] })];
      window[action]();
      await Promise.all(promises.map((promise, index) => canceled(promise, names[index])));
      await eventually(() => window.isDestroyed(), Boolean, `queued owner ${action}`);
      await pause(250);
      for (const name of names) {
        assert.equal((await dialogs()).some(item => item.title === name), false, 'canceled queued dialog must never appear later');
        assert.equal(callbacks.get(name), 1, 'queued owner cancellation must complete exactly once');
      }
    });
  }
  await run('reentrant', async () => {
    const window = owner('callback-reentry'), name = title('callback-reentry');
    let callbacks = 0;
    const settled = new Promise(resolve => {
      dialog._showOpenDialog(window.id, { title: name }, (wasCanceled, paths, error) => {
        callbacks++; assert.equal(wasCanceled, true); assert.deepEqual(paths, []); assert.equal(error, undefined);
        window.destroy(); resolve();
      });
    });
    await native('cancel', await visible(name)); await settled; await pause(150);
    assert.equal(callbacks, 1, 'callback must run exactly once even when it destroys its owner');
  });
  await run('no-owner', async () => {
    const name = title('ownerless'), promise = dialog.showOpenDialog({ title: name });
    const item = await visible(name);
    await native('cancel', item); await canceled(promise, 'ordinary ownerless cancellation');
  });
  for (const save of [false, true]) await run(save ? 'accept-save' : 'accept-open', async () => {
    const window = owner('accept'), name = title('accept');
    const file = path.join(temporary, save ? 'saved café.txt' : 'existing café.txt');
    if (!save) fs.writeFileSync(file, 'native open fixture');
    const options = { title: name, defaultPath: file, filters: [{ name: 'Text', extensions: ['txt'] }] };
    const promise = save ? dialog.showSaveDialog(window, options) : dialog.showOpenDialog(window, options);
    await native('accept', { ...await visible(name), path: file });
    const result = await Promise.race([promise, pause(7000).then(() => { throw new Error('Native acceptance did not settle'); })]);
    assert.equal(result.canceled, false);
    assert.equal(path.resolve(save ? result.filePath : result.filePaths[0]), path.resolve(file));
    await absent(name); window.destroy();
  });
  if (selected) assert.ok(['binding', 'destroyed-owner', 'close', 'destroy', 'queued-close', 'queued-destroy', 'reentrant', 'no-owner', 'accept-save', 'accept-open'].includes(selected), 'unknown MINI_OWNED_DIALOG_CASE');
}).then(() => finish(), finish);
