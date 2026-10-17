'use strict'
const { app, BrowserWindow, Menu, ipcMain } = require('electron')
const fs = require('fs')
const os = require('os')
const path = require('path')

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-exit-lifecycle-'))
let finished = false
const deadline = setTimeout(() => finish(new Error('Native exit lifecycle smoke timed out')), 20000)

function finish(error) {
  if (finished) return
  finished = true
  clearTimeout(deadline)
  fs.rmSync(temporary, { recursive: true, force: true })
  if (error) console.error(error.stack || error)
  else console.log('PASS: native application menu and preload-backed windows exit cleanly')
  app.exit(error ? 1 : 0)
}

app.whenReady().then(async () => {
  const preload = path.join(temporary, 'preload.js')
  fs.writeFileSync(preload, `
    const { contextBridge, ipcRenderer } = require('electron')
    contextBridge.exposeInMainWorld('exitLifecycle', {
      start() {
        ipcRenderer.send('exit-lifecycle:ready')
        setInterval(() => void ipcRenderer.invoke('exit-lifecycle:ping').catch(() => {}), 0)
      },
    })
  `)
  Menu.setApplicationMenu(Menu.buildFromTemplate([
    { label: 'Edit', submenu: [{ role: 'undo' }, { role: 'redo' }, { role: 'copy' }, { role: 'paste' }] },
  ]))
  ipcMain.handle('exit-lifecycle:ping', () => 'pong')
  ipcMain.once('exit-lifecycle:ready', () => setTimeout(() => finish(), 10))

  const preferences = {
    preload,
    sandbox: true,
    contextIsolation: true,
    nodeIntegration: false,
  }
  const main = new BrowserWindow({ width: 480, height: 320, webPreferences: preferences })
  const companion = new BrowserWindow({
    width: 200,
    height: 200,
    transparent: true,
    frame: false,
    focusable: false,
    show: false,
    alwaysOnTop: true,
    webPreferences: preferences,
  })
  await main.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent('<textarea>exit lifecycle</textarea>'))
  await companion.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(
    '<img src="data:image/png;base64,invalid" onerror="exitLifecycle.start()">',
  ))
  companion.showInactive()
}).catch(finish)
