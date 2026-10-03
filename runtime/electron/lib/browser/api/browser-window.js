'use strict';

process._linkedBinding('electron_browser_web_contents');
const { EventEmitter } = require('events');
const BrowserWindow = process._linkedBinding('electron_browser_browserwindow').BrowserWindow;
Object.setPrototypeOf(BrowserWindow.prototype, EventEmitter.prototype);
require('./web-contents');

Object.defineProperty(BrowserWindow.prototype, 'webContents', {
  configurable: true,
  get() {
    const contents = this._getWebContents();
    if (contents) contents._init();
    return contents;
  }
});

BrowserWindow.prototype.setTouchBar = function () {};
BrowserWindow.prototype.setTitle = function (title) {
  if (typeof title === 'string') this._setTitle(title);
};

Object.assign(BrowserWindow.prototype, {
  loadURL(...args) { return this.webContents.loadURL(...args); },
  loadFile(...args) { return this.webContents.loadFile(...args); },
  getURL() { return this.webContents.getURL(); },
  reload(...args) { return this.webContents.reload(...args); },
  send(...args) { return this.webContents.send(...args); },
  showDefinitionForSelection(...args) {
    return this.webContents.showDefinitionForSelection(...args);
  },
  capturePage(...args) { return this.webContents.capturePage(...args); }
});

module.exports = BrowserWindow;
