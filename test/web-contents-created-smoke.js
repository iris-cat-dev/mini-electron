'use strict';

// Run with the native runtime, not Node: mini-electron test/web-contents-created-smoke.js.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserView, BrowserWindow, webContents } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-web-contents-created-'));
const profile = path.join(temporary, 'profile');
const creationCounts = new Map();
const creationOrder = [];
let activeAttempt = null;
let window = null;
let viewContents = null;
let finished = false;

app.setPath('userData', profile);
app.on('window-all-closed', () => {});
app.on('web-contents-created', (_event, contents) => {
  creationCounts.set(contents.id, (creationCounts.get(contents.id) || 0) + 1);
  creationOrder.push(contents.id);
  if (!activeAttempt) return;

  activeAttempt.contents = contents;
  if (activeAttempt.destroy) contents.destroy();
  if (activeAttempt.error) throw activeAttempt.error;
});

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  try {
    if (viewContents && !viewContents.isDestroyed()) viewContents.destroy();
    if (window && !window.isDestroyed()) window.destroy();
  } catch (cleanupError) {
    if (!error) error = cleanupError;
  }
  try {
    fs.rmSync(temporary, { recursive: true, force: true });
  } catch (cleanupError) {
    if (!error) error = cleanupError;
  }
  if (error) console.error(error.stack || error);
  else console.log('PASS: failed WebContents creation handlers tear down prospective owners and leave later BrowserWindow and BrowserView operation healthy');
  app.exit(error ? 1 : 0);
}

const deadline = setTimeout(
  () => finish(new Error('WebContents creation lifetime smoke timed out')),
  30000,
);

function rejectCreation(label, construct, options = {}) {
  const { listenerError = null, destroy = true } = options;
  const attempt = { label, contents: null, error: listenerError, destroy };
  activeAttempt = attempt;
  let caught = null;
  try {
    construct();
  } catch (error) {
    caught = error;
  } finally {
    activeAttempt = null;
  }

  assert.ok(caught instanceof Error, `${label} construction reports failure`);
  if (listenerError) {
    assert.strictEqual(caught, listenerError,
      `${label} preserves the creation listener exception policy`);
  }
  assert.ok(attempt.contents, `${label} publishes WebContents before construction aborts`);
  assert.equal(attempt.contents.isDestroyed(), true,
    `${label} WebContents is synchronously destroyed`);
  assert.equal(attempt.contents.getOwnerBrowserWindow(), null,
    `${label} WebContents drops its prospective owner`);
  assert.equal(webContents.fromId(attempt.contents.id), null,
    `${label} WebContents is removed from the live registry`);
  assert.ok(!webContents.getAllWebContents().includes(attempt.contents),
    `${label} WebContents is absent from the live contents list`);
  assert.equal(creationCounts.get(attempt.contents.id), 1,
    `${label} publishes exactly one creation event`);
  return attempt.contents;
}

app.whenReady().then(async () => {
  const windowsBefore = BrowserWindow.getAllWindows().slice();

  const failedWindowContents = rejectCreation(
    'BrowserWindow',
    () => new BrowserWindow({ show: false, width: 320, height: 200 }),
  );
  assert.deepEqual(BrowserWindow.getAllWindows(), windowsBefore,
    'aborted BrowserWindow is never registered');

  const listenerError = new Error('intentional creation-listener failure');
  const failedViewContents = rejectCreation(
    'BrowserView',
    () => new BrowserView(),
    { listenerError },
  );
  assert.deepEqual(BrowserWindow.getAllWindows(), windowsBefore,
    'aborted BrowserView creates no owner window');

  const throwOnlyError = new Error('intentional throw-only creation-listener failure');
  const throwOnlyContents = rejectCreation(
    'BrowserWindow throw-only listener',
    () => new BrowserWindow({ show: false, width: 320, height: 200 }),
    { listenerError: throwOnlyError, destroy: false },
  );
  assert.deepEqual(BrowserWindow.getAllWindows(), windowsBefore,
    'throwing creation listener leaves no registered BrowserWindow');
  assert.equal(new Set([
    failedWindowContents.id,
    failedViewContents.id,
    throwOnlyContents.id,
  ]).size, 3, 'each rejected factory published its own real WebContents');

  window = new BrowserWindow({
    show: false,
    width: 420,
    height: 280,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  const view = new BrowserView({
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  view.setBounds({ x: 20, y: 100, width: 180, height: 120 });
  window.setBrowserView(view);

  const windowContents = window.webContents;
  viewContents = view.webContents;
  assert.strictEqual(window.webContents, windowContents,
    're-reading BrowserWindow.webContents preserves the created instance');
  assert.strictEqual(view.webContents, viewContents,
    're-reading BrowserView.webContents preserves the created instance');
  assert.equal(creationCounts.get(windowContents.id), 1,
    'BrowserWindow WebContents creation is emitted exactly once');
  assert.equal(creationCounts.get(viewContents.id), 1,
    'BrowserView WebContents creation is emitted exactly once');
  assert.strictEqual(windowContents.getOwnerBrowserWindow(), window,
    'healthy BrowserWindow WebContents retains its real owner');

  const rootURL = 'data:text/html;charset=utf-8,' + encodeURIComponent(
    '<!doctype html><title>healthy root</title><script>window.nativeResult = 6 * 7</script>',
  );
  const viewURL = 'data:text/html;charset=utf-8,' + encodeURIComponent(
    '<!doctype html><title>healthy view</title><script>window.nativeResult = "view-ready"</script>',
  );
  await Promise.all([
    window.loadURL(rootURL),
    viewContents.loadURL(viewURL),
  ]);
  assert.equal(await windowContents.executeJavaScript('window.nativeResult'), 42,
    'subsequent BrowserWindow executes a real page operation');
  assert.equal(await viewContents.executeJavaScript('window.nativeResult'), 'view-ready',
    'subsequent BrowserView executes a real page operation');

  assert.equal(creationOrder.length, 5,
    'only the three rejected and two healthy WebContents creations are published');
  assert.equal(new Set(creationOrder).size, creationOrder.length,
    'every real WebContents receives one creation publication');
  assert.equal(BrowserWindow.getAllWindows().length, windowsBefore.length + 1,
    'only the healthy BrowserWindow remains registered');
  finish();
}).catch(finish);
