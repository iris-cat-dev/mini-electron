'use strict';

// Run with the native runtime, not Node: mini-electron test/same-document-navigation-smoke.js.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const http = require('node:http');
const { app, BrowserWindow, ipcMain, protocol } = require('electron');

const scheme = 'mini-same-document-smoke';
const preloadChannel = 'same-document-smoke:preload';
const inspectChannel = 'same-document-smoke:inspect';
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-same-document-smoke-'));
app.setPath('userData', path.join(temporary, 'user-data'));
const events = [];
const preloads = [];
const inspections = [];
let window;
let httpServer;
let initialFrame;
let responseGeneration = 0;
let protocolHandled = false;
let finished = false;
let phase = 'startup';

protocol.registerSchemesAsPrivileged([{
  scheme,
  privileges: { standard: true, secure: true, supportFetchAPI: true },
}]);
app.on('window-all-closed', () => {});

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  try {
    ipcMain.removeHandler(inspectChannel);
    ipcMain.removeAllListeners(preloadChannel);
    if (window && !window.isDestroyed()) window.destroy();
    if (httpServer) {
      httpServer.closeAllConnections();
      httpServer.close();
    }
    if (protocolHandled) protocol.unhandle(scheme);
    fs.rmSync(temporary, { recursive: true, force: true });
  } finally {
    if (error) {
      console.error(error.stack || error);
      console.error(JSON.stringify({ phase, responseGeneration, preloadCount: preloads.length, events }));
    } else console.log('PASS: same-document history preserves document, preload, IPC frame, and navigation classification while reload replaces the document');
    app.exit(error ? 1 : 0);
  }
}

const deadline = setTimeout(() =>
  finish(new Error(`Same-document navigation smoke timed out during ${phase}`)), 30000);
const pause = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
async function eventually(label, predicate) {
  const until = Date.now() + 4000;
  for (;;) {
    if (await predicate()) return;
    if (Date.now() >= until) throw new Error(`Timed out waiting for ${label}`);
    await pause(20);
  }
}
async function evaluate(source) {
  return window.webContents.executeJavaScript(source);
}
async function inspect(label) {
  const inspectionCount = inspections.length;
  const serialized = await evaluate(`(() => {
    const snapshot = {
      href: location.href,
      state: history.state,
      markerValue: window.__sameDocumentMarker.value,
      markerGeneration: window.__sameDocumentMarker.generation,
      markerIsOriginalObject: window.__sameDocumentMarker === window.__sameDocumentMarkerAlias,
      pageEvents: window.__sameDocumentPageEvents.slice(),
      preloadToken: sameDocumentSmoke.preloadToken
    };
    sameDocumentSmoke.inspect(${JSON.stringify(label)}, snapshot);
    return JSON.stringify(snapshot);
  })()`);
  await eventually(`${label} native IPC inspection`, () => inspections.length === inspectionCount + 1);
  return JSON.parse(serialized);
}
function lifecycleCounts(records) {
  const counts = {
    navigate: 0, domReady: 0, finishLoad: 0, startLoading: 0, stopLoading: 0,
  };
  for (const record of records) {
    if (record.type === 'did-navigate') ++counts.navigate;
    else if (record.type === 'dom-ready') ++counts.domReady;
    else if (record.type === 'did-finish-load') ++counts.finishLoad;
    else if (record.type === 'did-start-loading') ++counts.startLoading;
    else if (record.type === 'did-stop-loading') ++counts.stopLoading;
  }
  return counts;
}
async function sameDocumentStep(label, script, expectedUrl, expectedState) {
  phase = label;
  const eventOffset = events.length;
  const preloadCount = preloads.length;
  await evaluate(script);
  await eventually(`${label} did-navigate-in-page`, () => events.slice(eventOffset).some(
    event => event.type === 'did-navigate-in-page' && event.url === expectedUrl));
  await pause(25);
  const records = events.slice(eventOffset);
  const starts = records.filter(event => event.type === 'did-start-navigation');
  assert.ok(starts.every(event => event.isSameDocument === true),
    `${label} must never be reported as a cross-document navigation`);
  assert.ok(starts.every(event => event.isMainFrame === true),
    `${label} did-start-navigation must identify the main frame`);
  const inPage = records.filter(event => event.type === 'did-navigate-in-page');
  assert.equal(inPage.length, 1, `${label} must emit one in-page navigation`);
  assert.equal(inPage[0].url, expectedUrl);
  assert.equal(inPage[0].isMainFrame, true);
  assert.deepEqual(lifecycleCounts(records), {
    navigate: 0, domReady: 0, finishLoad: 0, startLoading: 0, stopLoading: 0,
  }, `${label} must not invent a new-document lifecycle`);
  assert.equal(preloads.length, preloadCount, `${label} must not run the preload again`);

  const snapshot = await inspect(label);
  assert.equal(snapshot.href, expectedUrl);
  assert.deepEqual(snapshot.state, expectedState);
  assert.equal(snapshot.markerValue, 'owned-live-document');
  assert.equal(snapshot.markerGeneration, 1);
  assert.equal(snapshot.markerIsOriginalObject, true);
  assert.equal(snapshot.preloadToken, preloads[0].token);
  assert.equal(initialFrame.detached, false, `${label} must keep the original main frame live`);
  return snapshot;
}

app.whenReady().then(async () => {
  const preload = path.join(temporary, 'preload.js');
  fs.writeFileSync(preload, `const {contextBridge,ipcRenderer}=require('electron');
    const preloadToken = [process.pid, Date.now(), Math.random()].join(':');
    ipcRenderer.send(${JSON.stringify(preloadChannel)}, preloadToken);
    contextBridge.exposeInMainWorld('sameDocumentSmoke', {
      preloadToken,
      inspect: (label, snapshot) => ipcRenderer.invoke(
        ${JSON.stringify(inspectChannel)}, label, preloadToken, snapshot)
    });`);

  ipcMain.on(preloadChannel, (event, token) => {
    try {
      assert.equal(event.sender, window.webContents);
      assert.ok(event.senderFrame, 'Preload IPC must carry its native source frame');
      assert.equal(event.senderFrame, event.sender.mainFrame,
        'Preload must execute in the live main frame');
      assert.equal(event.senderFrame.detached, false);
      preloads.push({ token, frame: event.senderFrame });
    } catch (error) { finish(error); }
  });
  ipcMain.handle(inspectChannel, (event, label, preloadToken, snapshot) => {
    try {
      assert.equal(event.sender, window.webContents);
      assert.ok(event.senderFrame, 'Inspection IPC must carry its native source frame');
      assert.equal(event.senderFrame, event.sender.mainFrame,
        'Inspection IPC must retain main-frame authority');
      assert.equal(event.senderFrame.detached, false);
      assert.equal(preloadToken, snapshot.preloadToken);
      if (label === 'initial') initialFrame = event.senderFrame;
      else if (label !== 'reload') {
        assert.equal(event.senderFrame, initialFrame,
          `${label} IPC must originate from the original live main frame`);
      }
      inspections.push({ label, frame: event.senderFrame });
    } catch (error) {
      finish(error);
      throw error;
    }
  });

  protocol.handle(scheme, request => {
    const url = new URL(request.url);
    assert.equal(url.hostname, 'app');
    assert.ok(url.pathname === '/fixture' || url.pathname === '/beta',
      `Unexpected document request for ${url.pathname}`);
    const generation = ++responseGeneration;
    return new Response(`<!doctype html><meta charset="utf-8">
      <title>Same-document navigation smoke</title>
      <script>
        window.__sameDocumentMarker = { generation: ${generation}, value: 'fresh' };
        window.__sameDocumentMarkerAlias = window.__sameDocumentMarker;
        window.__sameDocumentPageEvents = [];
        addEventListener('popstate', event => window.__sameDocumentPageEvents.push({
          type: 'popstate', href: location.href, state: event.state
        }));
        addEventListener('hashchange', event => window.__sameDocumentPageEvents.push({
          type: 'hashchange', oldURL: event.oldURL, newURL: event.newURL
        }));
      </script>`, { headers: {
        'content-type': 'text/html; charset=utf-8',
        'cache-control': 'no-store',
      } });
  });
  protocolHandled = true;

  window = new BrowserWindow({
    width: 480,
    height: 320,
    show: false,
    webPreferences: {
      preload,
      sandbox: true,
      contextIsolation: true,
      nodeIntegration: false,
    },
  });
  const contents = window.webContents;
  contents.on('did-start-navigation', (_event, url, isSameDocument, isMainFrame,
    processId, routingId) => events.push({
    type: 'did-start-navigation', url, isSameDocument, isMainFrame, processId, routingId,
  }));
  contents.on('did-navigate-in-page', (_event, url, isMainFrame, processId, routingId) =>
    events.push({ type: 'did-navigate-in-page', url, isMainFrame, processId, routingId }));
  contents.on('did-navigate', (_event, url) => events.push({ type: 'did-navigate', url }));
  contents.on('dom-ready', () => events.push({ type: 'dom-ready' }));
  contents.on('did-finish-load', () => events.push({ type: 'did-finish-load' }));
  contents.on('did-start-loading', () => events.push({ type: 'did-start-loading' }));
  contents.on('did-stop-loading', () => events.push({ type: 'did-stop-loading' }));
  contents.on('did-fail-load', (_event, code, description, url, isMainFrame) =>
    events.push({ type: 'did-fail-load', code, description, url, isMainFrame }));
  contents.on('render-process-gone', (...args) =>
    finish(new Error('Same-document fixture renderer exited: ' + JSON.stringify(args.at(-1)))));
  const blockedNavigations = [];
  const navigationGuards = ['will-frame-navigate', 'will-navigate'].map(type => {
    const listener = (event, url) => {
      blockedNavigations.push({ type, url });
      event.preventDefault();
    };
    contents.on(type, listener);
    return [type, listener];
  });

  phase = 'initial document load';
  const initialUrl = `${scheme}://app/fixture`;
  await window.loadURL(initialUrl);
  assert.deepEqual(blockedNavigations, [],
    'programmatic initial loadURL must not emit cancellable navigation events');
  await eventually('initial preload IPC', () => preloads.length === 1);
  assert.equal(responseGeneration, 1);
  assert.equal(preloads.length, 1);
  let snapshot = await inspect('initial');
  assert.equal(snapshot.href, initialUrl);
  assert.equal(snapshot.markerValue, 'fresh');
  assert.equal(snapshot.markerGeneration, 1);
  assert.equal(snapshot.markerIsOriginalObject, true);
  assert.equal(snapshot.preloadToken, preloads[0].token);
  assert.equal(inspections.length, 1);

  phase = 'renderer navigation remains cancellable';
  const blockedUrl = `${scheme}://app/blocked-by-guard`;
  await evaluate(`location.href = ${JSON.stringify(blockedUrl)}; true`);
  await eventually('renderer navigation guard',
    () => blockedNavigations.some(event => event.url === blockedUrl));
  await pause(50);
  assert.equal(await evaluate('location.href'), initialUrl,
    'preventDefault must still block renderer-initiated navigation');
  assert.equal(responseGeneration, 1, 'blocked navigation must not load another document');
  assert.equal(preloads.length, 1, 'blocked navigation must preserve the existing preload');
  for (const [type, listener] of navigationGuards) contents.removeListener(type, listener);

  await evaluate(`window.__sameDocumentMarker.value = 'owned-live-document'`);
  const alphaUrl = `${scheme}://app/alpha?step=1`;
  snapshot = await sameDocumentStep('pushState',
    `history.pushState({kind:'push', sequence:1}, '', '/alpha?step=1')`,
    alphaUrl, { kind: 'push', sequence: 1 });
  assert.deepEqual(snapshot.pageEvents, [], 'pushState must not synthesize DOM traversal events');

  const betaUrl = `${scheme}://app/beta?step=2`;
  snapshot = await sameDocumentStep('replaceState',
    `history.replaceState({kind:'replace', sequence:2}, '', '/beta?step=2')`,
    betaUrl, { kind: 'replace', sequence: 2 });
  assert.deepEqual(snapshot.pageEvents, [], 'replaceState must not synthesize DOM traversal events');

  const fragmentUrl = `${betaUrl}#fragment`;
  snapshot = await sameDocumentStep('fragment navigation',
    `location.hash = 'fragment'`, fragmentUrl, null);
  await eventually('fragment hashchange', async () => {
    snapshot = await inspect('fragment-event');
    return snapshot.pageEvents.some(event => event.type === 'hashchange'
      && event.newURL === fragmentUrl);
  });

  snapshot = await sameDocumentStep('history back', `history.back()`, betaUrl,
    { kind: 'replace', sequence: 2 });
  await eventually('back popstate', async () => {
    snapshot = await inspect('back-event');
    return snapshot.pageEvents.some(event => event.type === 'popstate'
      && event.href === betaUrl && event.state && event.state.kind === 'replace');
  });

  snapshot = await sameDocumentStep('history forward', `history.forward()`, fragmentUrl, null);
  await eventually('forward popstate', async () => {
    snapshot = await inspect('forward-event');
    return snapshot.pageEvents.some(event => event.type === 'popstate'
      && event.href === fragmentUrl && event.state === null);
  });
  assert.equal(responseGeneration, 1,
    'Same-document navigation must not request another HTML document');
  assert.equal(preloads.length, 1,
    'The full same-document sequence must retain one preload context');

  phase = 'same-URL reload';
  const reloadOffset = events.length;
  const oldPreloadToken = preloads[0].token;
  const reloaded = new Promise(resolve => contents.once('did-finish-load', resolve));
  window.reload();
  await reloaded;
  await eventually('reload preload IPC', () => preloads.length === 2);
  await pause(25);
  const reloadEvents = events.slice(reloadOffset);
  const reloadStarts = reloadEvents.filter(event => event.type === 'did-start-navigation');
  assert.ok(reloadStarts.length >= 1, 'Reload must begin a genuine navigation');
  assert.ok(reloadStarts.every(event => event.isSameDocument === false),
    'Reloading the same URL must remain a cross-document navigation');
  assert.ok(reloadStarts.every(event => event.isMainFrame === true));
  assert.ok(reloadEvents.some(event => event.type === 'did-navigate'
    && event.url === fragmentUrl), 'Reload must commit a new document at the same URL');
  const reloadLifecycle = lifecycleCounts(reloadEvents);
  assert.ok(reloadLifecycle.navigate >= 1);
  assert.ok(reloadLifecycle.domReady >= 1);
  assert.ok(reloadLifecycle.finishLoad >= 1);
  assert.ok(reloadLifecycle.startLoading >= 1);
  assert.ok(reloadLifecycle.stopLoading >= 1);
  assert.equal(responseGeneration, 2,
    'Reload must request the fixture even though its URL is unchanged');
  assert.notEqual(preloads[1].token, oldPreloadToken,
    'Reload must create and run a new preload context');

  snapshot = await inspect('reload');
  assert.equal(snapshot.href, fragmentUrl);
  assert.equal(snapshot.state, null);
  assert.equal(snapshot.markerValue, 'fresh', 'Reload must reset document-owned state');
  assert.equal(snapshot.markerGeneration, 2);
  assert.equal(snapshot.markerIsOriginalObject, true);
  assert.equal(snapshot.preloadToken, preloads[1].token);
  assert.equal(preloads[1].frame, window.webContents.mainFrame);
  assert.equal(preloads[1].frame.detached, false);
  assert.equal(preloads[0].frame.detached, true,
    'A true reload must revoke the previous document frame');

  phase = 'HTTP query-and-fragment navigation';
  let httpResponses = 0;
  httpServer = http.createServer((request, response) => {
    if (request.url !== '/fixture?step=7') {
      response.writeHead(404); response.end(); return;
    }
    ++httpResponses;
    response.writeHead(200, { 'content-type': 'text/html', 'cache-control': 'no-store' });
    response.end(`<!doctype html><script>window.httpNavigationGeneration=${httpResponses}</script>`);
  });
  await new Promise((resolve, reject) => {
    httpServer.once('error', reject);
    httpServer.listen(0, '127.0.0.1', resolve);
  });
  const httpUrl = `http://127.0.0.1:${httpServer.address().port}/fixture?step=7#anchor`;
  const httpPreloadCount = preloads.length;
  await window.loadURL(httpUrl);
  await eventually('HTTP preload', () => preloads.length === httpPreloadCount + 1);
  const inspectHttp = async () => JSON.parse(await evaluate(
    `JSON.stringify({href:location.href,generation:window.httpNavigationGeneration})`));
  assert.deepEqual(await inspectHttp(), { href: httpUrl, generation: 1 });
  phase = 'HTTP query-and-fragment reload';
  const httpReloadOffset = events.length;
  const httpReloaded = new Promise(resolve => contents.once('did-finish-load', resolve));
  window.reload();
  await httpReloaded;
  await eventually('HTTP reload preload', () => preloads.length === httpPreloadCount + 2);
  assert.equal(httpResponses, 2, 'HTTP reload must preserve the query and request fresh bytes');
  assert.deepEqual(await inspectHttp(), { href: httpUrl, generation: 2 });
  assert.ok(events.slice(httpReloadOffset).some(event =>
    event.type === 'did-start-navigation' && event.url === httpUrl && !event.isSameDocument));
  assert.ok(events.slice(httpReloadOffset).some(event =>
    event.type === 'did-navigate' && event.url === httpUrl));
  assert.equal(preloads.at(-2).frame.detached, true);
  assert.equal(preloads.at(-1).frame, contents.mainFrame);
  assert.notEqual(preloads.at(-1).token, preloads.at(-2).token);
  finish();
}).catch(finish);
