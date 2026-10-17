'use strict'
const { app, BrowserWindow, Menu, clipboard } = require('electron')
const assert = require('assert/strict')
const fs = require('fs')
const os = require('os')
const path = require('path')
const { execFileSync } = require('child_process')
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-shortcuts-'))
let helper, window
const deadline = setTimeout(() => finish(new Error('Native shortcuts smoke timed out')), 30000)
const CMD = 1 << 20, SHIFT = 1 << 17, CTRL = 1 << 18
function finish(error) {
  clearTimeout(deadline)
  if (helper) helper.clipboard(true)
  if (window && !window.isDestroyed()) window.destroy()
  fs.rmSync(temporary, { recursive: true, force: true })
  if (error) console.error(error)
  else console.log('Native macOS shortcuts smoke passed')
  app.exit(error ? 1 : 0)
}
async function eventually(check) {
  for (let i = 0; i < 100; i++) {
    if (await check()) return
    await new Promise(resolve => setTimeout(resolve, 30))
  }
  throw new Error('Native shortcut assertion did not become true')
}
async function evaluate(source) {
  const result = await window.webContents.executeJavaScript(source)
  return result && typeof result === 'object' && 'value' in result ? result.value : result
}
app.whenReady().then(async () => {
  const bundle = path.join(temporary, 'helper.node')
  const sdk = process.env.SDKROOT || execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim()
  const compiler = execFileSync('xcrun', ['--sdk', 'macosx', '--find', 'clang++'], { encoding: 'utf8' }).trim()
  execFileSync(compiler, ['-isysroot', sdk, '-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup',
    '-framework', 'AppKit', '-framework', 'Carbon', '-I', path.join(__dirname, '../third_party/libnode/src'),
    path.join(__dirname, 'macos-shortcuts-helper.mm'), '-o', bundle])
  helper = require(bundle)
  helper.clipboard(false)
  const title = `Native shortcuts ${process.pid}`
  window = new BrowserWindow({ width: 600, height: 400, title, webPreferences: { sandbox: true } })
  let callbackWindow, callbackEvent, count = 0
  const zoom = (item, focusedWindow, event) => {
    callbackWindow = focusedWindow; callbackEvent = event; count++
    focusedWindow.webContents.setZoomLevel(focusedWindow.webContents.getZoomLevel() + 0.5)
  }
  const menu = Menu.buildFromTemplate([
    { label: 'Edit', submenu: ['undo', 'redo', 'cut', 'copy', 'paste', 'selectAll'].map(role => ({ role })) },
    { label: 'View', submenu: [
      { label: 'Native Zoom In', accelerator: 'CmdOrCtrl+=', click: zoom },
      { role: 'zoomOut' }, { role: 'resetZoom' },
      { label: 'Unregistered', accelerator: 'CmdOrCtrl+K', registerAccelerator: false, click: () => { throw new Error('Unregistered accelerator fired') } }
    ] }
  ])
  Menu.setApplicationMenu(menu)
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent('<textarea id="edit">Native Unicode café 日本語</textarea><script>window.keys=[]; document.addEventListener("keydown",e=>keys.push({key:e.key,meta:e.metaKey,ctrl:e.ctrlKey}));edit.focus()</script>'))
  window.show(); window.focus()
  await eventually(() => window.isFocused())
  await eventually(async () => await evaluate('edit.focus(); document.readyState === "complete" && document.activeElement === edit && document.hasFocus()'))
  helper.key(title, 0, CMD)
  await eventually(async () => await evaluate('edit.selectionEnd-edit.selectionStart') === 'Native Unicode café 日本語'.length)
  helper.key(title, 8, CMD)
  await eventually(() => clipboard.readText() === 'Native Unicode café 日本語')
  helper.key(title, 7, CMD)
  await eventually(async () => await evaluate('edit.value') === '')
  await eventually(() => clipboard.readText() === 'Native Unicode café 日本語')
  helper.key(title, 6, CMD)
  await eventually(async () => await evaluate('edit.value') === 'Native Unicode café 日本語')
  helper.key(title, 6, CMD | SHIFT)
  await eventually(async () => await evaluate('edit.value') === '')
  helper.key(title, 9, CMD)
  await eventually(async () => await evaluate('edit.value') === 'Native Unicode café 日本語')
  helper.key(title, 24, CMD)
  await eventually(() => count === 1 && window.webContents.getZoomLevel() === 0.5)
  assert.equal(callbackWindow, window)
  assert.equal(callbackEvent.triggeredByAccelerator, true)
  helper.key(title, 24, CMD | SHIFT)
  await eventually(() => count === 2 && window.webContents.getZoomLevel() === 1)
  helper.key(title, 29, CMD)
  await eventually(() => window.webContents.getZoomLevel() === 0)
  helper.click('Native Zoom In')
  await eventually(() => count === 3 && window.webContents.getZoomLevel() === 0.5)
  assert.equal(callbackEvent.triggeredByAccelerator, false)
  menu.items[1].submenu.items[0].enabled = false
  helper.key(title, 24, CMD)
  await new Promise(resolve => setTimeout(resolve, 150))
  assert.equal(count, 3)
  helper.key(title, 40, CMD)
  helper.key(title, 40, CTRL)
  await eventually(async () => await evaluate('keys.some(e=>e.key.toLowerCase()==="k"&&e.meta&&!e.ctrl)&&keys.some(e=>e.key.toLowerCase()==="k"&&e.ctrl&&!e.meta)'))
}).then(() => finish(), finish)
