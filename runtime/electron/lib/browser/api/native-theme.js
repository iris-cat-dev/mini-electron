'use strict';

const { EventEmitter } = require('events');
const { NativeTheme } = process._linkedBinding('electron_browser_native_theme');

Object.setPrototypeOf(NativeTheme.prototype, EventEmitter.prototype);

module.exports = new NativeTheme();
