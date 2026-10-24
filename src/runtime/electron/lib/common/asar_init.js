;(function () {
    return function (process, require, asarSource) {
        
        
        try {
            // Monkey-patch the fs module.
            require('ELECTRON_ASAR').wrapFsWithAsar(require('fs'));
            
            // Make graceful-fs work with asar.
            var source = process.binding('natives');
            source['original-fs'] = source.fs;
            
            source['fs'] = `
var nativeModule = new process.NativeModule('original-fs')
nativeModule.cache()
nativeModule.compile()
var asar = require('ELECTRON_ASAR')
asar.wrapFsWithAsar(nativeModule.exports)
module.exports = nativeModule.exports`;
            
        } catch(e) {
            mini_electron_console_log("asar_init.js fail: " + e);
        }
    }
})()

