'use strict';
// Run with the macOS mini-electron executable. Exercises AppKit -> Blink -> NSPasteboard.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, Menu, clipboard } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-clipboard-editing-'));
const title = `mini-clipboard-editing-${process.pid}`;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let window, native, saved = false, finished = false;
const deadline = setTimeout(() => finish(new Error('Clipboard editing smoke timed out')), 60000);
app.on('window-all-closed', () => {});
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (saved) native.clipboard(true);
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: native clipboard rich copy/cut/paste, Unicode, independent commits and web image Files in editable/textarea');
  app.exit(error ? 1 : 0);
}
const evaluate = async source => (await window.webContents.executeJavaScript(source)).value;
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
const key = code => native.key(title, code, 1 << 20);
async function select(id) {
  await evaluate(`(() => { const element = document.getElementById(${JSON.stringify(id)}); element.focus(); const range = document.createRange(); range.selectNodeContents(element); const selection = getSelection(); selection.removeAllRanges(); selection.addRange(range); return document.activeElement.id; })()`);
}
async function focusPasteTarget(id) {
  await evaluate(`(() => { window.imagePastes = []; const target = document.getElementById(${JSON.stringify(id)}); target.textContent = ''; if ('value' in target) target.value = ''; target.focus(); return true; })()`);
}
async function pasteNativeImage(format, id = 'target') {
  await focusPasteTarget(id);
  if (format) native.imageClipboard(format);
  key(9);
  await eventually(() => evaluate('window.imagePastes.length'), 1);
  const paste = JSON.parse(await evaluate('JSON.stringify(window.imagePastes[0])'));
  assert.equal(paste.target, id);
  assert.ok(paste.types.includes('Files'), `${format}: ${paste.types}`);
  assert.equal(paste.files.length, 1, `${format}: one pasted file`);
  assert.equal(paste.files[0].type, 'image/png');
  assert.equal(paste.files[0].itemType, 'image/png');
  assert.equal(paste.files[0].kind, 'file');
  assert.ok(paste.files[0].bytes > 8, `${format}: nonempty PNG bytes`);
  assert.deepEqual(paste.files[0].signature, [137, 80, 78, 71, 13, 10, 26, 10]);
  assert.equal(paste.items.length, 1, `${format}: getAsFile exposes the image`);
  assert.deepEqual(paste.items[0], paste.files[0]);
  if (format && format.startsWith('web-')) {
    assert.ok(paste.types.includes('text/html'));
    assert.equal(paste.text, 'https://example.invalid/copied-image.png');
    assert.ok(paste.html.includes('<img'));
  }
}
async function rejectNativeImage(format, id = 'target') {
  await focusPasteTarget(id);
  native.imageClipboard(format);
  key(9);
  await eventually(() => evaluate('window.imagePastes.length'), 1);
  const paste = JSON.parse(await evaluate('JSON.stringify(window.imagePastes[0])'));
  assert.equal(paste.files.length, 0, `${format}: invalid image must not become a File`);
  assert.equal(paste.items.length, 0, `${format}: getAsFile must reject missing/invalid bytes`);
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const root = path.resolve(__dirname, '..');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'shortcuts.node');
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup',
    '-isysroot', sdk, `-I${path.join(root, 'third_party/libnode/src')}`, '-framework', 'AppKit',
    '-framework', 'Carbon', path.join(__dirname, 'macos-shortcuts-helper.mm'), '-o', helper],
    { stdio: 'inherit', timeout: 30000 });
  native = require(helper);
  native.clipboard(false);
  saved = true;
  window = new BrowserWindow({ width: 600, height: 400, title });
  Menu.setApplicationMenu(Menu.buildFromTemplate([{ label: 'Edit', submenu: [
    { role: 'undo' }, { role: 'redo' }, { type: 'separator' }, { role: 'cut' },
    { role: 'copy' }, { role: 'paste' }, { role: 'selectAll' },
  ] }]));
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><title>${title}</title>
    <div id="rich" contenteditable><b>café 中文 😀</b></div>
    <textarea id="plain">independent λ</textarea><div id="target" contenteditable></div>
    <div id="empty" contenteditable>copy empty representation</div>
    <img id="web-image" width="32" height="32" alt="copied web image"
      src="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==">
    <script>window.imagePastes = [];
    document.addEventListener('paste', event => {
      const types = Array.from(event.clipboardData.types);
      const items = Array.from(event.clipboardData.items);
      const files = Array.from(event.clipboardData.files);
      const fileItems = items.filter(value => value.kind === 'file');
      const itemFiles = fileItems.map(value => value.getAsFile()).filter(Boolean);
      const fileMetadata = fileItems.map(value => ({ kind: value.kind, type: value.type }));
      const target = event.target.id;
      const text = event.clipboardData.getData('text/plain');
      const html = event.clipboardData.getData('text/html');
      const describe = async (file, index) => {
        const bytes = new Uint8Array(await file.arrayBuffer());
        const item = fileMetadata[index];
        return { type: file.type, bytes: bytes.length, signature: Array.from(bytes.slice(0, 8)),
          kind: item && item.kind, itemType: item && item.type };
      };
      Promise.all([Promise.all(files.map(describe)), Promise.all(itemFiles.map(describe))])
        .then(([files, items]) => window.imagePastes.push({ types, target, text, html, files, items }));
    });
    document.getElementById('empty').addEventListener('copy', event => {
      event.clipboardData.setData('text/plain', ''); event.preventDefault();
    });</script>`));
  window.show();
  app.focus();
  window.focus();
  await eventually(() => window.isFocused(), true);
  assert.equal(await evaluate("document.getElementById('rich').textContent"), 'café 中文 😀');
  assert.equal(await evaluate("document.getElementById('plain').value"), 'independent λ');

  clipboard.writeText('stale sentinel');
  await select('rich');
  key(8); // Cmd+C
  await eventually(() => clipboard.readText(), 'café 中文 😀');
  assert.equal(clipboard.has('text/html'), true);
  const richHtml = clipboard.readHTML();
  assert.ok(richHtml.includes('café 中文 😀'), richHtml);
  assert.ok(/font-weight|<b[ >]/i.test(richHtml), richHtml);
  await select('target');
  key(9); // Cmd+V into the real Blink editable.
  await eventually(() => evaluate("document.getElementById('target').textContent"), 'café 中文 😀');
  assert.equal(await evaluate("!!document.getElementById('target').querySelector('b,span[style*=font-weight]')"), true);

  await evaluate("(() => { const element = document.getElementById('plain'); element.focus(); element.select(); return true; })()");
  key(7); // Cmd+X, a separate text-only transaction.
  await eventually(() => clipboard.readText(), 'independent λ');
  await eventually(() => evaluate("document.getElementById('plain').value"), '');
  assert.equal(clipboard.has('text/html'), false, 'previous rich representation must not leak across commits');
  key(9);
  await eventually(() => evaluate("document.getElementById('plain').value"), 'independent λ');

  await select('empty');
  key(8);
  await eventually(() => clipboard.readText(), '');
  assert.equal(clipboard.has('text/plain'), true, 'empty text is a written representation, not omitted');
  assert.equal(clipboard.has('text/html'), false);
  await select('rich');
  key(8);
  await eventually(() => clipboard.readText(), 'café 中文 😀');
  assert.equal(clipboard.has('text/html'), true, 'subsequent rich commit still includes both representations');
  await pasteNativeImage('png');
  await pasteNativeImage('tiff');
  for (const id of ['target', 'plain']) {
    await pasteNativeImage('web-png', id);
    await pasteNativeImage('web-tiff', id);
    await rejectNativeImage('web-malformed-png', id);
    await rejectNativeImage('web-no-image', id);
  }
  await eventually(() => evaluate("document.getElementById('web-image').naturalWidth"), 1);
  const point = JSON.parse(await evaluate(`(() => {
    const rect = document.getElementById('web-image').getBoundingClientRect();
    return JSON.stringify({ x: Math.floor(rect.x + rect.width / 2), y: Math.floor(rect.y + rect.height / 2) });
  })()`));
  clipboard.writeText('before browser image copy');
  await window.webContents.copyImageAt(point.x, point.y);
  await eventually(() => clipboard.has('image/png'), true);
  assert.equal(clipboard.has('text/html'), true, 'browser image copy keeps its HTML co-representation');
  await pasteNativeImage(null, 'target');
  await pasteNativeImage(null, 'plain');
  await select('rich');
  key(8);
  await eventually(() => clipboard.readText(), 'café 中文 😀');
  assert.equal(clipboard.has('image/png'), false, 'image must not leak into the next text/rich commit');
  await rejectNativeImage('malformed-png');
  await rejectNativeImage('oversized-png');
  finish();
}).catch(finish);
