'use strict';

// Run with the native runtime, not Node: mini-electron test/console-message-smoke.js.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const { app, BrowserWindow } = require('electron');

const marker = 'mini-console-message-smoke|';
const methods = [
  ['log', 1],
  ['info', 1],
  ['warn', 2],
  ['error', 3],
  ['debug', 0],
];
const formattedMessages = [
  'values α true 12 null undefined 123n Symbol(s) [object Object]',
  'array 1,,,2,3',
  'circular 1,',
  'proxy [object Proxy]',
  'boxed 2n false 7 hi Symbol(boxed)',
  'format %s %d %f text 12 3.5',
  'throwing-array ',
  'after-throwing-array',
];
const stages = ['window-a-initial', 'window-b-initial', 'window-a-navigation', 'window-b-reload'];
const roles = ['main', 'iframe'];
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-console-message-'));
const mainPage = path.join(temporary, 'main.html');
const framePage = path.join(temporary, 'frame.html');
const mainPageURL = pathToFileURL(mainPage).href;
const framePageURL = pathToFileURL(framePage).href;
const expected = new Map();
const received = new Map();
const failures = [];
const windows = new Set();
const created = new Map();
const owners = new Map();
let finished = false;

app.setPath('userData', path.join(temporary, 'user-data'));
app.on('window-all-closed', () => {});

for (const stage of stages) {
  for (const role of roles) {
    for (const [method, level] of methods) {
      const message = `${marker}${stage}|${role}|${method}`;
      const sourceId = `${role === 'main' ? mainPageURL : framePageURL}?stage=${encodeURIComponent(stage)}`;
      expected.set(message, {
        level,
        sourceId,
        owner: stage.startsWith('window-a-') ? 'window-a' : 'window-b',
      });
      received.set(message, []);
    }
    if (role === 'main') {
      for (const suffix of formattedMessages) {
        const message = `${marker}${stage}|main|${suffix}`;
        expected.set(message, {
          level: 1,
          sourceId: `${mainPageURL}?stage=${encodeURIComponent(stage)}`,
          owner: stage.startsWith('window-a-') ? 'window-a' : 'window-b',
        });
        received.set(message, []);
      }
    }
  }
}

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  for (const window of windows) {
    if (!window.isDestroyed()) window.destroy();
  }
  try {
    fs.rmSync(temporary, { recursive: true, force: true });
  } catch (cleanupError) {
    if (!error) error = cleanupError;
  }
  if (error) console.error(error.stack || error);
  else console.log('PASS: renderer console methods reach console-message exactly once across frames, navigation, reload, and concurrent windows');
  app.exit(error ? 1 : 0);
}

const deadline = setTimeout(() => finish(new Error('Console-message smoke timed out')), 30000);

function pageURL(stage) {
  return `${mainPageURL}?stage=${encodeURIComponent(stage)}`;
}

function recordConsoleMessage(owner, _event, level, message, lineNumber, sourceId) {
  if (typeof message !== 'string' || !message.startsWith(marker)) return;

  const observations = received.get(message);
  if (!observations) {
    failures.push(new Error(`Unexpected smoke console message: ${JSON.stringify(message)}`));
    return;
  }

  observations.push({ level, lineNumber, sourceId });
  const wanted = expected.get(message);
  try {
    assert.equal(level, wanted.level, `${message} has the standard console level`);
    assert.equal(owner, wanted.owner, `${message} is emitted by the expected webContents`);
    assert.equal(sourceId, wanted.sourceId, `${message} has its emitting document URL`);
    assert.ok(Number.isInteger(lineNumber) && lineNumber > 0, `${message} has a positive integer line number`);
  } catch (error) {
    failures.push(error);
  }
}

app.on('web-contents-created', (_event, contents) => {
  created.set(contents.id, (created.get(contents.id) || 0) + 1);
  contents.on('console-message', (...args) =>
    recordConsoleMessage(owners.get(contents.id), ...args));
});

function reload(window) {
  return new Promise((resolve, reject) => {
    const contents = window.webContents;
    const cleanup = () => {
      contents.removeListener('did-finish-load', loaded);
      contents.removeListener('did-fail-load', failed);
    };
    const loaded = () => {
      cleanup();
      resolve();
    };
    const failed = (_event, code, description, validatedURL, isMainFrame) => {
      if (!isMainFrame) return;
      cleanup();
      reject(new Error(`Reload failed (${code} ${description}): ${validatedURL}`));
    };
    contents.once('did-finish-load', loaded);
    contents.on('did-fail-load', failed);
    contents.reload();
  });
}

function waitForExpectedMessages(requiredStages = stages) {
  const required = [...received.keys()].filter(message =>
    requiredStages.some(stage => message.startsWith(`${marker}${stage}|`)));
  return new Promise((resolve, reject) => {
    const end = Date.now() + 5000;
    const inspect = () => {
      if (failures.length > 0) {
        reject(failures[0]);
        return;
      }
      if (required.every(message => received.get(message).length > 0)) {
        // Keep listening briefly so duplicate forwarding cannot pass by racing
        // the last expected event.
        setTimeout(resolve, 500);
        return;
      }
      if (Date.now() >= end) {
        const missing = required.filter(message => received.get(message).length === 0);
        reject(new Error(`Missing console-message events: ${missing.join(', ')}`));
        return;
      }
      setTimeout(inspect, 20);
    };
    inspect();
  });
}

app.whenReady().then(async () => {
  fs.writeFileSync(mainPage, `<!doctype html>
<meta charset="utf-8">
<script>
const stage = new URLSearchParams(location.search).get('stage');
const prefix = '${marker}' + stage + '|main|';
console.log(prefix + 'log');
console.info(prefix + 'info');
console.warn(prefix + 'warn');
console.error(prefix + 'error');
console.debug(prefix + 'debug');
console.log(prefix + 'values', 'α', true, 12, null, undefined, 123n, Symbol('s'), {});
console.log(prefix + 'array', [1, null, undefined, [2, 3]]);
const circular = [1]; circular.push(circular);
console.log(prefix + 'circular', circular);
console.log(prefix + 'proxy', new Proxy({}, {
  get() { throw new Error('Console must not read proxy properties'); },
}));
console.log(prefix + 'boxed', Object(2n), Object(false), Object(7), Object('hi'), Object(Symbol('boxed')));
console.log(prefix + 'format %s %d %f', 'text', 12.8, 3.5);
const throwingArray = [];
Object.defineProperty(throwingArray, '0', {
  get() { throw new Error('Console argument conversion failed'); },
});
console.log(prefix + 'throwing-array', throwingArray);
console.log(prefix + 'after-throwing-array');
</script>
<iframe id="child"></iframe>
<script>
document.getElementById('child').src = 'frame.html?stage=' + encodeURIComponent(stage);
</script>
`);
  fs.writeFileSync(framePage, `<!doctype html>
<meta charset="utf-8">
<script>
const stage = new URLSearchParams(location.search).get('stage');
const prefix = '${marker}' + stage + '|iframe|';
console.log(prefix + 'log');
console.info(prefix + 'info');
console.warn(prefix + 'warn');
console.error(prefix + 'error');
console.debug(prefix + 'debug');
</script>
`);

  const first = new BrowserWindow({
    show: false,
    width: 320,
    height: 200,
    webPreferences: { contextIsolation: true, nodeIntegration: false, sandbox: true },
  });
  const second = new BrowserWindow({
    show: false,
    width: 320,
    height: 200,
    webPreferences: { contextIsolation: true, nodeIntegration: false, sandbox: true },
  });
  windows.add(first);
  windows.add(second);
  owners.set(first.webContents.id, 'window-a');
  owners.set(second.webContents.id, 'window-b');
  assert.equal(created.get(first.webContents.id), 1, 'first WebContents creation is published once');
  assert.equal(created.get(second.webContents.id), 1, 'second WebContents creation is published once');
  // Exercise resize while both displays are hidden: logging and document load
  // must not depend on a BeginFrame from an invisible root display.
  first.setSize(480, 320);
  second.setSize(520, 360);

  await Promise.all([
    first.loadURL(pageURL('window-a-initial')),
    second.loadURL(pageURL('window-b-initial')),
  ]);
  // A dynamically inserted iframe can still be loading when the main frame's
  // did-finish-load arrives. Do not cancel that document before its logs run.
  await waitForExpectedMessages(stages.slice(0, 2));

  await second.webContents.executeJavaScript(
    `history.replaceState(null, '', '?stage=${encodeURIComponent('window-b-reload')}')`,
    true,
  );
  await Promise.all([
    first.loadURL(pageURL('window-a-navigation')),
    reload(second),
  ]);

  await waitForExpectedMessages();
  if (failures.length > 0) throw failures[0];
  for (const [message, observations] of received) {
    assert.equal(observations.length, 1, `${message} is delivered exactly once`);
  }
  finish();
}).catch(finish);
