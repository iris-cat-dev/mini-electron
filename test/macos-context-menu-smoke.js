'use strict';
// Run with the macOS mini-electron executable; no DOM event synthesis or menu bridge.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, Menu, clipboard, nativeImage } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-context-menu-'));
const title = `mini-context-menu-${process.pid}`;
const imageURL = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==';
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let window, otherWindow, pointer, shortcuts, saved = false, finished = false;
const events = [];
const deadline = setTimeout(() => finish(new Error('Native context menu smoke timed out')), 60000);
app.on('window-all-closed', () => {});
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (saved) shortcuts.clipboard(true);
  if (window && !window.isDestroyed()) window.destroy();
  if (otherWindow && !otherWindow.isDestroyed()) otherWindow.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: native image/editor context-menu, CSS image pixels, rich/image Paste, popup owner targeting and DOM suppression');
  app.exit(error ? 1 : 0);
}
const evaluate = source => window.webContents.executeJavaScript(source);
async function eventually(read, expected) {
  const until = Date.now() + 4000;
  let actual;
  do {
    actual = await read();
    if (actual === expected) return;
    await pause(25);
  } while (Date.now() < until);
  assert.equal(actual, expected);
}
async function point(id) {
  return JSON.parse(await evaluate(`(() => {
    const rect = document.getElementById(${JSON.stringify(id)}).getBoundingClientRect();
    return JSON.stringify({ x: Math.floor(rect.x + rect.width / 2), y: Math.floor(rect.y + rect.height / 2) });
  })()`));
}
async function rightClick(id, suppressed = false) {
  const position = await point(id);
  const count = events.length;
  const domCount = await evaluate('window.contextEvents');
  pointer.mouse(title, 'rightDown', position.x, position.y);
  pointer.mouse(title, 'rightUp', position.x, position.y);
  await eventually(() => evaluate('window.contextEvents'), domCount + 1);
  if (!suppressed) await eventually(() => events.length, count + 1);
  await pause(150);
  assert.equal(events.length, count + (suppressed ? 0 : 1), 'one native event, never a duplicate or handled DOM menu');
  if (suppressed) return;
  const params = events[count];
  assert.ok(Math.abs(params.x - position.x) <= 1, 'window-relative DIP x');
  assert.ok(Math.abs(params.y - position.y) <= 1, 'window-relative DIP y');
  assert.equal(typeof params.selectionText, 'string');
  assert.equal(typeof params.misspelledWord, 'string');
  assert.ok(Array.isArray(params.dictionarySuggestions));
  for (const key of ['canUndo', 'canRedo', 'canCut', 'canCopy', 'canPaste', 'canDelete', 'canSelectAll', 'canEditRichly']) {
    assert.equal(typeof params.editFlags[key], 'boolean', key);
  }
  return params;
}
function menu(items) {
  Menu.setApplicationMenu(Menu.buildFromTemplate([{ label: 'Context regression', submenu: items }]));
}
async function imageCopy(id) {
  const params = await rightClick(id);
  assert.equal(params.mediaType, 'image');
  assert.equal(params.srcURL, imageURL);
  assert.equal(params.hasImageContents, true);
  if (id === 'image') assert.equal(params.linkURL, 'https://example.invalid/image-link');
  clipboard.writeText('image copy sentinel');
  menu([{ label: 'Copy Context Image', click: () => window.webContents.copyImageAt(params.x, params.y) }]);
  shortcuts.click('Copy Context Image');
  await eventually(() => clipboard.has('image/png'), true);
  const copied = clipboard.readImage();
  const expected = nativeImage.createFromDataURL(imageURL);
  assert.deepEqual(copied.getSize(), { width: 1, height: 1 });
  assert.deepEqual(copied.toBitmap(), expected.toBitmap(), 'native clipboard contains actual source pixels');
  assert.deepEqual([...copied.toPNG().subarray(0, 8)], [137, 80, 78, 71, 13, 10, 26, 10]);
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const root = path.resolve(__dirname, '..');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  function helper(name, frameworks) {
    const destination = path.join(temporary, `${name}.node`);
    execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup',
      '-isysroot', sdk, `-I${path.join(root, 'third_party/libnode/src')}`,
      ...frameworks.flatMap(value => ['-framework', value]),
      path.join(__dirname, `${name}.mm`), '-o', destination], { stdio: 'inherit', timeout: 30000 });
    return require(destination);
  }
  pointer = helper('macos-pointer-interaction-helper', ['AppKit']);
  shortcuts = helper('macos-shortcuts-helper', ['AppKit', 'Carbon']);
  shortcuts.clipboard(false);
  saved = true;
  window = new BrowserWindow({ width: 640, height: 520, title });
  window.webContents.on('context-menu', (_event, params) => events.push(params));
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><title>${title}</title>
    <style>body{margin:20px}img,.background{width:64px;height:64px}.background{background-size:contain;background-repeat:no-repeat}#editor{height:60px;border:1px solid}textarea{display:block;margin:12px 0}#handled{width:80px;height:30px;background:#ccc}</style>
    <a href="https://example.invalid/image-link"><img id="image" src="${imageURL}"></a>
    <div id="background" class="background" style="background-image:url('${imageURL}')"><img style="opacity:0;pointer-events:none" src="${imageURL}"></div>
    <div id="editor" contenteditable></div><textarea id="plain"></textarea>
    <textarea id="readonly" readonly>read only</textarea><div id="handled">Custom menu</div>
    <script>window.contextEvents=0;window.pastes=[];
    document.addEventListener('contextmenu',()=>window.contextEvents++);
    document.getElementById('handled').addEventListener('contextmenu',event=>event.preventDefault());
    document.addEventListener('paste',async event=>{
      const data=event.clipboardData;
      const snapshot={target:event.target.id,types:Array.from(data.types),html:data.getData('text/html'),text:data.getData('text/plain')};
      const files=await Promise.all(Array.from(data.files,async file=>({type:file.type,signature:Array.from(new Uint8Array(await file.arrayBuffer()).slice(0,8))})));
      window.pastes.push({...snapshot,files});
    });</script>`));
  window.show();
  app.focus();
  window.focus();
  await eventually(() => window.isFocused(), true);
  await eventually(() => evaluate("Array.from(document.images).every(image=>image.complete&&image.naturalWidth===1)"), true);
  await imageCopy('image');
  await imageCopy('background');
  await rightClick('handled', true);

  clipboard.write({ text: 'rich café 中文', html: '<b>rich café 中文</b>' });
  await evaluate("document.getElementById('editor').focus(); true");
  const editable = await rightClick('editor');
  assert.equal(editable.isEditable, true);
  assert.equal(editable.editFlags.canPaste, true);
  menu([{ label: 'Paste Context Rich', role: 'paste' }]);
  shortcuts.click('Paste Context Rich');
  await eventually(() => evaluate("document.getElementById('editor').textContent"), 'rich café 中文');
  assert.equal(await evaluate("!!document.getElementById('editor').querySelector('b,span[style*=font-weight]')"), true);
  await eventually(() => evaluate('window.pastes.length'), 1);
  assert.equal(await evaluate("window.pastes[0].html.includes('rich café 中文')"), true);

  shortcuts.imageClipboard('web-png');
  await evaluate("document.getElementById('plain').focus(); true");
  const plain = await rightClick('plain');
  assert.equal(plain.isEditable, true);
  assert.equal(plain.editFlags.canPaste, true);
  menu([{ label: 'Paste Context Image', role: 'paste' }]);
  shortcuts.click('Paste Context Image');
  await eventually(() => evaluate('window.pastes.length'), 2);
  const paste = JSON.parse(await evaluate('JSON.stringify(window.pastes[1])'));
  assert.equal(paste.target, 'plain');
  assert.ok(paste.types.includes('Files'));
  assert.deepEqual(paste.files, [{ type: 'image/png', signature: [137, 80, 78, 71, 13, 10, 26, 10] }]);

  await evaluate("document.getElementById('readonly').focus(); true");
  const readonly = await rightClick('readonly');
  assert.equal(readonly.isEditable, false);
  assert.equal(readonly.editFlags.canPaste, false, 'capability is not forced on readonly fields');

  await evaluate("document.getElementById('plain').value='before AFTER';document.getElementById('plain').focus();document.getElementById('plain').setSelectionRange(7,12);true");
  otherWindow = new BrowserWindow({ width: 320, height: 240, title: `${title}-other` });
  await otherWindow.loadURL('data:text/html,<textarea id="other" autofocus>other untouched</textarea>');
  otherWindow.show();
  otherWindow.focus();
  await eventually(() => window.isFocused(), false);
  clipboard.writeText('粘贴归属');
  const popup = Menu.buildFromTemplate([{ label: 'Paste into owner', role: 'paste' }]);
  pointer.selectNextPopup('Paste into owner');
  popup.popup({ window });
  await eventually(() => evaluate("document.getElementById('plain').value"), 'before 粘贴归属');
  assert.equal(await otherWindow.webContents.executeJavaScript("document.getElementById('other').value"), 'other untouched');

  finish();
}).catch(finish);
