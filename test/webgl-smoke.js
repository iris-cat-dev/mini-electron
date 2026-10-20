'use strict';

// /path/to/mini-electron --app=test/webgl-smoke.js
// Requires a graphical session and real WebGL1/WebGL2. On macOS the existing
// Core Graphics sampler decodes PNGs and captures the owned WindowServer window.
// Native captures require an unlocked session and OS capture authorization.
const assert = require('node:assert/strict');
const os = require('node:os');
const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const { app, BrowserWindow, nativeImage } = require('electron');
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let window, finished = false;
let native, temporary;
const report = { test: 'webgl-smoke', captures: [], nativeCaptures: [] };
const watchdog = setTimeout(() => finish(new Error('WebGL smoke exceeded 90s watchdog')), 90000);
app.on('window-all-closed', () => {});

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  try {
    if (window && !window.isDestroyed()) window.destroy();
  } finally {
    if (temporary) fs.rmSync(temporary, { recursive: true, force: true });
    if (error) console.error(error.stack || error);
    else console.log(JSON.stringify({ ...report, status: 'passed' }));
    app.exit(error ? 1 : 0);
  }
}

function closeColor(actual, expected, tolerance = 4) {
  return actual.length === 4 && actual.every((channel, index) => Math.abs(channel - expected[index]) <= tolerance);
}

// PNG sampling must not depend on the build's internal Skia N32 byte order.
function pngSamples(png, probes, logicalSize) {
  assert.deepEqual([...png.subarray(0, 8)], [137, 80, 78, 71, 13, 10, 26, 10], 'Real PNG encoding required');
  const size = { width: png.readUInt32BE(16), height: png.readUInt32BE(20) };
  assert.ok(size.width > 0 && size.height > 0);
  assert.ok(Math.abs(size.width / logicalSize.width - size.height / logicalSize.height) < 0.02,
    `Capture aspect ratio: ${JSON.stringify(size)} versus ${JSON.stringify(logicalSize)}`);
  if (native) {
    return native.imageSamples(png, probes.map(({ x, y }) => ({
      x: Math.floor(x * size.width / logicalSize.width), y: Math.floor(y * size.height / logicalSize.height),
    })));
  }
  const decoded = nativeImage.createFromBuffer(png);
  assert.equal(decoded.isEmpty(), false, 'Native PNG decoding required');
  assert.deepEqual(decoded.getSize(), size);
  const pixels = decoded.toBitmap();
  assert.equal(pixels.length, size.width * size.height * 4, 'Tightly packed native bitmap required');
  const samples = probes.map(({ x, y }) => {
    const px = Math.floor(x * size.width / logicalSize.width);
    const py = Math.floor(y * size.height / logicalSize.height);
    assert.ok(px >= 0 && py >= 0 && px < size.width && py < size.height);
    const offset = (py * size.width + px) * 4;
    const bytes = [...pixels.subarray(offset, offset + 4)];
    return os.endianness() === 'LE' ? [bytes[2], bytes[1], bytes[0], bytes[3]]
      : [bytes[1], bytes[2], bytes[3], bytes[0]];
  });
  return { ...size, samples };
}

async function capture(probes, expected, label) {
  const began = Date.now();
  let last;
  do {
    const image = await window.webContents.capturePage();
    assert.equal(image.isEmpty(), false, `${label}: capturePage must contain actual composed pixels`);
    last = pngSamples(image.toPNG(), probes, report.renderer.viewport);
    if (last.samples.length === expected.length && last.samples.every((rgba, index) => closeColor(rgba, expected[index]))) {
      if (native) {
        const presented = await native.snapshot(window.getTitle(), probes);
        if (presented.samples.length !== expected.length
            || !presented.samples.every((rgba, index) => closeColor(rgba, expected[index], 10))) {
          last = { ...last, native: presented };
          await pause(30);
          continue;
        }
        report.nativeCaptures.push({ label, ...presented });
      }
      report.captures.push({ label, ...last });
      return;
    }
    await pause(30);
  } while (Date.now() - began < 8000);
  throw new Error(`${label}: stale/incorrect composed WebGL pixels: actual=${JSON.stringify(last)} expected=${JSON.stringify(expected)}`);
}

// Serialized into the isolated renderer; deliberately uses only browser APIs.
async function exerciseWebGL() {
  const results = [];
  const red = [255, 0, 0, 255], green = [0, 255, 0, 255];
  const blue = [0, 0, 255, 255], yellow = [255, 255, 0, 255];
  const rgba = [red, green, blue, yellow]; // DOM order: TL, TR, BL, BR.
  const uploaded = [blue, yellow, red, green];
  const ensure = (value, label) => { if (!value) throw new Error(label); return value; };
  const equal = (actual, expected, label, tolerance = 2) => ensure(
    actual.length === expected.length && actual.every((value, index) => Math.abs(value - expected[index]) <= tolerance),
    `${label}: actual=${JSON.stringify(actual)} expected=${JSON.stringify(expected)}`);
  const clean = (gl, label) => equal([gl.getError()], [gl.NO_ERROR], `${label}: GL error`, 0);
  const canvas = (id, x, y, width = 32, height = 32) => {
    const result = document.createElement('canvas');
    result.id = id; result.width = width; result.height = height;
    result.style.cssText = `position:absolute;left:${x}px;top:${y}px;width:80px;height:80px`;
    document.body.appendChild(result);
    return result;
  };
  const context = (surface, version, overrides = {}) => {
    const gl = ensure(surface.getContext(version, {
      alpha: false, antialias: false, depth: false, stencil: false,
      preserveDrawingBuffer: true, ...overrides,
    }), `${version} must create a real context`);
    const attrs = ensure(gl.getContextAttributes(), `${version}: context attributes`);
    ensure(attrs.preserveDrawingBuffer && !attrs.antialias, `${version}: requested buffer attributes`);
    gl.disable(gl.DITHER);
    return gl;
  };
  const compile = (gl, type, source) => {
    const shader = ensure(gl.createShader(type), 'createShader');
    gl.shaderSource(shader, source); gl.compileShader(shader);
    ensure(gl.getShaderParameter(shader, gl.COMPILE_STATUS), `Shader compilation: ${gl.getShaderInfoLog(shader)}`);
    return shader;
  };
  const program = (gl, vertex, fragment) => {
    const result = ensure(gl.createProgram(), 'createProgram');
    const vs = compile(gl, gl.VERTEX_SHADER, vertex), fs = compile(gl, gl.FRAGMENT_SHADER, fragment);
    gl.attachShader(result, vs); gl.attachShader(result, fs); gl.linkProgram(result);
    ensure(gl.getProgramParameter(result, gl.LINK_STATUS), `Program link: ${gl.getProgramInfoLog(result)}`);
    gl.deleteShader(vs); gl.deleteShader(fs);
    gl.useProgram(result);
    return result;
  };
  const quad = (gl, linked) => {
    const buffer = ensure(gl.createBuffer(), 'createBuffer');
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1]), gl.STATIC_DRAW);
    const location = gl.getAttribLocation(linked, 'position');
    ensure(location >= 0, 'position attribute');
    gl.enableVertexAttribArray(location); gl.vertexAttribPointer(location, 2, gl.FLOAT, false, 0, 0);
    return buffer;
  };
  const solid = (gl, color) => {
    const linked = program(gl, 'attribute vec2 position; void main(){gl_Position=vec4(position,0.,1.);}',
      'precision mediump float; uniform vec4 color; void main(){gl_FragColor=color;}');
    quad(gl, linked);
    gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
    gl.uniform4fv(ensure(gl.getUniformLocation(linked, 'color'), 'solid color uniform'), color);
    gl.drawArrays(gl.TRIANGLES, 0, 6);
    clean(gl, 'solid vertex-buffer draw');
    return linked;
  };
  const pixel = (gl, x, y) => {
    const bytes = new Uint8Array(4);
    gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, bytes);
    clean(gl, 'readPixels');
    return [...bytes];
  };
  const quadrants = (gl, expected, label) => {
    const width = gl.drawingBufferWidth, height = gl.drawingBufferHeight;
    [[.25, .75], [.75, .75], [.25, .25], [.75, .25]].forEach(([x, y], index) =>
      equal(pixel(gl, Math.floor(width * x), Math.floor(height * y)), expected[index], `${label} quadrant ${index}`));
  };
  const sample2D = (surface, expected, label) => {
    const copy = document.createElement('canvas'); copy.width = surface.width; copy.height = surface.height;
    const ctx = ensure(copy.getContext('2d'), `${label}: 2D context`);
    ctx.drawImage(surface, 0, 0);
    [[.25, .25], [.75, .25], [.25, .75], [.75, .75]].forEach(([x, y], index) =>
      equal([...ctx.getImageData(Math.floor(copy.width * x), Math.floor(copy.height * y), 1, 1).data],
        expected[index], `${label} quadrant ${index}`));
  };
  for (const type of ['webgl', 'webgl2']) {
    for (const options of [undefined, { stencil: true, antialias: false }]) {
      const surface = document.createElement('canvas'); surface.width = surface.height = 8;
      const gl = ensure(options ? surface.getContext(type, options) : surface.getContext(type), `${type}: default/depth-stencil context`);
      const v2 = type === 'webgl2';
      const linked = program(gl, `${v2 ? '#version 300 es\nin' : 'attribute'} vec2 position; uniform float z; void main(){gl_Position=vec4(position,z,1.);}`,
        `${v2 ? '#version 300 es\n' : ''}precision mediump float; uniform vec4 color; ${v2 ? 'out vec4 outputColor;' : ''} void main(){${v2 ? 'outputColor' : 'gl_FragColor'}=color;}`);
      quad(gl, linked); gl.viewport(0, 0, 8, 8); gl.enable(gl.DEPTH_TEST);
      const z = gl.getUniformLocation(linked, 'z'), color = gl.getUniformLocation(linked, 'color');
      gl.clearColor(0, 0, 1, 1); gl.clearDepth(1); gl.clearStencil(1);
      gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT | gl.STENCIL_BUFFER_BIT);
      if (options) {
        gl.enable(gl.STENCIL_TEST); gl.stencilFunc(gl.EQUAL, 0, 255);
        gl.uniform1f(z, 0); gl.uniform4f(color, 0, 1, 0, 1); gl.drawArrays(gl.TRIANGLES, 0, 6);
        equal(pixel(gl, 4, 4), blue, `${type}: stencil rejects nonmatching fragments`);
        gl.stencilFunc(gl.EQUAL, 1, 255);
      }
      gl.uniform1f(z, -.5); gl.uniform4f(color, 1, 0, 0, 1); gl.drawArrays(gl.TRIANGLES, 0, 6);
      gl.uniform1f(z, .5); gl.uniform4f(color, 0, 1, 0, 1); gl.drawArrays(gl.TRIANGLES, 0, 6);
      equal(pixel(gl, 4, 4), red, `${type}: depth preserves foreground fragments`);
      surface.width = 11; surface.height = 7;
      equal(pixel(gl, 10, 6), [0, 0, 0, 0], `${type}: resized depth/stencil storage is cleared`);
      gl.disable(gl.DEPTH_TEST); gl.disable(gl.STENCIL_TEST); gl.viewport(0, 0, 11, 7);
      gl.uniform4f(color, 0, 0, 1, 1); gl.drawArrays(gl.TRIANGLES, 0, 6);
      equal(pixel(gl, 10, 6), blue, `${type}: resized default framebuffer resolves drawing`);
    }
  }
  results.push('Default WebGL1/2 rendering, depth occlusion, stencil rejection, resize and resolve');

  const textureDraw = (gl, source) => {
    const linked = program(gl,
      'attribute vec2 position; varying vec2 uv; void main(){uv=position*.5+.5;gl_Position=vec4(position,0.,1.);}',
      'precision mediump float; varying vec2 uv; uniform sampler2D image; void main(){gl_FragColor=texture2D(image,uv);}');
    quad(gl, linked);
    const texture = ensure(gl.createTexture(), 'createTexture');
    gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    if (source) {
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source);
    } else {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 2, 2, 0, gl.RGBA, gl.UNSIGNED_BYTE,
        new Uint8Array([...blue, ...yellow, ...red, ...green]));
    }
    gl.uniform1i(ensure(gl.getUniformLocation(linked, 'image'), 'sampler uniform'), 0);
    gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
    gl.drawArrays(gl.TRIANGLES, 0, 6);
    clean(gl, 'texImage2D and textured vertex-buffer draw');
  };

  const first = canvas('webgl1', 20, 20), gl1 = context(first, 'webgl');
  const second = canvas('webgl2', 120, 20), gl2 = context(second, 'webgl2');
  ensure(gl1 instanceof WebGLRenderingContext && gl2 instanceof WebGL2RenderingContext, 'Real WebGL1 and WebGL2 browser interfaces');
  ensure(first.getContext('experimental-webgl') === gl1, 'experimental-webgl aliases the existing WebGL1 context');
  const isolated = canvas('isolation', 220, 20), isolation = context(isolated, 'webgl');
  solid(isolation, [1, 0, 0, 1]);
  textureDraw(gl1);
  quadrants(gl1, rgba, 'WebGL1 typed-array texture');
  results.push('WebGL1 shader compile/link, vertex buffers, typed-array texImage2D, RGBA quadrant readPixels');

  const source = document.createElement('canvas'); source.width = source.height = 2;
  const source2d = ensure(source.getContext('2d'), 'Texture source 2D context');
  const sourcePixels = source2d.createImageData(2, 2);
  sourcePixels.data.set(uploaded.flat()); source2d.putImageData(sourcePixels, 0, 0);
  textureDraw(gl1, source);
  quadrants(gl1, uploaded, 'WebGL1 canvas texture with Y flip');
  results.push('2D canvas texImage2D and UNPACK_FLIP_Y_WEBGL');

  // ES3-only paths: an indexed VAO, four instances, std140 uniform-buffer
  // palette, a texture-backed FBO, and explicit blit to the default framebuffer.
  const p2 = program(gl2, `#version 300 es
    in vec2 position; flat out int quadrant;
    void main(){quadrant=gl_InstanceID;vec2 offset=vec2(float(gl_InstanceID%2),float(gl_InstanceID/2))-.5;
      gl_Position=vec4(position*.5+offset,0.,1.);}`,
  `#version 300 es
    precision highp float; flat in int quadrant;
    layout(std140) uniform Palette { vec4 colors[4]; }; out vec4 outputColor;
    void main(){outputColor=colors[quadrant];}`);
  const vao = ensure(gl2.createVertexArray(), 'WebGL2 VAO'); gl2.bindVertexArray(vao);
  const vertices = ensure(gl2.createBuffer(), 'WebGL2 vertex buffer'); gl2.bindBuffer(gl2.ARRAY_BUFFER, vertices);
  gl2.bufferData(gl2.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl2.STATIC_DRAW);
  const position = gl2.getAttribLocation(p2, 'position'); ensure(position >= 0, 'WebGL2 position');
  gl2.enableVertexAttribArray(position); gl2.vertexAttribPointer(position, 2, gl2.FLOAT, false, 0, 0);
  const indices = ensure(gl2.createBuffer(), 'WebGL2 element buffer'); gl2.bindBuffer(gl2.ELEMENT_ARRAY_BUFFER, indices);
  gl2.bufferData(gl2.ELEMENT_ARRAY_BUFFER, new Uint16Array([0, 1, 2, 2, 1, 3]), gl2.STATIC_DRAW);
  const block = gl2.getUniformBlockIndex(p2, 'Palette'); ensure(block !== gl2.INVALID_INDEX, 'Active std140 block');
  gl2.uniformBlockBinding(p2, block, 0);
  const ubo = ensure(gl2.createBuffer(), 'WebGL2 UBO'); gl2.bindBuffer(gl2.UNIFORM_BUFFER, ubo);
  gl2.bufferData(gl2.UNIFORM_BUFFER, new Float32Array([0, 0, 1, 1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1]), gl2.STATIC_DRAW);
  gl2.bindBufferBase(gl2.UNIFORM_BUFFER, 0, ubo);
  const target = ensure(gl2.createTexture(), 'FBO texture'); gl2.bindTexture(gl2.TEXTURE_2D, target);
  gl2.texStorage2D(gl2.TEXTURE_2D, 1, gl2.RGBA8, 32, 32);
  const fbo = ensure(gl2.createFramebuffer(), 'FBO'); gl2.bindFramebuffer(gl2.FRAMEBUFFER, fbo);
  gl2.framebufferTexture2D(gl2.FRAMEBUFFER, gl2.COLOR_ATTACHMENT0, gl2.TEXTURE_2D, target, 0);
  equal([gl2.checkFramebufferStatus(gl2.FRAMEBUFFER)], [gl2.FRAMEBUFFER_COMPLETE], 'WebGL2 FBO completeness', 0);
  gl2.viewport(0, 0, 32, 32); gl2.drawElementsInstanced(gl2.TRIANGLES, 6, gl2.UNSIGNED_SHORT, 0, 4);
  clean(gl2, 'WebGL2 indexed instanced UBO draw'); quadrants(gl2, rgba, 'WebGL2 FBO');
  gl2.bindFramebuffer(gl2.READ_FRAMEBUFFER, fbo); gl2.bindFramebuffer(gl2.DRAW_FRAMEBUFFER, null);
  gl2.blitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, gl2.COLOR_BUFFER_BIT, gl2.NEAREST);
  gl2.bindFramebuffer(gl2.FRAMEBUFFER, null); quadrants(gl2, rgba, 'WebGL2 default framebuffer blit');
  results.push('WebGL2 indexed/instanced draw, VAO, UBO, immutable texture FBO, readPixels and default framebuffer blit');
  gl2.readBuffer(gl2.NONE);
  sample2D(second, rgba, 'WebGL2 snapshots ignore application READ_BUFFER=NONE');
  clean(gl2, 'Internal snapshot does not leak a GL read-buffer error');
  equal([gl2.getParameter(gl2.READ_BUFFER)], [gl2.NONE], 'Snapshot preserves READ_BUFFER=NONE', 0);
  gl2.readBuffer(gl2.BACK);
  results.push('WebGL2 snapshot pixels and application read-buffer state are independent');

  quadrants(isolation, [red, red, red, red], 'Simultaneous context retains red');
  solid(isolation, [0, 1, 0, 1]);
  quadrants(gl1, uploaded, 'WebGL1 isolated from other contexts');
  quadrants(gl2, rgba, 'WebGL2 isolated from other contexts');
  results.push('Three simultaneous contexts retain independent bindings, resources and pixels');

  const alphaCanvas = canvas('alpha', 320, 20);
  const alpha = context(alphaCanvas, 'webgl', { alpha: true, premultipliedAlpha: false });
  ensure(alpha.getContextAttributes().alpha && !alpha.getContextAttributes().premultipliedAlpha, 'Unpremultiplied alpha context');
  solid(alpha, [1, 0, 0, .5]);
  equal(pixel(alpha, 16, 16), [255, 0, 0, 128], 'Unpremultiplied readPixels');
  const alphaCopy = document.createElement('canvas'); alphaCopy.width = alphaCopy.height = 32;
  const alpha2d = ensure(alphaCopy.getContext('2d'), 'Alpha drawImage context');
  alpha2d.fillStyle = '#204060'; alpha2d.fillRect(0, 0, 32, 32); alpha2d.drawImage(alphaCanvas, 0, 0);
  equal([...alpha2d.getImageData(16, 16, 1, 1).data], [144, 32, 48, 255], 'Unpremultiplied drawImage over known background');
  results.push('alpha:true/premultipliedAlpha:false raw readPixels and 2D source-over');

  const copy = canvas('draw-image', 420, 20);
  ensure(copy.getContext('2d'), 'Visible drawImage context').drawImage(first, 0, 0);
  sample2D(copy, uploaded, 'drawImage(WebGL canvas)');
  const dataURL = first.toDataURL('image/png');
  ensure(dataURL.startsWith('data:image/png;base64,'), 'WebGL toDataURL must encode PNG');
  const image = new Image();
  await new Promise((resolve, reject) => {
    image.onload = resolve; image.onerror = () => reject(new Error('WebGL toDataURL Image decoding failed'));
    image.src = dataURL;
  });
  sample2D(image, uploaded, 'Image decoded from WebGL toDataURL');
  results.push('WebGL drawImage, toDataURL PNG, Image decode and 2D pixel correctness');

  const resizedCanvas = canvas('resize', 120, 140, 16, 16);
  const resized = context(resizedCanvas, 'webgl', { alpha: true });
  solid(resized, [1, 0, 1, 1]);
  resizedCanvas.width = 40; resizedCanvas.height = 24;
  equal([resized.drawingBufferWidth, resized.drawingBufferHeight], [40, 24], 'Drawing buffer resize dimensions', 0);
  equal(pixel(resized, 39, 23), [0, 0, 0, 0], 'Resizing discards old drawing buffer');
  solid(resized, [0, 1, 1, 1]);
  for (const [x, y] of [[0, 0], [39, 0], [0, 23], [39, 23]]) equal(pixel(resized, x, y), [0, 255, 255, 255], 'Resized viewport reaches every corner');
  sample2D(resizedCanvas, Array(4).fill([0, 255, 255, 255]), 'Resized canvas drawImage');
  results.push('Drawing buffer non-square resize clears storage and redraws all corners');

  ensure(typeof OffscreenCanvas === 'function', 'OffscreenCanvas required');
  const offscreen = new OffscreenCanvas(32, 32), offgl = context(offscreen, 'webgl', { alpha: true });
  textureDraw(offgl);
  const bitmap = offscreen.transferToImageBitmap();
  equal([bitmap.width, bitmap.height], [32, 32], 'Transferred bitmap dimensions', 0);
  quadrants(offgl, Array(4).fill([0, 0, 0, 0]), 'Offscreen transfer replaces drawing buffer with transparent storage');
  solid(offgl, [0, 1, 0, 1]);
  sample2D(bitmap, rgba, 'Transferred bitmap independent of later producer writes');
  const bitmapCanvas = canvas('bitmap', 20, 140);
  const bitmapContext = ensure(bitmapCanvas.getContext('bitmaprenderer'), 'ImageBitmapRenderingContext required');
  bitmapContext.transferFromImageBitmap(bitmap);
  equal([bitmap.width, bitmap.height], [0, 0], 'transferFromImageBitmap consumes ownership', 0);
  sample2D(bitmapCanvas, rgba, 'Consumed ImageBitmap remains visible in destination');
  const nextBitmap = offscreen.transferToImageBitmap();
  bitmapContext.transferFromImageBitmap(nextBitmap);
  equal([nextBitmap.width, nextBitmap.height], [0, 0], 'Second bitmap ownership consumed', 0);
  sample2D(bitmapCanvas, Array(4).fill(green), 'Second transfer publishes current offscreen pixels');
  results.push('OffscreenCanvas transferToImageBitmap reset, snapshot independence and bitmaprenderer ownership transfer');

  const lostCanvas = canvas('loss-restore', 220, 140), lost = context(lostCanvas, 'webgl');
  solid(lost, [1, 0, 0, 1]);
  const oldBuffer = lost.createBuffer(); lost.bindBuffer(lost.ARRAY_BUFFER, oldBuffer);
  lost.bufferData(lost.ARRAY_BUFFER, new Float32Array([1, 2]), lost.STATIC_DRAW);
  const extension = ensure(lost.getExtension('WEBGL_lose_context'), 'ANGLE must expose WEBGL_lose_context');
  const event = name => new Promise((resolve, reject) => {
    const timer = setTimeout(() => { lostCanvas.removeEventListener(name, listener); reject(new Error(`${name} timeout`)); }, 8000);
    const listener = value => { value.preventDefault(); clearTimeout(timer); resolve(value); };
    lostCanvas.addEventListener(name, listener, { once: true });
  });
  const lossEvent = event('webglcontextlost'); extension.loseContext(); await lossEvent;
  ensure(lost.isContextLost(), 'Context must transition to lost');
  equal([lost.getError()], [lost.CONTEXT_LOST_WEBGL], 'Lost context reports CONTEXT_LOST_WEBGL', 0);
  ensure(lost.createBuffer() === null, 'Lost context cannot allocate GL objects');
  quadrants(gl1, uploaded, 'Context loss leaves WebGL1 peer intact');
  quadrants(gl2, rgba, 'Context loss leaves WebGL2 peer intact');
  // Restoration is permitted after the cancellable loss event finishes dispatch.
  await new Promise(resolve => setTimeout(resolve, 0));
  const restoredEvent = event('webglcontextrestored'); extension.restoreContext(); await restoredEvent;
  ensure(!lost.isContextLost(), 'Context must transition to restored');
  ensure(!lost.isBuffer(oldBuffer), 'Restoration invalidates old resources');
  solid(lost, [1, 1, 0, 1]); quadrants(lost, Array(4).fill(yellow), 'Restored context rebuilds resources and draws');
  results.push('WEBGL_lose_context loss event, lost errors/allocation semantics, isolated peers, restore event and real redraw');
  const workerCanvas = canvas('worker', 320, 140);
  const workerURL = URL.createObjectURL(new Blob([`let gl; self.onmessage=({data})=>{
    try {
      if(data.canvas) gl=data.canvas.getContext('webgl2',{alpha:false,preserveDrawingBuffer:true});
      if(!gl) throw new Error('No worker WebGL2 context');
      gl.clearColor(...data.color); gl.clear(gl.COLOR_BUFFER_BIT);
      const pixel=new Uint8Array(4); gl.readPixels(0,0,1,1,gl.RGBA,gl.UNSIGNED_BYTE,pixel);
      if(gl.getError()!==gl.NO_ERROR) throw new Error('Worker GL error');
      self.postMessage({pixel:[...pixel]});
    } catch(error) { self.postMessage({error:String(error)}); }
  };`], { type: 'text/javascript' }));
  const worker = new Worker(workerURL);
  const workerDraw = (message, transfers = []) => new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('Worker WebGL response timeout')), 8000);
    worker.onerror = event => { clearTimeout(timer); reject(new Error(`Worker load/execution: ${event.message}`)); };
    worker.onmessage = ({ data }) => {
      clearTimeout(timer);
      if (data.error) reject(new Error(data.error));
      else resolve(data.pixel);
    };
    worker.postMessage(message, transfers);
  });
  const transferred = workerCanvas.transferControlToOffscreen();
  equal(await workerDraw({ canvas: transferred, color: [1, 0, 0, 1] }, [transferred]), red, 'Worker owns transferred GL context and draws');
  URL.revokeObjectURL(workerURL);
  results.push('Dedicated Worker OffscreenCanvas ownership, real WebGL2 drawing and placeholder publication');

  window.webglSmokeUpdate = async () => {
    solid(isolation, [0, 0, 1, 1]);
    solid(resized, [1, 0, 1, 1]);
    equal(await workerDraw({ color: [0, 1, 0, 1] }), green, 'Worker publishes subsequent pixels');
    quadrants(gl1, uploaded, 'Peers remain intact after later frame');
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    return true;
  };
  await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
  return { checks: results, viewport: { width: innerWidth, height: innerHeight }, dataURL,
    contextVersions: [gl1.getParameter(gl1.VERSION), gl2.getParameter(gl2.VERSION)],
    renderer: [gl1.getParameter(gl1.RENDERER), gl2.getParameter(gl2.RENDERER)] };
}

app.whenReady().then(async () => {
  if (process.platform === 'darwin') {
    temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-webgl-smoke-'));
    const root = path.resolve(__dirname, '..');
    const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
    const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
    const helper = path.join(temporary, 'image-samples.node');
    const includes = ['', 'gen', 'third_party/abseil-cpp', 'base/allocator/partition_allocator/src',
      'gen/base/allocator/partition_allocator/src', 'third_party/libnode/src'];
    execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup', '-isysroot', sdk,
      ...includes.map(include => `-I${path.join(root, include)}`), '-framework', 'AppKit', '-framework', 'CoreGraphics',
      '-framework', 'ScreenCaptureKit', '-framework', 'ImageIO', path.join(__dirname, 'macos-owned-window-capture.mm'),
      path.join(__dirname, 'macos-gpu-compositing-helper.mm'), '-o', helper], { stdio: 'inherit', timeout: 30000 });
    native = require(helper);
  }
  window = new BrowserWindow({ width: 560, height: 260, useContentSize: true, frame: false, show: true,
    backgroundColor: '#204060', title: `mini-webgl-smoke-${process.pid}`,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  window.webContents.once('render-process-gone', (_, details) => finish(new Error(`WebGL renderer exited: ${JSON.stringify(details)}`)));
  window.webContents.once('unresponsive', () => finish(new Error('WebGL renderer became unresponsive')));
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(
    '<!doctype html><title>WebGL smoke</title><style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#204060}canvas{image-rendering:pixelated}</style>'));
  window.setContentSize(560, 260);
  if (native) await window.webContents.prepareToShow();
  const renderer = await window.webContents.executeJavaScript(`(${exerciseWebGL.toString()})()`);
  assert.ok(renderer.viewport.width >= 520 && renderer.viewport.height >= 240, 'Visible page must fit smoke canvases');
  const dataURL = renderer.dataURL;
  delete renderer.dataURL;
  report.renderer = renderer;
  const red = [255, 0, 0, 255], green = [0, 255, 0, 255];
  const blue = [0, 0, 255, 255], yellow = [255, 255, 0, 255];
  const quadrants = (x, y) => [{ x: x + 20, y: y + 20 }, { x: x + 60, y: y + 20 },
    { x: x + 20, y: y + 60 }, { x: x + 60, y: y + 60 }];
  const png = Buffer.from(dataURL.slice('data:image/png;base64,'.length), 'base64');
  const encodedSamples = pngSamples(png, [{ x: 8, y: 8 }, { x: 24, y: 8 }, { x: 8, y: 24 }, { x: 24, y: 24 }], { width: 32, height: 32 });
  assert.deepEqual({ width: encodedSamples.width, height: encodedSamples.height }, { width: 32, height: 32 }, 'toDataURL preserves drawing buffer dimensions');
  [blue, yellow, red, green].forEach((color, index) => assert.ok(closeColor(encodedSamples.samples[index], color, 2),
    `Native-decoded toDataURL quadrant ${index}: ${encodedSamples.samples[index]} expected ${color}`));
  report.png = encodedSamples;
  const probes = [...quadrants(20, 20), ...quadrants(120, 20), ...quadrants(420, 20),
    { x: 260, y: 60 }, { x: 360, y: 60 }, { x: 60, y: 180 }, { x: 160, y: 180 },
    { x: 260, y: 180 }, { x: 360, y: 180 }, { x: 500, y: 180 }];
  const expected = [blue, yellow, red, green, red, green, blue, yellow, blue, yellow, red, green,
    green, [144, 32, 48, 255], green, [0, 255, 255, 255], yellow, red, [32, 64, 96, 255]];
  await capture(probes, expected, 'Initial actual BrowserWindow WebGL resources, alpha, copies, bitmap, resize and restore');
  assert.equal(await window.webContents.executeJavaScript('webglSmokeUpdate()'), true);
  const updated = expected.slice(); updated[12] = blue; updated[15] = [255, 0, 255, 255]; updated[17] = green;
  await capture(probes, updated, 'Later actual BrowserWindow resources replace old pixels without damaging peers');
  finish();
}).catch(finish);
