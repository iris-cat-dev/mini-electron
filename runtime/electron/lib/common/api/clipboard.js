'use strict';

const Clipboard = process._linkedBinding('electron_common_clipboard').Clipboard;

Clipboard.prototype.readText = function (type = '') {
  return this._readText(type);
};

Clipboard.prototype.readImage = function (type = '') {
  return this._readImage(type);
};

Clipboard.prototype.writeText = function (text, type = '') {
  return this._writeText(text, type);
};

Clipboard.prototype.writeImage = function (image, type = '') {
  return this._writeImage(image ?? null, type);
};

Clipboard.prototype.clear = function (type = '') {
  return this._clear(type);
};

// Preserve Electron's historical aliases while keeping the actual native
// operations as the single source of behavior.
Clipboard.prototype.readHtml = Clipboard.prototype.readHTML;
Clipboard.prototype.writeHtml = Clipboard.prototype.writeHTML;
Clipboard.prototype.readRtf = Clipboard.prototype.readRTF;
Clipboard.prototype.writeRtf = Clipboard.prototype.writeRTF;

module.exports = new Clipboard();
