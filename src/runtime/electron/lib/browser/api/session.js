const EventEmitter = require('events').EventEmitter;
const sessionBinding = process._linkedBinding('electron_browser_session');
const Session = sessionBinding.Session;

Object.setPrototypeOf(Session.prototype, EventEmitter.prototype);
// Initialize the native constructor used by the lazy webRequest property.
process._linkedBinding('electron_browser_webrequest');


// Do not instantiate the default Session while Electron's exports are loading:
// App must first select its final userData directory and call SessionMgr::setRootDir.
Object.defineProperty(Session, 'defaultSession', {
  configurable: false,
  enumerable: true,
  get() {
    return Session.fromPartition('');
  },
});

const downloaditemBinding = process._linkedBinding('electron_browser_downloaditem');
const DownloadItem = downloaditemBinding.DownloadItem;
Object.setPrototypeOf(DownloadItem.prototype, EventEmitter.prototype);

exports.session = Session;