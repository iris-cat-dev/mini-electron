'use strict';

// Real Response -> native custom scheme -> Blink document and subresources.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const { app, BrowserWindow, protocol, net } = require('electron');
const scheme = 'native-response-smoke';
const sprite = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==', 'base64');
const policy = "default-src 'none'; script-src 'self' 'unsafe-inline'; img-src 'self'; connect-src 'self'";
let window;
let finished = false;
let phase = 'startup';
const deadline = setTimeout(() => finish(new Error(`Native protocol response smoke timed out during ${phase}`)), 60000);
app.on('window-all-closed', () => {});
protocol.registerSchemesAsPrivileged([{ scheme, privileges: {
  standard: true, secure: true, supportFetchAPI: true, corsEnabled: true,
} }]);
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  try {
    if (window && !window.isDestroyed()) window.destroy();
    protocol.unhandle(scheme);
  } finally {
    if (error) console.error(error.stack || error);
    else console.log('PASS: native Response HTML parsing, scripts, charset, headers and binary PNG');
    app.exit(error ? 1 : 0);
  }
}
async function verifyLocalFileResponses() {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-protocol-file-'));
  try {
    const bytes = Buffer.alloc(3 * 1024 * 1024 + 137);
    for (let index = 0; index < bytes.length; ++index)
      bytes[index] = (index * 17 + (index >>> 16)) & 255;
    const file = path.join(directory, 'payload.bin');
    fs.writeFileSync(file, bytes);
    const url = pathToFileURL(file).toString();
    const response = await net.fetch(url);
    assert.equal(response.headers.get('content-length'), String(bytes.length));
    assert.deepEqual(Buffer.from(await response.arrayBuffer()), bytes);
    const head = await net.fetch(url, { method: 'HEAD' });
    assert.equal(head.body, null);
    assert.equal(head.headers.get('content-length'), String(bytes.length));
    const emptyPath = path.join(directory, 'empty.txt');
    fs.writeFileSync(emptyPath, '');
    const empty = await net.fetch(pathToFileURL(emptyPath));
    assert.equal(empty.headers.get('content-length'), '0');
    assert.equal(await empty.text(), '');
    const canceled = await net.fetch(url);
    const reader = canceled.body.getReader();
    const chunk = await reader.read();
    assert.deepEqual(Buffer.from(chunk.value), bytes.subarray(0, chunk.value.byteLength));
    await reader.cancel();
    assert.deepEqual(await reader.read(), { done: true, value: undefined });
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  phase = 'reading local streamed files';
  await verifyLocalFileResponses();
  protocol.handle(scheme, request => {
    const pathname = new URL(request.url).pathname;
    if (pathname === '/sprite.png') return new Response(sprite, { headers: new Headers({
      'content-type': 'image/png', 'cache-control': 'no-store',
    }) });
    if (pathname === '/script.js') return new Response('window.externalScriptRan = true;', {
      headers: { 'content-type': 'application/javascript; charset=utf-8' },
    });
    if (pathname === '/plain') return new Response(
      '<!doctype html><canvas id="pet"></canvas><script>window.plainScriptRan = true;</script>',
      { headers: { 'content-type': 'text/html' } });
    if (pathname === '/latin1') return new Response(Buffer.from(
      '<!doctype html><meta><title>caf\xe9</title><body><canvas id="pet"></canvas><script>window.charsetScriptRan = true;</script>', 'latin1'),
    { headers: { 'Content-Type': 'text/html; charset="windows-1252"' } });
    return new Response(`<!doctype html><title>native protocol 文档</title>
      <canvas id="pet" width="1" height="1"></canvas><img id="sprite" src="/sprite.png">
      <script src="/script.js"></script><script>
      window.inlineScriptRan = true;
      fetch('/sprite.png').then(async response => {
        const bytes = Array.from(new Uint8Array(await response.arrayBuffer()));
        const documentResponse = await fetch('/');
        window.protocolResult = JSON.stringify({ bytes,
          contentType: documentResponse.headers.get('content-type'),
          cacheControl: documentResponse.headers.get('cache-control'),
          policy: documentResponse.headers.get('content-security-policy') });
      }).catch(error => { window.protocolError = String(error); });
      </script>`, { headers: new Headers({
      'content-type': 'text/html; charset=utf-8',
      'cache-control': 'no-store', 'content-security-policy': policy,
    }) });
  });
  window = new BrowserWindow({ show: false, webPreferences: {
    sandbox: true, contextIsolation: true, nodeIntegration: false,
  } });
  window.webContents.on('render-process-gone', (...args) =>
    finish(new Error('Protocol response killed renderer: ' + JSON.stringify(args.at(-1)))));
  phase = 'loading HTML Response';
  await window.loadURL(`${scheme}://app/`);
  const parsed = await window.webContents.executeJavaScript(`JSON.stringify({
    contentType: document.contentType, charset: document.characterSet,
    title: document.title, canvas: !!document.querySelector('canvas#pet'),
    inline: window.inlineScriptRan === true, external: window.externalScriptRan === true
  })`);
  assert.deepEqual(JSON.parse(parsed.value), {
    contentType: 'text/html', charset: 'UTF-8', title: 'native protocol 文档',
    canvas: true, inline: true, external: true,
  });
  phase = 'loading binary sprite and reading response headers';
  let result;
  for (;;) {
    const state = await window.webContents.executeJavaScript(`JSON.stringify({
      result: window.protocolResult || null, error: window.protocolError || null,
      imageReady: document.querySelector('#sprite').complete && document.querySelector('#sprite').naturalWidth === 1
    })`);
    const value = JSON.parse(state.value);
    assert.equal(value.error, null);
    if (value.result && value.imageReady) { result = JSON.parse(value.result); break; }
    await new Promise(resolve => setTimeout(resolve, 25));
  }
  assert.deepEqual(result.bytes, Array.from(sprite));
  assert.equal(result.contentType, 'text/html; charset=utf-8');
  assert.equal(result.cacheControl, 'no-store');
  assert.equal(result.policy, policy);
  phase = 'loading parameterized non-UTF-8 Response';
  await window.loadURL(`${scheme}://app/latin1`);
  const encoded = await window.webContents.executeJavaScript(`JSON.stringify({
    contentType: document.contentType, charset: document.characterSet,
    title: document.title, script: window.charsetScriptRan === true
  })`);
  assert.deepEqual(JSON.parse(encoded.value), {
    contentType: 'text/html', charset: 'windows-1252', title: 'café', script: true,
  });
  phase = 'loading exact HTML media type';
  await window.loadURL(`${scheme}://app/plain`);
  const plain = await window.webContents.executeJavaScript(`JSON.stringify({
    contentType: document.contentType, canvas: !!document.querySelector('canvas#pet'),
    script: window.plainScriptRan === true
  })`);
  assert.deepEqual(JSON.parse(plain.value), {
    contentType: 'text/html', canvas: true, script: true,
  });
  finish();
}).catch(finish);
