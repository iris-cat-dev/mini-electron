#!/usr/bin/env node
'use strict';

const childProcess = require('node:child_process');

function run(argv = process.argv.slice(2)) {
  const executable = require('./');
  const parent = process;
  const spawn = childProcess.spawn;
  const child = spawn(executable, argv, {
    stdio: 'inherit',
    windowsHide: false,
    env: parent.env,
  });
  let closed = false;
  let forwardedSignal = null;
  const signals = ['SIGINT', 'SIGTERM', 'SIGHUP', 'SIGBREAK', 'SIGUSR1', 'SIGUSR2'];
  const handlers = new Map();

  function removeHandlers() {
    for (const [signal, handler] of handlers) parent.removeListener(signal, handler);
  }

  for (const signal of signals) {
    const handler = () => {
      if (closed) return;
      forwardedSignal = signal;
      try {
        child.kill(signal);
      } catch (error) {
        console.error(`mini-electron could not forward ${signal}: ${error.message}`);
        parent.exitCode = 1;
      }
    };
    try {
      parent.on(signal, handler);
      handlers.set(signal, handler);
    } catch {
      // Some signals do not exist on every supported operating system.
    }
  }

  child.once('error', (error) => {
    closed = true;
    removeHandlers();
    console.error(`mini-electron failed to start ${executable}: ${error.message}`);
    parent.exitCode = 1;
  });
  child.once('close', (code, signal) => {
    if (closed) return;
    closed = true;
    removeHandlers();
    if (code !== null) {
      parent.exitCode = code;
      return;
    }
    const terminationSignal = signal || forwardedSignal;
    if (!terminationSignal) {
      parent.exitCode = 1;
      return;
    }
    try {
      parent.kill(parent.pid, terminationSignal);
    } catch (error) {
      console.error(`mini-electron exited with ${terminationSignal}: ${error.message}`);
      parent.exitCode = 1;
    }
  });
  return child;
}

if (require.main === module) run();
