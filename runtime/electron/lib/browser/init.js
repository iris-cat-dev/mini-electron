'use strict';

const fs = require('node:fs');
const path = require('node:path');
const Module = require('node:module');
const { pathToFileURL } = require('node:url');

Object.defineProperty(process, 'type', { configurable: true, value: 'browser' });
process.resourcesPath ||= path.join(path.dirname(process.execPath), 'resources');
const packagedRoot = ['app.asar', 'app'].map(name => path.join(process.resourcesPath, name))
  .find(candidate => fs.existsSync(candidate.endsWith('.asar') ? candidate : path.join(candidate, 'package.json')));

if (!packagedRoot && (process.argv[1] === '--version' || process.argv[1] === '-v')) {
  process.stdout.write(`v${process.versions.miniElectron}\n`);
  process.exit(0);
} else if (!packagedRoot && process.argv[1] === '--abi') {
  process.stdout.write(`${process.versions.modules}\n`);
  process.exit(0);
} else if (!packagedRoot && (process.argv[1] === '--help' || process.argv[1] === '-h')) {
  process.stdout.write('Usage: electron [app-directory | main-script] [arguments]\n');
  process.exit(0);
} else {
  const apiKey = Symbol.for('mini-electron.main-api');
  const electron = globalThis[apiKey] = {};
  const names = [
    'app', 'ipcMain', 'BrowserWindow', 'BrowserView', 'webContents', 'session',
    'MenuItem', 'Menu', 'dialog', 'net', 'protocol', 'nativeImage', 'clipboard',
    'nativeTheme', 'Notification', 'Tray', 'shell', 'screen', 'safeStorage',
    'powerMonitor', 'autoUpdater', 'utilityProcess', 'webFrameMain', 'MessageChannelMain',
  ];
  const moduleSource = "const api = globalThis[Symbol.for('mini-electron.main-api')];\nmodule.exports = api;\n" +
    names.map(name => `module.exports.${name} = api.${name};`).join('\n');
  Module.registerHooks({
    resolve(specifier, context, nextResolve) {
      if (specifier === 'electron' || specifier === 'electron/main') {
        return { url: 'mini-electron:main', format: 'commonjs', shortCircuit: true };
      }
      return nextResolve(specifier, context);
    },
    load(url, context, nextLoad) {
      if (url === 'mini-electron:main') {
        return { format: 'commonjs', source: moduleSource, shortCircuit: true };
      }
      return nextLoad(url, context);
    },
  });
  require('../common/init');
  require('./electron');

  function resolveApplication() {
    let argument;
    for (let index = 1; index < process.argv.length; ++index) {
      const value = process.argv[index];
      if (value === '--') {
        argument = process.argv[index + 1];
        break;
      }
      if (value.startsWith('--app=')) {
        argument = value.slice(6);
        break;
      }
      if (!packagedRoot && !value.startsWith('-')) {
        argument = value;
        process.argv[index] = path.resolve(value);
        break;
      }
    }
    let root;
    let entry;
    let packaged = false;
    if (argument) {
      root = path.resolve(argument);
      if (!fs.statSync(root).isDirectory()) {
        entry = root;
        root = path.dirname(root);
      }
    } else {
      root = packagedRoot;
      packaged = Boolean(root);
      root ||= path.join(__dirname, '..', 'default_app');
    }
    const manifest = path.join(root, 'package.json');
    const metadata = fs.existsSync(manifest) ? JSON.parse(fs.readFileSync(manifest, 'utf8')) : {};
    return { root, entry: entry || path.resolve(root, metadata.main || 'index.js'), metadata, packaged };
  }

  async function startApplication() {
    const application = resolveApplication();
    const { app } = electron;
    if (!application.packaged) process.defaultApp = true;
    else delete process.defaultApp;
    app._setIsPackaged(application.packaged);
    app.setName(application.metadata.productName || application.metadata.name || 'mini-electron');
    app.setVersion(application.metadata.version || process.versions.miniElectron);
    app.setAppPath(application.root);
    if (application.metadata.v8Flags) require('node:v8').setFlagsFromString(application.metadata.v8Flags);
    if (application.metadata.type === 'module' || path.extname(application.entry) === '.mjs') {
      await import(pathToFileURL(application.entry).href);
    } else {
      Module._load(application.entry, null, true);
    }
    setImmediate(() => {
      app.emit('will-finish-launching', {});
      app._setIsReady();
      app.emit('ready', {});
    });
  }

  startApplication().catch(error => {
    console.error(error.stack || error);
    electron.app.exit(1);
  });
}
