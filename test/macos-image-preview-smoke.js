'use strict';

// Invoke: /path/to/mini-electron --app=test/macos-image-preview-smoke.js
// Exercises actual sandbox/contextBridge IPC, disk persistence, Blob URL image
// decoding and renderer mouse-input response above the IPC dictionary limit.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const zlib = require('node:zlib');
const { app, BrowserWindow, ipcMain } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-image-preview-'));
const title = `mini-image-preview-${process.pid}`;
const width = 384, height = 320;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let window, finished = false, phase = 'startup';
const watchdog = setTimeout(() => finish(new Error(`Image preview smoke timed out at ${phase}`)), 30000);
app.on('window-all-closed', () => {});
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: nested large Uint8Array IPC, exact persisted/read image bytes, Blob URL image decode and renderer input response');
  app.exit(error ? 1 : 0);
}
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
    pixels[offset + 2] = (x + y) % 256;
    pixels[offset + 3] = 255;
  }
}
const header = Buffer.alloc(13);
header.writeUInt32BE(width); header.writeUInt32BE(height, 4);
header[8] = 8; header[9] = 6;
// Uncompressed IDAT makes the regression independent of encoder compression:
// the binary payload must exceed 65536 entries without enumerating image bytes.
const png = Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
  chunk('IHDR', header), chunk('IDAT', zlib.deflateSync(pixels, { level: 0 })), chunk('IEND', Buffer.alloc(0))]);
const savedPath = path.join(temporary, 'preview.png');
const evaluate = async source => (await window.webContents.executeJavaScript(source)).value;
async function eventually(label, predicate) {
  let last;
  for (let turn = 0; turn < 100; ++turn) {
    last = await predicate();
    if (last === true) return;
    await pause(25);
  }
  throw new Error(`${label}: ${JSON.stringify(last)}`);
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const preload = path.join(temporary, 'preload.js');
  fs.writeFileSync(preload, `const {contextBridge,ipcRenderer}=require('electron');
    contextBridge.exposeInMainWorld('imagePreview', {
      save: input=>ipcRenderer.invoke('preview:save', input),
      read: ()=>ipcRenderer.invoke('preview:read'),
      phase: value=>ipcRenderer.send('preview:phase', value)
    });`);
  ipcMain.on('preview:phase', (_event, value) => { phase = value; console.log(`Image preview phase: ${phase}`); });
  ipcMain.handle('preview:save', async (_event, input) => {
    assert.ok(input.attachment.bytes instanceof Uint8Array, 'Nested image bytes must remain binary, not an indexed dictionary');
    const bytes = input.attachment.bytes;
    const buffer = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    assert.deepEqual(buffer, png, 'A sliced typed-array view must persist only its image bytes');
    assert.ok(bytes.byteOffset > 0, 'The fixture exercises nonzero view offset');
    await fs.promises.writeFile(savedPath, buffer);
    return { byteSize: (await fs.promises.stat(savedPath)).size };
  });
  ipcMain.handle('preview:read', async () => {
    const buffer = await fs.promises.readFile(savedPath);
    return { image: { bytes: new Uint8Array(buffer.buffer, buffer.byteOffset, buffer.byteLength), mime: 'image/png' } };
  });
  window = new BrowserWindow({ title, width: 640, height: 600, show: true,
    webPreferences: { preload, sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><title>${title}</title>
    <style>body{margin:24px;background:#181818;color:white}img{display:block;width:${width}px;height:${height}px}button{margin-top:24px;width:180px;height:40px}</style>
    <img id="preview"><button id="respond">Check interface response</button><script>
    window.previewState={phase:'startup',ticks:0,clicks:0};setInterval(()=>previewState.ticks++,25);
    document.getElementById('respond').onclick=()=>previewState.clicks++;
    </script>`));
  phase = 'renderer persist';
  await evaluate(`(() => { (async () => { let url; try {
    const source=Uint8Array.from(atob(${JSON.stringify(png.toString('base64'))}),value=>value.charCodeAt(0));
    const backing=new Uint8Array(source.length+19);backing.fill(211);backing.set(source,7);
    imagePreview.phase('persist');
    const saved=await imagePreview.save({attachment:{bytes:backing.subarray(7,7+source.length)}});
    if(saved.byteSize!==source.length)throw new Error('Persisted image byte length changed');
    imagePreview.phase('read');const result=await imagePreview.read();
    if(!(result.image.bytes instanceof Uint8Array))throw new Error('Nested binary reply lost its typed-array type');
    const bytes=result.image.bytes;
    if(bytes.length!==source.length||!bytes.every((value,index)=>value===source[index]))throw new Error('Read image bytes changed');
    imagePreview.phase('decode');url=URL.createObjectURL(new Blob([bytes],{type:result.image.mime}));
    const image=document.getElementById('preview');image.src=url;await image.decode();
    previewState.width=image.naturalWidth;previewState.height=image.naturalHeight;previewState.phase='ready';
    imagePreview.phase('ready');
    }catch(error){previewState.error=String(error.stack||error);previewState.phase='error';}
    finally{if(url)URL.revokeObjectURL(url);}
  })();return true;})()`);
  await eventually('Image becomes a rendered preview', async () => {
    const state = JSON.parse(await evaluate('JSON.stringify(previewState)'));
    if (state.error) throw new Error(state.error);
    return state.phase === 'ready' || state;
  });
  const state = JSON.parse(await evaluate('JSON.stringify(previewState)'));
  assert.equal(state.width, width); assert.equal(state.height, height);
  assert.deepEqual(await fs.promises.readFile(savedPath), png);
  const button = JSON.parse(await evaluate("JSON.stringify((()=>{const r=document.getElementById('respond').getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})())"));
  window.webContents.sendInputEvent({ type: 'mouseDown', x: button.x, y: button.y, button: 'left' });
  window.webContents.sendInputEvent({ type: 'mouseUp', x: button.x, y: button.y, button: 'left' });
  await eventually('Preview remains responsive to renderer mouse input', async () => await evaluate('previewState.clicks') === 1);
  const ticks = await evaluate('previewState.ticks');
  await pause(100);
  assert.ok(await evaluate('previewState.ticks') > ticks, 'Renderer timers must continue after preview decoding');
  console.log(JSON.stringify({ bytes: png.length, dimensions: [state.width, state.height], rendererClicks: 1 }));
  finish();
}).catch(finish);
