'use strict';

const electron = require('electron');
const { EventEmitter } = require('events');

electron.app = require('./api/app');
electron.ipcMain = require('./api/ipc-main');
electron.BrowserWindow = require('./api/browser-window');
electron.BrowserView = require('./api/browser-view');
electron.webContents = require('./api/web-contents');
electron.session = require('./api/session').session;
electron.app.commandLine = require('./api/command-line');
electron.MenuItem = require('./api/menu-item');
electron.Menu = require('./api/menu');
electron.dialog = require('./api/dialog').dialog;
electron.net = require('./api/net').net;
electron.protocol = require('./api/protocol').protocol;
electron.nativeImage = require('../common/api/native-image').NativeImage;
electron.clipboard = require('../common/api/clipboard');
electron.nativeTheme = require('./api/native-theme');
electron.Notification = require('./api/notification');
electron.Tray = require('./api/tray').Tray;
electron.shell = require('../common/api/shell').Shell;
electron.screen = require('../common/api/screen').Screen;
electron.safeStorage = require('./api/safe-storage');
electron.powerMonitor = require('./api/power-monitor');

if (process.platform === 'darwin') {
  const updater = process._linkedBinding('electron_browser_auto_updater').autoUpdater;
  for (const key of Reflect.ownKeys(EventEmitter.prototype)) {
    if (key !== 'constructor' && !(key in updater)) {
      Object.defineProperty(updater, key, Object.getOwnPropertyDescriptor(EventEmitter.prototype, key));
    }
  }
  electron.autoUpdater = updater;
} else {
  // NsisUpdater performs Windows updates and emits its handoff event here
  // before calling app.quit(). Keep the same event source for OMP listeners.
  electron.autoUpdater = new EventEmitter();
  electron.utilityProcess = require('./api/utility-process').utilityProcess;
  require('./api/parent-port');
  electron.webFrameMain = require('./api/web-frame-main').webFrameMain;
  electron.MessageChannelMain = require('./api/message-channel-main').MessageChannelMain;
}

module.exports = electron;
