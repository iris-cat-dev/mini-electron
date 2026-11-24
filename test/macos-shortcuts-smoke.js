'use strict'
const { app, BrowserWindow, Menu, clipboard } = require('electron')
const assert = require('assert/strict')
const fs = require('fs')
const os = require('os')
const path = require('path')
const { execFileSync } = require('child_process')
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-shortcuts-'))
app.setPath('userData', path.join(temporary, 'profile'))
let helper, window
const deadline = setTimeout(() => finish(new Error('Native shortcuts smoke timed out')), 30000)
const CMD = 1 << 20, OPT = 1 << 19, CTRL = 1 << 18, SHIFT = 1 << 17
function finish(error) {
  clearTimeout(deadline)
  if (helper) helper.clipboard(true)
  if (window && !window.isDestroyed()) window.destroy()
  fs.rmSync(temporary, { recursive: true, force: true })
  if (error) console.error(error)
  else console.log('Native macOS shortcuts smoke passed')
  app.exit(error ? 1 : 0)
}
async function eventually(check, label = 'Native shortcut assertion') {
  for (let i = 0; i < 100; i++) {
    if (await check()) return
    await new Promise(resolve => setTimeout(resolve, 30))
  }
  throw new Error(`${label} did not become true`)
}
async function evaluate(source) {
  return window.webContents.executeJavaScript(source)
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
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html>
    <title>${title}</title>
    <textarea id="edit" rows="5">Native Unicode café 日本語</textarea>
    <input id="single">
    <div id="rich" contenteditable style="white-space: pre-wrap"></div>
    <script>
      window.keys = []
      document.addEventListener('keydown', e => keys.push({key:e.key,meta:e.metaKey,ctrl:e.ctrlKey}))
      edit.focus()
    </script>`))
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

  const text = 'first line\nbeta gamma\nlast'
  await evaluate(`window.cancelShortcut = '';
    for (const type of ['keydown', 'beforeinput']) {
      document.addEventListener(type, event => {
        if (cancelShortcut === type) event.preventDefault()
      })
    }
    document.addEventListener('keyup', () => { window.editKeyReleased = true }); true`)
  for (const [label, code, modifiers, result, field = 'edit', cancel = '', vertical = false] of [
    ['Cmd+Left: start of single-line input', 123, CMD, ['first line beta gamma last', 0, 0], 'single'],
    ['Cmd+Shift+Right: select to end of line', 124, CMD | SHIFT, [text, 12, 21]],
    ['Cmd+Up: start of textarea', 126, CMD, [text, 0, 0]],
    ['Cmd+Shift+Down: select to end of textarea', 125, CMD | SHIFT, [text, 12, 26]],
    ['Cmd+Backspace: delete to start of line', 51, CMD, ['first line\neta gamma\nlast', 11, 11]],
    ['Option+Down: end of current paragraph', 125, OPT, [text, 21, 21]],
    ['Option+Shift+Up: extend selection', 126, OPT | SHIFT, [text, 1, 12]],
    ['Shift+Home: select to start of document', 115, SHIFT, [text, 0, 12]],
    ['Option+Forward Delete: delete to word end', 117, OPT, ['first line\nb gamma\nlast', 12, 12]],
    ['Ctrl+A: start of line', 0, CTRL, [text, 11, 11]],
    ['Ctrl+E: end of line', 14, CTRL, [text, 21, 21]],
    ['Ctrl+H: delete one character', 4, CTRL, ['first line\neta gamma\nlast', 11, 11]],
    ['Ctrl+T: transpose adjacent characters', 17, CTRL, ['first line\nebta gamma\nlast', 13, 13]],
    ['Ctrl+K: delete to end of paragraph in contenteditable', 40, CTRL, ['first line\nb\nlast', 12, 12], 'rich'],
    ['Down Arrow: move by one character in vertical text', 125, 0, [text, 13, 13], 'rich', '', true],
    ['keydown cancellation leaves text untouched', 40, CTRL, [text, 12, 12], 'rich', 'keydown'],
    ['a canceled binding cannot leak into the next key', 7, 0, ['first line\nbxeta gamma\nlast', 13, 13]],
    ['beforeinput cancellation leaves text untouched', 40, CTRL, [text, 12, 12], 'rich', 'beforeinput'],
  ]) {
    await evaluate(`{
      const editor = document.getElementById(${JSON.stringify(field)})
      const value = ${JSON.stringify(field === 'single' ? text.replaceAll('\n', ' ') : text)}
      if (editor.isContentEditable) {
        editor.textContent = value
        editor.style.writingMode = ${JSON.stringify(vertical ? 'vertical-rl' : '')}
        editor.focus()
        getSelection().collapse(editor.firstChild, 12)
      } else {
        editor.value = value
        editor.focus()
        editor.setSelectionRange(12, 12)
      }
      window.keys = []
      window.editKeyReleased = false
      window.cancelShortcut = ${JSON.stringify(cancel)}
    } true`)
    helper.key(title, code, modifiers)
    const delivered = modifiers & CMD ? 'keys.length === 1' : 'keys.length === 1 && editKeyReleased'
    await eventually(async () => await evaluate(delivered), label)
    const actual = JSON.parse(await evaluate(`JSON.stringify((() => {
      const editor = document.activeElement
      return editor.isContentEditable
        ? [editor.innerText, getSelection().anchorOffset, getSelection().focusOffset]
        : [editor.value, editor.selectionStart, editor.selectionEnd]
    })())`))
    assert.deepEqual(actual, result, label)
  }
}).then(() => finish(), finish)
