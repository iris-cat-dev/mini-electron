'use strict';
// Prebuild macos-owned-dialog-helper.mm against this repository's node headers.
// node test/macos-owned-dialog-smoke.js --runtime=<owned candidate Electron> --helper=<owned helper.node>
// Manual acceptance (launch only after an explicit user/GUI handoff): --case=manual-accept
const assert = require('node:assert/strict');
const { randomUUID } = require('node:crypto');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const ROOT = '/tmp/omp-unified-plugin-smoke-2s3uFs';
const inside = (root, item) => {
  const relative = path.relative(root, fs.realpathSync(item));
  return relative === '' || (!relative.startsWith(`..${path.sep}`) && relative !== '..' && !path.isAbsolute(relative));
};
const argument = name => process.argv.find(value => value.startsWith(`--${name}=`))?.slice(name.length + 3);
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));

function launch() {
  assert.equal(process.platform, 'darwin');
  const root = fs.realpathSync(ROOT);
  const runtime = fs.realpathSync(argument('runtime'));
  const helper = fs.realpathSync(argument('helper'));
  assert.ok(inside(root, runtime) && inside(root, helper), 'Runtime and prebuilt helper must be owned');
  const modes = argument('case') ? [argument('case')] : ['matrix', 'exit-pending', 'quit-pending'];
  let failed = false;
  for (const mode of modes) {
    assert.ok(['matrix', 'matrix-remaining', 'manual-accept', 'exit-pending', 'quit-pending'].includes(mode));
    const run = fs.mkdtempSync(path.join(root, 'native-owned-dialog-'));
    const name = `omp-native-owned-dialog-${randomUUID()}`;
    const paths = Object.fromEntries(['userData', 'appData', 'sessionData', 'temp', 'logs', 'crashDumps', 'cache']
      .map(key => [key, path.join(run, key)]));
    for (const directory of Object.values(paths)) fs.mkdirSync(directory);
    fs.mkdirSync(path.join(root, 'Library', 'Application Support', name), { recursive: true });
    fs.copyFileSync(__filename, path.join(run, 'main.cjs'));
    fs.writeFileSync(path.join(run, 'package.json'), JSON.stringify({ name, productName: name, version: '1.0.0', main: 'main.cjs' }));
    fs.writeFileSync(path.join(run, 'ownership.json'), JSON.stringify({ kind: 'mini-owned-dialog', root, run, name }));
    const env = {
      PATH: process.env.PATH, LANG: process.env.LANG || 'en_US.UTF-8',
      HOME: root, CFFIXED_USER_HOME: root, TMPDIR: `${root}${path.sep}`,
      XDG_CONFIG_HOME: paths.appData, XDG_CACHE_HOME: paths.cache,
      MINI_OWNED_DIALOG_CONFIG: JSON.stringify({ root, run, name, paths, helper, mode, runtime })
    };
    const stdout = path.join(run, 'native.stdout.log');
    const stderr = path.join(run, 'native.stderr.log');
    const args = ['-W', '-n', '-a', path.resolve(runtime, '../../..'), '--stdout', stdout, '--stderr', stderr,
      ...Object.entries(env).flatMap(([key, value]) => ['--env', `${key}=${value}`]), '--args', `--app=${run}`];
    console.log(JSON.stringify({ kind: 'dialog-launch', mode, run, runtime, helper }));
    const result = spawnSync('/usr/bin/open', args, { cwd: run, env, stdio: 'inherit', timeout: mode === 'manual-accept' ? 180000 : 90000 });
    for (const file of [stdout, stderr]) if (fs.existsSync(file)) process.stdout.write(fs.readFileSync(file, 'utf8'));
    const evidenceFile = path.join(run, 'observations.json');
    const rows = fs.existsSync(evidenceFile) ? JSON.parse(fs.readFileSync(evidenceFile, 'utf8')) : [];
    const passed = !result.error && result.status === 0 && rows.at(-1)?.label === 'complete' && rows.at(-1)?.kind === 'pass';
    console.log(JSON.stringify({ kind: 'dialog-launch-result', mode, run, passed, status: result.status, error: result.error?.message || null }));
    failed ||= !passed;
  }
  process.exitCode = failed ? 1 : 0;
}

async function nativeMain() {
  const { app, BrowserWindow, dialog } = require('electron');
  assert.ok(process.versions.miniElectron, 'Real mini-electron is required');
  const config = JSON.parse(process.env.MINI_OWNED_DIALOG_CONFIG || 'null');
  assert.ok(config, 'Use the owned launcher');
  const { root, run, name, paths, helper, mode, runtime } = config;
  assert.equal(fs.realpathSync(ROOT), root);
  assert.ok(inside(root, run) && inside(root, helper) && inside(root, runtime));
  assert.equal(app.getName(), name);
  // Canonical early guard precedes sessions, settings and every BrowserWindow.
  const early = app.getPath('userData');
  assert.ok(fs.existsSync(early) && inside(root, early), `Native early userData escaped: ${early}`);
  assert.equal(fs.realpathSync(early), fs.realpathSync(path.join(root, 'Library', 'Application Support', name)));
  const marker = JSON.parse(fs.readFileSync(path.join(run, 'ownership.json'), 'utf8'));
  assert.equal(marker.kind, 'mini-owned-dialog');
  assert.equal(marker.name, name);
  for (const variable of ['HOME', 'CFFIXED_USER_HOME', 'TMPDIR'])
    assert.equal(fs.realpathSync(process.env[variable]), root);
  for (const [key, directory] of Object.entries(paths)) {
    assert.ok(inside(run, directory));
    app.setPath(key, fs.realpathSync(directory));
  }
  app.setPath('home', root);
  app.on('window-all-closed', () => {});
  const native = require(helper);
  assert.equal(native.watchdogProvenance, true, 'Build the current helper; watchdog cleanup must never masquerade as Escape delivery');
  const rows = [];
  const failures = [];
  const record = (kind, label, observed) => {
    rows.push({ kind, label, observed });
    fs.writeFileSync(path.join(run, 'observations.json'), JSON.stringify(rows, null, 2));
    console.log(JSON.stringify(rows.at(-1)));
  };
  const check = (label, observed, passes) => {
    record(passes ? 'pass' : 'fail', label, observed);
    if (!passes) failures.push(label);
  };
  const deadline = setTimeout(() => {
    record('fail', 'deadline', { mode });
    native.stop();
    app.exit(1);
  }, mode === 'manual-accept' ? 150000 : 60000);
  let other;
  const rawCounts = {};
  // Observe exact normalized arguments and original Native callbacks; no fake
  // bridge, callback paths, Promise replacement or URL-based owner inference.
  for (const method of ['_showOpenDialog', '_showSaveDialog']) {
    const original = dialog[method];
    dialog[method] = function(ownerId, options, callback) {
      rawCounts[options.title] = 0;
      record('observation', 'native-entry', { method, ownerId, title: options.title, options });
      return original.call(this, ownerId, options, (...args) => {
        ++rawCounts[options.title];
        return callback(...args);
      });
    };
  }
  const snapshot = () => JSON.parse(native.snapshot());
  const makeOwner = label => new BrowserWindow({ title: `${name}-${label}`, x: 220, y: 180,
    width: 640, height: 440, show: true, webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  const fixture = path.join(run, 'fixtures');
  fs.mkdirSync(fixture);
  fs.writeFileSync(path.join(fixture, 'chosen.txt'), 'Native owned dialog fixture\n');
  await app.whenReady();
  other = makeOwner('survivor');

  if (mode === 'manual-accept') {
    // This consumer observes only. No watchdog, key posting, AppKit action or
    // synthetic result can accept either panel; the user must click the real UI.
    for (const save of [false, true]) {
      const label = save ? 'manual-save-accept' : 'manual-open-accept';
      const title = `${name}-${label}`;
      const expectedPath = path.join(fixture, save ? 'manual-saved.txt' : 'chosen.txt');
      const instructions = save
        ? `在原生保存面板中保留 manual-saved.txt 文件名和默认目录，然后点击“保存”。目标：${expectedPath}`
        : `在原生打开面板的默认目录中选择 chosen.txt，然后点击“打开”。目标：${expectedPath}`;
      await other.webContents.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(
        `<!doctype html><meta charset="utf-8"><title>Native dialog manual acceptance</title><style>body{font:18px -apple-system;padding:32px;line-height:1.6;overflow-wrap:anywhere}</style><h1>原生文件面板人工验收</h1><p>${instructions}</p><p>请只操作本次自有文件面板。取消会记录失败；完成后将自动打开下一项。</p>`));
      native.observe(other.getTitle(), title);
      record('observation', `${label}: instructions`, { instructions, expectedPath, ownerId: other.id });
      let visibleTicks = 0, settled = false;
      const interval = setInterval(() => { if (snapshot().visible) ++visibleTicks; }, 25);
      try {
        const options = { title, message: instructions, defaultPath: save ? expectedPath : fixture,
          filters: [{ name: 'Text', extensions: ['txt'] }], properties: ['openFile'] };
        const pending = save ? dialog.showSaveDialog(other, options) : dialog.showOpenDialog(other, options);
        const callbacksAtReturn = rawCounts[title];
        pending.then(() => { settled = true; });
        while (!settled && visibleTicks < 2) await pause(25);
        const state = snapshot();
        check(`${label}: real pending owned sheet`, { callbacksAtReturn, visibleTicks, settled, state },
          callbacksAtReturn === 0 && visibleTicks >= 2 && !settled && state.visible &&
          state.sheetParentNumber === state.ownerNumber && state.ownerNumber !== 0 &&
          state.ownerAttachedSheetNumber === state.panelNumber);
        const result = await pending;
        await pause(100);
        const after = snapshot();
        check(`${label}: actual AppKit result`, { result, expectedPath }, !result.canceled &&
          (save ? result.filePath === expectedPath : result.filePaths.length === 1 && result.filePaths[0] === expectedPath));
        check(`${label}: one callback and panel dismissal`, { callbackCount: rawCounts[title], after },
          rawCounts[title] === 1 && !after.visible && after.ownerAttachedSheetNumber === 0);
      } catch (error) {
        check(`${label}: exception`, { message: error.message, stack: error.stack, state: snapshot() }, false);
      } finally {
        clearInterval(interval);
      }
    }
    native.stop();
    other.destroy();
    check('all owned windows closed', { windows: BrowserWindow.getAllWindows().length }, BrowserWindow.getAllWindows().length === 0);
    clearTimeout(deadline);
    record(failures.length ? 'fail' : 'pass', 'complete', { mode, failures, runtime });
    app.exit(failures.length ? 1 : 0);
    return;
  }

  if (mode === 'exit-pending' || mode === 'quit-pending') {
    const title = `${name}-${mode}`;
    native.arm('', title);
    let callbacks = 0;
    const started = Date.now();
    const pending = dialog.showOpenDialog({ title, defaultPath: fixture, properties: ['openFile'] });
    pending.then(() => ++callbacks);
    const returnedMs = Date.now() - started;
    const callbacksAtReturn = rawCounts[title];
    await pause(250);
    const state = snapshot();
    check('pending panel before shutdown', { returnedMs, callbacksAtReturn, callbacks, state },
      callbacksAtReturn === 0 && callbacks === 0 && state.visible);
    // Exit observation is written after the native host destroyed its windows.
    process.on('exit', () => {
      const remaining = BrowserWindow.getAllWindows().length;
      check('shutdown window cleanup', { mode, remaining, callbacks }, remaining === 0 && callbacks === 0);
      record(failures.length ? 'fail' : 'pass', 'complete', { mode, failures, runtime });
    });
    clearTimeout(deadline);
    if (mode === 'exit-pending') app.exit(failures.length ? 1 : 0);
    else app.quit();
    return;
  }

  async function panelCase(label, { save = false, parent = true, action = 'cancel', properties = ['openFile', 'multiSelections'] } = {}) {
    const owner = parent ? makeOwner(label) : null;
    const title = `${name}-${label}`;
    const options = { title, message: 'Owned native dialog regression', buttonLabel: save ? 'Keep file' : 'Choose file',
      defaultPath: save ? path.join(fixture, 'saved.txt') : fixture,
      filters: [{ name: 'Text', extensions: ['txt'] }], properties };
    native.arm(owner?.getTitle() || '', title);
    let ticks = 0, settled = false;
    const interval = setInterval(() => { if (snapshot().visible) ++ticks; }, 25);
    const started = Date.now();
    try {
      const invoke = save ? dialog.showSaveDialog.bind(dialog) : dialog.showOpenDialog.bind(dialog);
      const pending = parent ? invoke(owner, options) : invoke(options);
      const returnedMs = Date.now() - started;
      const callbacksAtReturn = rawCounts[title];
      pending.then(() => { settled = true; });
      // Presentation initialization can consume the first timer deadline. Wait
      // for two independent Node callbacks that actually saw the visible panel.
      // The native watchdog bounds old blocking or broken presentations.
      while (!settled && ticks < 2) await pause(25);
      const state = snapshot();
      check(`${label}: Node timer while visible`, { returnedMs, callbacksAtReturn, ticks, settled, state },
        callbacksAtReturn === 0 && ticks >= 2 && !settled && state.visible);
      check(`${label}: exact native owner`, state, parent
        ? state.sheetParentNumber === state.ownerNumber && state.ownerNumber !== 0 && state.ownerAttachedSheetNumber === state.panelNumber
        : state.sheetParentNumber === 0);
      check(`${label}: options`, state, state.visible && state.prompt === options.buttonLabel && state.message === options.message &&
        state.allowedFileTypes.includes('txt') && state.allowsOtherFileTypes === false &&
        (save || (state.canChooseFiles === properties.includes('openFile') &&
          state.canChooseDirectories === properties.includes('openDirectory') && state.allowsMultipleSelection === properties.includes('multiSelections'))));
      if (owner && !owner.isDestroyed()) {
        const bounds = owner.getBounds();
        owner.setBounds({ ...bounds, x: bounds.x + 15 });
        check(`${label}: owner operation while sheet visible`, { bounds: owner.getBounds(), state: snapshot() }, owner.getBounds().x === bounds.x + 15 && snapshot().visible);
      }
      if (state.visible) {
        if (action === 'close') owner.close();
        else if (action === 'destroy') owner.destroy();
        else record('observation', `${label}: AppKit action`, JSON.parse(native.act(action)));
      }
      const result = await pending;
      await pause(100);
      const after = snapshot();
      const history = JSON.parse(native.stop());
      check(`${label}: original Promise result`, result, action === 'accept'
        ? !result.canceled && (save ? result.filePath === path.join(fixture, 'saved.txt')
          : result.filePaths.length === 1 && result.filePaths[0] === fixture)
        : result.canceled && (save ? result.filePath === undefined : result.filePaths.length === 0));
      check(`${label}: one callback and native dismissal`, { callbackCount: rawCounts[title], after, history },
        rawCounts[title] === 1 && !after.visible && after.ownerAttachedSheetNumber === 0);
      if (action === 'cancel') {
        const watchdogCancelled = history.some(item => item.watchdogCancellation === true);
        check(`${label}: Escape cancellation without watchdog`, { watchdogCancelled, callbackCount: rawCounts[title], after },
          result.canceled && rawCounts[title] === 1 && !after.visible && !watchdogCancelled);
      }
    } catch (error) {
      check(`${label}: exception`, { message: error.message, stack: error.stack, history: JSON.parse(native.stop()) }, false);
    } finally {
      clearInterval(interval);
      if (snapshot().visible) native.act('cancel');
      if (owner && !owner.isDestroyed()) owner.destroy();
    }
  }

  if (mode === 'matrix') {
    await panelCase('open-owner-cancel');
    await panelCase('save-owner-cancel', { save: true });
    await panelCase('open-owner-close', { action: 'close' });
    await panelCase('save-owner-destroy', { save: true, action: 'destroy' });
    await panelCase('open-no-parent', { parent: false, properties: ['openDirectory'] });
    await panelCase('save-no-parent', { parent: false, save: true });
  }
  await panelCase('save-owner-accept', { save: true, action: 'accept' });
  await panelCase('open-owner-accept-directory', { action: 'accept', properties: ['openDirectory'] });
  for (const action of ['close', 'destroy']) {
    const owner = makeOwner(`queued-${action}`);
    const title = `${name}-queued-${action}`;
    native.arm(owner.getTitle(), title);
    const pending = dialog.showOpenDialog(owner, { title, defaultPath: fixture });
    owner[action]();
    const result = await pending;
    await pause(100);
    check(`owner ${action} before queued presentation`, { result, state: snapshot(), callbackCount: rawCounts[title] },
      owner.isDestroyed() && result.canceled && result.filePaths.length === 0 &&
      rawCounts[title] === 1 && !snapshot().visible);
    native.stop();
  }
  for (const save of [false, true]) {
    const title = `${name}-${save ? 'save' : 'open'}-sync`;
    native.arm('', title);
    let timerRan = false;
    const timer = setTimeout(() => { timerRan = true; }, 50);
    const started = Date.now();
    const result = save ? dialog.showSaveDialogSync({ title, defaultPath: fixture })
      : dialog.showOpenDialogSync({ title, defaultPath: fixture });
    const elapsedMs = Date.now() - started;
    clearTimeout(timer);
    const history = JSON.parse(native.stop());
    check(`${save ? 'save' : 'open'} sync retains blocking cancellation`, { elapsedMs, timerRan, result, history },
      !timerRan && (save ? result === null : Array.isArray(result) && result.length === 0) &&
      history.some(state => state.visible && state.modalWindowNumber === state.panelNumber) && !snapshot().visible);
  }
  const dead = makeOwner('closed');
  dead.destroy();
  const deadTitle = `${name}-dead-owner`;
  native.arm('', deadTitle);
  let rejected = false;
  try { await dialog.showOpenDialog(dead, { title: deadTitle }); }
  catch (error) { rejected = /owner.*destroyed|owner.*unknown/.test(error.message); }
  check('closed owner rejects without unparented panel', { rejected, state: snapshot() }, rejected && !snapshot().visible);
  native.stop();
  if (other && !other.isDestroyed()) other.destroy();
  check('all owned windows closed', { windows: BrowserWindow.getAllWindows().length }, BrowserWindow.getAllWindows().length === 0);
  clearTimeout(deadline);
  record(failures.length ? 'fail' : 'pass', 'complete', { mode, failures, runtime });
  app.exit(failures.length ? 1 : 0);
}

if (process.versions.miniElectron) nativeMain().catch(error => {
  console.error(error.stack || error);
  require('electron').app.exit(1);
});
else launch();
