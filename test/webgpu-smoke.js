'use strict';

// Run with mini-electron --app=test/webgpu-smoke.js in a graphical session.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const { app, BrowserWindow, nativeImage } = require('electron');
let window, server, native, temporary, finished = false;
const report = { test: 'webgpu-smoke', captures: [] };
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const watchdog = setTimeout(() => finish(new Error('WebGPU smoke timeout')), 120000);
app.on('window-all-closed', () => {});
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (window && !window.isDestroyed()) window.destroy();
  if (server) server.close();
  if (temporary) fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log(JSON.stringify({ ...report, status: 'passed' }));
  app.exit(error ? 1 : 0);
}
function closeColor(a, b, tolerance = 3) {
  return a.length === b.length && a.every((n, i) => Math.abs(n - b[i]) <= tolerance);
}
function samples(image) {
  assert.equal(image.isEmpty(), false);
  const size = image.getSize();
  const points = [{ x: 48, y: 48 }, { x: 128, y: 48 }].map(({ x, y }) => ({
    x: Math.floor(x * size.width / report.renderer.viewport.width),
    y: Math.floor(y * size.height / report.renderer.viewport.height),
  }));
  if (native) return native.imageSamples(image.toPNG(), points).samples;
  const decoded = nativeImage.createFromBuffer(image.toPNG());
  const pixels = decoded.toBitmap();
  return points.map(({ x, y }) => {
    const p = (y * size.width + x) * 4;
    return [pixels[p + 2], pixels[p + 1], pixels[p], pixels[p + 3]];
  });
}
async function capture(expected, label) {
  let actual;
  const start = Date.now();
  do {
    actual = samples(await window.webContents.capturePage());
    if (actual.length === expected.length && actual.every((rgba, i) => closeColor(rgba, expected[i]))) {
      report.captures.push({ label, samples: actual });
      return;
    }
    await pause(30);
  } while (Date.now() - start < 8000);
  throw new Error(`${label}: composed pixels ${JSON.stringify(actual)} expected ${JSON.stringify(expected)}`);
}
async function exercise() {
  const checks = [];
  const ensure = (value, message) => { if (!value) throw new Error(message); return value; };
  const equal = (a, b, message) => ensure(JSON.stringify(a) === JSON.stringify(b), `${message}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`);
  const near = (a, b, message) => ensure(a.length === b.length && a.every((n, i) => Math.abs(n - b[i]) <= 2), `${message}: ${a} != ${b}`);
  ensure(isSecureContext && navigator.gpu, 'Secure-context native WebGPU exposure');
  const adapter = ensure(await navigator.gpu.requestAdapter(), 'Real WebGPU adapter');
  const device = await adapter.requestDevice();
  const failures = [];
  device.addEventListener('uncapturederror', event => failures.push(event.error.message));
  const format = navigator.gpu.getPreferredCanvasFormat();
  const code = `@vertex fn vertex(@builtin(vertex_index) index:u32)->@builtin(position) vec4f {
    let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[index],0.,1.);
  } @fragment fn fragment()->@location(0) vec4f {return vec4f(.25,.5,.75,.1);}`;
  const module = device.createShaderModule({ code });
  const compilation = await module.getCompilationInfo();
  ensure(!compilation.messages.some(message => message.type === 'error'), 'Native WGSL compilation');
  const pipeline = await device.createRenderPipelineAsync({ layout: 'auto',
    vertex: { module, entryPoint: 'vertex' }, fragment: { module, entryPoint: 'fragment', targets: [{ format }] },
  });
  function surface(x, alphaMode) {
    const c = document.createElement('canvas'); c.width = c.height = 32;
    c.style.cssText = `position:absolute;left:${x}px;top:16px;width:64px;height:64px`;
    document.body.appendChild(c);
    const context = ensure(c.getContext('webgpu'), 'GPUCanvasContext factory and union binding');
    context.configure({ device, format, alphaMode, usage: GPUTextureUsage.RENDER_ATTACHMENT });
    return { canvas: c, context };
  }
  const opaque = surface(16, 'opaque'), premul = surface(96, 'premultiplied');
  function draw(context, renderPipeline, clearValue) {
    const texture = context.getCurrentTexture();
    ensure(texture === context.getCurrentTexture(), 'Same current texture within a frame');
    const encoder = device.createCommandEncoder();
    const pass = encoder.beginRenderPass({ colorAttachments: [{ view: texture.createView(), clearValue,
      loadOp: 'clear', storeOp: 'store' }] });
    if (renderPipeline) { pass.setPipeline(renderPipeline); pass.draw(3); }
    pass.end(); device.queue.submit([encoder.finish()]);
    return texture;
  }
  const opaqueTexture = draw(opaque.context, pipeline, [0, 0, 0, 0]);
  equal(opaqueTexture.usage, GPUTextureUsage.RENDER_ATTACHMENT, 'Native readback usage stays internal');
  draw(premul.context, null, [.5, 0, 0, .5]);
  const copy = document.createElement('canvas'); copy.width = copy.height = 32;
  const c2d = copy.getContext('2d'); c2d.drawImage(opaque.canvas, 0, 0);
  near([...c2d.getImageData(16, 16, 1, 1).data], [64, 128, 191, 255], 'Opaque canvas synchronous snapshot and channel order');
  checks.push('WGSL compilation, async pipeline, canvas rendering, hidden internal usage, opaque snapshot');

  const storage = device.createBuffer({ size: 64, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC });
  const source = Uint32Array.from({ length: 32 }, (_, i) => i);
  device.queue.writeBuffer(storage, 0, source, 8, 16);
  const computeModule = device.createShaderModule({ code: `@group(0) @binding(0) var<storage,read_write> values:array<u32>;
    @compute @workgroup_size(8) fn main(@builtin(global_invocation_id) id:vec3u){values[id.x]=values[id.x]*7u+11u;}` });
  const compute = await device.createComputePipelineAsync({ layout: 'auto', compute: { module: computeModule, entryPoint: 'main' } });
  const group = device.createBindGroup({ layout: compute.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: storage } }] });
  const readback = device.createBuffer({ size: 64, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
  const encoder = device.createCommandEncoder(); const pass = encoder.beginComputePass();
  pass.setPipeline(compute); pass.setBindGroup(0, group); pass.dispatchWorkgroups(2); pass.end();
  encoder.copyBufferToBuffer(storage, 0, readback, 0, 64); device.queue.submit([encoder.finish()]);
  await readback.mapAsync(GPUMapMode.READ);
  const mapped = readback.getMappedRange();
  equal([...new Uint32Array(mapped)], Array.from({ length: 16 }, (_, i) => (i + 8) * 7 + 11), 'Actual GPU compute and typed-array write offsets');
  readback.unmap(); equal(mapped.byteLength, 0, 'Unmap detaches mapped ArrayBuffer');
  await device.queue.onSubmittedWorkDone();
  checks.push('Compute pipeline, bind groups, writeBuffer element offsets, copy, async map and detached mapped range');

  device.pushErrorScope('validation');
  device.createBuffer({ size: 4, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.STORAGE });
  ensure((await device.popErrorScope()) instanceof GPUValidationError, 'Dawn validation error scopes');
  device.pushErrorScope('validation');
  const invalid = device.createShaderModule({ code: 'this is not WGSL' });
  ensure((await invalid.getCompilationInfo()).messages.some(message => message.type === 'error'), 'WGSL diagnostics');
  ensure((await device.popErrorScope()) instanceof GPUValidationError, 'Invalid shader validation delivery');
  checks.push('Native validation/error scopes and WGSL compilation diagnostics');

  const detached = new ImageData(1, 1);
  structuredClone(detached.data.buffer, { transfer: [detached.data.buffer] });
  const upload = device.createTexture({ size: [1, 1], format: 'rgba8unorm',
    usage: GPUTextureUsage.COPY_DST | GPUTextureUsage.TEXTURE_BINDING });
  let detachedError;
  try { device.queue.copyExternalImageToTexture({ source: detached }, { texture: upload }, [1, 1]); }
  catch (error) { detachedError = error; }
  ensure(detachedError instanceof DOMException && detachedError.name === 'InvalidStateError',
    'Detached ImageData produces InvalidStateError instead of terminating renderer');

  const rgbaFrame = new VideoFrame(Uint8Array.from([
    255, 0, 0, 255, 128, 64, 32, 128,
    0, 0, 255, 255, 0, 255, 0, 255,
  ]), { format: 'RGBA', codedWidth: 2, codedHeight: 2, timestamp: 0 });
  const rgbCopy = new Uint8Array(20).fill(90);
  await rgbaFrame.copyTo(rgbCopy, { format: 'BGRA', rect: { x: 1, y: 0, width: 1, height: 2 },
    layout: [{ offset: 4, stride: 8 }], colorSpace: 'srgb' });
  equal([...rgbCopy], [90, 90, 90, 90, 32, 64, 128, 128, 90, 90, 90, 90, 0, 255, 0, 255, 90, 90, 90, 90],
    'CPU VideoFrame RGB copy preserves crop, channel order, unpremultiplied alpha and destination padding');
  rgbaFrame.close();
  const yuvFrame = new VideoFrame(Uint8Array.from([16, 16, 235, 235, 128, 128]),
    { format: 'I420', codedWidth: 2, codedHeight: 2, timestamp: 0,
      colorSpace: { primaries: 'bt709', transfer: 'bt709', matrix: 'bt709', fullRange: false } });
  const yuvCopy = new Uint8Array(16);
  await yuvFrame.copyTo(yuvCopy, { format: 'RGBA' });
  near([...yuvCopy], [0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255],
    'CPU VideoFrame YUV-to-RGB conversion');
  yuvFrame.close();
  checks.push('CPU VideoFrame RGB/YUV conversion, cropped strided copies and alpha preservation');

  const offscreen = new OffscreenCanvas(8, 8), off = ensure(offscreen.getContext('webgpu'), 'Offscreen WebGPU');
  off.configure({ device, format, alphaMode: 'opaque', usage: GPUTextureUsage.RENDER_ATTACHMENT });
  draw(off, null, [0, 1, 0, 1]);
  const bitmap = offscreen.transferToImageBitmap();
  c2d.clearRect(0, 0, 32, 32); c2d.drawImage(bitmap, 0, 0);
  near([...c2d.getImageData(4, 4, 1, 1).data], [0, 255, 0, 255], 'Independent OffscreenCanvas ImageBitmap pixels'); bitmap.close();
  checks.push('OffscreenCanvas transferToImageBitmap and owned CPU image interop');

  off.configure({ device, format, alphaMode: 'opaque',
    usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
  const liveTexture = draw(off, null, [.25, .5, .75, .25]);
  c2d.clearRect(0, 0, 32, 32); c2d.drawImage(offscreen, 0, 0);
  near([...c2d.getImageData(4, 4, 1, 1).data], [64, 128, 191, 255], 'Opaque CPU snapshot forces alpha without changing color');
  const blobPromise = offscreen.convertToBlob();
  ensure(off.getCurrentTexture() === liveTexture, 'convertToBlob does not expire a same-task texture');
  const bitmapPromise = createImageBitmap(offscreen);
  ensure(off.getCurrentTexture() === liveTexture, 'createImageBitmap does not expire a same-task texture');
  const snapshotReadback = device.createBuffer({ size: 2048, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
  const snapshotEncoder = device.createCommandEncoder();
  snapshotEncoder.copyTextureToBuffer({ texture: liveTexture }, { buffer: snapshotReadback, bytesPerRow: 256 }, [8, 8]);
  device.queue.submit([snapshotEncoder.finish()]);
  await snapshotReadback.mapAsync(GPUMapMode.READ);
  near([...new Uint8Array(snapshotReadback.getMappedRange(), 0, 4)],
    format === 'bgra8unorm' ? [191, 128, 64, 64] : [64, 128, 191, 64],
    'Opaque snapshots leave the live application GPU texture alpha untouched');
  snapshotReadback.unmap();
  const snapshotBitmap = await bitmapPromise;
  c2d.clearRect(0, 0, 32, 32); c2d.drawImage(snapshotBitmap, 0, 0); snapshotBitmap.close();
  near([...c2d.getImageData(4, 4, 1, 1).data], [64, 128, 191, 255], 'createImageBitmap retains opaque snapshot pixels');
  const encodedBitmap = await createImageBitmap(await blobPromise);
  c2d.clearRect(0, 0, 32, 32); c2d.drawImage(encodedBitmap, 0, 0); encodedBitmap.close();
  near([...c2d.getImageData(4, 4, 1, 1).data], [64, 128, 191, 255], 'Serialized snapshot round-trip retains opaque GPU pixels');
  checks.push('Detached input rejection, snapshot-only texture lifetime and opaque snapshot noninterference');

  const workerURL = URL.createObjectURL(new Blob([`onmessage=async()=>{try{
    const f=new VideoFrame(new Uint8Array([128,64,32,128]),{format:'RGBA',codedWidth:1,codedHeight:1,timestamp:0});
    const rgb=new Uint8Array(4);await f.copyTo(rgb,{format:'BGRA'});f.close();
    if(rgb.join(',')!=='32,64,128,128')throw Error('Worker CPU VideoFrame RGB copy');
    const a=await navigator.gpu.requestAdapter();if(!a)throw Error('No worker adapter');const d=await a.requestDevice();
    const b=d.createBuffer({size:4,usage:GPUBufferUsage.COPY_DST|GPUBufferUsage.MAP_READ});
    const s=d.createBuffer({size:4,mappedAtCreation:true,usage:GPUBufferUsage.COPY_SRC});new Uint32Array(s.getMappedRange())[0]=0x1234abcd;s.unmap();
    const e=d.createCommandEncoder();e.copyBufferToBuffer(s,0,b,0,4);d.queue.submit([e.finish()]);await b.mapAsync(GPUMapMode.READ);
    const value=new Uint32Array(b.getMappedRange())[0];b.unmap();d.destroy();postMessage({value});
  }catch(error){postMessage({error:String(error)})}}`], { type: 'text/javascript' }));
  const worker = new Worker(workerURL);
  const workerValue = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('Worker WebGPU callback timeout')), 10000);
    worker.onerror = event => { clearTimeout(timer); reject(new Error(event.message)); };
    worker.onmessage = ({ data }) => { clearTimeout(timer); data.error ? reject(new Error(data.error)) : resolve(data.value); };
    worker.postMessage(null);
  });
  equal(workerValue, 0x1234abcd, 'Worker provider ownership and real map/copy'); worker.terminate(); URL.revokeObjectURL(workerURL);
  const disposableAdapter = ensure(await navigator.gpu.requestAdapter(), 'Adapter for separate device lifetime');
  const disposable = await disposableAdapter.requestDevice(); const lost = disposable.lost; disposable.destroy();
  equal((await lost).reason, 'destroyed', 'Real device destruction/loss callback');
  checks.push('Dedicated Worker GPU device/map and VideoFrame lifecycle, and device.lost delivery');
  window.updateWebGPU = async () => {
    opaque.canvas.width = 40; opaque.canvas.height = 24;
    draw(opaque.context, null, [0, 1, 0, 1]);
    draw(premul.context, null, [0, 0, .5, .5]);
    await device.queue.onSubmittedWorkDone();
    equal(failures, [], 'No uncaptured errors');
  };
  equal(failures, [], 'No uncaptured errors');
  return { checks, viewport: { width: innerWidth, height: innerHeight }, format,
    adapter: { vendor: adapter.info.vendor, architecture: adapter.info.architecture,
      device: adapter.info.device, description: adapter.info.description } };
}
app.whenReady().then(async () => {
  if (process.platform === 'darwin') {
    temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-webgpu-smoke-'));
    const root = path.resolve(__dirname, '..');
    const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
    const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
    const helper = path.join(temporary, 'pixels.node');
    const includes = ['', 'gen', 'third_party/abseil-cpp', 'base/allocator/partition_allocator/src', 'gen/base/allocator/partition_allocator/src', 'third_party/libnode/src'];
    execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup', '-isysroot', sdk,
      ...includes.map(include => `-I${path.join(root, include)}`), '-framework', 'AppKit', '-framework', 'CoreGraphics',
      '-framework', 'ScreenCaptureKit', '-framework', 'ImageIO', path.join(__dirname, 'macos-owned-window-capture.mm'),
      path.join(__dirname, 'macos-gpu-compositing-helper.mm'), '-o', helper], { stdio: 'inherit', timeout: 30000 });
    native = require(helper);
  }
  server = http.createServer((_request, response) => {
    response.writeHead(200, { 'Content-Type': 'text/html' });
    response.end('<!doctype html><style>html,body{margin:0;background:#204060}</style>');
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  window = new BrowserWindow({ width: 192, height: 112, frame: false, useContentSize: true, show: true,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  window.webContents.on('render-process-gone', (_, details) => finish(new Error(JSON.stringify(details))));
  await window.loadURL(`http://127.0.0.1:${server.address().port}/`);
  report.renderer = await window.webContents.executeJavaScript(`(${exercise.toString()})()`);
  report.compositor = await window.webContents._getCompositorInfo();
  await capture([[64, 128, 191, 255], [144, 32, 48, 255]], 'native WGSL rendering and premultiplied composition');
  await window.webContents.executeJavaScript('updateWebGPU()');
  await capture([[0, 255, 0, 255], [16, 32, 176, 255]], 'resize and subsequent GPU frame');
  finish();
}).catch(finish);
