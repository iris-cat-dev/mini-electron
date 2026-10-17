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
  else console.log('PASS: native clipboard rich copy/cut/paste, Unicode, empty payload and independent commits');
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
async function pasteNativeImage(format) {
  await evaluate("(() => { window.imagePastes = []; const target = document.getElementById('target'); target.textContent = ''; target.focus(); return true; })()");
  native.imageClipboard(format);
  key(9);
  await eventually(() => evaluate('window.imagePastes.length'), 1);
  const paste = JSON.parse(await evaluate('JSON.stringify(window.imagePastes[0])'));
  assert.ok(paste.types.includes('Files'), `${format}: ${paste.types}`);
  assert.equal(paste.files.length, 1, `${format}: one pasted file`);
  assert.equal(paste.files[0].type, 'image/png');
  assert.equal(paste.files[0].itemType, 'image/png');
  assert.equal(paste.files[0].kind, 'file');
  assert.ok(paste.files[0].bytes > 8, `${format}: nonempty PNG bytes`);
  assert.deepEqual(paste.files[0].signature, [137, 80, 78, 71, 13, 10, 26, 10]);
}
async function rejectNativeImage(format) {
  await evaluate("(() => { window.imagePastes = []; const target = document.getElementById('target'); target.textContent = ''; target.focus(); return true; })()");
  native.imageClipboard(format);
  key(9);
  await eventually(() => evaluate('window.imagePastes.length'), 1);
  const paste = JSON.parse(await evaluate('JSON.stringify(window.imagePastes[0])'));
  assert.equal(paste.files.length, 0, `${format}: invalid image must not become a File`);
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
    <script>window.imagePastes = [];
    document.addEventListener('paste', event => {
      const types = Array.from(event.clipboardData.types);
      const items = Array.from(event.clipboardData.items);
      const files = Array.from(event.clipboardData.files);
      const fileItems = items.filter(value => value.kind === 'file')
        .map(value => ({ kind: value.kind, type: value.type }));
      Promise.all(files.map(async (file, index) => {
        const bytes = new Uint8Array(await file.arrayBuffer());
        const item = fileItems[index];
        return { type: file.type, bytes: bytes.length, signature: Array.from(bytes.slice(0, 8)),
          kind: item && item.kind, itemType: item && item.type };
      })).then(files => window.imagePastes.push({ types, files }));
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
  await rejectNativeImage('malformed-png');
  await rejectNativeImage('oversized-png');
  finish();
}).catch(finish);
