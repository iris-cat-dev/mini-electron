'use strict';

// Run with the native runtime, not Node:
// mini-electron test/websocket-receive-lifecycle-smoke.js
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, ipcMain, protocol } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-ws-lifecycle-'));
const sockets = new Set();
const peerCloses = new Map();
const payloadBytes = 12 * 1024 * 1024 + 13;
let server;
let window;
let finished = false;

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  window?.destroy();
  for (const socket of sockets) socket.destroy();
  server?.close();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: unread stream cancellation, ordinary close, and rejected-send receive-credit cleanup');
  app.exit(error ? 1 : 0);
}

const deadline = setTimeout(() => finish(new Error('WebSocket receive lifecycle smoke timed out')), 45000);

function frame(opcode, body) {
  let header;
  if (body.length < 126) {
    header = Buffer.from([0x80 | opcode, body.length]);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x80 | opcode;
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(body.length), 2);
  }
  return Buffer.concat([header, body]);
}

function observeClientClose(socket, route) {
  let buffered = Buffer.alloc(0);
  socket.on('data', chunk => {
    buffered = Buffer.concat([buffered, chunk]);
    while (buffered.length >= 2) {
      let offset = 2;
      let length = buffered[1] & 0x7f;
      if (length === 126) {
        if (buffered.length < 4) return;
        length = buffered.readUInt16BE(2);
        offset = 4;
      } else if (length === 127) {
        if (buffered.length < 10) return;
        const wide = buffered.readBigUInt64BE(2);
        assert.ok(wide <= BigInt(Number.MAX_SAFE_INTEGER));
        length = Number(wide);
        offset = 10;
      }
      const masked = !!(buffered[1] & 0x80);
      const total = offset + (masked ? 4 : 0) + length;
      if (buffered.length < total) return;
      const opcode = buffered[0] & 0x0f;
      let payload = buffered.subarray(offset + (masked ? 4 : 0), total);
      if (masked) {
        const mask = buffered.subarray(offset, offset + 4);
        payload = Buffer.from(payload);
        for (let i = 0; i < payload.length; ++i) payload[i] ^= mask[i % 4];
      }
      buffered = buffered.subarray(total);
      if (opcode === 8) {
        peerCloses.set(route, true);
        socket.receiveCancelled = true;
        socket.end(frame(8, payload));
        return;
      }
    }
  });
}

async function sendUnreadData(socket) {
  const data = frame(2, Buffer.alloc(payloadBytes, 0x6c));
  for (let index = 0; index < 4 && !socket.receiveCancelled && !socket.destroyed; ++index) {
    if (socket.write(data)) continue;
    await new Promise((resolve, reject) => {
      const cleanup = () => {
        socket.off('drain', drained);
        socket.off('close', closed);
        socket.off('error', failed);
      };
      const drained = () => { cleanup(); resolve(); };
      const closed = () => { cleanup(); resolve(); };
      const failed = error => { cleanup(); reject(error); };
      socket.once('drain', drained);
      socket.once('close', closed);
      socket.once('error', failed);
    });
  }
}

protocol.registerSchemesAsPrivileged([
  { scheme: 'mini-ws-lifecycle', privileges: { standard: true, secure: true, supportFetchAPI: true } },
]);

app.whenReady().then(async () => {
  server = http.createServer();
  server.on('upgrade', (request, socket) => {
    const route = request.url;
    assert.match(route, /^\/(cancel\/[0-4]|ordinary|rejected-send)$/);
    const accept = crypto.createHash('sha1')
      .update(request.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
      .digest('base64');
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
    socket.on('error', error => { if (!socket.receiveCancelled) finish(error); });
    peerCloses.set(route, false);
    observeClientClose(socket, route);
    sendUnreadData(socket).catch(finish);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const baseUrl = `ws://127.0.0.1:${server.address().port}`;
  const preload = path.join(temporary, 'preload.js');
  fs.writeFileSync(preload, "const{contextBridge,ipcRenderer}=require('electron');contextBridge.exposeInMainWorld('lifecycle',{report:value=>ipcRenderer.send('ws-lifecycle:result',value)});");

  protocol.handle('mini-ws-lifecycle', () => new Response(`<!doctype html><meta charset="utf-8"><script>
    (async () => {
      if (typeof WebSocketStream !== 'function')
        throw new Error('WebSocketStream API is unavailable in this runtime');
      const base = ${JSON.stringify(baseUrl)};
      const pause = () => new Promise(resolve => setTimeout(resolve, 350));
      for (let index = 0; index < 5; ++index) {
        const stream = new WebSocketStream(base + '/cancel/' + index);
        const { readable } = await stream.opened;
        // Leave the first oversize payload unread and the following transport
        // messages backpressured. No read()/Pull is permitted before cancel.
        await pause();
        await readable.cancel();
        await stream.closed;
        const result = await readable.getReader().read();
        if (!result.done || result.value !== undefined)
          throw new Error('Cancelled stream delivered discarded data');
      }
      await new Promise((resolve, reject) => {
        const socket = new WebSocket(base + '/ordinary');
        let messages = 0;
        socket.binaryType = 'arraybuffer';
        socket.onmessage = () => {
          if (++messages !== 1) return reject(new Error('Ordinary close delivered discarded data'));
          socket.close(1000);
        };
        socket.onerror = () => reject(new Error('Ordinary close transport error'));
        socket.onclose = event => {
          if (!event.wasClean || messages !== 1) reject(new Error('Ordinary close did not complete cleanly'));
          else resolve();
        };
      });
      await new Promise((resolve, reject) => {
        const socket = new WebSocket(base + '/rejected-send');
        let errors = 0;
        socket.onopen = () => socket.send(new ArrayBuffer(60 * 1024 * 1024 + 1));
        socket.onerror = () => { ++errors; };
        socket.onclose = () => {
          if (errors !== 1) reject(new Error('Rejected send did not produce exactly one operation error'));
          else resolve();
        };
      });
      lifecycle.report({ cancelled: 5, ordinary: true, rejectedSend: true });
    })().catch(error => lifecycle.report({ error: error.stack || String(error) }));
  </script>`, { headers: { 'content-type': 'text/html' } }));

  const resultPromise = new Promise(resolve => ipcMain.once('ws-lifecycle:result', (_event, result) => resolve(result)));
  window = new BrowserWindow({ show: false, webPreferences: { preload, sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadURL('mini-ws-lifecycle://app/');
  const result = await resultPromise;
  assert.deepEqual(result, { cancelled: 5, ordinary: true, rejectedSend: true });
  // Renderer close notification alone is insufficient: a synthetic send error
  // must also close the otherwise-live native broker socket. Allow its command
  // to traverse IPC and release the peer's final backpressured payload.
  for (let attempt = 0; attempt < 100 && [...peerCloses.values()].some(value => !value); ++attempt)
    await new Promise(resolve => setTimeout(resolve, 20));
  assert.equal(peerCloses.size, 7);
  assert.ok([...peerCloses.values()].every(Boolean), 'A cancelled receive path left its native socket running');
  finish();
}).catch(finish);
