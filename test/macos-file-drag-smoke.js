'use strict';
// Run with the packaged macOS mini-electron executable. Drives the real
// MiniElectronRendererView NSDraggingDestination callbacks.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-file-drag-'));
const title = `mini-file-drag-${process.pid}`;
const png = Buffer.from(
  'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==',
  'base64');
const imagePath = path.join(temporary, 'native-pixel.png');
const folderPath = path.join(temporary, 'native-folder');
fs.writeFileSync(imagePath, png);
fs.mkdirSync(folderPath);
const fifoPath = path.join(temporary, 'native-fifo');
const oversizedPath = path.join(temporary, 'oversized.bin');
execFileSync('mkfifo', [fifoPath]);
fs.writeFileSync(oversizedPath, Buffer.alloc(0));
fs.truncateSync(oversizedPath, 50 * 1024 * 1024 + 1);
const expectedImagePath = fs.realpathSync(imagePath);
const expectedFolderPath = fs.realpathSync(folderPath);
let window;
let finished = false;
const deadline = setTimeout(() => finish(new Error('Native file drag smoke timed out')), 60000);
app.on('window-all-closed', () => {});

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: native AppKit file/folder drag lifecycle and brokered DOM Files');
  app.exit(error ? 1 : 0);
}

const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const evaluate = async source => (await window.webContents.executeJavaScript(source)).value;
async function eventually(source, predicate, label) {
  const end = Date.now() + 10000;
  let value;
  while (Date.now() < end) {
    value = JSON.parse(await evaluate(`JSON.stringify(${source})`));
    if (predicate(value)) return value;
    await pause(20);
  }
  assert.fail(`${label}: ${JSON.stringify(value)}`);
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const root = path.resolve(__dirname, '..');
  const sdk = process.env.SDKROOT || execFileSync(
    'xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync(
    'xcrun', ['--sdk', 'macosx', '--find', 'clang++'], { encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'file-drag.node');
  execFileSync(compiler, [
    '-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup',
    '-isysroot', sdk, `-I${path.join(root, 'third_party/libnode/src')}`,
    '-framework', 'AppKit', path.join(__dirname, 'macos-file-drag-helper.mm'),
    '-o', helper,
  ], { stdio: 'inherit', timeout: 30000 });
  const native = require(helper);
  const preload = path.join(temporary, 'preload.js');
  fs.writeFileSync(preload, `
    const { contextBridge, webUtils } = require('electron');
    contextBridge.exposeInMainWorld('nativeFileDrag', {
      getPath: file => webUtils.getPathForFile(file)
    });
  `);
  window = new BrowserWindow({
    width: 600,
    height: 400,
    title,
    webPreferences: {
      preload,
      sandbox: true,
      contextIsolation: true,
      nodeIntegration: false,
    },
  });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html>
    <meta charset="utf-8"><title>${title}</title>
    <style>html,body{width:100%;height:100%;margin:0}body{background:#fff}
      #drop-target{position:absolute;left:320px;top:200px;width:160px;height:120px;background:#ddd}</style>
    <div id="drop-target">Drop files here</div>
    <script>
      window.acceptNativeDrags = true;
      window.dragState = { events: [], dropResult: null };
      const remember = event => window.dragState.events.push(event.type);
      addEventListener('dragenter', event => {
        remember(event);
        if (window.acceptNativeDrags && event.target.id === 'drop-target') event.preventDefault();
      });
      addEventListener('dragover', event => {
        remember(event);
        if (window.acceptNativeDrags && event.target.id === 'drop-target') event.preventDefault();
      });
      addEventListener('dragleave', remember);
      addEventListener('drop', async event => {
        remember(event);
        if (window.acceptNativeDrags && event.target.id === 'drop-target') event.preventDefault();
        try {
          const files = Array.from(event.dataTransfer.files);
          const types = Array.from(event.dataTransfer.types);
          const location = { x: event.clientX, y: event.clientY, target: event.target.id };
          const entries = Array.from(event.dataTransfer.items).map(item => {
            const entry = item.webkitGetAsEntry();
            return entry && { name: entry.name, fullPath: entry.fullPath,
              isFile: entry.isFile, isDirectory: entry.isDirectory };
          });
          const described = await Promise.all(files.map(async file => ({
            name: file.name,
            type: file.type,
            size: file.size,
            path: nativeFileDrag.getPath(file),
            bytes: Array.from(new Uint8Array(await file.arrayBuffer())),
          })));
          window.dragState.dropResult = {
            types,
            location,
            files: described,
            entries,
            forgedPath: nativeFileDrag.getPath(new File(['forged'], 'native-pixel.png', { type: 'image/png' })),
          };
        } catch (error) {
          window.dragState.dropResult = { error: String(error && error.stack || error) };
        }
      });
    </script>`));
  window.show();
  window.focus();

  assert.equal(native.nonFile(title), 0, 'non-file pasteboard drags must be rejected');
  assert.equal(native.begin(title, [fifoPath], 400, 300), 0,
    'special files must be rejected without blocking AppKit');
  assert.equal(native.begin(title, [oversizedPath], 400, 300), 0,
    'oversized files must be rejected before renderer allocation');

  assert.equal(native.begin(title, [imagePath], 400, 300), 1);
  native.leave();
  const immediateLeave = await eventually('window.dragState', value =>
    value.events.includes('dragleave'), 'immediate dragleave');
  assert.deepEqual(immediateLeave.events, ['dragenter', 'dragleave']);
  assert.equal(immediateLeave.dropResult, null);
  await evaluate('window.dragState = { events: [], dropResult: null }');

  assert.equal(native.begin(title, [imagePath, folderPath], 400, 300), 1);
  await eventually('window.dragState.events', events =>
    events.length === 1 && events[0] === 'dragenter', 'dragenter');
  assert.equal(native.update(), 1);
  await eventually('window.dragState.events', events =>
    events.length === 2 && events[1] === 'dragover', 'dragover');
  assert.equal(native.drop(), true);
  const state = await eventually('window.dragState', value =>
    value.dropResult !== null, 'drop result');
  assert.deepEqual(state.events, ['dragenter', 'dragover', 'drop']);
  assert.deepEqual(state.dropResult.location, { x: 400, y: 300, target: 'drop-target' },
    'native logical coordinates must hit the DOM drop target on Retina displays');
  assert.equal(state.dropResult.error, undefined);
  assert.ok(state.dropResult.types.includes('Files'), JSON.stringify(state));
  assert.equal(state.dropResult.files.length, 2);
  const image = state.dropResult.files.find(file => file.name === path.basename(imagePath));
  const folder = state.dropResult.files.find(file => file.name === path.basename(folderPath));
  assert.ok(image, 'PNG File must be present');
  assert.ok(folder, 'directory File must be present');
  assert.equal(image.type, 'image/png');
  assert.equal(image.size, png.length);
  assert.deepEqual(image.bytes, Array.from(png));
  assert.equal(image.path, expectedImagePath);
  assert.equal(folder.size, 0);
  assert.deepEqual(folder.bytes, []);
  assert.equal(folder.path, expectedFolderPath);
  const imageEntry = state.dropResult.entries.find(entry => entry && entry.name === image.name);
  const folderEntry = state.dropResult.entries.find(entry => entry && entry.name === folder.name);
  assert.deepEqual(imageEntry, { name: image.name, fullPath: `/${image.name}`,
    isFile: true, isDirectory: false });
  assert.deepEqual(folderEntry, { name: folder.name, fullPath: `/${folder.name}`,
    isFile: false, isDirectory: true });
  assert.equal(state.dropResult.forgedPath, '');

  await evaluate('window.dragState = { events: [], dropResult: null }');
  assert.equal(native.begin(title, [imagePath], 400, 300), 1);
  await eventually('window.dragState.events', events =>
    events.length === 1 && events[0] === 'dragenter', 'second dragenter');
  assert.equal(native.update(), 1);
  await eventually('window.dragState.events', events =>
    events.length === 2 && events[1] === 'dragover', 'second dragover');
  native.leave();
  const left = await eventually('window.dragState', value =>
    value.events.includes('dragleave'), 'dragleave');
  await pause(200);
  assert.deepEqual(left.events, ['dragenter', 'dragover', 'dragleave']);
  assert.equal(JSON.parse(await evaluate('JSON.stringify(window.dragState.dropResult)')), null,
    'drag leave must clear pending drop state');

  await evaluate(`window.acceptNativeDrags = false;
    window.dragState = { events: [], dropResult: null }`);
  assert.equal(native.begin(title, [imagePath], 400, 300), 1);
  await eventually('window.dragState.events', events =>
    events.length === 1 && events[0] === 'dragenter', 'rejected dragenter');
  let rejected = false;
  for (let attempt = 0; attempt < 100; ++attempt) {
    if (native.update() === 0) {
      rejected = true;
      break;
    }
    await pause(10);
  }
  assert.equal(rejected, true, 'AppKit must receive Blink drag rejection');
  assert.equal(native.drop(), false, 'rejected targets must not accept a drop');
  const rejectedState = await eventually('window.dragState', value =>
    value.events.includes('dragleave'), 'rejected dragleave');
  assert.equal(rejectedState.events.includes('drop'), false);
  assert.equal(rejectedState.dropResult, null);
  const originalDpr = await evaluate('devicePixelRatio');
  window.webContents.setZoomFactor(1.2);
  await evaluate(`window.acceptNativeDrags = true;
    window.dragState = { events: [], dropResult: null }`);
  await eventually('devicePixelRatio', value => Math.abs(value - originalDpr * 1.2) < 0.01,
    'browser zoom layout commit');
  const target = JSON.parse(await evaluate(`JSON.stringify((() => {
    const rect = document.getElementById('drop-target').getBoundingClientRect();
    return { x: rect.x + rect.width / 2, y: rect.y + rect.height / 2 };
  })())`));
  const zoom = window.webContents.getZoomFactor();
  assert.equal(native.begin(title, [imagePath, folderPath], target.x * zoom, target.y * zoom), 1);
  await eventually('window.dragState.events', events => events.includes('dragenter'),
    'zoomed dragenter');
  assert.equal(native.update(), 1, await evaluate(`JSON.stringify({
    dpr: devicePixelRatio, width: innerWidth, height: innerHeight,
    target: document.getElementById('drop-target').getBoundingClientRect().toJSON()
  })`));
  await eventually('window.dragState.events', events => events.includes('dragover'),
    'zoomed dragover');
  assert.equal(native.drop(), true);
  const zoomed = await eventually('window.dragState', value => value.dropResult !== null,
    'zoomed drop result');
  assert.equal(zoomed.dropResult.location.target, 'drop-target');
  assert.ok(Math.abs(zoomed.dropResult.location.x - target.x) < 1);
  assert.ok(Math.abs(zoomed.dropResult.location.y - target.y) < 1);
  assert.deepEqual(zoomed.dropResult.files.map(file => file.name),
    [path.basename(imagePath), path.basename(folderPath)]);
  finish();
}).catch(finish);
