'use strict';

process.atomBinding = function (name) {
  try {
    return process._linkedBinding(`electron_${process.type}_${name}`);
  } catch (error) {
    if (!/No such module/.test(error.message)) throw error;
    return process._linkedBinding(`electron_common_${name}`);
  }
};

if (process.platform === 'win32' && __dirname.includes('\\Program Files\\WindowsApps\\')) {
  process.windowsStore = true;
}
