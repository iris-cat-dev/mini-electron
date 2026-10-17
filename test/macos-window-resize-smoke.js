'use strict';

// Requires a source checkout and Xcode. All input is genuine NSEvent input sent
// to the owned NSWindow, not DOM events or webContents.sendInputEvent(). No
// Accessibility permission, hardware pointer movement, or unlocked desktop is
// required. The helper prequeues an escape mouseUp for old AppKit tracking loops.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-window-resize-'));
const baseline = { x: 180, y: 180, width: 520, height: 400 };
const minimum = { width: 360, height: 280 };
const maximum = { width: 760, height: 600 };
let window;
let native;
let sequence = 0;
let finished = false;
const deadline = setTimeout(() => finish(new Error('Native window resize smoke timed out')), 120000);
app.on('window-all-closed', () => {});

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  try {
    if (window && !window.isDestroyed()) window.destroy();
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
    if (error) console.error(error.stack || error);
    else console.log('PASS: native window edges/corners, anchoring, constraints, guards, resize cursors and renderer viewport');
    app.exit(error ? 1 : 0);
  }
}

const pause = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
const evaluate = async source => (await window.webContents.executeJavaScript(source)).value;

async function eventually(read, expected, message) {
  const until = Date.now() + 4000;
  let actual;
  do {
    actual = await read();
    if (JSON.stringify(actual) === JSON.stringify(expected)) return;
    await pause(20);
  } while (Date.now() < until);
  assert.deepEqual(actual, expected, message);
}

async function viewport(title) {
  const content = native.snapshot(title).content;
  await eventually(async () => JSON.parse(await evaluate('JSON.stringify([innerWidth,innerHeight])')),
    [Math.round(content.width), Math.round(content.height)],
    'The renderer viewport must eventually match the actual resized native content');
}

async function withWindow(options, run) {
  const title = `mini-native-resize-${process.pid}-${++sequence}`;
  window = new BrowserWindow({
    width: baseline.width, height: baseline.height, title,
    minWidth: minimum.width, minHeight: minimum.height,
    maxWidth: maximum.width, maxHeight: maximum.height,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
    ...options,
  });
  try {
    await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html>
      <title>${title}</title><style>
        html,body { margin:0; width:100%; height:100%; background:#2576a2; }
        #caption { position:absolute; left:0; top:0; width:100%; height:64px; -webkit-app-region:drag; }
        #control { position:absolute; left:220px; top:40px; width:100px; height:20px; padding:0; border:0; -webkit-app-region:no-drag; }
      </style><div id="caption"><button id="control">no-drag</button></div>
      <script>window.controlDowns=0;document.querySelector('#control').addEventListener('mousedown',()=>++window.controlDowns);</script>`));
    assert.deepEqual(native.setFrame(title, baseline), baseline, 'Native baseline must be exact');
    await viewport(title);
    console.log(`${title}: frontmost=${native.snapshot(title).frontmostBundleIdentifier || '(none)'}`);
    await run(title);
  } finally {
    window.destroy();
    window = undefined;
  }
}

const directions = [
  { name: 'left', left: true },
  { name: 'right', right: true },
  { name: 'bottom', bottom: true },
  { name: 'top', top: true },
  { name: 'bottom-left', bottom: true, left: true },
  { name: 'bottom-right', bottom: true, right: true },
  { name: 'top-left', top: true, left: true },
  { name: 'top-right', top: true, right: true },
];

function startPoint(direction, inset = 1) {
  return {
    x: direction.left ? inset : direction.right ? baseline.width - inset : baseline.width / 2,
    y: direction.bottom ? inset : direction.top ? baseline.height - inset : baseline.height / 2,
  };
}

function expectedFrame(direction, dx, dy, min = minimum, max = maximum) {
  const width = Math.max(min.width, Math.min(max.width,
    baseline.width + (direction.left ? -dx : direction.right ? dx : 0)));
  const height = Math.max(min.height, Math.min(max.height,
    baseline.height + (direction.bottom ? -dy : direction.top ? dy : 0)));
  return {
    x: direction.left ? baseline.x + baseline.width - width : baseline.x,
    y: direction.bottom ? baseline.y + baseline.height - height : baseline.y,
    width, height,
  };
}

async function resize(title, direction, dx, dy, label, min = minimum, max = maximum, inset = 1) {
  assert.deepEqual(native.setFrame(title, baseline), baseline);
  const result = native.gesture(title, { ...startPoint(direction, inset), dx, dy });
  assert.deepEqual(result.before, baseline, `${label}: initial frame`);
  assert.deepEqual(result.half, expectedFrame(direction, dx / 2, dy / 2, min, max),
    `${label}: first drag must use screen-space deltas and anchor the opposite edge`);
  const expected = expectedFrame(direction, dx, dy, min, max);
  assert.deepEqual(result.dragged, expected,
    `${label}: final native geometry must use total, not accumulated, pointer displacement`);
  assert.deepEqual(result.released, expected, `${label}: mouseUp must not alter the frame`);
  assert.deepEqual(result.afterUp, expected, `${label}: a drag after mouseUp must not resize`);
  await viewport(title);
}

async function unchanged(title, point, label, extra = {}) {
  assert.deepEqual(native.setFrame(title, baseline), baseline);
  const result = native.gesture(title, { ...point, dx: 48, dy: 36, ...extra });
  assert.deepEqual(result.before, baseline, `${label}: style change must preserve the baseline`);
  for (const phase of ['half', 'dragged', 'released', 'afterUp'])
    assert.deepEqual(result[phase], baseline, `${label}: ${phase} must not resize or move`);
  assert.deepEqual(native.snapshot(title).frame, baseline, `${label}: restoring the style preserves the frame`);
  await viewport(title);
}

function resizeCursors(title, enabled) {
  for (const direction of directions) {
    assert.equal(native.cursorAt(title, startPoint(direction)), !enabled,
      `${direction.name}: hover must ${enabled ? 'show' : 'not show'} a resize cursor`);
  }
  assert.equal(native.cursorAt(title, { x: baseline.width / 2, y: baseline.height / 2 }), true,
    'Interior hover must not show a resize cursor');
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const root = path.resolve(__dirname, '..');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'resize.node');
  const includes = ['', 'gen', 'third_party/abseil-cpp', 'base/allocator/partition_allocator/src', 'gen/base/allocator/partition_allocator/src', 'third_party/libnode/src'];
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup', '-isysroot', sdk,
    ...includes.map(include => `-I${path.join(root, include)}`), '-framework', 'AppKit',
    path.join(__dirname, 'macos-window-resize-helper.mm'), '-o', helper], { stdio: 'inherit', timeout: 30000 });
  native = require(helper);

  for (const options of [{ titleBarStyle: 'hidden' }, { frame: false }]) {
    await withWindow(options, async title => {
      // Geometry runs first so the old runtime fails on actual NSWindow geometry,
      // rather than on a cursor-registration or implementation-wiring assertion.
      for (const direction of directions) {
        const dx = direction.left ? -48 : direction.right ? 48 : 0;
        const dy = direction.bottom ? -36 : direction.top ? 36 : 0;
        await resize(title, direction, dx, dy, `${direction.name} expand`);
        await resize(title, direction, -dx, -dy, `${direction.name} shrink`);
      }
      await resize(title, directions[4], 1000, 1000, 'constructor minimum clamp, opposite top/right anchored');
      await resize(title, directions[4], -1000, -1000, 'constructor maximum clamp, opposite top/right anchored');
      await resize(title, directions[7], -1000, -1000, 'constructor minimum clamp, opposite bottom/left anchored');
      await resize(title, directions[7], 1000, 1000, 'constructor maximum clamp, opposite bottom/left anchored');
      // A point strictly inside the 5pt border must not resize, while 4pt must.
      await resize(title, directions[1], 48, 0, 'four-point border hit', minimum, maximum, 4);
      await unchanged(title, startPoint(directions[1], 6), 'six-point interior boundary');
      await unchanged(title, { x: baseline.width / 2, y: baseline.height / 2 }, 'interior');
      assert.deepEqual(native.setFrame(title, baseline), baseline);
      // AppKit does not offer native full screen for this frameless style.
      // Exercise the real transition on the hidden-titlebar style used by OMP.
      if (options.frame !== false) {
        window.setFullScreen(true);
        await eventually(() => window.isFullScreen(), true, 'The window must enter native full screen');
        await eventually(() => native.snapshot(title).fullScreenSettled, true,
          'AppKit must finish entering full screen before the guarded gesture');
        const fullScreenFrame = native.snapshot(title).frame;
        const guarded = native.gesture(title, {
          x: fullScreenFrame.width - 1, y: 1, dx: 48, dy: -36,
        });
        for (const phase of ['half', 'dragged', 'released', 'afterUp'])
          assert.deepEqual(guarded[phase], fullScreenFrame, 'Full-screen edge input must not resize');
        window.setFullScreen(false);
        await eventually(() => window.isFullScreen(), false, 'The window must exit native full screen');
        await eventually(() => native.snapshot(title).fullScreenSettled, false,
          'AppKit must finish leaving full screen');
        await eventually(() => native.snapshot(title).frame, baseline,
          'Leaving native full screen must restore the prior bounds');
      }
      assert.deepEqual(native.setFrame(title, baseline), baseline);
      resizeCursors(title, true);

      // Below AppKit's titlebar buttons, a real no-drag control remains web input
      // even though its parent is an app-region:drag custom caption.
      const before = await evaluate('window.controlDowns');
      await unchanged(title, { x: 250, y: baseline.height - 50 }, 'caption no-drag control');
      await eventually(() => evaluate('window.controlDowns'), before + 1,
        'A native mouseDown in the no-drag control must reach the renderer');

      window.setMinimumSize(440, 340);
      await resize(title, directions[4], 1000, 1000, 'setMinimumSize remains effective', { width: 440, height: 340 });
    });
    await withWindow({ ...options, resizable: false }, async title => {
      await unchanged(title, startPoint(directions[5]), 'resizable:false');
      resizeCursors(title, false);
    });
  }
  finish();
}).catch(finish);
