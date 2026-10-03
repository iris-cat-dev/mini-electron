'use strict';

const binding = process._linkedBinding('electron_browser_web_contents');
const WebContents = binding.WebContents;
const { EventEmitter } = require('events');
const { format } = require('url');
const path = require('path');
const electron = require('electron');
const app = electron.app;
const ipcMain = electron.ipcMain;

Object.setPrototypeOf(WebContents.prototype, EventEmitter.prototype);

const popupWindows = new Map();
const guestOwners = new Map();
const guestInstances = new Map();
const devToolsWindows = new WeakMap();

function createReplyEvent(contents, nativeEvent, requestId, innerChannel) {
  let prevented = false;
  let responded = false;
  const respond = value => {
    if (!responded) {
      responded = true;
      contents._respond(requestId, { value });
    }
  };
  const event = {
    sender: contents,
    frameId: nativeEvent && nativeEvent.frameId,
    innnerChannel: innerChannel,
    preventDefault() { prevented = true; },
    get defaultPrevented() { return prevented; },
    get responded() { return responded; },
    reply(channel, ...args) { contents.send(channel, ...args); },
    sendReply(value) { respond(value); }
  };
  Object.defineProperty(event, 'returnValue', {
    set(value) { respond(value); }
  });
  return event;
}

function dispatchRendererEvent(contents, nativeEvent, type, payload, requestId) {
  payload = payload || {};
  const args = Array.isArray(payload.args) ? payload.args : [];
  if (type === 'ipc-invoke') {
    const event = createReplyEvent(contents, nativeEvent, requestId, 'ipc-render-invoke');
    const localHandler = contents.ipc.m_invokeHandlers.get(payload.channel);
    const handler = localHandler || ipcMain.m_invokeHandlers.get(payload.channel);
    if (!handler) {
      contents._respond(requestId, { value: undefined },
        `No handler registered for '${payload.channel}'`);
      return;
    }
    Promise.resolve()
      .then(() => handler(event, ...args))
      .then(result => contents._respond(requestId, { value: result }, ''))
      .catch(error => contents._respond(
        requestId,
        { value: undefined },
        error instanceof Error ? error.message : String(error)));
    return;
  }
  if (type === 'ipc-message' || type === 'ipc-message-sync') {
    const event = createReplyEvent(contents, nativeEvent, requestId, type);
    ipcMain.emit(payload.channel, event, ...args);
    contents.ipc.emit(payload.channel, event, ...args);
    if (type === 'ipc-message-sync' && requestId && !event.responded) {
      // A listener that assigns returnValue responds synchronously. If none did,
      // complete the request rather than leaving the sandbox renderer blocked.
      contents._respond(requestId, { value: undefined });
    }
    return;
  }
  if (type === 'cdp-message') {
    if (contents.debugger)
      contents.debugger.emit('message', {}, payload.method, payload.params || {});
    return;
  }
  if (type === 'cdp-detached') {
    if (contents.debugger)
      contents.debugger.emit('detach', {}, payload.reason || 'target closed');
    return;
  }
  if (type === 'preload-error') {
    const error = new Error(payload.error || 'Preload execution failed');
    if (payload.stack) error.stack = payload.stack;
    contents.emit('preload-error', nativeEvent || {}, payload.file, error);
    return;
  }
  if (type === 'file-chooser') {
    showFileChooser(contents, payload, requestId);
    return;
  }
  if (type === 'guest-create') {
    createGuest(contents, payload, requestId);
    return;
  }
  if (type === 'guest-bounds') {
    updateGuestBounds(contents, payload);
    return;
  }
  if (type === 'guest-destroy') {
    destroyGuest(contents, payload);
    return;
  }
  if (type === 'guest-command') {
    commandGuest(contents, payload);
    return;
  }
  if (type === 'guest-input') {
    const guestId = Number(payload.guestContentsId);
    const owner = guestOwners.get(guestId);
    const guest = WebContents.fromId(guestId);
    if (owner && owner.hostId === contents.id &&
        guest && !guest.isDestroyed() && payload.event) {
      guest.sendInputEvent(payload.event);
    }
    return;
  }
  if (type === 'window-open') {
    if (payload.action === 'allow') {
      const BrowserWindow = electron.BrowserWindow;
      const options = payload.overrideBrowserWindowOptions &&
        typeof payload.overrideBrowserWindowOptions === 'object'
        ? { ...payload.overrideBrowserWindowOptions } : {};
      const webPreferences = options.webPreferences &&
        typeof options.webPreferences === 'object'
        ? { ...options.webPreferences } : {};
      if (typeof webPreferences.partition !== 'string') {
        webPreferences.partition = contents.session.getPartition();
      }
      options.webPreferences = webPreferences;
      const popup = new BrowserWindow(options);
      const popupContents = popup.webContents;
      popupWindows.set(popupContents.id, popup);
      popup.once('closed', () => popupWindows.delete(popupContents.id));
      contents.emit('did-create-window', popup, payload);
      if (payload.url) void popup.loadURL(payload.url).catch(error => {
        popupContents.emit('did-fail-load', {}, -2, error.message, payload.url, true);
      });
      if (options.show !== false) popup.show();
    }
    return;
  }
  contents.emit(type, nativeEvent || {}, payload);
}

class WebContentsDebugger extends EventEmitter {
  constructor(contents) {
    super();
    this.contents = contents;
  }

  isAttached() { return this.contents._debuggerIsAttached(); }

  attach(protocolVersion = '1.3') {
    this.contents._debuggerAttach(protocolVersion);
  }

  detach() {
    this.contents._debuggerDetach();
  }

  sendCommand(method, commandParams = {}) {
    return this.contents._debuggerSendCommand(method, commandParams);
  }
}

WebContents.prototype._init = function () {
  if (this.m_isInited) return;
  Object.defineProperty(this, 'm_isInited', { value: true });
  this.setMaxListeners(0);
  this.on('_download-save-dialog', (item, directory) => {
    const owner = electron.BrowserWindow.fromWebContents(this);
    const options = { defaultPath: path.join(directory, item.getFilename()) };
    const selected = owner
      ? electron.dialog.showSaveDialogSync(owner, options)
      : electron.dialog.showSaveDialogSync(options);
    if (selected) item.setSavePath(selected);
    else item.cancel();
  });
  this.webContents = this;
  this.ipc = ipcMain.createIpcMain();
  if (typeof this._debuggerAttach === 'function')
    this.debugger = new WebContentsDebugger(this);
  this.on('_renderer-event', (nativeEvent, type, payload, requestId) => {
    dispatchRendererEvent(this, nativeEvent, type, payload, requestId);
  });
  Object.defineProperty(this, 'mainFrame', {
    configurable: true,
    get: () => {
      const frame = {
        processId: this.getProcessId(),
        routingId: this.id,
        detached: this.isDestroyed(),
        send: (channel, ...args) => this.sendToFrame(this.id, channel, ...args)
      };
      frame.framesInSubtree = [frame];
      return frame;
    }
  });
};

WebContents.prototype.send = function (channel, ...args) {
  if (channel == null) throw new TypeError('Missing required channel argument');
  return this._send(0, false, channel, ...args);
};
WebContents.prototype.sendToAll = function (channel, ...args) {
  if (channel == null) throw new TypeError('Missing required channel argument');
  return this._send(0, true, channel, ...args);
};
WebContents.prototype.sendToFrame = function (frameId, channel, ...args) {
  if (channel == null) throw new TypeError('Missing required channel argument');
  return this._send(frameId, false, channel, ...args);
};
WebContents.prototype.postMessage = function (channel, data) { return this.send(channel, data); };
WebContents.prototype.getURL = function () { return this._getURL(); };
WebContents.prototype.canGoBack = function () { return this._canGoBack(); };
WebContents.prototype.canGoForward = function () { return this._canGoForward(); };
WebContents.prototype.getZoomLevel = function () { return this._getZoomLevel(); };
WebContents.prototype.setZoomLevel = function (level) { return this._setZoomLevel(level); };
WebContents.prototype.destroy = function () { return this._destroy(); };
WebContents.prototype.setBackgroundThrottling = function (allowed) {
  return this._sendCommand('setBackgroundThrottling', { allowed: !!allowed });
};
WebContents.prototype.openDevTools = function (options = {}) {
  if (typeof this._openDevTools !== 'function')
    throw new Error('DevTools are not supported in this build');
  const existing = devToolsWindows.get(this);
  if (existing && !existing.isDestroyed()) {
    if (options.activate !== false) existing.focus();
    return;
  }

  const frontendURL = this._openDevTools(options);
  if (!frontendURL) throw new Error('Unable to create the DevTools frontend');
  const frontend = new electron.BrowserWindow({
    width: 1024,
    height: 768,
    minWidth: 400,
    minHeight: 300,
    show: options.activate !== false,
    title: `DevTools - ${this.getTitle() || this.getURL()}`,
    webPreferences: {
      partition: this.session.getPartition(),
      sandbox: true,
      contextIsolation: true,
      nodeIntegration: false,
      webSecurity: true
    }
  });
  devToolsWindows.set(this, frontend);
  frontend.webContents.once('did-finish-load', () => {
    if (options.activate !== false && !frontend.isDestroyed()) {
      frontend.show();
      frontend.focus();
    }
  });
  frontend.once('closed', () => {
    if (devToolsWindows.get(this) === frontend) {
      devToolsWindows.delete(this);
      if (!this.isDestroyed()) this._closeDevTools();
      this.emit('devtools-closed');
    }
  });
  this.once('destroyed', () => {
    if (!frontend.isDestroyed()) frontend.destroy();
  });
  void frontend.loadURL(frontendURL).catch(error => {
    if (!frontend.isDestroyed()) frontend.destroy();
    if (!this.isDestroyed()) this._closeDevTools();
    this.emit('devtools-failed', error);
  });
  this.emit('devtools-opened');
};
WebContents.prototype.closeDevTools = function () {
  if (typeof this._closeDevTools !== 'function')
    throw new Error('DevTools are not supported in this build');
  const frontend = devToolsWindows.get(this);
  if (frontend && !frontend.isDestroyed()) frontend.destroy();
  else this._closeDevTools();
};
WebContents.prototype.isDevToolsOpened = function () {
  if (typeof this._isDevToolsOpened !== 'function') return false;
  const frontend = devToolsWindows.get(this);
  return !!(frontend && !frontend.isDestroyed() && this._isDevToolsOpened());
};
WebContents.prototype.inspectElement = function (x, y) {
  this.openDevTools({ mode: 'detach' });
  return this._inspectElement(Number(x) || 0, Number(y) || 0);
};
WebContents.prototype.replaceMisspelling = function (word) {
  return this._sendCommand('replaceMisspelling', { word: String(word) });
};
WebContents.prototype.copyImageAt = function (x, y) {
  return this._sendCommand('copyImageAt', { x: Number(x) || 0, y: Number(y) || 0 });
};

WebContents.prototype.loadURL = function (target) {
  if (this.isDestroyed()) return Promise.reject(new Error('WebContents was destroyed'));
  return new Promise((resolve, reject) => {
    const loaded = () => { cleanup(); resolve(); };
    const failed = (_event, code, description, _url, isMainFrame = true) => {
      if (!isMainFrame) return;
      cleanup();
      const error = new Error(description || `Failed to load ${target}`);
      error.errno = code;
      reject(error);
    };
    const destroyed = () => failed(null, -2, 'WebContents was destroyed');
    const cleanup = () => {
      this.removeListener('did-finish-load', loaded);
      this.removeListener('did-fail-load', failed);
      this.removeListener('destroyed', destroyed);
    };
    this.once('did-finish-load', loaded);
    this.on('did-fail-load', failed);
    this.once('destroyed', destroyed);
    try {
      this._loadURL(target);
    } catch (error) {
      cleanup();
      reject(error);
    }
  });
};

WebContents.prototype.loadFile = function (filePath) {
  if (typeof filePath !== 'string') throw new TypeError('Must pass filePath as a string');
  return this.loadURL(format({ protocol: 'file', slashes: true,
    pathname: path.resolve(app.getAppPath(), filePath) }));
};

WebContents.prototype.executeJavaScript = function (code, hasUserGesture = false, callback) {
  if (typeof hasUserGesture === 'function') {
    callback = hasUserGesture;
    hasUserGesture = false;
  }
  const promise = this._request('executeJavaScript', { code: String(code), userGesture: !!hasUserGesture })
    .then(value => Object.prototype.hasOwnProperty.call(value, 'result') ? value.result : value);
  if (typeof callback === 'function') promise.then(callback);
  return promise;
};
WebContents.prototype.insertCSS = function (css) {
  return this._request('insertCSS', { css: String(css) })
    .then(value => value.key || value.result || '');
};
WebContents.prototype.removeInsertedCSS = function (key) {
  return this._request('removeInsertedCSS', { key: String(key) }).then(() => undefined);
};
WebContents.prototype.setVisualZoomLevelLimits = function (minimumLevel, maximumLevel) {
  return this._sendCommand('setVisualZoomLevelLimits', { minimumLevel, maximumLevel });
};
WebContents.prototype.setLayoutZoomLevelLimits = function (minimumLevel, maximumLevel) {
  return this._sendCommand('setLayoutZoomLevelLimits', { minimumLevel, maximumLevel });
};

const fileTypeExtensions = Object.freeze({
  'image/*': ['png', 'jpg', 'jpeg', 'gif', 'webp', 'bmp', 'ico', 'svg', 'avif'],
  'image/png': ['png'],
  'image/jpeg': ['jpg', 'jpeg'],
  'image/gif': ['gif'],
  'image/webp': ['webp'],
  'audio/*': ['mp3', 'wav', 'ogg', 'm4a', 'aac', 'flac'],
  'video/*': ['mp4', 'webm', 'ogv', 'mov', 'm4v'],
  'text/plain': ['txt'],
  'text/csv': ['csv'],
  'application/json': ['json'],
  'application/pdf': ['pdf'],
  'application/zip': ['zip']
});

function fileChooserOptions(payload) {
  let properties;
  if (payload.mode === 'open') properties = ['openFile'];
  else if (payload.mode === 'openMultiple')
    properties = ['openFile', 'multiSelections'];
  else if (payload.mode === 'folder') properties = ['openDirectory'];
  else throw new Error('Invalid file chooser mode');
  const acceptTypes = payload.acceptTypes === undefined ? [] : payload.acceptTypes;
  if (!Array.isArray(acceptTypes) || acceptTypes.length > 128)
    throw new Error('Invalid file chooser acceptTypes');
  const extensions = new Set();
  for (const rawType of acceptTypes) {
    if (typeof rawType !== 'string' || Buffer.byteLength(rawType, 'utf8') > 256 ||
        rawType.includes('\0')) {
      throw new Error('Invalid file chooser acceptTypes');
    }
    const type = rawType.trim().toLowerCase();
    if (!type) continue;
    if (type.startsWith('.')) {
      const extension = type.slice(1);
      if (!/^[a-z0-9][a-z0-9+_.-]{0,63}$/.test(extension))
        throw new Error('Invalid file chooser acceptTypes');
      if (!extension.includes('.')) extensions.add(extension);
      continue;
    }
    if (!/^[a-z0-9][a-z0-9!#$&^_.+-]*\/(?:\*|[a-z0-9][a-z0-9!#$&^_.+-]*)$/.test(type))
      throw new Error('Invalid file chooser acceptTypes');
    const mapped = fileTypeExtensions[type];
    if (mapped) {
      for (const extension of mapped) extensions.add(extension);
    }
  }
  const options = { properties };
  if (payload.title !== undefined) {
    if (typeof payload.title !== 'string' ||
        Buffer.byteLength(payload.title, 'utf8') > 1024 ||
        payload.title.includes('\0')) {
      throw new Error('Invalid file chooser title');
    }
    options.title = payload.title;
  }
  if (extensions.size) {
    options.filters = [{
      name: 'Accepted files',
      extensions: Array.from(extensions)
    }];
  }
  return options;
}

function showFileChooser(contents, payload, requestId) {
  let responded = false;
  const respond = (result, error = '') => {
    if (responded) return;
    responded = true;
    contents._respond(requestId, result, error);
  };
  if (!requestId) {
    respond({}, 'Invalid file chooser request');
    return;
  }
  let options;
  try {
    options = fileChooserOptions(payload);
  } catch (error) {
    respond({}, error && error.message ? error.message : String(error));
    return;
  }
  const owner = electron.BrowserWindow.fromWebContents(contents);
  let chooser;
  try {
    chooser = owner
      ? electron.dialog.showOpenDialog(owner, options)
      : electron.dialog.showOpenDialog(options);
  } catch (error) {
    respond({}, error && error.message ? error.message : String(error));
    return;
  }
  Promise.resolve(chooser).then(result => {
    if (!result || result.canceled) {
      respond({ canceled: true, files: [] });
      return;
    }
    if (contents.isDestroyed())
      throw new Error('WebContents was destroyed during file selection');
    if (!Array.isArray(result.filePaths))
      throw new Error('Native file chooser returned invalid paths');
    const files = contents._authorizeFileSelection(result.filePaths, requestId);
    if (!Array.isArray(files))
      throw new Error('File selection authorization failed');
    respond({ canceled: false, files });
  }).catch(error => {
    respond({}, error && error.message ? error.message : String(error));
  });
}

function instanceMap(host, create = false) {
  let instances = guestInstances.get(host.id);
  if (!instances && create) {
    instances = new Map();
    guestInstances.set(host.id, instances);
  }
  return instances;
}

function ownedGuest(host, elementInstanceId) {
  const instances = instanceMap(host);
  const guestId = instances && instances.get(elementInstanceId);
  const owner = guestOwners.get(guestId);
  const guest = guestId && WebContents.fromId(guestId);
  if (!guest || guest.isDestroyed() || !owner || owner.hostId !== host.id ||
      owner.instanceId !== elementInstanceId) {
    return null;
  }
  return { guest, owner };
}

function releaseGuest(host, guestId, elementInstanceId) {
  guestOwners.delete(guestId);
  const instances = instanceMap(host);
  if (!instances || instances.get(elementInstanceId) !== guestId) return;
  instances.delete(elementInstanceId);
  if (instances.size === 0) guestInstances.delete(host.id);
}

function guestAttachEvent() {
  return {
    defaultPrevented: false,
    preventDefault() { this.defaultPrevented = true; }
  };
}

function createGuest(host, payload, requestId) {
  const respond = (result, error = '') => {
    if (requestId) host._respond(requestId, result, error);
  };
  const elementInstanceId = Number(payload.elementInstanceId);
  if (!requestId || !Number.isInteger(elementInstanceId) || elementInstanceId <= 0) {
    respond({}, 'Invalid guest creation request');
    return;
  }
  const instances = instanceMap(host, true);
  if (instances.has(elementInstanceId)) {
    respond({}, 'Guest element is already attached');
    return;
  }
  const attributes = payload.attributes && typeof payload.attributes === 'object'
    ? { ...payload.attributes } : {};
  const webPreferences = {
    partition: typeof attributes.partition === 'string' ? attributes.partition : '',
    preload: typeof attributes.preload === 'string' ? attributes.preload : '',
    contextIsolation: true,
    nodeIntegration: false,
    webviewTag: false,
    webSecurity: !Object.prototype.hasOwnProperty.call(attributes, 'disablewebsecurity'),
    allowRunningInsecureContent: false,
    type: 'webview'
  };
  const attachEvent = guestAttachEvent();
  host.emit('will-attach-webview', attachEvent, webPreferences, attributes);
  if (attachEvent.defaultPrevented) {
    respond({}, 'WebView attachment was cancelled');
    return;
  }
  // The embedder may harden preferences in will-attach-webview, but it cannot
  // grant Node privileges to an untrusted guest renderer.
  webPreferences.nodeIntegration = false;
  webPreferences.sandbox = true;

  let guest;
  try {
    guest = WebContents._createGuest(webPreferences, host.id);
    if (!guest) throw new Error('Failed to launch sandboxed guest renderer');
    guest._init();
    guestOwners.set(guest.id, { hostId: host.id, instanceId: elementInstanceId });
    instances.set(elementInstanceId, guest.id);
    guest.once('destroyed', () =>
      releaseGuest(host, guest.id, elementInstanceId));
    const attached = host._attachGuest(guest.id, {
      elementInstanceId,
      x: 0,
      y: 0,
      width: 1,
      height: 1
    });
    if (!attached) throw new Error('Failed to attach sandboxed guest renderer');
  } catch (error) {
    if (guest) {
      releaseGuest(host, guest.id, elementInstanceId);
      if (!guest.isDestroyed()) guest._destroy();
    }
    respond({}, error && error.message ? error.message : String(error));
    return;
  }

  host.emit('did-attach-webview', {}, guest);
  respond({ guestContentsId: guest.id });
  if (typeof attributes.src === 'string' && attributes.src) {
    void guest.loadURL(attributes.src).catch(error => {
      guest.emit('did-fail-load', {}, -2, error.message, attributes.src, true);
    });
  }
}

function normalizedBounds(payload) {
  return {
    x: Number(payload.x) || 0,
    y: Number(payload.y) || 0,
    width: Math.max(1, Number(payload.width) || 1),
    height: Math.max(1, Number(payload.height) || 1)
  };
}

function updateGuestBounds(host, payload) {
  const elementInstanceId = Number(payload.elementInstanceId);
  const owned = ownedGuest(host, elementInstanceId);
  if (!owned) return;
  const bounds = normalizedBounds(payload);
  host._sendCommand('guest.bounds', {
    guestContentsId: owned.guest.id,
    elementInstanceId,
    ...bounds
  });
  owned.guest._sendCommand('setViewport', {
    width: bounds.width,
    height: bounds.height,
    scaleFactor: 1
  });
}

function destroyGuest(host, payload) {
  const elementInstanceId = Number(payload.elementInstanceId);
  const owned = ownedGuest(host, elementInstanceId);
  if (!owned) return;
  releaseGuest(host, owned.guest.id, elementInstanceId);
  host._sendCommand('guest.detach', {
    guestContentsId: owned.guest.id,
    elementInstanceId
  });
  owned.guest._destroy();
}

function commandGuest(host, payload) {
  const elementInstanceId = Number(payload.elementInstanceId);
  const owned = ownedGuest(host, elementInstanceId);
  if (!owned) return;
  const params = payload.params && typeof payload.params === 'object'
    ? payload.params : {};
  if (payload.method === 'navigate' && typeof params.url === 'string') {
    void owned.guest.loadURL(params.url).catch(error => {
      owned.guest.emit('did-fail-load', {}, -2, error.message, params.url, true);
    });
  } else if (payload.method === 'ipc-message' &&
      typeof params.channel === 'string' && params.channel) {
    owned.guest.send(params.channel,
      ...(Array.isArray(params.args) ? params.args : []));
  } else if (payload.method === 'executeJavaScript') {
    void owned.guest.executeJavaScript(String(params.code || ''),
      !!params.userGesture).catch(() => {});
  } else if (payload.method === 'setFocus') {
    const focused = !!params.focused;
    host._sendCommand('guest.focus', {
      guestContentsId: owned.guest.id,
      elementInstanceId,
      focused
    });
    owned.guest._sendCommand('setFocus', { focused });
  }
}

module.exports = WebContents;
