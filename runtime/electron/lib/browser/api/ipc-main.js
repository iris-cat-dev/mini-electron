const EventEmitter = require('events').EventEmitter;

function createIpcMain() {
    var ipcMain = new EventEmitter();
    ipcMain.m_invokeHandlers = new Map();
    
    ipcMain.__origOn__ = ipcMain.on;
    ipcMain.on = function (channel, callback) {
        let cbStub = function (event, ...args) {
            if (event.innnerChannel == 'ipc-render-invoke')
                return;
            callback(event, ...args);
        }
        return ipcMain.__origOn__(channel, cbStub);
    }
    
    ipcMain.handle = function (channel, listener) { // 这里的channel是用户定义的channel
        if (ipcMain.m_invokeHandlers.has(channel)) {
            throw new Error("Attempted to register a second handler for " + channel);
        }
        if (typeof listener !== 'function') {
            throw new TypeError("Expected handler to be a function, but found type " + (typeof listener));
        }
        const invokeHandler = async (event, requestId, ...args) => {
            if (event.innnerChannel !== 'ipc-render-invoke')
                return;
            const replyChannel = 'ipc-main-handle-reply-' + requestId;
            try {
                const result = await listener(event, ...args);
                event.sender.send(replyChannel, { result });
            } catch (error) {
                const message = error instanceof Error ? error.message : String(error);
                event.sender.send(replyChannel, { error: message });
            }
        };
        ipcMain.m_invokeHandlers.set(channel, invokeHandler);
        ipcMain.__origOn__(channel, invokeHandler);
    }
    
    ipcMain.removeHandler = function (channel /*string*/) {
        if (ipcMain.m_invokeHandlers.has(channel)) {
            let listener = ipcMain.m_invokeHandlers.get(channel);
            ipcMain.removeListener(channel, listener);
            ipcMain.m_invokeHandlers.delete(channel);
        }
    }
    
    return ipcMain;
}

var ipcMain = createIpcMain();
ipcMain.createIpcMain = createIpcMain;

module.exports = ipcMain;

// Do not throw exception when channel name is "error".
module.exports.on('error', () => {})
