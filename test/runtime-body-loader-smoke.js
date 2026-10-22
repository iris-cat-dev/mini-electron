'use strict';

// Run: out/mac-arm64/mini-electron test/runtime-body-loader-smoke.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-runtime-body-'));
app.setPath('userData', path.join(temporary, 'user-data'));
app.on('window-all-closed', () => {});
let window;
let finished = false;
const server = http.createServer((request, response) => {
  if (request.url === '/') {
    response.writeHead(200, { 'Content-Type': 'text/html' });
    response.end('<!doctype html><title>runtime body loader smoke</title>');
  } else if (request.url === '/redirect') {
    response.writeHead(307, { Location: '/echo' });
    response.end();
  } else if (request.url === '/echo') {
    const chunks = [];
    request.on('data', chunk => chunks.push(chunk));
    request.on('end', () => {
      const body = Buffer.concat(chunks);
      response.writeHead(200, { 'Content-Type': 'application/octet-stream', 'X-Request-Method': request.method });
      response.end(body);
    });
  } else if (request.url.startsWith('/empty/')) {
    response.writeHead(Number(request.url.slice('/empty/'.length)), { 'X-Empty-Response': 'yes' });
    response.end();
  } else {
    response.writeHead(404);
    response.end();
  }
});
const deadline = setTimeout(() => finish(new Error('Body loader smoke timed out')), 45000);
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (window && !window.isDestroyed()) window.destroy();
  server.close();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: empty HTTP status/headers, blob/file bodies, redirected uploads, streamed bodies and blob URL reads');
  app.exit(error ? 1 : 0);
}

app.whenReady().then(async () => {
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  window = new BrowserWindow({ show: false, webPreferences: { sandbox: true, contextIsolation: true } });
  await window.loadURL(`http://127.0.0.1:${server.address().port}/`);
  const result = await window.webContents.executeJavaScript(`(async () => {
    const emptyResponses = [];
    for (const [method, status] of [['GET', 200], ['GET', 204], ['GET', 304], ['GET', 404], ['HEAD', 200]]) {
      const response = await fetch('/empty/' + status, {method, cache: 'no-store'});
      emptyResponses.push([method, response.status, response.headers.get('X-Empty-Response'),
        (await response.arrayBuffer()).byteLength]);
    }
    const bytes = Uint8Array.from({length: 256 * 1024 + 37}, (_, i) => (i * 37 + 11) & 255);
    function check(actual, label) {
      if (actual.length !== bytes.length || actual.some((value, index) => value !== bytes[index]))
        throw new Error(label + ' payload mismatch: ' + actual.length);
    }
    const file = new File([bytes], 'body.bin', {type: 'application/octet-stream'});
    for (const [label, body, url] of [
      ['blob', new Blob([bytes]), '/echo'],
      ['file', file, '/echo'],
      ['redirected-file', file, '/redirect']
    ]) {
      const response = await fetch(url, {method: 'POST', body});
      if (!response.ok || response.headers.get('X-Request-Method') !== 'POST') throw new Error(label + ' response');
      check(new Uint8Array(await response.arrayBuffer()), label);
    }
    const stream = new ReadableStream({
      start(controller) {
        for (let offset = 0; offset < bytes.length; offset += 8191)
          controller.enqueue(bytes.slice(offset, offset + 8191));
        controller.close();
      }
    });
    const streamed = await fetch('/echo', {method: 'POST', body: stream, duplex: 'half'});
    check(new Uint8Array(await streamed.arrayBuffer()), 'stream');
    const abortController = new AbortController();
    let pulls = 0, markStarted;
    const uploadStarted = new Promise(resolve => { markStarted = resolve; });
    const unfinished = new ReadableStream({
      pull(controller) {
        controller.enqueue(bytes.slice(0, 8191));
        if (++pulls === 4) markStarted();
      }
    });
    const cancelled = fetch('/echo', {
      method: 'POST', body: unfinished, duplex: 'half', signal: abortController.signal
    });
    await uploadStarted;
    abortController.abort();
    let aborted = false;
    try { await cancelled; } catch (error) {
      if (error.name !== 'AbortError') throw error;
      aborted = true;
    }
    if (!aborted) throw new Error('unfinished upload was not aborted');
    const recovered = new Uint8Array(await (await fetch('/echo', {
      method: 'POST', body: Uint8Array.of(7, 0, 9)
    })).arrayBuffer());
    if (recovered.length !== 3 || recovered[0] !== 7 || recovered[1] !== 0 || recovered[2] !== 9)
      throw new Error('network upload did not recover after cancellation');
    const blobUrl = URL.createObjectURL(file);
    check(new Uint8Array(await (await fetch(blobUrl)).arrayBuffer()), 'blob URL');
    const xhr = new XMLHttpRequest();
    xhr.open('GET', blobUrl, false);
    xhr.overrideMimeType('text/plain; charset=x-user-defined');
    xhr.send();
    check(Uint8Array.from(xhr.responseText, c => c.charCodeAt(0) & 255), 'sync blob XHR');
    URL.revokeObjectURL(blobUrl);
    let rejected = false;
    try { await fetch(blobUrl); } catch (_) { rejected = true; }
    if (!rejected) throw new Error('revoked blob unexpectedly loaded');
    return emptyResponses;
  })()`);
  assert.deepEqual(result, [
    ['GET', 200, 'yes', 0], ['GET', 204, 'yes', 0], ['GET', 304, 'yes', 0],
    ['GET', 404, 'yes', 0], ['HEAD', 200, 'yes', 0]
  ]);
  finish();
}).catch(finish);
