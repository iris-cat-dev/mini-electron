'use strict';

const { EventEmitter } = require('events');
const { Notification } = process._linkedBinding('electron_browser_notification');

Object.setPrototypeOf(Notification.prototype, EventEmitter.prototype);

module.exports = Notification;
module.exports.Notification = Notification;
