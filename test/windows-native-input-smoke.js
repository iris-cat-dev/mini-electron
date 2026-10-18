'use strict';
// Run with out/windows-x64/runtime-dist/electron.exe on an interactive Windows desktop.
// All edits and drops use SendInput / real OLE CF_HDROP, not synthetic DOM events.
const assert = require('node:assert/strict');
const { startNativeInput } = require('./windows-native-input-driver');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, Menu, clipboard } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-windows-native-input-'));
const title = `mini-windows-native-input-${process.pid}`;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let window, driver, finished = false;
const request = (...args) => driver.request(...args);
const deadline = setTimeout(() => finish(new Error('Windows native input smoke timed out')), 120000);
app.on('window-all-closed', () => {});

async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (error && window && !window.isDestroyed()) {
    try {
      console.error('NATIVE_FAILURE_STATE ' + await evaluate(`JSON.stringify({
        active:document.activeElement&&document.activeElement.id,focused:document.hasFocus(),
        text:document.getElementById('plain')&&document.getElementById('plain').value,
        keys:window.keys,edits:window.edits,drag:window.dragState})`));
      console.error('NATIVE_FAILURE_GEOMETRY ' + JSON.stringify(await request('geometry')));
      if (process.env.MINI_NATIVE_INPUT_ARTIFACTS) {
        fs.mkdirSync(process.env.MINI_NATIVE_INPUT_ARTIFACTS, { recursive: true });
        await request('capture', { path: path.join(process.env.MINI_NATIVE_INPUT_ARTIFACTS, 'native-input-failure.png') });
      }
    } catch (diagnosticError) { console.error('Native failure diagnostics: ' + diagnosticError); }
  }
  if (driver) {
    try { await driver.close(); }
    catch (restoreError) { error = error || restoreError; }
  }
  if (window && !window.isDestroyed()) window.destroy();
  try { fs.rmSync(temporary, { recursive: true, force: true }); }
  catch (cleanupError) { error = error || cleanupError; }
  if (error) console.error(error.stack || error);
  else console.log('PASS: Windows native Unicode/rich/empty clipboard, PNG/DIB paste, keyboard policy, double-click, OLE Files and rejection boundaries');
  app.exit(error ? 1 : 0);
}
async function evaluate(source) {
  const value = await window.webContents.executeJavaScript(source);
  return value && typeof value === 'object' && 'value' in value ? value.value : value;
}
async function eventually(read, predicate, label) {
  const until = Date.now() + 5000;
  let value;
  do {
    value = await read();
    if (predicate(value)) return value;
    await pause(30);
  } while (Date.now() < until);
  assert.fail(`${label}: ${JSON.stringify(value)}`);
}
const value = source => evaluate(`JSON.stringify(${source})`).then(JSON.parse);
const key = (...keys) => request('key', { keys });
const setText = text => request('clipboardWrite', { formats: [
  { name: 'UnicodeText', data: Buffer.from(text + '\0', 'utf16le').toString('base64') },
] });
async function select(id) {
  await evaluate(`(() => { const element = document.getElementById(${JSON.stringify(id)}); element.focus();
    if ('select' in element) element.select();
    else { const range = document.createRange(); range.selectNodeContents(element);
      getSelection().removeAllRanges(); getSelection().addRange(range); } return true; })()`);
}
async function point(id) {
  const geometry = await request('geometry');
  const rect = await value(`(() => { const rect = document.getElementById(${JSON.stringify(id)}).getBoundingClientRect();
    return { x: rect.x + rect.width / 2, y: rect.y + rect.height / 2, dpr: devicePixelRatio }; })()`);
  return { x: Math.round(geometry.clientX + rect.x * rect.dpr), y: Math.round(geometry.clientY + rect.y * rect.dpr), cssX: rect.x, cssY: rect.y };
}
async function resetDrag(accept = true, hold = false) {
  await evaluate(`acceptNativeDrags=${accept}; holdFiles=${hold}; dragState={events:[],result:null}; true`);
}
async function drag(files, options = {}) {
  const location = await point('drop-target');
  const result = await request('drag', { files, x: location.x, y: location.y, ...options });
  return { ...result, location };
}
async function rejected(files, label, options = {}) {
  await resetDrag();
  const result = await drag(files, options);
  assert.equal(result.effect, 'None', label);
  await pause(120);
  assert.equal(await evaluate('dragState.result'), null, label);
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  driver = await startNativeInput(temporary, title);
  await request('clipboardSave');
  const images = await request('images');
  const png = Buffer.from(images.png, 'base64');
  const dib = Buffer.from(images.dib, 'base64');
  assert.equal(dib.readUInt32LE(0), 40); assert.equal(dib.readUInt16LE(14), 32);
  const dibV5 = Buffer.alloc(124 + dib.length - 40);
  dib.copy(dibV5, 0, 0, 40); dib.copy(dibV5, 124, 40);
  dibV5.writeUInt32LE(124, 0); dibV5.writeUInt32LE(3, 16);
  for (const [offset, mask] of [[40, 0x00ff0000], [44, 0x0000ff00], [48, 0x000000ff], [52, 0xff000000], [56, 0x73524742]]) dibV5.writeUInt32LE(mask, offset);
  const textPath = path.join(temporary, '文本 café.txt');
  const imagePath = path.join(temporary, 'native-pixel.png');
  const folderPath = path.join(temporary, 'native-folder');
  const textBytes = Buffer.from('native file café 中文 😀\n', 'utf8');
  fs.writeFileSync(textPath, textBytes); fs.writeFileSync(imagePath, png); fs.mkdirSync(folderPath);
  fs.writeFileSync(path.join(folderPath, 'nested.txt'), 'ordinary file behind a junction');
  const preload = path.join(temporary, 'preload.js');
  fs.writeFileSync(preload, `const { contextBridge, webUtils }=require('electron');
    contextBridge.exposeInMainWorld('nativeFileDrag',{getPath:file=>webUtils.getPathForFile(file)});`);
  window = new BrowserWindow({ x: 180, y: 70, width: 900, height: 650, title,
    webPreferences: { preload, sandbox: true, contextIsolation: true, nodeIntegration: false } });
  const actions = { zoom: 0, disabled: 0, unregistered: 0, accelerator: null };
  const applicationMenu = Menu.buildFromTemplate([
    { label: 'Edit', submenu: ['undo', 'redo', 'cut', 'copy', 'paste', 'selectAll'].map(role => ({ role })) },
    { label: 'Probe', submenu: [
      { label: 'Zoom', accelerator: 'Ctrl+=', click: (item, focused, event) => {
        assert.equal(focused, window); actions.zoom++; actions.accelerator = event.triggeredByAccelerator;
        window.webContents.setZoomLevel(window.webContents.getZoomLevel() + 0.5);
      } },
      { role: 'resetZoom' },
      { label: 'Disabled', accelerator: 'Ctrl+J', enabled: false, click: () => actions.disabled++ },
      { label: 'Unregistered', accelerator: 'Ctrl+K', registerAccelerator: false, click: () => actions.unregistered++ },
    ] },
  ]);
  Menu.setApplicationMenu(applicationMenu);
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><meta charset="utf-8"><title>${title}</title>
    <style>body{background:#fff;font:16px sans-serif;margin:20px}textarea{width:340px;height:75px;display:block;margin:8px}
    #rich,#target{width:340px;min-height:35px;border:1px solid;margin:8px}#drop-target{position:absolute;left:430px;top:210px;width:300px;height:230px;background:#ddd}
    #click-target{position:absolute;left:440px;top:90px;width:280px;height:60px;background:#dde}</style>
    <textarea id="plain">café 中文 😀 clipboard</textarea><div id="rich" contenteditable><b>rich café 中文 😀</b></div>
    <div id="target" contenteditable></div><div id="empty" contenteditable>empty copy</div><textarea id="paste-target">old value</textarea>
    <div id="click-target">Native delayed double-click</div><div id="drop-target">Real native file/folder drop target</div>
    <script>window.keys=[];window.pastes=[];window.clicks=[];window.acceptNativeDrags=true;window.holdFiles=false;
    window.dragState={events:[],result:null};window.savedFiles=[];
    window.edits=[];for(const type of ['beforeinput','input','compositionstart','compositionupdate','compositionend'])
      addEventListener(type,event=>edits.push({type,data:event.data,inputType:event.inputType,target:event.target.id}));
    for(const type of ['keydown','keyup'])addEventListener(type,event=>keys.push({type,key:event.key,code:event.code,ctrl:event.ctrlKey,shift:event.shiftKey,alt:event.altKey,repeat:event.repeat,location:event.location}));
    document.getElementById('empty').addEventListener('copy',event=>{event.clipboardData.setData('text/plain','');event.preventDefault()});
    document.getElementById('click-target').addEventListener('click',event=>clicks.push(event.detail));
    addEventListener('paste',async event=>{const types=Array.from(event.clipboardData.types),text=event.clipboardData.getData('text/plain');
      const files=await Promise.all(Array.from(event.clipboardData.files).map(async file=>{const bytes=new Uint8Array(await file.arrayBuffer());
        return {type:file.type,size:file.size,header:Array.from(bytes.slice(0,24))}}));pastes.push({types,text,files})});
    for(const type of ['dragenter','dragover','dragleave'])addEventListener(type,event=>{dragState.events.push(type);
      if(type!=='dragleave'&&acceptNativeDrags&&event.target.id==='drop-target')event.preventDefault()});
    addEventListener('drop',async event=>{dragState.events.push('drop');if(acceptNativeDrags&&event.target.id==='drop-target')event.preventDefault();
      const files=Array.from(event.dataTransfer.files),types=Array.from(event.dataTransfer.types);savedFiles=files;
      const entries=Array.from(event.dataTransfer.items).map(item=>{const entry=item.webkitGetAsEntry();return entry&&{name:entry.name,isFile:entry.isFile,isDirectory:entry.isDirectory}});
      try{const described=await Promise.all(files.map(async file=>({name:file.name,type:file.type,size:file.size,path:nativeFileDrag.getPath(file),bytes:holdFiles?null:Array.from(new Uint8Array(await file.arrayBuffer()))})));
      dragState.result={target:event.target.id,x:event.clientX,y:event.clientY,types,files:described,entries,
        forgedPath:nativeFileDrag.getPath(new File(['forged'],'native-pixel.png',{type:'image/png'}))}}catch(error){dragState.result={error:String(error)}}});
    </script>`));
  window.show(); window.focus();
  await eventually(() => window.isFocused(), value => value, 'fixture focus');
  const plainText = 'café 中文 😀 clipboard';
  await evaluate('plain.focus();plain.setSelectionRange(3,3);true'); await key(17, 65);
  await eventually(() => evaluate('plain.selectionEnd-plain.selectionStart'), count => count === plainText.length, 'Ctrl+A selection');
  await setText('stale copy sentinel'); await key(17, 67);
  await eventually(() => request('clipboardRead'), data => data.text === plainText, 'Ctrl+C native Unicode clipboard');
  await setText('stale cut sentinel'); await key(17, 88);
  await eventually(() => request('clipboardRead'), data => data.text === plainText, 'Ctrl+X must replace native clipboard');
  await eventually(() => evaluate('plain.value'), text => text === '', 'Ctrl+X deletion');
  await key(17, 90);
  await eventually(() => evaluate('plain.value'), text => text === plainText, 'Ctrl+Z');
  await key(17, 89);
  await eventually(() => evaluate('plain.value'), text => text === '', 'Ctrl+Y');
  await key(17, 86);
  await eventually(() => evaluate('plain.value'), text => text === plainText, 'Ctrl+V round trip');
  console.log('PASS: real Ctrl+A/C/X/Z/Y/V Unicode round trip');

  await select('rich'); await key(17, 67);
  const richClipboard = await eventually(() => request('clipboardRead'), data => data.text === 'rich café 中文 😀' && data.formats.includes(data.html), 'atomic rich copy');
  assert.ok(clipboard.readHTML().includes('rich café 中文 😀'));
  await select('target'); await key(17, 86);
  await eventually(() => evaluate('target.textContent'), text => text === richClipboard.text, 'rich paste');
  assert.equal(await evaluate('!!target.querySelector("b,span[style*=font-weight]")'), true);
  await select('plain'); await key(17, 88);
  const textOnly = await eventually(() => request('clipboardRead'), data => data.text === plainText, 'independent text-only commit');
  assert.equal(textOnly.formats.includes(textOnly.html), false, 'old HTML must not survive a text-only commit');
  await select('empty'); await key(17, 67);
  const empty = await eventually(() => request('clipboardRead'), data => data.text === '', 'empty text copy');
  assert.ok(empty.formats.includes(13)); assert.equal(empty.formats.includes(empty.html), false);
  await select('paste-target'); await setText('external λ 原生粘贴 😀\r\nnext'); await key(17, 86);
  await eventually(() => evaluate('document.getElementById("paste-target").value'), text => text === 'external λ 原生粘贴 😀\nnext', 'external native text paste');
  console.log('PASS: atomic rich copy, independent/empty commits and external text paste');
  for (const [name, formats] of [
    ['PNG', [{ name: 'PNG', data: images.png }]],
    ['DIB', [{ name: 'DIB', data: images.dib }]],
    ['DIBV5', [{ name: 'DIBV5', data: dibV5.toString('base64') }]],
    ['invalid PNG with valid DIB', [{ name: 'PNG', data: Buffer.from('invalid PNG').toString('base64') }, { name: 'DIB', data: images.dib }]],
  ]) {
    await select('target'); await evaluate('pastes=[];true');
    await request('clipboardWrite', { formats }); await key(17, 86);
    const pasted = await eventually(() => value('pastes'), pastes => pastes.length === 1, `${name} paste event`);
    assert.ok(pasted[0].types.includes('Files'));
    assert.equal(pasted[0].files.length, 1, `${name}: actual image File`);
    assert.equal(pasted[0].files[0].type, 'image/png');
    assert.deepEqual(pasted[0].files[0].header.slice(0, 8), [137, 80, 78, 71, 13, 10, 26, 10]);
    assert.deepEqual(pasted[0].files[0].header.slice(16, 24), [0, 0, 0, 16, 0, 0, 0, 16]);
    console.log(`PASS: native ${name} image paste`);
  }
  const oversizedPng = Buffer.from(png);
  oversizedPng.writeUInt32BE(10000, 16); oversizedPng.writeUInt32BE(10000, 20);
  let crc = 0xffffffff;
  for (const byte of oversizedPng.subarray(12, 29)) {
    crc ^= byte;
    for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  oversizedPng.writeUInt32BE((crc ^ 0xffffffff) >>> 0, 29);
  const oversizedDib = Buffer.from(dib);
  oversizedDib.writeInt32LE(10000, 4); oversizedDib.writeInt32LE(10000, 8);
  for (const [label, name, bytes] of [
    ['malformed PNG', 'PNG', Buffer.from('malformed PNG')],
    ['oversized PNG', 'PNG', oversizedPng],
    ['oversized DIB', 'DIB', oversizedDib],
  ]) {
    await select('target'); await evaluate('pastes=[];true');
    await request('clipboardWrite', { formats: [{ name, data: bytes.toString('base64') }] });
    await key(17, 86);
    const rejected = await eventually(() => value('pastes'), pastes => pastes.length === 1, `${label} paste`);
    assert.deepEqual(rejected[0].files, [], label);
    console.log(`PASS: ${label} rejection`);
  }
  console.log('PASS: native PNG/DIB/DIBV5 images, DIB fallback and malformed/oversized rejection');

  await select('plain'); await evaluate('keys=[];plain.value="";true');
  await key(17, 75); await key(17, 188); await key(17, 74);
  const keys = await eventually(() => value('keys'), events =>
    events.some(event => event.type === 'keyup' && event.code === 'KeyJ') &&
    events.some(event => event.type === 'keyup' && event.key === 'Control' && !event.ctrl), 'DOM shortcut key-up');
  for (const [keyName, code] of [['k', 'KeyK'], [',', 'Comma'], ['j', 'KeyJ']]) {
    assert.ok(keys.some(event => event.type === 'keydown' && event.key === keyName && event.code === code && event.ctrl), JSON.stringify(keys));
    assert.ok(keys.some(event => event.type === 'keyup' && event.key === keyName && event.code === code && event.ctrl), JSON.stringify(keys));
  }
  assert.ok(keys.some(event => event.type === 'keyup' && event.key === 'Control' && !event.ctrl));
  assert.ok(keys.filter(event => event.type === 'keyup').every(event => !event.repeat), 'key releases are never auto-repeat');
  assert.equal(actions.unregistered, 0); assert.equal(actions.disabled, 0);
  await key(16, 65);
  const shifted = await eventually(() => value('keys'), events =>
    events.some(event => event.type === 'keyup' && event.code === 'KeyA'), 'shifted native key delivery');
  for (const type of ['keydown', 'keyup']) {
    assert.ok(shifted.some(event => event.type === type && event.key === 'A' && event.code === 'KeyA' && event.shift));
  }
  // Pinyin may consume physical Shift+A without committing text, just like a
  // native Windows textbox. VK_PACKET verifies committed text independently.
  await evaluate('plain.value="";plain.setSelectionRange(0,0);true');
  await request('unicode', { text: 'A λ中文😀' });
  await eventually(() => evaluate('plain.value'), text => text === 'A λ中文😀', 'Unicode SendInput');
  await key(36); await key(35); await key(37);
  const navigation = await value('keys');
  for (const name of ['Home', 'End', 'ArrowLeft']) assert.ok(navigation.some(event => event.key === name && event.code === name));
  await key(17, 187);
  await eventually(() => actions.zoom, count => count === 1, 'registered zoom callback');
  assert.equal(actions.accelerator, true); assert.equal(window.webContents.getZoomLevel(), 0.5);
  await key(17, 48);
  await eventually(() => window.webContents.getZoomLevel(), zoom => zoom === 0, 'Ctrl+0 reset');
  const zoomItem = applicationMenu.items[1].submenu.items[0];
  zoomItem.enabled = false;
  await evaluate('keys=[];true'); await key(17, 187);
  await eventually(() => value('keys'), events =>
    events.some(event => event.type === 'keydown' && event.code === 'Equal' && event.ctrl) &&
    events.some(event => event.type === 'keyup' && event.code === 'Equal'), 'dynamically disabled accelerator forwarding');
  assert.equal(actions.zoom, 1); assert.equal(window.webContents.getZoomLevel(), 0);
  zoomItem.enabled = true;
  const clickPoint = await point('click-target');
  await request('doubleClick', clickPoint);
  await eventually(() => value('clicks'), clicks => clicks.length === 2, 'native delayed double-click');
  assert.deepEqual(await value('clicks'), [1, 2], 'honor the Windows double-click interval on both down and up');
  console.log('PASS: DOM key/code/modifiers/key-up, accelerator policy and delayed double-click');

  const paths = [textPath, imagePath, folderPath];
  const control = await drag(paths, { control: true });
  assert.equal(control.effect, 'Copy'); assert.equal(control.controlAccepted, 3, 'native CF_HDROP positive control');
  await resetDrag();
  const nativeDrop = await drag(paths);
  assert.equal(nativeDrop.effect, 'Copy');
  const dropped = await eventually(() => value('dragState'), state => state.result !== null, 'brokered native drop');
  assert.equal(dropped.result.error, undefined); assert.equal(dropped.result.target, 'drop-target');
  assert.ok(dropped.events.includes('dragenter') && dropped.events.includes('dragover') && dropped.events.includes('drop'));
  assert.ok(Math.abs(dropped.result.x - nativeDrop.location.cssX) < 1);
  assert.ok(Math.abs(dropped.result.y - nativeDrop.location.cssY) < 1);
  assert.ok(dropped.result.types.includes('Files'));
  assert.equal(dropped.result.forgedPath, '');
  assert.deepEqual(dropped.result.files.map(file => file.name), paths.map(file => path.basename(file)));
  assert.deepEqual(dropped.result.files.map(file => file.path), paths.map(file => fs.realpathSync(file)));
  assert.deepEqual(dropped.result.files[0].bytes, Array.from(textBytes));
  assert.deepEqual(dropped.result.files[1].bytes, Array.from(png));
  assert.deepEqual(dropped.result.files[2].bytes, []);
  assert.equal(dropped.result.files[1].type, 'image/png');
  assert.deepEqual(dropped.result.entries.map(entry => [entry.isFile, entry.isDirectory]), [[true, false], [true, false], [false, true]]);
  await resetDrag(false);
  assert.equal((await drag(paths)).effect, 'None', 'non-preventDefault target rejection');
  assert.equal(await evaluate('dragState.result'), null);
  await resetDrag();
  assert.equal((await drag(paths, { cancel: true })).effect, 'None');
  await eventually(() => value('dragState.events'), events => events.includes('dragleave'), 'native drag cancellation');
  assert.equal(await evaluate('dragState.result'), null);
  await rejected([], 'non-file IDataObject rejection', { nonFile: true });
  const crowded = path.join(temporary, 'too-many-files'); fs.mkdirSync(crowded);
  const excessiveEntries = Array.from({ length: 1025 }, (_, index) => {
    const file = path.join(crowded, `${index}.txt`); fs.writeFileSync(file, ''); return file;
  });
  await rejected(excessiveEntries, '1024 native-entry count boundary');
  const oversized = path.join(temporary, 'oversized.bin');
  fs.writeFileSync(oversized, ''); fs.truncateSync(oversized, 50 * 1024 * 1024 + 1);
  await rejected([oversized], '50 MiB per-file boundary');
  const aggregate = [];
  for (let i = 0; i < 6; ++i) {
    const file = path.join(temporary, `aggregate-${i}.bin`); fs.writeFileSync(file, ''); fs.truncateSync(file, 45 * 1024 * 1024); aggregate.push(file);
  }
  await rejected(aggregate, '256 MiB aggregate boundary');
  const junction = path.join(temporary, 'reparse-folder'); fs.symlinkSync(folderPath, junction, 'junction');
  await rejected([junction], 'Windows reparse point rejection');
  await rejected([path.join(junction, 'nested.txt')], 'ancestor Windows reparse point rejection');
  console.log('PASS: real OLE Unicode files/folder, authorized bytes/paths and rejection boundaries');
  const originalDpr = await evaluate('devicePixelRatio');
  window.webContents.setZoomFactor(1.2);
  await eventually(() => evaluate('devicePixelRatio'), dpr => Math.abs(dpr - originalDpr * 1.2) < 0.01, 'zoom commit');
  await resetDrag();
  const zoomedDrop = await drag([imagePath]);
  assert.equal(zoomedDrop.effect, 'Copy');
  const zoomed = await eventually(() => value('dragState.result'), result => result !== null, 'zoomed native drop');
  assert.equal(zoomed.target, 'drop-target'); assert.deepEqual(zoomed.files[0].bytes, Array.from(png));
  assert.ok(Math.abs(zoomed.x - zoomedDrop.location.cssX) < 1 && Math.abs(zoomed.y - zoomedDrop.location.cssY) < 1);
  await resetDrag(true, true);
  assert.equal((await drag([textPath])).effect, 'Copy');
  await eventually(() => value('dragState.result'), result => result !== null, 'held native File');
  fs.writeFileSync(textPath, 'changed after authorization: stale native file');
  await evaluate(`window.heldRead=null;savedFiles[0].arrayBuffer().then(
    bytes=>{heldRead=Array.from(new Uint8Array(bytes))},
    error=>{heldRead={error:error.name}});true`);
  const snapshot = await eventually(() => value('heldRead'), result => result !== null, 'immutable dropped File snapshot');
  assert.deepEqual(snapshot, Array.from(textBytes), 'a materialized File must not read new host bytes after the grant is revoked');
  console.log('PASS: zoomed OLE coordinates and immutable authorized File snapshots');
  if (process.env.MINI_NATIVE_INPUT_ARTIFACTS) {
    await request('focus');
    await request('capture', { path: path.join(process.env.MINI_NATIVE_INPUT_ARTIFACTS, 'native-input-success.png') });
  }
}).then(() => finish(), finish);
