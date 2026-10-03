'use strict';

const { NativeImage } = process._linkedBinding('electron_common_nativeImage');
const nativeToJPEG = NativeImage.prototype.toJPEG;

NativeImage.prototype.toJPEG = function (quality = 100) {
  return nativeToJPEG.call(this, { quality });
};

exports.NativeImage = NativeImage;
