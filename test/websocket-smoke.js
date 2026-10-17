'use strict';

// Run with the native runtime, not Node: mini-electron test/websocket-smoke.js.
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, ipcMain, protocol } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-websocket-smoke-'));
const sockets = new Set();
const expectedText = 'renderer → daemon';
const expectedBinary = Buffer.from([0, 127, 128, 255]);
let receivedText = false;
let receivedBinary = false;
let receivedPong = false;
let finished = false;
let server;
let window;

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  for (const socket of sockets) socket.destroy();
  server?.close();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: native WebSocket handshake, text, binary, ping/pong, and clean close');
  app.exit(error ? 1 : 0);
}
const deadline = setTimeout(() => finish(new Error('Native WebSocket smoke timed out')), 20000);

function frame(opcode, bytes, final = true) {
  const body = Buffer.from(bytes);
  assert.ok(body.length < 126);
  return Buffer.concat([Buffer.from([(final ? 128 : 0) | opcode, body.length]), body]);
}

protocol.registerSchemesAsPrivileged([
  { scheme: 'mini-websocket-smoke', privileges: { standard: true, secure: true, supportFetchAPI: true } },
]);

app.whenReady().then(async () => {
  server = http.createServer();
  server.on('upgrade', (request, socket) => {
    sockets.add(socket);
    socket.on('close', () => sockets.delete(socket));
    socket.on('error', finish);
    try {
      assert.equal(request.url, '/ws');
      assert.equal(request.headers['sec-websocket-protocol'], 'mini-smoke');
      const accept = crypto.createHash('sha1')
        .update(request.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
        .digest('base64');
      socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
        `Sec-WebSocket-Accept: ${accept}\r\nSec-WebSocket-Protocol: mini-smoke\r\n\r\n`);
      socket.write(Buffer.concat([
        frame(1, Buffer.from('daemon → '), false),
        frame(9, Buffer.from('ping')),
        frame(0, Buffer.from('renderer')),
        frame(2, expectedBinary),
      ]));
      let buffered = Buffer.alloc(0);
      let closing = false;
      socket.on('data', data => {
        try {
          buffered = Buffer.concat([buffered, data]);
          while (buffered.length >= 6) {
            const opcode = buffered[0] & 15;
            const length = buffered[1] & 127;
            assert.equal(buffered[1] & 128, 128, 'client frames must be masked');
            assert.ok(length < 126, 'smoke messages use short frames');
            if (buffered.length < 6 + length) break;
            const bytes = Buffer.alloc(length);
            for (let i = 0; i < length; ++i) bytes[i] = buffered[6 + i] ^ buffered[2 + i % 4];
            buffered = buffered.subarray(6 + length);
            if (opcode === 1) {
              assert.equal(bytes.toString(), expectedText);
              receivedText = true;
            } else if (opcode === 2) {
              assert.deepEqual(bytes, expectedBinary);
              receivedBinary = true;
            } else if (opcode === 10) {
              assert.equal(bytes.toString(), 'ping');
              receivedPong = true;
            } else if (opcode === 8) {
              assert.equal(bytes.readUInt16BE(), 1000);
              socket.end();
            } else {
              throw new Error(`Unexpected client opcode: ${opcode}`);
            }
          }
          if (!closing && receivedText && receivedBinary && receivedPong) {
            closing = true;
            socket.write(frame(8, Buffer.from([3, 232])));
          }
        } catch (error) { finish(error); }
      });
    } catch (error) { finish(error); }
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const url = `ws://127.0.0.1:${server.address().port}/ws`;
  const preload = path.join(temporary, 'preload.js');
  fs.writeFileSync(preload, "const {contextBridge,ipcRenderer}=require('electron'); contextBridge.exposeInMainWorld('smoke',{finish:result=>ipcRenderer.send('smoke:result',result)});");
  ipcMain.on('smoke:result', (_event, result) => {
    try {
      assert.deepEqual(result, { protocol: 'mini-smoke', text: 'daemon → renderer', bytes: [0, 127, 128, 255], code: 1000, clean: true });
      assert.ok(receivedText && receivedBinary && receivedPong);
      finish();
    } catch (error) { finish(error); }
  });
  protocol.handle('mini-websocket-smoke', () => new Response(`<!doctype html><title>WebSocket smoke</title><script>
    const result = {};
    const socket = new WebSocket(${JSON.stringify(url)}, 'mini-smoke');
    socket.binaryType = 'arraybuffer';
    socket.onopen = () => {
      result.protocol = socket.protocol;
      socket.send(${JSON.stringify(expectedText)});
      socket.send(new Uint8Array([0,127,128,255]));
    };
    socket.onmessage = event => {
      if (typeof event.data === 'string') result.text = event.data;
      else result.bytes = Array.from(new Uint8Array(event.data));
    };
    socket.onerror = () => { result.error = 'Native WebSocket transport error'; smoke.finish(result); };
    socket.onclose = event => { result.code = event.code; result.clean = event.wasClean; smoke.finish(result); };
  </script>`, { headers: { 'content-type': 'text/html' } }));
  window = new BrowserWindow({ width: 320, height: 200, webPreferences: { preload, sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadURL('mini-websocket-smoke://app/');
}).catch(finish);
