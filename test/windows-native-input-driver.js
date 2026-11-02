'use strict';
const { execFileSync, spawn } = require('node:child_process');
const path = require('node:path');
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));

async function startNativeInput(temporary, title, helperName = 'windows-native-input-helper.cs') {
  const compiler = path.join(process.env.WINDIR || 'C:\\Windows', 'Microsoft.NET', 'Framework64', 'v4.0.30319', 'csc.exe');
  const executable = path.join(temporary, 'native-input.exe');
  execFileSync(compiler, ['/nologo', '/target:exe', `/out:${executable}`,
    '/reference:System.Windows.Forms.dll', '/reference:System.Drawing.dll', '/reference:System.Web.Extensions.dll',
    path.join(__dirname, helperName)], { stdio: 'inherit', timeout: 30000, windowsHide: true });
  const child = spawn(executable, [], { stdio: ['pipe', 'pipe', 'inherit'], windowsHide: true });
  const pending = new Map();
  let sequence = 0, stdout = '', closing = false;
  let readyResolve, readyReject;
  const ready = new Promise((resolve, reject) => { readyResolve = resolve; readyReject = reject; });
  const readyTimer = setTimeout(() => readyReject(new Error('Native input helper did not become ready')), 10000);
  const closed = new Promise(resolve => child.once('close', code => {
    clearTimeout(readyTimer);
    readyReject(new Error(`Native input helper exited (${code})`));
    for (const waiter of pending.values()) {
      clearTimeout(waiter.timer);
      waiter.reject(new Error(`Native input helper exited (${code})`));
    }
    pending.clear(); resolve(code);
  }));
  child.once('error', readyReject);
  child.stdout.setEncoding('utf8');
  child.stdout.on('data', chunk => {
    stdout += chunk;
    let newline;
    while ((newline = stdout.indexOf('\n')) !== -1) {
      const line = stdout.slice(0, newline).trim(); stdout = stdout.slice(newline + 1);
      if (line === 'NATIVE_READY') { clearTimeout(readyTimer); readyResolve(); }
      else if (line.startsWith('NATIVE_RESULT ')) {
        const message = JSON.parse(line.slice(14)), waiter = pending.get(message.id);
        if (!waiter) continue;
        pending.delete(message.id); clearTimeout(waiter.timer);
        if (message.error) waiter.reject(new Error(message.error)); else waiter.resolve(message.result);
      }
    }
  });
  function request(action, values = {}) {
    if (child.exitCode !== null || child.signalCode !== null) return Promise.reject(new Error('Native input helper is closed'));
    const id = ++sequence;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { pending.delete(id); reject(new Error(`Native helper ${action} timed out`)); }, 12000);
      pending.set(id, { resolve, reject, timer });
      child.stdin.write(JSON.stringify({ id, action, title, ...values }) + '\n', error => {
        if (!error || !pending.delete(id)) return;
        clearTimeout(timer); reject(error);
      });
    });
  }
  async function close() {
    if (closing) return;
    closing = true;
    let failure;
    try { if (child.exitCode === null && child.signalCode === null) await request('quit'); }
    catch (error) { failure = error; }
    child.stdin.end();
    const code = await Promise.race([closed, pause(3000).then(() => undefined)]);
    if (code === undefined) {
      child.kill(); await closed;
      failure = failure || new Error('Native input helper did not close and restore native state normally');
    } else if (code !== 0) failure = failure || new Error(`Native input helper exited (${code})`);
    if (failure) throw failure;
  }
  try {
    await ready;
    return { request, close };
  } catch (error) {
    try { await close(); } catch (cleanupError) { console.error(cleanupError); }
    throw error;
  }
}

module.exports = { startNativeInput };
