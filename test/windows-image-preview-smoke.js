'use strict';

// Run with out/windows-x64/runtime-dist/electron.exe test/windows-image-preview-smoke.js
// on an interactive Windows desktop. The final click is real Win32 SendInput, not synthetic DOM input.
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const zlib = require('node:zlib');
const { app, BrowserWindow, ipcMain } = require('electron');
const { startNativeInput } = require('./windows-native-input-driver');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-windows-image-preview-'));
app.setPath('userData', path.join(temporary, 'user-data'));
const savedPath = path.join(temporary, 'persisted-preview.png');
const title = `mini-windows-image-preview-${process.pid}`;
const artifacts = process.env.MINI_NATIVE_INPUT_ARTIFACTS;
const artifactStem = `windows-image-preview-${process.pid}`;
const width = 384, height = 320;
const uploadPrefix = 37, uploadSuffix = 23;
const replyPrefix = 29, replySuffix = 31;
const uploadPrefixByte = 0xd3, uploadSuffixByte = 0x6c;
const replyPrefixByte = 0xa5, replySuffixByte = 0x5a;
const pause = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
const mainEvents = [];
const evidence = {
  title,
  expected: { dimensions: [width, height], uploadPrefix, uploadSuffix, replyPrefix, replySuffix },
  mainEvents,
};
let window, driver, finished = false, phase = 'startup';
const request = (...args) => driver.request(...args);
const watchdog = setTimeout(() => void finish(new Error(`Windows image preview smoke timed out during ${phase}`)), 60000);
app.on('window-all-closed', () => {});

const crcTable = Uint32Array.from({ length: 256 }, (_, index) => {
  let crc = index;
  for (let bit = 0; bit < 8; ++bit) crc = crc & 1 ? 0xedb88320 ^ (crc >>> 1) : crc >>> 1;
  return crc >>> 0;
});
function chunk(type, data) {
  const body = Buffer.concat([Buffer.from(type), data]);
  let crc = 0xffffffff;
  for (const byte of body) crc = crcTable[(crc ^ byte) & 255] ^ (crc >>> 8);
  const header = Buffer.alloc(4), trailer = Buffer.alloc(4);
  header.writeUInt32BE(data.length);
  trailer.writeUInt32BE((crc ^ 0xffffffff) >>> 0);
  return Buffer.concat([header, body, trailer]);
}
const pixels = Buffer.alloc((width * 4 + 1) * height);
for (let y = 0; y < height; ++y) {
  for (let x = 0; x < width; ++x) {
    const offset = y * (width * 4 + 1) + 1 + x * 4;
    pixels[offset] = x % 256;
    pixels[offset + 1] = y % 256;
    pixels[offset + 2] = (x * 3 + y * 5) % 256;
    pixels[offset + 3] = 255;
  }
}
const header = Buffer.alloc(13);
header.writeUInt32BE(width); header.writeUInt32BE(height, 4);
header[8] = 8; header[9] = 6;
const generatedPng = Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
  chunk('IHDR', header), chunk('IDAT', zlib.deflateSync(pixels, { level: 0 })), chunk('IEND', Buffer.alloc(0))]);
// Own an exact-size backing store so payload-bound assertions never depend on Buffer pool placement.
const pngBytes = Uint8Array.from(generatedPng);
const png = Buffer.from(pngBytes.buffer, pngBytes.byteOffset, pngBytes.byteLength);
const expectedSha256 = crypto.createHash('sha256').update(png).digest('hex');
assert.equal(pngBytes.byteOffset, 0);
assert.equal(pngBytes.buffer.byteLength, pngBytes.byteLength);
assert.ok(pngBytes.byteLength > 65536, 'Image fixture must exceed the old indexed-dictionary boundary');
evidence.expected.bytes = pngBytes.byteLength;
evidence.expected.sha256 = expectedSha256;
evidence.expected.storage = { byteOffset: pngBytes.byteOffset, backingBytes: pngBytes.buffer.byteLength };
if (artifacts) {
  fs.mkdirSync(artifacts, { recursive: true });
  const sourcePath = path.join(artifacts, 'windows-large-preview-source.png');
  fs.writeFileSync(sourcePath, png);
  evidence.sourceImage = sourcePath;
}

async function evaluate(source) {
  return window.webContents.executeJavaScript(source);
}
const value = source => evaluate(`JSON.stringify(${source})`).then(JSON.parse);
async function eventually(read, predicate, label, timeout = 6000) {
  const until = Date.now() + timeout;
  let result;
  do {
    result = await read();
    if (predicate(result)) return result;
    await pause(30);
  } while (Date.now() < until);
  assert.fail(`${label}: ${JSON.stringify(result)}`);
}
async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (error && artifacts && driver && window && !window.isDestroyed()) {
    try {
      fs.mkdirSync(artifacts, { recursive: true });
      await request('focus');
      const capturePath = path.join(artifacts, `${artifactStem}-failure.png`);
      await request('capture', { path: capturePath });
      evidence.failureCapture = capturePath;
    } catch (diagnosticError) {
      evidence.diagnosticError = String(diagnosticError.stack || diagnosticError);
    }
  }
  if (driver) {
    try { await driver.close(); }
    catch (cleanupError) { error = error || cleanupError; }
  }
  ipcMain.removeHandler('preview:save');
  ipcMain.removeHandler('preview:read');
  ipcMain.removeAllListeners('preview:phase');
  if (window && !window.isDestroyed()) window.destroy();
  try { fs.rmSync(temporary, { recursive: true, force: true }); }
  catch (cleanupError) { error = error || cleanupError; }
  evidence.phase = phase;
  evidence.passed = !error;
  evidence.error = error ? String(error.stack || error) : null;
  if (artifacts) {
    try {
      fs.mkdirSync(artifacts, { recursive: true });
      fs.writeFileSync(path.join(artifacts, `${artifactStem}.json`), JSON.stringify(evidence, null, 2));
    } catch (artifactError) {
      error = error || artifactError;
      evidence.passed = false;
      evidence.error = String(error.stack || error);
    }
  }
  console.log('WINDOWS_IMAGE_PREVIEW_EVIDENCE ' + JSON.stringify(evidence));
  if (error) console.error(error.stack || error);
  else console.log('PASS: Windows nested sliced binary IPC, exact persisted image, Blob decode, timer progress and trusted native click');
  app.exit(error ? 1 : 0);
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  phase = 'native helper startup';
  driver = await startNativeInput(temporary, title);
  const preload = path.join(temporary, 'preload.js');
  fs.writeFileSync(preload, `const {contextBridge,ipcRenderer}=require('electron');
    contextBridge.exposeInMainWorld('imagePreview', {
      save: input => ipcRenderer.invoke('preview:save', input),
      read: () => ipcRenderer.invoke('preview:read'),
      phase: value => ipcRenderer.send('preview:phase', value)
    });`);

  ipcMain.on('preview:phase', (event, rendererPhase) => {
    try {
      assert.equal(event.sender, window.webContents);
      phase = `renderer ${rendererPhase}`;
      mainEvents.push({ type: 'renderer-phase', phase: rendererPhase });
    } catch (error) { void finish(error); }
  });
  ipcMain.handle('preview:save', async (event, input) => {
    assert.equal(event.sender, window.webContents);
    const transfer = input.request.attachment.binary;
    const bytes = transfer.bytes;
    assert.ok(bytes instanceof Uint8Array, 'Nested image bytes must remain a Uint8Array, not an indexed dictionary');
    assert.equal(transfer.kind, 'sliced-uint8array');
    assert.equal(bytes.byteLength, pngBytes.byteLength);
    assert.equal(bytes.byteOffset, uploadPrefix, 'IPC must preserve the deliberate nonzero view offset');
    assert.equal(bytes.buffer.byteLength, uploadPrefix + pngBytes.byteLength + uploadSuffix,
      'IPC must preserve the deliberate backing-store bounds');
    const backing = new Uint8Array(bytes.buffer);
    for (let index = 0; index < uploadPrefix; ++index) assert.equal(backing[index], uploadPrefixByte);
    for (let index = bytes.byteOffset + bytes.byteLength; index < backing.length; ++index) {
      assert.equal(backing[index], uploadSuffixByte);
    }
    const payload = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    assert.deepEqual(payload, png, 'Only the sliced view payload may be persisted');
    const receivedSha256 = crypto.createHash('sha256').update(payload).digest('hex');
    assert.equal(receivedSha256, expectedSha256);
    await fs.promises.writeFile(savedPath, payload);
    const disk = await fs.promises.readFile(savedPath);
    const diskSha256 = crypto.createHash('sha256').update(disk).digest('hex');
    assert.equal(disk.length, pngBytes.byteLength);
    assert.equal(diskSha256, expectedSha256);
    assert.deepEqual(disk, png);
    evidence.upload = {
      bytes: bytes.byteLength,
      byteOffset: bytes.byteOffset,
      backingBytes: bytes.buffer.byteLength,
      prefixBytes: uploadPrefix,
      suffixBytes: uploadSuffix,
      sha256: receivedSha256,
    };
    evidence.persisted = { bytes: disk.length, sha256: diskSha256 };
    mainEvents.push({ type: 'persisted', bytes: disk.length, sha256: diskSha256 });
    return { persisted: { bytes: disk.length, sha256: diskSha256 } };
  });
  ipcMain.handle('preview:read', async event => {
    assert.equal(event.sender, window.webContents);
    const disk = await fs.promises.readFile(savedPath);
    const backing = new Uint8Array(replyPrefix + disk.length + replySuffix);
    backing.fill(replyPrefixByte, 0, replyPrefix);
    backing.set(disk, replyPrefix);
    backing.fill(replySuffixByte, replyPrefix + disk.length);
    const bytes = backing.subarray(replyPrefix, replyPrefix + disk.length);
    assert.equal(bytes.byteOffset, replyPrefix);
    assert.equal(bytes.byteLength, disk.length);
    assert.equal(bytes.buffer.byteLength, replyPrefix + disk.length + replySuffix);
    evidence.replySource = {
      bytes: bytes.byteLength,
      byteOffset: bytes.byteOffset,
      backingBytes: bytes.buffer.byteLength,
      prefixBytes: replyPrefix,
      suffixBytes: replySuffix,
      sha256: crypto.createHash('sha256').update(bytes).digest('hex'),
    };
    mainEvents.push({ type: 'nested-binary-reply', bytes: bytes.byteLength, byteOffset: bytes.byteOffset });
    return { result: { image: { payload: { bytes }, mime: 'image/png' } } };
  });

  window = new BrowserWindow({
    x: 280, y: 90, width: 700, height: 620, title, show: true, alwaysOnTop: true,
    webPreferences: { preload, sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  phase = 'document load';
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><meta charset="utf-8"><title>${title}</title>
    <style>
      html,body{margin:0;background:#181c22;color:#f5f7fa;font:16px system-ui,sans-serif}
      main{padding:22px}img{display:block;width:${width}px;height:${height}px;background:#303641;border:2px solid #768296}
      button{display:block;margin-top:20px;width:250px;height:52px;font:17px system-ui,sans-serif}
      #status{margin-top:12px}
    </style><main><img id="preview" alt="decoded preview"><button id="respond" type="button">Native response check</button>
    <div id="status">Preparing preview</div></main><script>
      window.previewState={phase:'loaded',ticks:0,events:[{type:'script-ready'}],clicks:[]};
      setInterval(()=>previewState.ticks++,20);
      document.getElementById('respond').addEventListener('pointerdown',event=>previewState.events.push({
        type:'pointerdown',isTrusted:event.isTrusted,pointerType:event.pointerType,button:event.button
      }));
      document.getElementById('respond').addEventListener('click',event=>{
        const row={type:'click',isTrusted:event.isTrusted,detail:event.detail,clientX:event.clientX,clientY:event.clientY};
        previewState.clicks.push(row);previewState.events.push(row);document.getElementById('status').textContent='Native click received';
      });
    </script>`));
  window.show();
  window.focus();
  await request('focus');

  phase = 'renderer upload';
  const encodedPng = png.toString('base64');
  await evaluate(`(() => { (async () => { let objectUrl; try {
    const source=Uint8Array.from(atob(${JSON.stringify(encodedPng)}),value=>value.charCodeAt(0));
    if(source.length!==${pngBytes.byteLength})throw new Error('Renderer source byte length differs from fixture');
    const backing=new Uint8Array(${uploadPrefix}+source.length+${uploadSuffix});
    backing.fill(${uploadPrefixByte},0,${uploadPrefix});backing.set(source,${uploadPrefix});backing.fill(${uploadSuffixByte},${uploadPrefix}+source.length);
    const sliced=backing.subarray(${uploadPrefix},${uploadPrefix}+source.length);
    if(sliced.byteOffset!==${uploadPrefix}||sliced.byteLength!==source.length||sliced.buffer.byteLength!==backing.length)throw new Error('Upload slice bounds are not deliberate');
    previewState.uploadSource={bytes:sliced.byteLength,byteOffset:sliced.byteOffset,backingBytes:sliced.buffer.byteLength};
    previewState.events.push({type:'upload-start',ticks:previewState.ticks});imagePreview.phase('upload-start');
    const saved=await imagePreview.save({request:{attachment:{binary:{kind:'sliced-uint8array',bytes:sliced}}}});
    if(saved.persisted.bytes!==source.length||saved.persisted.sha256!==${JSON.stringify(expectedSha256)})throw new Error('Main persistence evidence differs');
    previewState.events.push({type:'upload-complete',ticks:previewState.ticks});imagePreview.phase('read-start');
    const response=await imagePreview.read();const bytes=response.result.image.payload.bytes;
    if(!(bytes instanceof Uint8Array))throw new Error('Nested binary reply lost its Uint8Array type');
    if(bytes.byteOffset!==${replyPrefix}||bytes.byteLength!==source.length||bytes.buffer.byteLength!==${replyPrefix}+source.length+${replySuffix})throw new Error('Nested binary reply lost sliced backing bounds');
    const replyBacking=new Uint8Array(bytes.buffer);
    for(let index=0;index<${replyPrefix};++index)if(replyBacking[index]!==${replyPrefixByte})throw new Error('Reply prefix sentinel changed');
    for(let index=bytes.byteOffset+bytes.byteLength;index<replyBacking.length;++index)if(replyBacking[index]!==${replySuffixByte})throw new Error('Reply suffix sentinel changed');
    if(bytes.length!==source.length||!bytes.every((value,index)=>value===source[index]))throw new Error('Nested binary reply bytes changed');
    const blob=new Blob([bytes],{type:response.result.image.mime});objectUrl=URL.createObjectURL(blob);
    const blobBytes=new Uint8Array(await (await fetch(objectUrl)).arrayBuffer());
    if(blobBytes.length!==source.length||!blobBytes.every((value,index)=>value===source[index]))throw new Error('Blob URL bytes changed');
    const image=document.getElementById('preview');image.src=objectUrl;await image.decode();
    if(image.naturalWidth!==${width}||image.naturalHeight!==${height})throw new Error('Decoded image dimensions changed');
    previewState.reply={bytes:bytes.byteLength,byteOffset:bytes.byteOffset,backingBytes:bytes.buffer.byteLength};
    previewState.blob={bytes:blobBytes.byteLength,type:blob.type};
    previewState.dimensions=[image.naturalWidth,image.naturalHeight];previewState.phase='ready';
    previewState.events.push({type:'image-decoded',ticks:previewState.ticks,dimensions:previewState.dimensions});imagePreview.phase('ready');
    }catch(error){previewState.error=String(error.stack||error);previewState.phase='error';imagePreview.phase('error');}
    finally{if(objectUrl)URL.revokeObjectURL(objectUrl);}
  })();return true;})()`);
  const readyState = await eventually(() => value('previewState'), state => {
    if (state.error) throw new Error(state.error);
    return state.phase === 'ready';
  }, 'decoded image preview');
  assert.deepEqual(readyState.dimensions, [width, height]);
  assert.equal(readyState.blob.bytes, pngBytes.byteLength);
  assert.deepEqual(await fs.promises.readFile(savedPath), png);

  phase = 'native responsiveness';
  const geometry = await request('geometry');
  const button = await value(`(() => {const rect=document.getElementById('respond').getBoundingClientRect();return {
    x:rect.x+rect.width/2,y:rect.y+rect.height/2,dpr:devicePixelRatio,ticks:previewState.ticks};})()`);
  const clickPoint = {
    x: Math.round(geometry.clientX + button.x * button.dpr),
    y: Math.round(geometry.clientY + button.y * button.dpr),
  };
  assert.ok(clickPoint.x >= geometry.clientX && clickPoint.x < geometry.clientX + geometry.clientWidth);
  assert.ok(clickPoint.y >= geometry.clientY && clickPoint.y < geometry.clientY + geometry.clientHeight);
  await request('click', clickPoint);
  const clickedState = await eventually(() => value('previewState'), state => state.clicks.length === 1,
    'trusted native preview response click');
  assert.equal(clickedState.clicks[0].isTrusted, true);
  assert.equal(clickedState.clicks[0].detail, 1);
  assert.ok(clickedState.events.some(event => event.type === 'pointerdown' && event.isTrusted === true));
  await pause(140);
  const finalState = await value('previewState');
  assert.ok(finalState.ticks > clickedState.ticks, 'Renderer timer must progress after Blob decode and native input');
  assert.ok(finalState.ticks > button.ticks, 'Renderer timer must progress after preview becomes ready');
  evidence.renderer = {
    uploadSource: finalState.uploadSource,
    reply: finalState.reply,
    blob: finalState.blob,
    dimensions: finalState.dimensions,
    events: finalState.events,
    ticksBeforeClick: button.ticks,
    ticksAfterClick: finalState.ticks,
  };
  evidence.native = { geometry, clickPoint, click: finalState.clicks[0] };
  if (artifacts) {
    phase = 'artifact capture';
    fs.mkdirSync(artifacts, { recursive: true });
    await request('focus');
    const capturePath = path.join(artifacts, `${artifactStem}.png`);
    await request('capture', { path: capturePath });
    evidence.capture = capturePath;
  }
  phase = 'complete';
}).then(() => finish(), finish);
