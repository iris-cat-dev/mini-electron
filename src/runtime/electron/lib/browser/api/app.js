'use strict';

const { EventEmitter } = require('events');
let app;
if (process.platform === 'darwin') {
  app = process._linkedBinding('electron_browser_mac_host').app;
  for (const key of Reflect.ownKeys(EventEmitter.prototype)) {
    if (key !== 'constructor' && !(key in app)) {
      Object.defineProperty(app, key, Object.getOwnPropertyDescriptor(EventEmitter.prototype, key));
    }
  }
} else {
  const App = process._linkedBinding('electron_browser_app').App;
  Object.setPrototypeOf(App.prototype, EventEmitter.prototype);
  app = new App();
}

app.whenReady = function () {
  if (this.isReady()) return Promise.resolve();
  return new Promise(resolve => this.once('ready', () => resolve()));
};

let applicationPath = typeof app.getAppPath === 'function' ? app.getAppPath() : null;
app.getAppPath = function () { return applicationPath; };
app.setAppPath = function (value) {
  this._setAppPath(value);
  applicationPath = value;
};
app.getApplicationMenu = function () {
  return require('./menu').getApplicationMenu();
};
if (typeof app._relaunch === 'function') {
  app.relaunch = function (options = {}) { this._relaunch(options); };
}

module.exports = app;
