'use strict';

// Run with the native runtime, not Node, in both transport modes:
// mini-electron test/websocket-frame-retirement-smoke.js
// mini-electron --single-process test/websocket-frame-retirement-smoke.js
// Unread WebSocketStream receive queues keep native credits outstanding across
// real document commits and iframe detachments; no private broker hooks are used.
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const http = require('node:http');
const { app, BrowserWindow } = require('electron');

const messageBytes = 12 * 1024 * 1024 + 19;
const payload = Buffer.alloc(messageBytes, 0x6d);
const sessions = new Map();
const connections = new Set();
const windows = new Set();
let server;
let origin;
let finished = false;

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  for (const window of windows) {
    if (!window.isDestroyed()) window.destroy();
  }
  for (const socket of connections) socket.destroy();
  server?.close();
  if (error) console.error(error.stack || error);
  else console.log('PASS: queued oversize WebSocket credits retire on document commit and iframe detach without starving another document or renderer');
  app.exit(error ? 1 : 0);
}

const deadline = setTimeout(
  () => finish(new Error('WebSocket frame retirement smoke timed out')), 60000);

function bounded(promise, label, milliseconds = 8000) {
  let timer;
  return Promise.race([
    promise,
    new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(`Timed out: ${label}`)), milliseconds);
    }),
  ]).finally(() => clearTimeout(timer));
}

function frame(opcode, body, final = true) {
  const header = Buffer.alloc(body.length < 126 ? 2 : body.length <= 0xffff ? 4 : 10);
  header[0] = (final ? 0x80 : 0) | opcode;
  if (header.length === 2) header[1] = body.length;
  else if (header.length === 4) {
    header[1] = 126;
    header.writeUInt16BE(body.length, 2);
  } else {
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(body.length), 2);
  }
  return Buffer.concat([header, body]);
}

async function writeFrame(session, value) {
  if (session.socket.write(value)) return;
  await new Promise((resolve, reject) => {
    const socket = session.socket;
    const cleanup = () => {
      socket.off('drain', drained);
      socket.off('close', closed);
      socket.off('error', failed);
    };
    const drained = () => { cleanup(); resolve(); };
    const closed = () => { cleanup(); reject(new Error('Retired peer closed')); };
    const failed = error => { cleanup(); reject(error); };
    socket.once('drain', drained);
    socket.once('close', closed);
    socket.once('error', failed);
  });
}

async function sendMessages(session, held) {
  const split = 6 * 1024 * 1024 + 7;
  await writeFrame(session, frame(2, payload.subarray(0, split), false));
  await writeFrame(session, frame(0, payload.subarray(split)));
  session.primaryDelivered();
  if (held) {
    // A second bounded message sits behind the unread oversize message.
    await writeFrame(session, frame(2, Buffer.alloc(1024 * 1024, 0x37)));
  }
  await writeFrame(session, frame(1, Buffer.from(`done:${session.tag}`)));
}

function document(source) {
  return `<!doctype html><meta charset="utf-8"><body><script>${source}</script>`;
}

function holdSource(tag) {
  return `window.holdReady = (async () => {
    if (typeof WebSocketStream !== 'function') throw new Error('WebSocketStream unavailable');
    window.heldStream = new WebSocketStream(${JSON.stringify(`ws://${new URL(origin).host}/socket/hold/${tag}`)});
    heldStream.closed.catch(() => {});
    window.heldOpen = await heldStream.opened;
    // Never acquire a reader: native backpressure retains the received payload.
    await new Promise(resolve => setTimeout(resolve, 350));
    return true;
  })();`;
}

function probeSource(tag) {
  return `window.probeResult = new Promise((resolve, reject) => {
    const socket = new WebSocket(${JSON.stringify(`ws://${new URL(origin).host}/socket/probe/${tag}`)});
    socket.binaryType = 'arraybuffer';
    let binary = false;
    socket.onerror = () => reject(new Error('New document WebSocket failed'));
    socket.onmessage = event => {
      if (!binary) {
        const bytes = new Uint8Array(event.data);
        if (bytes.length !== ${messageBytes} || bytes[0] !== 0x6d || bytes[bytes.length - 1] !== 0x6d)
          return reject(new Error('New document received stale or incomplete data'));
        binary = true;
      } else if (event.data === ${JSON.stringify(`done:${tag}`)}) {
        resolve({ bytes: ${messageBytes}, marker: event.data });
        socket.close();
      } else reject(new Error('New document received a retired socket event'));
    };
  });`;
}

async function heldSession(window, tag) {
  await window.loadURL(`${origin}/hold/${tag}`);
  assert.equal(await bounded(window.webContents.executeJavaScript('window.holdReady'), `${tag} unread stream`), true);
  const session = sessions.get(tag);
  assert.ok(session, `${tag} did not open its socket`);
  await bounded(session.primarySent, `${tag} oversize payload send`);
  return session;
}

async function probe(window, tag, load = true) {
  if (load) await window.loadURL(`${origin}/probe/${tag}`);
  else await window.webContents.executeJavaScript(probeSource(tag));
  const result = await bounded(window.webContents.executeJavaScript('window.probeResult'), `${tag} fresh receive credits`);
  assert.deepEqual(result, { bytes: messageBytes, marker: `done:${tag}` });
}

app.whenReady().then(async () => {
  server = http.createServer((request, response) => {
    const [, operation, tag] = new URL(request.url, origin).pathname.split('/');
    response.writeHead(200, { 'content-type': 'text/html' });
    response.end(document(operation === 'hold' ? holdSource(tag) : operation === 'probe' ? probeSource(tag) : ''));
  });
  server.on('connection', socket => {
    connections.add(socket);
    socket.once('close', () => connections.delete(socket));
  });
  server.on('upgrade', (request, socket) => {
    const [, route, mode, tag] = new URL(request.url, origin).pathname.split('/');
    if (route !== 'socket' || !['hold', 'probe'].includes(mode) || !tag || sessions.has(tag)) {
      socket.destroy();
      finish(new Error(`Unexpected upgrade: ${request.url}`));
      return;
    }
    const accept = crypto.createHash('sha1')
      .update(request.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
      .digest('base64');
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
    const session = { tag, socket };
    session.closed = new Promise(resolve => socket.once('close', resolve));
    session.primarySent = new Promise(resolve => { session.primaryDelivered = resolve; });
    sessions.set(tag, session);
    // The test does not wait for a graceful client close. Drain client bytes so
    // its close frame cannot block retirement or become an unrelated timeout.
    socket.on('data', () => {});
    // HTTP upgrades are half-open: a renderer FIN does not emit `close` until
    // this peer closes its write half, even though the native socket is gone.
    socket.once('end', () => socket.end());
    socket.on('error', error => { if (mode !== 'hold' && !finished) finish(error); });
    sendMessages(session, mode === 'hold').catch(error => {
      if (!socket.destroyed && !finished) finish(error);
    });
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  origin = `http://127.0.0.1:${server.address().port}`;
  const createWindow = () => {
    const window = new BrowserWindow({ show: false, webPreferences: {
      sandbox: true, contextIsolation: true, nodeIntegration: false,
    } });
    windows.add(window);
    return window;
  };
  const window = createWindow();
  const independent = createWindow();
  const survivor = await heldSession(independent, 'independent-renderer');

  // Repeated commits reuse a frame token with new browser generations. Each
  // fresh oversize probe must progress even though its predecessor never read.
  for (let index = 0; index < 4; ++index) {
    const old = await heldSession(window, `navigation-${index}`);
    await probe(window, `after-navigation-${index}`);
    await bounded(old.closed, `navigation-${index} native socket retirement`);
    assert.equal(survivor.socket.destroyed, false, 'A different WebContents was retired');
  }

  await window.loadURL(`${origin}/container`);
  for (let index = 0; index < 4; ++index) {
    const tag = `iframe-${index}`;
    const created = await bounded(window.webContents.executeJavaScript(`new Promise((resolve, reject) => {
      window.holder = document.createElement('iframe');
      holder.onload = () => holder.contentWindow.holdReady.then(resolve, reject);
      holder.src = ${JSON.stringify(`${origin}/hold/${tag}`)};
      document.body.append(holder);
    })`), `${tag} unread stream`);
    assert.equal(created, true);
    const old = sessions.get(tag);
    assert.ok(old, `${tag} did not open its socket`);
    await bounded(old.primarySent, `${tag} oversize payload send`);
    await window.webContents.executeJavaScript('holder.remove(); window.holder = null; true');
    await probe(window, `after-iframe-${index}`, false);
    await bounded(old.closed, `${tag} native socket retirement`);
    assert.equal(survivor.socket.destroyed, false, 'A different renderer lost its queued socket');
  }

  // A separate WebContents held the same eventId values throughout retirement.
  // Its own credits must still be releasable, with its original data intact.
  const resumed = await bounded(independent.webContents.executeJavaScript(`(async () => {
    const reader = heldOpen.readable.getReader();
    const first = new Uint8Array((await reader.read()).value);
    const queued = new Uint8Array((await reader.read()).value);
    const marker = (await reader.read()).value;
    const result = { bytes: first.length, first: first[0], last: first[first.length - 1],
      queued: queued.length, queuedByte: queued[0], marker };
    heldStream.close();
    return result;
  })()`), 'independent renderer resumes its exact pending credits');
  assert.deepEqual(resumed, { bytes: messageBytes, first: 0x6d, last: 0x6d,
    queued: 1024 * 1024, queuedByte: 0x37, marker: 'done:independent-renderer' });
  finish();
}).catch(finish);
