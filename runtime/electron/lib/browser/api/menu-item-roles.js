const app = require('electron').app;

const keyCommand = (keyCode, modifiers = 0) => (webContents) => {
  webContents.sendInputEvent({ type: 'keyDown', keyCode, modifiers })
  webContents.sendInputEvent({ type: 'keyUp', keyCode, modifiers })
}

const controlKeyCommand = (keyCode, shift = false) =>
  keyCommand(keyCode, 0x08 | (shift ? 0x04 : 0))

const roles = {
  about: {
    get label () {
      return process.platform === 'linux' ? 'About' : `About ${app.getName()}`
    }
  },
  close: {
    label: process.platform === 'darwin' ? 'Close Window' : 'Close',
    accelerator: 'CommandOrControl+W',
    windowMethod: 'close'
  },
  copy: {
    label: 'Copy',
    accelerator: 'CommandOrControl+C',
    webContentsMethod: controlKeyCommand(0x43)
  },
  cut: {
    label: 'Cut',
    accelerator: 'CommandOrControl+X',
    webContentsMethod: controlKeyCommand(0x58)
  },
  delete: {
    label: 'Delete',
    webContentsMethod: keyCommand(0x2e)
  },
  front: {
    label: 'Bring All to Front'
  },
  help: {
    label: 'Help'
  },
  hide: {
    get label () {
      return `Hide ${app.getName()}`
    },
    accelerator: 'Command+H'
  },
  hideothers: {
    label: 'Hide Others',
    accelerator: 'Command+Alt+H'
  },
  minimize: {
    label: 'Minimize',
    accelerator: 'CommandOrControl+M',
    windowMethod: 'minimize'
  },
  paste: {
    label: 'Paste',
    accelerator: 'CommandOrControl+V',
    webContentsMethod: controlKeyCommand(0x56)
  },
  pasteandmatchstyle: {
    label: 'Paste and Match Style',
    accelerator: 'Shift+CommandOrControl+V',
    webContentsMethod: controlKeyCommand(0x56, true)
  },
  quit: {
    get label () {
      switch (process.platform) {
        case 'darwin': return `Quit ${app.getName()}`
        case 'win32': return 'Exit'
        default: return 'Quit'
      }
    },
    accelerator: process.platform === 'win32' ? null : 'CommandOrControl+Q',
    appMethod: 'quit'
  },
  redo: {
    label: 'Redo',
    accelerator: 'Shift+CommandOrControl+Z',
    webContentsMethod: controlKeyCommand(0x5a, true)
  },
  resetzoom: {
    label: 'Actual Size',
    accelerator: 'CommandOrControl+0',
    webContentsMethod: (webContents) => {
      webContents.setZoomLevel(0)
    }
  },
  selectall: {
    label: 'Select All',
    accelerator: 'CommandOrControl+A',
    webContentsMethod: controlKeyCommand(0x41)
  },
  services: {
    label: 'Services'
  },
  startspeaking: {
    label: 'Start Speaking'
  },
  stopspeaking: {
    label: 'Stop Speaking'
  },
  togglefullscreen: {
    label: 'Toggle Full Screen',
    accelerator: process.platform === 'darwin' ? 'Control+Command+F' : 'F11',
    windowMethod: (window) => {
      window.setFullScreen(!window.isFullScreen())
    }
  },
  undo: {
    label: 'Undo',
    accelerator: 'CommandOrControl+Z',
    webContentsMethod: controlKeyCommand(0x5a)
  },
  unhide: {
    label: 'Show All'
  },
  window: {
    label: 'Window'
  },
  zoom: {
    label: 'Zoom'
  },
  zoomin: {
    label: 'Zoom In',
    accelerator: 'CommandOrControl+Plus',
    webContentsMethod: (webContents) => {
      webContents.setZoomLevel(webContents.getZoomLevel() + 0.5)
    }
  },
  zoomout: {
    label: 'Zoom Out',
    accelerator: 'CommandOrControl+-',
    webContentsMethod: (webContents) => {
      webContents.setZoomLevel(webContents.getZoomLevel() - 0.5)
    }
  }
}

const canExecuteRole = (role) => {
  if (!roles.hasOwnProperty(role)) return false
  if (process.platform !== 'darwin') return true
  // macOS handles all roles natively except the ones listed below
  return ['zoomin', 'zoomout', 'resetzoom'].includes(role)
}

exports.getDefaultLabel = (role) => {
  if (roles.hasOwnProperty(role)) {
    return roles[role].label
  } else {
    return ''
  }
}

exports.getDefaultAccelerator = (role) => {
  if (roles.hasOwnProperty(role)) return roles[role].accelerator
}

exports.execute = (role, focusedWindow, focusedWebContents) => {
  if (!canExecuteRole(role)) return false

  //const {appMethod, webContentsMethod, windowMethod} = roles[role]
  const rolesRole = roles[role];
  const appMethod = rolesRole.appMethod;
  const webContentsMethod = rolesRole.webContentsMethod;
  const windowMethod = rolesRole.windowMethod;

  if (appMethod) {
    app[appMethod]()
    return true
  }

  if (windowMethod && focusedWindow != null) {
    if (typeof windowMethod === 'function') {
      windowMethod(focusedWindow)
    } else {
      focusedWindow[windowMethod]()
    }
    return true
  }

  if (webContentsMethod && focusedWebContents != null) {
    if (typeof webContentsMethod === 'function') {
      webContentsMethod(focusedWebContents)
    } else {
      focusedWebContents[webContentsMethod]()
    }
    return true
  }

  return false
}
