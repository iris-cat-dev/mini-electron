'use strict';

// Run with a freshly built Windows runtime and an unlocked desktop.
const assert = require('node:assert/strict');
const { execFileSync, spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, Menu, clipboard, nativeImage } = require('electron');
const { startNativeInput } = require('./windows-native-input-driver');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-windows-context-menu-'));
app.setPath('userData', path.join(temporary, 'profile'));
app.on('window-all-closed', () => {});
const title = `mini-windows-context-menu-${process.pid}`;
const imageURL = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==';
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const children = new Set();
const events = [];
let window, other, driver, selector, finished = false;
const deadline = setTimeout(() => finish(new Error('Windows context-menu smoke timed out')), 90000);

async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  for (const child of children) if (child.exitCode === null) child.kill();
  if (driver) {
    try { await driver.close(); } catch (cleanupError) { error ||= cleanupError; }
  }
  for (const owned of [other, window]) if (owned && !owned.isDestroyed()) owned.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: Windows native image/CSS context menus, image clipboard pixels, editable roles, DOM suppression and explicit popup owner');
  app.exit(error ? 1 : 0);
}

async function eventually(read, predicate, label) {
  const until = Date.now() + 6000;
  let actual;
  do {
    actual = await read();
    if (predicate(actual)) return actual;
    await pause(25);
  } while (Date.now() < until);
  assert.fail(`${label}: ${JSON.stringify(actual)}`);
}
const evaluate = code => window.webContents.executeJavaScript(code);

async function rightClick(id, suppressed = false) {
  await driver.request('focus');
  const point = await evaluate(`(() => { const r=document.getElementById(${JSON.stringify(id)}).getBoundingClientRect(); return {x:r.x+r.width/2,y:r.y+r.height/2,dpr:devicePixelRatio}; })()`);
  const geometry = await driver.request('geometry');
  const count = events.length;
  const domCount = await evaluate('contextEvents');
  execFileSync(selector, ['right-click', String(process.pid),
    String(Math.round(geometry.clientX + point.x * point.dpr)),
    String(Math.round(geometry.clientY + point.y * point.dpr))], { timeout: 12000, windowsHide: true });
  await eventually(() => evaluate('contextEvents'), value => value === domCount + 1, 'real DOM contextmenu event');
  if (suppressed) {
    await pause(100);
    assert.equal(events.length, count, 'preventDefault suppresses native menu delivery');
    return;
  }
  await eventually(() => events.length, value => value === count + 1, 'authenticated context-menu delivery');
  return events[count];
}

async function popup(template, owner = window) {
  const label = template[0].label;
  const child = spawn(selector, ['select', String(process.pid), label], { stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
  children.add(child);
  let output = '', errors = '', readyResolve, readyReject;
  const ready = new Promise((resolve, reject) => { readyResolve = resolve; readyReject = reject; });
  const completion = new Promise((resolve, reject) => {
    child.once('error', error => { readyReject(error); reject(error); });
    child.once('close', code => {
      children.delete(child);
      if (code === 0 && output.includes(`SELECTED ${label}`)) resolve();
      else { const error = new Error(`Native popup selection failed (${code}): ${errors || output}`); readyReject(error); reject(error); }
    });
  });
  completion.catch(() => {});
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stdout.on('data', chunk => { output += chunk; if (output.includes('READY\n') || output.includes('READY\r\n')) readyResolve(); });
  child.stderr.on('data', chunk => { errors += chunk; });
  await ready;
  Menu.buildFromTemplate(template).popup({ window: owner });
  await completion;
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  const compiler = path.join(process.env.WINDIR || 'C:\\Windows', 'Microsoft.NET', 'Framework64', 'v4.0.30319', 'csc.exe');
  selector = path.join(temporary, 'context-menu-helper.exe');
  execFileSync(compiler, ['/nologo', '/platform:x64', '/target:exe', `/out:${selector}`,
    path.join(__dirname, 'windows-context-menu-helper.cs')], { stdio: 'inherit', timeout: 30000, windowsHide: true });
  window = new BrowserWindow({ title, frame: false, alwaysOnTop: true, x: 100, y: 100, width: 640, height: 520, show: false });
  window.webContents.on('context-menu', (_event, params) => events.push(params));
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><title>${title}</title>
    <style>body{margin:20px}img,.background{width:64px;height:64px}.background{background-size:contain;background-repeat:no-repeat}#editor{height:60px;border:1px solid}textarea{display:block;margin:12px 0}#handled{width:100px;height:30px;background:#ccc}</style>
    <a href="https://example.invalid/image-link"><img id="image" src="${imageURL}"></a>
    <div id="background" class="background" style="background-image:url('${imageURL}')"></div>
    <div id="editor" contenteditable></div><textarea id="plain"></textarea><textarea id="readonly" readonly>read only</textarea><div id="handled">Custom menu</div>
    <script>window.contextEvents=0;document.addEventListener('contextmenu',()=>contextEvents++);document.getElementById('handled').addEventListener('contextmenu',event=>event.preventDefault());</script>`));
  window.show();
  execFileSync(selector, ['activate-window', String(process.pid), window.getNativeWindowHandle().readBigUInt64LE().toString()],
    { timeout: 12000, windowsHide: true });
  driver = await startNativeInput(temporary, title);
  await driver.request('clipboardSave');
  execFileSync(selector, ['activate-window', String(process.pid), window.getNativeWindowHandle().readBigUInt64LE().toString()],
    { timeout: 12000, windowsHide: true });
  await driver.request('focus');
  await eventually(() => evaluate('document.getElementById("image").complete'), Boolean, 'source image decode');

  for (const id of ['image', 'background']) {
    const params = await rightClick(id);
    assert.equal(params.mediaType, 'image');
    assert.equal(params.srcURL, imageURL);
    assert.equal(params.hasImageContents, true);
    if (id === 'image') assert.equal(params.linkURL, 'https://example.invalid/image-link');
    clipboard.writeText('copy sentinel');
    await popup([{ label: `Copy ${id}`, click: () => window.webContents.copyImageAt(params.x, params.y) }]);
    await eventually(() => clipboard.has('PNG'), Boolean, 'native image clipboard');
    const copied = nativeImage.createFromBuffer(clipboard.readBuffer('PNG'));
    assert.deepEqual(copied.getSize(), { width: 1, height: 1 });
    assert.deepEqual(copied.toBitmap(), nativeImage.createFromDataURL(imageURL).toBitmap());
  }
  await rightClick('handled', true);
  const readonly = await rightClick('readonly');
  assert.equal(readonly.isEditable, false);
  assert.equal(readonly.editFlags.canPaste, false);

  clipboard.write({ text: 'rich café 中文', html: '<b>rich café 中文</b>' });
  await evaluate('document.getElementById("editor").focus();true');
  const editable = await rightClick('editor');
  assert.equal(editable.isEditable, true);
  assert.equal(editable.editFlags.canPaste, true);
  await popup([{ label: 'Paste rich content', role: 'paste' }]);
  await eventually(() => evaluate('document.getElementById("editor").textContent'), value => value === 'rich café 中文', 'native menu rich paste');
  assert.equal(await evaluate('!!document.getElementById("editor").querySelector("b,span[style*=font-weight]")'), true);

  await evaluate('const target=document.getElementById("plain");target.value="before AFTER";target.focus();target.setSelectionRange(7,12);true');
  other = new BrowserWindow({ title: `${title}-other`, frame: false, x: 850, y: 100, width: 300, height: 240 });
  await other.loadURL('data:text/html,<textarea id="other" autofocus>other untouched</textarea>');
  await driver.request('focus', { title: `${title}-other` });
  assert.equal(BrowserWindow.getFocusedWindow(), other);
  clipboard.writeText('粘贴归属');
  await popup([{ label: 'Paste into explicit owner', role: 'paste' }]);
  await eventually(() => evaluate('document.getElementById("plain").value'), value => value === 'before 粘贴归属', 'popup owner edit target');
  assert.equal(await other.webContents.executeJavaScript('document.getElementById("other").value'), 'other untouched');
  await driver.request('focusSource');
  const source = await driver.request('pointerProbe', { x: 0, y: 0 });
  execFileSync(selector, ['activate-window', String(source.helperProcess), String(source.foreground)],
    { timeout: 12000, windowsHide: true });
  let clickedWindow;
  await popup([{ label: 'Explicit owner callback', click: (_item, owner) => { clickedWindow = owner; } }]);
  assert.equal(clickedWindow, window, 'custom menu click reports its explicit owner even from another native process');
  await finish();
}).catch(finish);
