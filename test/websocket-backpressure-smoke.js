'use strict';

// Run with the native runtime, not Node:
// mini-electron test/websocket-backpressure-smoke.js
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, ipcMain, protocol } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-ws-pressure-'));
const sockets = new Set();
const socketClosed = new Map();
const socketLifecycle = new Map();
const flowEmptyMessages = 2050; // Minimum charges cross the 8 MiB ordinary window.
const flowBurstMessages = 1024;
const flowBurstMessageBytes = 128 * 1024;
const flowProducerWrites = 3 + flowEmptyMessages + flowBurstMessages + 1;
const largeMessageBytes = 60 * 1024 * 1024;
const aggregateMessageBytes = 12 * 1024 * 1024 + 31;
const pendingFlowStarts = [];
const flowProgress = [];
let server;
let finished = false;

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  for (const socket of sockets) socket.destroy();
  server?.close();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: byte-bounded WebSocket receive credit, oversize fragmentation, aggregate flow, and paused disposal');
  app.exit(error ? 1 : 0);
}

function failSocket(socket, error) {
  const lifecycle = socketLifecycle.get(socket) || {
    route: 'unknown',
    phase: 'untracked',
    planned: 'unknown',
    intentionallyAbandoned: false,
  };
  if (lifecycle.route === '/dispose' && lifecycle.intentionallyAbandoned &&
      error?.code === 'ECONNRESET') {
    lifecycle.phase = 'peer-reset-after-intentional-renderer-destroy';
    return;
  }
  const cause = error instanceof Error ? error : new Error(String(error));
  const failure = new Error(
    `WebSocket ${lifecycle.route} failed during ${lifecycle.phase} ` +
    `(planned lifecycle: ${lifecycle.planned}): ${cause.message}`);
  if (cause.stack) failure.stack += `\nCaused by: ${cause.stack}`;
  finish(failure);
}

const deadline = setTimeout(
  () => finish(new Error('WebSocket backpressure smoke timed out')), 45000);

function frame(opcode, bytes, final = true) {
  const body = Buffer.from(bytes);
  let header;
  if (body.length < 126) {
    header = Buffer.from([(final ? 0x80 : 0) | opcode, body.length]);
  } else if (body.length <= 0xffff) {
    header = Buffer.alloc(4);
    header[0] = (final ? 0x80 : 0) | opcode;
    header[1] = 126;
    header.writeUInt16BE(body.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = (final ? 0x80 : 0) | opcode;
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(body.length), 2);
  }
  return Buffer.concat([header, body]);
}

function installCloseResponder(socket) {
  let buffered = Buffer.alloc(0);
  socket.once('end', () => socket.end());
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
        if (wide > BigInt(Number.MAX_SAFE_INTEGER)) throw new Error('Client frame is too large');
        length = Number(wide);
        offset = 10;
      }
      const masked = !!(buffered[1] & 0x80);
      const frameBytes = offset + (masked ? 4 : 0) + length;
      if (buffered.length < frameBytes) return;
      const opcode = buffered[0] & 0x0f;
      let payload = buffered.subarray(offset + (masked ? 4 : 0), frameBytes);
      if (masked) {
        const mask = buffered.subarray(offset, offset + 4);
        payload = Buffer.from(payload);
        for (let i = 0; i < payload.length; ++i) payload[i] ^= mask[i % 4];
      }
      buffered = buffered.subarray(frameBytes);
      if (opcode === 8) {
        socket.end(frame(8, payload));
        return;
      }
    }
  });
}

function waitForDrain(socket) {
  return new Promise((resolve, reject) => {
    const cleanup = () => {
      socket.off('drain', drained);
      socket.off('error', failed);
      socket.off('close', closed);
    };
    const drained = () => { cleanup(); resolve(); };
    const failed = error => { cleanup(); reject(error); };
    const closed = () => { cleanup(); reject(new Error('Socket closed while its peer was paused')); };
    socket.once('drain', drained);
    socket.once('error', failed);
    socket.once('close', closed);
  });
}

async function writeFrame(socket, value) {
  if (!socket.write(value)) await waitForDrain(socket);
}

function upgrade(request, socket) {
  const accept = crypto.createHash('sha1')
    .update(request.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
    .digest('base64');
  socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
    `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
  sockets.add(socket);
  socket.on('close', () => sockets.delete(socket));
  installCloseResponder(socket);
}

async function sendFlow(socket) {
  const progress = { completed: 0, snapshots: [] };
  flowProgress.push(progress);
  progress.ready = Promise.all([2000, 4000].map(delay =>
    new Promise(resolve => setTimeout(() => {
      progress.snapshots.push(progress.completed);
      resolve();
    }, delay))));
  const write = async value => {
    await writeFrame(socket, value);
    ++progress.completed;
  };

  const large = Buffer.allocUnsafe(largeMessageBytes);
  for (let i = 0; i < large.length; ++i) large[i] = i % 251;
  const split = 5 * 1024 * 1024 + 3;
  await write(frame(2, large.subarray(0, split), false));
  await write(frame(0, large.subarray(split), false));
  await write(frame(0, Buffer.alloc(0)));
  const empty = frame(1, Buffer.alloc(0));
  for (let i = 0; i < flowEmptyMessages; ++i) await write(empty);
  for (let i = 0; i < flowBurstMessages; ++i) {
    const bytes = Buffer.alloc(flowBurstMessageBytes, i & 0xff);
    bytes.writeUInt32BE(i);
    await write(frame(2, bytes));
  }
  await write(frame(1, Buffer.from('flow-done')));
}

async function sendAggregate(socket, index) {
  const bytes = Buffer.alloc(aggregateMessageBytes, 0x40 + index);
  await writeFrame(socket, frame(2, bytes));
  await writeFrame(socket, frame(1, Buffer.from(`aggregate-${index}`)));
}

async function sendUntilDisposed(socket) {
  const bytes = Buffer.alloc(1024 * 1024, 0x7d);
  const value = frame(2, bytes);
  for (let i = 0; i < 256; ++i) await writeFrame(socket, value);
}

function resultFromPage(name) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${name} page did not report`)), 15000);
    ipcMain.once(`ws-pressure:${name}`, (_event, result) => {
      clearTimeout(timer);
      resolve(result);
    });
  });
}

function page(source) {
  return new Response(`<!doctype html><meta charset="utf-8"><script>${source}</script>`,
    { headers: { 'content-type': 'text/html' } });
}

protocol.registerSchemesAsPrivileged([
  { scheme: 'mini-ws-pressure', privileges: { standard: true, secure: true, supportFetchAPI: true } },
]);

app.whenReady().then(async () => {
  server = http.createServer();
  let disposeUpgrade;
  const disposeUpgraded = new Promise(resolve => { disposeUpgrade = resolve; });
  server.on('upgrade', (request, socket) => {
    const lifecycle = {
      route: request.url,
      phase: 'upgrading',
      planned: 'unclassified',
      intentionallyAbandoned: false,
    };
    socketLifecycle.set(socket, lifecycle);
    socket.on('error', error => failSocket(socket, error));
    try {
      upgrade(request, socket);
      if (request.url === '/flow' || request.url === '/flow-independent') {
        lifecycle.phase = 'waiting-for-blocked-renderer';
        lifecycle.planned = request.url === '/flow'
          ? 'bounded producer stall, ordered delivery, resume, and clean close'
          : 'independent renderer progress, ordered delivery, and clean close';
        pendingFlowStarts.push(() => {
          lifecycle.phase = 'sending-flow';
          sendFlow(socket).then(
            () => { lifecycle.phase = 'flow-sent-awaiting-close'; },
            error => failSocket(socket, error));
        });
      } else if (request.url.startsWith('/aggregate/')) {
        const index = Number(request.url.slice('/aggregate/'.length));
        assert.ok(Number.isInteger(index) && index >= 0 && index < 3);
        lifecycle.phase = 'sending-aggregate';
        lifecycle.planned = 'ordered aggregate delivery and clean close';
        sendAggregate(socket, index).then(
          () => { lifecycle.phase = 'aggregate-sent-awaiting-close'; },
          error => failSocket(socket, error));
      } else if (request.url === '/dispose') {
        lifecycle.phase = 'streaming-to-blocked-renderer';
        lifecycle.planned =
          'intentional renderer destroy with unread bytes, then prompt peer close';
        socketClosed.set(socket, new Promise(resolve => socket.once('close', resolve)));
        disposeUpgrade(socket);
        sendUntilDisposed(socket).then(
          () => { lifecycle.phase = 'dispose-stream-unexpectedly-completed'; },
          error => failSocket(socket, error));
      } else {
        throw new Error(`Unexpected WebSocket path: ${request.url}`);
      }
    } catch (error) { failSocket(socket, error); }
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const baseUrl = `ws://127.0.0.1:${server.address().port}`;

  const preload = path.join(temporary, 'preload.js');
  fs.writeFileSync(preload,
    "const{contextBridge,ipcRenderer}=require('electron');contextBridge.exposeInMainWorld('pressure',{report:(name,value)=>ipcRenderer.send('ws-pressure:'+name,value)});");

  protocol.handle('mini-ws-pressure', request => {
    const requestedUrl = new URL(request.url);
    const route = requestedUrl.pathname;
    if (route === '/flow') {
      const independentFlow = requestedUrl.searchParams.has('short');
      const blockMilliseconds = independentFlow ? 750 : 6000;
      const socketUrl = `${baseUrl}${independentFlow ? '/flow-independent' : '/flow'}`;
      return page(`
      const socket = new WebSocket(${JSON.stringify(socketUrl)});
      socket.binaryType = 'arraybuffer';
      let state = 'large', empty = 0, burst = 0;
      let completed;
      socket.onopen = () => {
        pressure.report('flow-blocked', true);
        const until = performance.now() + ${blockMilliseconds};
        while (performance.now() < until) {}
      };
      socket.onmessage = event => {
        if (state === 'large') {
          const bytes = new Uint8Array(event.data);
          if (bytes.length !== ${largeMessageBytes} || bytes[0] !== 0 || bytes[250] !== 250 || bytes[251] !== 0 || bytes[bytes.length - 1] !== (bytes.length - 1) % 251)
            return pressure.report('flow', { error: 'large fragmented binary mismatch' });
          state = 'empty';
        } else if (state === 'empty' && event.data === '') {
          if (++empty > ${flowEmptyMessages})
            return pressure.report('flow', { error: 'too many empty messages', empty });
        } else if ((state === 'empty' || state === 'burst') && event.data instanceof ArrayBuffer) {
          if (empty !== ${flowEmptyMessages})
            return pressure.report('flow', { error: 'burst overtook empty messages', empty, burst });
          state = 'burst';
          const bytes = new Uint8Array(event.data);
          const sequence = new DataView(event.data).getUint32(0);
          const fill = burst & 0xff;
          if (bytes.length !== ${flowBurstMessageBytes} || sequence !== burst || bytes[4] !== fill || bytes[bytes.length - 1] !== fill)
            return pressure.report('flow', { error: 'burst payload/order mismatch', sequence, burst });
          ++burst;
        } else if (event.data === 'flow-done' && state === 'burst' && burst === ${flowBurstMessages}) {
          completed = { empty, burst, state };
          socket.close(1000);
        } else {
          pressure.report('flow', { error: 'message order changed', empty, burst, state, value: String(event.data) });
        }
      };
      socket.onerror = () => pressure.report('flow', { error: 'transport error' });
      socket.onclose = event => {
        if (!completed || !event.wasClean || event.code !== 1000)
          return pressure.report('flow', { error: 'incomplete flow close', code: event.code, clean: event.wasClean });
        pressure.report('flow', completed);
      };
    `);
    }
    if (route === '/aggregate') return page(`
      const complete = new Set();
      const marked = new Set();
      const closed = new Set();
      for (let index = 0; index < 3; ++index) {
        const socket = new WebSocket(${JSON.stringify(`${baseUrl}/aggregate/`)} + index);
        socket.binaryType = 'arraybuffer';
        socket.onmessage = event => {
          if (event.data instanceof ArrayBuffer) {
            const bytes = new Uint8Array(event.data);
            if (bytes.length !== ${aggregateMessageBytes} || bytes[0] !== 0x40 + index || bytes[bytes.length - 1] !== 0x40 + index)
              return pressure.report('aggregate', { error: 'aggregate payload mismatch', index });
            complete.add(index);
          } else if (event.data === 'aggregate-' + index && complete.has(index)) {
            marked.add(index);
            socket.close(1000);
            // Report only after the peer has acknowledged every clean close;
            // navigating on the last message can reset an unfinished handshake.
          } else {
            pressure.report('aggregate', { error: 'aggregate order changed', index });
          }
        };
        socket.onerror = () => pressure.report('aggregate', { error: 'transport error', index });
        socket.onclose = event => {
          if (!marked.has(index) || !event.wasClean || event.code !== 1000)
            return pressure.report('aggregate', { error: 'incomplete aggregate close', index, code: event.code, clean: event.wasClean });
          closed.add(index);
          if (closed.size === 3) pressure.report('aggregate', { complete: [...closed].sort() });
        };
      }
      const until = performance.now() + 750; while (performance.now() < until) {}
    `);
    if (route === '/dispose') return page(`
      const socket = new WebSocket(${JSON.stringify(`${baseUrl}/dispose`)});
      socket.binaryType = 'arraybuffer';
      socket.onopen = () => { const until = performance.now() + 15000; while (performance.now() < until) {} };
    `);
    return new Response('not found', { status: 404 });
  });

  const webPreferences = { preload, sandbox: true, contextIsolation: true, nodeIntegration: false };
  let window = new BrowserWindow({ show: false, webPreferences });
  let blockedPromise = resultFromPage('flow-blocked');
  let resultPromise = resultFromPage('flow');
  const flowLoad = window.loadURL('mini-ws-pressure://app/flow');
  await blockedPromise;
  assert.ok(pendingFlowStarts.length, 'blocked flow did not reach the WebSocket server');
  pendingFlowStarts.shift()();
  await flowLoad;
  const flow = await resultPromise;
  assert.deepEqual(flow, { empty: flowEmptyMessages, burst: flowBurstMessages, state: 'burst' });
  const firstProgress = flowProgress[0];
  await firstProgress.ready;
  assert.equal(firstProgress.completed, flowProducerWrites,
    'flow producer did not complete after Blink resumed');
  assert.equal(firstProgress.snapshots.length, 2);
  assert.ok(firstProgress.snapshots[0] < flowProducerWrites,
    'blocked Blink consumer did not bound producer progress');
  assert.equal(firstProgress.snapshots[1], firstProgress.snapshots[0],
    'producer continued draining while Blink remained blocked');

  resultPromise = resultFromPage('aggregate');
  await window.loadURL('mini-ws-pressure://app/aggregate');
  const aggregate = await resultPromise;
  assert.deepEqual(aggregate, { complete: [0, 1, 2] });
  window.destroy();

  window = new BrowserWindow({ show: false, webPreferences });
  await window.loadURL('mini-ws-pressure://app/dispose');
  const disposeSocket = await disposeUpgraded;
  await new Promise(resolve => setTimeout(resolve, 250));
  // Default-session windows have independent renderer budgets. A blocked peer
  // must not monopolize receive credit for a responsive renderer.
  const independent = new BrowserWindow({ show: false, webPreferences });
  const independentStarted = Date.now();
  blockedPromise = resultFromPage('flow-blocked');
  const independentResult = resultFromPage('flow');
  const independentLoad = independent.loadURL('mini-ws-pressure://app/flow?short=1');
  await blockedPromise;
  assert.ok(pendingFlowStarts.length, 'independent flow did not reach the WebSocket server');
  pendingFlowStarts.shift()();
  await independentLoad;
  assert.deepEqual(await independentResult,
    { empty: flowEmptyMessages, burst: flowBurstMessages, state: 'burst' });
  assert.ok(Date.now() - independentStarted < 10000,
    'a paused renderer starved another window sharing the same Session');
  independent.destroy();
  const disposeLifecycle = socketLifecycle.get(disposeSocket);
  assert.ok(disposeLifecycle, 'dispose socket lifecycle was not tracked');
  disposeLifecycle.intentionallyAbandoned = true;
  disposeLifecycle.phase = 'renderer-destroy-requested';
  const destroyStarted = Date.now();
  window.destroy();
  assert.ok(Date.now() - destroyStarted < 3000, 'destroy blocked behind paused WebSocket receive credit');
  await Promise.race([
    socketClosed.get(disposeSocket),
    new Promise((_, reject) => setTimeout(
      () => reject(new Error('paused WebSocket did not close after renderer disposal')), 3000)),
  ]);
  finish();
}).catch(finish);
