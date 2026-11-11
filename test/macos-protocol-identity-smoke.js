'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { app, BrowserWindow, protocol } = require('electron');
const root = process.env.MINI_PROTOCOL_IDENTITY_ROOT;
assert.ok(root && path.isAbsolute(root), 'Run in the dedicated identity fixture with MINI_PROTOCOL_IDENTITY_ROOT');
const early = app.getPath('userData');
assert.ok(early.startsWith(root + path.sep) || early === path.join(process.env.MINI_PROTOCOL_IDENTITY_APP_DATA, 'omp-native-protocol-identity-2s3ufs'), 'Native startup must use the dedicated fixture namespace');
for (const name of ['userData', 'sessionData', 'temp', 'logs', 'crashDumps', 'cache']) {
  const directory = path.join(root, name);
  fs.mkdirSync(directory, { recursive: true });
  app.setPath(name, directory);
}
const scheme = 'native-identity-smoke';
const pixel = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==', 'base64');
const windows = [];
const documents = new Map();
let finished = false;
const timeout = setTimeout(() => finish(new Error('Native protocol identity smoke timed out')), 60000);
app.on('window-all-closed', () => {});
protocol.registerSchemesAsPrivileged([{ scheme, privileges: { standard: true, secure: true, supportFetchAPI: true, corsEnabled: true } }]);
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(timeout);
  for (const window of windows) if (!window.isDestroyed()) window.destroy();
  protocol.unhandle(scheme);
  if (error) console.error(error.stack || error);
  else console.log('PASS: readonly native identity, distinct windows/subframes, transported request headers, Range 206/416 responses, binary resources and document replacement');
  app.exit(error ? 1 : 0);
}
async function state(window, prefix = 'identity') {
  for (;;) {
    const result = JSON.parse(await window.webContents.executeJavaScript(`JSON.stringify({value:window.${prefix}State||null,error:window.${prefix}Error||null})`));
    assert.equal(result.error, null);
    if (result.value) return result.value;
    await new Promise(resolve => setTimeout(resolve, 25));
  }
}
app.whenReady().then(async () => {
  protocol.handle(scheme, request => {
    assert.ok(request instanceof Request);
    const identity = request.nativeRequest;
    assert.ok(identity, 'Canonical protocol.handle must preserve native browser identity');
    assert.ok(Object.isFrozen(identity));
    assert.equal(typeof identity.contentsId, 'number');
    assert.match(identity.frameId, /^[1-9][0-9]*$/);
    assert.equal(typeof identity.isMainFrame, 'boolean');
    assert.equal(typeof identity.isDocument, 'boolean');
    assert.ok(identity.navigationToken, 'Every subresource must carry its originating document token');
    assert.throws(() => { identity.contentsId = -1; }, TypeError);
    assert.throws(() => { request.nativeRequest = {}; }, TypeError);
    const url = new URL(request.url);
    if (identity.isDocument) documents.set(url.pathname, identity);
    const headers = { 'cache-control': 'no-store' };
    if (url.pathname === '/identity') {
      assert.equal(request.headers.get('x-native-contents-id'), '-1');
      assert.equal(request.headers.get('x-native-frame-id'), 'forged');
      assert.equal(request.headers.get('x-native-navigation-token'), 'forged');
      return Response.json(identity, { headers });
    }
    if (url.pathname === '/range') {
      assert.equal(request.headers.get('x-range-probe'), 'roundtrip');
      const range = request.headers.get('range');
      const responseHeaders = {
        ...headers, 'content-type': 'application/octet-stream',
        'accept-ranges': 'bytes', 'x-request-range': range === null ? 'none' : range,
      };
      if (range === null) return new Response(pixel, {
        status: 200, statusText: 'OK', headers: {
          ...responseHeaders, 'content-length': String(pixel.length),
        },
      });
      if (range === 'bytes=0-7') return new Response(pixel.subarray(0, 8), {
        status: 206, statusText: 'Partial Content', headers: {
          ...responseHeaders, 'content-range': `bytes 0-7/${pixel.length}`, 'content-length': '8',
        },
      });
      return new Response(null, {
        status: 416, statusText: 'Range Not Satisfiable', headers: {
          ...responseHeaders, 'content-range': `bytes */${pixel.length}`, 'content-length': '0',
        },
      });
    }
    if (url.pathname === '/pixel.png') return new Response(pixel, { headers: { ...headers, 'content-type': 'image/png' } });
    const child = url.pathname === '/child';
    const html = `<!doctype html><title>Native document identity</title><img id="pixel" src="/pixel.png">
      ${child ? '' : '<iframe id="child" src="/child"></iframe>'}
      <script>window.documentNativeIdentity=${JSON.stringify(identity)};fetch('/identity?contentsId=-1&frameId=forged&navigationToken=forged',{headers:{'x-native-contents-id':'-1','x-native-frame-id':'forged','x-native-navigation-token':'forged'}})
      .then(response=>response.json()).then(identity=>{window.identityState=identity;}).catch(error=>{window.identityError=String(error);});
      (async()=>{
        const results=[];
        for(const range of [null,'bytes=0-7','bytes=999999-','bytes=not-a-range']) {
          const headers=new Headers({'X-Range-Probe':'roundtrip'});
          if(range!==null) headers.set('rAnGe',range);
          const response=await fetch('/range?probe=request-headers',{headers,cache:'no-store'});
          results.push({status:response.status,statusText:response.statusText,
            range:response.headers.get('x-request-range'),contentRange:response.headers.get('content-range'),
            contentLength:response.headers.get('content-length'),cacheControl:response.headers.get('cache-control'),
            bytes:Array.from(new Uint8Array(await response.arrayBuffer()))});
        }
        window.rangeState=results;
      })().catch(error=>{window.rangeError=String(error);});</script>`;
    return new Response(html, { headers: { ...headers, 'content-type': 'text/html; charset=utf-8' } });
  });
  for (const name of ['a', 'b']) {
    const window = new BrowserWindow({ show: false, webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
    windows.push(window);
    await window.loadURL(`${scheme}://app/${name}`);
  }
  const [a, b] = windows;
  const aDocument = documents.get('/a');
  const bDocument = documents.get('/b');
  const aRequest = await state(a);
  const bRequest = await state(b);
  assert.equal(aDocument.contentsId, a.webContents.id);
  assert.equal(bDocument.contentsId, b.webContents.id);
  assert.equal(aDocument.frameId, a.webContents.mainFrame.nativeID);
  assert.equal(bDocument.frameId, b.webContents.mainFrame.nativeID);
  assert.notEqual(aDocument.contentsId, bDocument.contentsId);
  assert.notEqual(aDocument.navigationToken, bDocument.navigationToken);
  for (const [documentIdentity, resourceIdentity] of [[aDocument, aRequest], [bDocument, bRequest]]) {
    assert.equal(documentIdentity.isMainFrame, true);
    assert.equal(documentIdentity.isDocument, true);
    assert.deepEqual(resourceIdentity, { ...documentIdentity, isDocument: false });
  }
  const expectedRanges = [
    { status: 200, statusText: 'OK', range: 'none', contentRange: null,
      contentLength: String(pixel.length), cacheControl: 'no-store', bytes: Array.from(pixel) },
    { status: 206, statusText: 'Partial Content', range: 'bytes=0-7', contentRange: `bytes 0-7/${pixel.length}`,
      contentLength: '8', cacheControl: 'no-store', bytes: Array.from(pixel.subarray(0, 8)) },
    ...['bytes=999999-', 'bytes=not-a-range'].map(range => ({
      status: 416, statusText: 'Range Not Satisfiable', range, contentRange: `bytes */${pixel.length}`,
      contentLength: '0', cacheControl: 'no-store', bytes: [],
    })),
  ];
  for (const window of windows) assert.deepEqual(await state(window, 'range'), expectedRanges);
  const loaded = JSON.parse(await a.webContents.executeJavaScript('JSON.stringify({width:document.querySelector("#pixel").naturalWidth,height:document.querySelector("#pixel").naturalHeight})'));
  assert.deepEqual(loaded, { width: 1, height: 1 });
  let childIdentity;
  for (;;) {
    childIdentity = JSON.parse(await a.webContents.executeJavaScript('JSON.stringify(document.querySelector("#child").contentWindow.identityState||null)'));
    if (childIdentity) break;
    await new Promise(resolve => setTimeout(resolve, 25));
  }
  assert.equal(childIdentity.contentsId, a.webContents.id);
  assert.equal(childIdentity.isMainFrame, false);
  assert.equal(childIdentity.isDocument, false);
  assert.notEqual(childIdentity.frameId, aDocument.frameId);
  assert.notEqual(childIdentity.navigationToken, aDocument.navigationToken);
  const childDocument = JSON.parse(await a.webContents.executeJavaScript('JSON.stringify(document.querySelector("#child").contentWindow.documentNativeIdentity)'));
  assert.deepEqual(childIdentity, { ...childDocument, isDocument: false });
  await a.loadURL(`${scheme}://app/replaced`);
  const replaced = documents.get('/replaced');
  assert.equal(replaced.contentsId, a.webContents.id);
  assert.notEqual(replaced.navigationToken, aDocument.navigationToken);
  assert.deepEqual(await state(a), { ...replaced, isDocument: false });
  assert.equal((await state(b)).navigationToken, bDocument.navigationToken);
  finish();
}).catch(finish);
