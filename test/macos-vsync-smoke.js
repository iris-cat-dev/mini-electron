'use strict';

// Run with mini-electron --app=test/macos-vsync-smoke.js. Only owned windows move;
// physical display modes, arrangement, and primary designation are never changed.
// MINI_VSYNC_BASELINE=1 observes the old binary without invoking the new command.
// MINI_VSYNC_DISCONNECT=1 additionally waits for an operator to unplug/reconnect
// the secondary physical display. Otherwise JSON explicitly marks it not-run.
// DEVELOPER_DIR selects Xcode; MINI_VSYNC_SDK can explicitly select its macOS SDK.
// MINI_VSYNC_ARTIFACTS selects the persistent JSON/PNG evidence directory.
// PNGs capture only this process's actual composed owned windows in an unlocked
// graphical session. OS recording authorization may require restarting the launcher.
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, BrowserView } = require('electron');

const baseline = process.env.MINI_VSYNC_BASELINE === '1';
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-vsync-'));
const artifacts = path.resolve(process.env.MINI_VSYNC_ARTIFACTS ||
  path.join(__dirname, '..', 'out', 'rendering-vsync', `${Date.now()}-${process.pid}`));
fs.mkdirSync(artifacts, { recursive: true });
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const windows = [];
const contents = new Set();
const report = {
  test: 'macos-native-vsync', baseline, binary: process.execPath, pid: process.pid,
  artifacts, samples: [], idle: [], failures: [],
  coverage: { composedWindow: { required: true, status: 'incomplete',
    source: 'ScreenCaptureKit current-process window capture (macOS 14.4+) or legacy owned WindowServer window capture' } },
  screenshots: [],
  powerMeasurement: 'Process CPU time and package/interrupt wakeup counters are proxies, not electrical power.',
};
let native;
let finished = false;
let originalPrimary;
const watchdog = setTimeout(() => void finish(new Error('Native VSync smoke exceeded 210 seconds')), 210000);
app.on('window-all-closed', () => {});

function check(condition, message, evidence) {
  if (!condition) report.failures.push({ message, evidence });
  return !!condition;
}
function emit(record) { console.log(JSON.stringify(record)); }
async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (error) report.failures.push({ message: error.stack || String(error) });
  try { if (native) native.stopTiming(); } catch (cleanupError) {
    report.failures.push({ message: `Stopping timing observer: ${cleanupError}` });
  }
  // Closing owned windows first lets WebContents send their normal shutdown IPC.
  for (const window of windows.slice().reverse()) {
    try { if (!window.isDestroyed()) window.close(); } catch (cleanupError) {
      report.failures.push({ message: `Closing owned window: ${cleanupError}` });
    }
  }
  await pause(400);
  try { if (native) native.cleanup(); } catch (cleanupError) {
    report.failures.push({ message: `Cleaning helper: ${cleanupError}` });
  }
  fs.rmSync(temporary, { recursive: true, force: true });
  const missing = Object.entries(report.coverage).filter(([, value]) => value.required && value.status !== 'passed');
  report.missingRequiredCoverage = missing.map(([name]) => name);
  const captureIncomplete = report.coverage.composedWindow.status !== 'passed';
  report.status = captureIncomplete ? 'incomplete' : baseline ? 'baseline-observation' : report.failures.length ? 'failed' : missing.length ? 'incomplete' : 'passed';
  report.exitCode = captureIncomplete ? 2 : baseline ? 0 : report.failures.length ? 1 : missing.length ? 2 : 0;
  fs.writeFileSync(path.join(artifacts, 'result.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(`VSYNC_SMOKE_RESULT ${JSON.stringify(report)}`);
  app.exit(report.exitCode);
}
async function evaluate(webContents, source) {
  return webContents.executeJavaScript(source);
}
async function eventually(label, predicate, timeout = 10000) {
  const until = Date.now() + timeout;
  let last;
  do {
    last = await predicate();
    if (last === true) return;
    await pause(100);
  } while (Date.now() < until && !finished);
  throw new Error(`${label}: ${JSON.stringify(last)}`);
}
function cpuSnapshot() {
  const pids = new Set([process.pid]);
  for (const wc of contents) {
    try { const pid = wc.getProcessId(); if (pid > 0) pids.add(pid); } catch {}
  }
  return [...pids].map(pid => native.processStats(pid));
}
function cpuDelta(before, after) {
  return after.map(current => {
    const previous = before.find(item => item.pid === current.pid);
    if (!previous || previous.status !== 'observed' || current.status !== 'observed') return current;
    const seconds = current.wallSeconds - previous.wallSeconds;
    return { pid: current.pid, status: 'observed', seconds,
      cpuSeconds: current.cpuSeconds - previous.cpuSeconds,
      cpuPercentOfOneCore: 100 * (current.cpuSeconds - previous.cpuSeconds) / seconds,
      packageIdleWakeups: current.packageIdleWakeups - previous.packageIdleWakeups,
      interruptWakeups: current.interruptWakeups - previous.interruptWakeups,
      residentBytes: current.residentBytes };
  });
}
const documentURL = title => 'data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html>
<title>${title}</title><style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#172d43;color:#fff;font:18px sans-serif}
#dot{position:absolute;top:140px;left:30px;width:80px;height:80px;background:#7de2b1}h1{margin:40px}</style>
<h1>Native display-following presentation</h1><div id="dot"></div><script>
let token=0, running=false, result=null;
window.vsyncStop=()=>{token++;running=false;return true};
window.vsyncStart=(duration=2200)=>{
 const mine=++token;running=true;result=null;
 const wall=[],stamps=[];let began=0,first=0,index=0,wallStart=0;
 function frame(stamp){
  if(mine!==token)return;
  const now=performance.now();if(!began)began=now;
  document.getElementById('dot').style.transform='translateX('+(index++%700)+'px)';
  if(now-began>=500){if(!first){first=now;wallStart=Date.now()};wall.push(now);stamps.push(stamp)};
  if(first&&now-first>=duration){running=false;result={wall,stamps,dateElapsedMs:Date.now()-wallStart,
   width:innerWidth,height:innerHeight,dpr:devicePixelRatio};return};
  requestAnimationFrame(frame);
 };requestAnimationFrame(frame);return true;
};
window.vsyncResult=()=>result;
</script>`);
function makeWindow(label) {
  const title = `mini-vsync-${process.pid}-${label}`;
  const window = new BrowserWindow({ title, frame: false, width: 1000, height: 700, show: false,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  windows.push(window);
  contents.add(window.webContents);
  return { window, title };
}
async function loadOwned(owned) {
  await owned.window.loadURL(documentURL(owned.title));
  await owned.window.webContents.prepareToShow();
  owned.window.show();
  await pause(400);
}
async function move(owned, displayId) {
  const before = native.state(owned.title);
  native.move(owned.title, displayId);
  await eventually('AppKit must physically place owned window on requested screen', () => {
    const actual = native.state(owned.title);
    return actual.displayId === displayId || actual;
  });
  await pause(400);
  const after = native.state(owned.title);
  check(after.windowWidth === before.windowWidth && after.windowHeight === before.windowHeight,
    'Native display switch must preserve window size', { before, after });
  if (before.displayId !== displayId)
    check(after.screenChangeNotifications > before.screenChangeNotifications,
      'A real NSWindowDidChangeScreenNotification must accompany the screen switch', { before, after });
  check(after.primaryDisplayId === originalPrimary, 'Test must not change primary display', after);
  return { before, after };
}
function summarize(data) {
  const intervals = values => values.slice(1).map((value, index) => value - values[index]);
  const wallIntervalsMs = intervals(data.wall);
  const rafTimestampIntervalsMs = intervals(data.stamps);
  const sorted = wallIntervalsMs.slice().sort((a, b) => a - b);
  const elapsed = data.wall.at(-1) - data.wall[0];
  return { callbacks: data.wall.length, elapsedMs: elapsed, hz: 1000 * (data.wall.length - 1) / elapsed,
    medianMs: sorted[Math.floor(sorted.length / 2)], p95Ms: sorted[Math.floor(sorted.length * 0.95)],
    wallIntervalsMs, rafTimestampIntervalsMs, dateElapsedMs: data.dateElapsedMs,
    width: data.width, height: data.height, dpr: data.dpr };
}
function targetHz(screen, reference) {
  // ProMotion's advertised maximum and the live CV source may differ. Record
  // both, but never downgrade a real 120Hz screen's acceptance to a 60Hz source.
  if (screen.modeHz > 0) return screen.modeHz;
  if (screen.minimumRefreshIntervalMs > 0) return 1000 / screen.minimumRefreshIntervalMs;
  if (screen.maximumFramesPerSecond > 0) return screen.maximumFramesPerSecond;
  return reference.callbackHz || 0;
}
async function capture(owned, label) {
  const filename = path.join(artifacts, `${label.replace(/[^a-z0-9-]/gi, '-')}.png`);
  try {
    const bytes = await native.snapshot(owned.title, filename);
    report.screenshots.push({ label, file: filename, bytes, status: 'passed',
      source: 'WindowServer-composed owned window', displayId: native.state(owned.title).displayId });
  } catch (error) {
    report.screenshots.push({ label, status: 'incomplete', reason: String(error) });
  }
  report.coverage.composedWindow.status = report.screenshots.every(item => item.status === 'passed') ? 'passed' : 'incomplete';
}
async function cadence(owned, label, wc = owned.window.webContents, expectedOverride, duration = 2200) {
  const screen = native.state(owned.title);
  const cvStart = native.startTiming(screen.displayId);
  const framesBefore = native.frames();
  const cpuBefore = cpuSnapshot();
  await evaluate(wc, `vsyncStart(${duration})`);
  let data;
  await eventually(`${label} rAF wall-clock sample`, async () => {
    const serialized = await evaluate(wc, 'JSON.stringify(vsyncResult())');
    data = JSON.parse(serialized);
    return data !== null;
  }, 12000);
  const reference = native.timing();
  native.stopTiming(); // An observer DisplayLink must not contaminate idle work.
  const endScreen = native.state(owned.title);
  const framesAfter = native.frames();
  const result = { scenario: label, screen, endScreen, cvStart, reference,
    ...summarize(data), framesBefore, framesAfter, cpu: cpuDelta(cpuBefore, cpuSnapshot()) };
  const expectedHz = expectedOverride || targetHz(screen, reference);
  result.expectedHz = expectedHz;
  result.tolerance = { minimumHz: expectedHz * 0.72, maximumHz: expectedHz * 1.2 };
  const startFailures = report.failures.length;
  check(screen.displayId === endScreen.displayId, 'Window must remain on sampled actual display', { label, screen, endScreen });
  check(expectedHz > 0, 'Actual display refresh must be observable', { label, screen, reference });
  check(result.hz >= expectedHz * 0.72 && result.hz <= expectedHz * 1.2,
    'Active wall-clock rAF cadence must follow the actual display (72%-120% of target)', result);
  check(result.wallIntervalsMs.length >= Math.min(40, Math.floor(expectedHz * duration / 2000)) &&
    result.dateElapsedMs >= duration * 0.9,
    'Cadence must cover the requested real wall-clock interval', { label, callbacks: result.callbacks, dateElapsedMs: result.dateElapsedMs, duration });
  check(framesBefore.length > 0 && framesAfter.some(after => framesBefore.some(before => before.fd === after.fd && after.generation > before.generation)),
    'Active native frames must really publish while rAF runs (CPU or GPU)', { label, framesBefore, framesAfter });
  check(result.width === 1000 && result.height === 700,
    'Rendering workload must remain 1000x700 CSS pixels', { label, width: result.width, height: result.height });
  result.status = report.failures.length === startFailures ? 'passed' : 'failed';
  report.samples.push(result);
  emit(result);
  await capture(owned, label);
  return result;
}
async function idle(label) {
  for (const wc of contents) {
    try { await evaluate(wc, 'typeof vsyncStop === "function" && vsyncStop()'); } catch {}
  }
  native.stopTiming();
  await pause(1200);
  const before = native.frames();
  const cpuBefore = cpuSnapshot();
  // Do not poll executeJavaScript or create an observer DisplayLink here.
  await pause(2500);
  const after = native.frames();
  const cpuAfter = cpuSnapshot();
  const stable = before.length > 0 && JSON.stringify(before) === JSON.stringify(after) &&
    before.every(frame => frame.generation > 0 && frame.generation % 2 === 0);
  const cpu = cpuDelta(cpuBefore, cpuAfter);
  const rendererPids = [...contents].map(wc => wc.getProcessId());
  const observed = rendererPids.every(pid => pid > 0 && cpu.some(item => item.pid === pid)) &&
    cpu.every(item => item.status === 'observed');
  check(observed, 'Static CPU/wakeup evidence must include the browser and every owned renderer', cpu);
  const quiet = observed && cpu.filter(item => item.pid !== process.pid).every(item =>
    item.interruptWakeups / item.seconds < 30 && item.cpuPercentOfOneCore < 5);
  check(quiet, 'Static renderers must not retain active-rate wakeups or busy CPU work', cpu);
  check(stable, 'After every page stops rAF, ALL owned framebuffer generations must stay stable', { label, before, after });
  const result = { scenario: label, status: stable && quiet ? 'passed' : 'failed', before, after,
    cpu, intervalMs: 2500,
    measurement: 'All writable owned framebuffer mappings; browser and actual renderer process counters; no electrical power claim.' };
  report.idle.push(result);
  emit(result);
  return result;
}
async function optionalDisconnect(owned, secondary) {
  if (process.env.MINI_VSYNC_DISCONNECT !== '1' || !secondary) {
    report.coverage.physicalDisconnect = { required: false, status: 'not-run',
      reason: 'No virtual or physical display is disconnected automatically. Set MINI_VSYNC_DISCONNECT=1 and unplug/reconnect the secondary display during this phase.' };
    return;
  }
  await move(owned, secondary.displayId);
  emit({ action: 'Unplug the secondary display now; reconnect when prompted', secondary, timeoutSeconds: 45 });
  await eventually('Actual secondary display disconnect', () => !native.screens().some(screen => screen.displayId === secondary.displayId), 45000);
  await eventually('Window migrates to an attached display after disconnect', () => native.state(owned.title).displayId !== secondary.displayId, 10000);
  await cadence(owned, 'physical-disconnect-recovery');
  emit({ action: 'Reconnect the secondary display now', name: secondary.name, timeoutSeconds: 45 });
  let reconnected;
  await eventually('Actual secondary display reconnect', () => {
    reconnected = native.screens().find(screen => screen.name === secondary.name && screen.displayId !== native.state(owned.title).displayId);
    return !!reconnected;
  }, 45000);
  await move(owned, reconnected.displayId);
  const sample = await cadence(owned, 'physical-reconnect');
  report.coverage.physicalDisconnect = { required: false, status: sample.status, original: secondary, reconnected };
}

app.whenReady().then(async () => {
  if (process.platform !== 'darwin') throw new Error('This smoke requires actual macOS AppKit windows');
  const root = path.resolve(__dirname, '..');
  const env = { ...process.env };
  delete env.SDKROOT; // Do not mix a global CLT SDK with Xcode's selected clang.
  const sdk = process.env.MINI_VSYNC_SDK || execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { env, encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--sdk', 'macosx', '--find', 'clang++'], { env, encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'vsync.node');
  const includes = ['', 'gen', 'third_party/abseil-cpp', 'base/allocator/partition_allocator/src', 'gen/base/allocator/partition_allocator/src', 'third_party/libnode/src'];
  report.toolchain = { sdk, compiler, developerDir: env.DEVELOPER_DIR || null };
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup', '-isysroot', sdk,
    ...includes.map(include => `-I${path.join(root, include)}`), '-framework', 'AppKit', '-framework', 'CoreGraphics', '-framework', 'CoreVideo', '-framework', 'QuartzCore', '-framework', 'ScreenCaptureKit',
    path.join(__dirname, 'macos-owned-window-capture.mm'),
    path.join(__dirname, 'macos-vsync-helper.mm'), '-o', helper], { env, stdio: 'inherit', timeout: 30000 });
  native = require(helper);
  report.cpuCounterCalibration = native.processStats(process.pid);
  check(Number.isFinite(report.cpuCounterCalibration.selfRusageCpuSeconds) &&
    Math.abs(report.cpuCounterCalibration.cpuSeconds -
      report.cpuCounterCalibration.selfRusageCpuSeconds) < 0.005,
  'Mach CPU tick conversion must agree with independent getrusage seconds',
  report.cpuCounterCalibration);
  report.screens = native.screens();
  emit({ scenario: 'attached-native-screens', screens: report.screens, toolchain: report.toolchain });
  const main = makeWindow('main');
  await loadOwned(main);
  const initialState = native.state(main.title);
  originalPrimary = initialState.primaryDisplayId;
  report.initialWindowState = initialState;
  const initial = await cadence(main, 'current-physical-display');
  report.coverage.currentDisplay = { required: true, status: initial.status };
  const usable = report.screens.filter(screen => screen.width >= 1000 && screen.height >= 700);
  const high = usable.slice().sort((a, b) => targetHz(b, {}) - targetHz(a, {}))[0];
  const low = high && usable.filter(screen => screen.displayId !== high.displayId && screen.scale === high.scale && targetHz(screen, {}) < targetHz(high, {}) * 0.75)
    .sort((a, b) => targetHz(a, {}) - targetHz(b, {}))[0];
  if (high && low) {
    await move(main, high.displayId);
    const first = await cadence(main, 'physical-high-refresh');
    const transition = await move(main, low.displayId);
    const second = await cadence(main, 'physical-equal-dpr-low-refresh');
    await move(main, high.displayId);
    const third = await cadence(main, 'physical-equal-dpr-high-refresh-recovery');
    const sameDpr = transition.before.windowScale === transition.after.windowScale && first.dpr === second.dpr && second.dpr === third.dpr;
    check(sameDpr, 'Equal-size screen switch must also preserve the actual native and DOM DPR', { transition, dprs: [first.dpr, second.dpr, third.dpr] });
    report.coverage.equalSizeDprScreenSwitch = { required: true,
      status: sameDpr && [first, second, third].every(item => item.status === 'passed') ? 'passed' : 'failed', high, low, transition };
  } else {
    report.coverage.equalSizeDprScreenSwitch = { required: true, status: 'unavailable',
      reason: 'Requires two attached screens with equal DPR, different refresh rates, and room for 1000x700; only owned windows may move.' };
  }
  await idle('static-after-native-screen-switches');
  if (baseline) {
    report.coverage.invalidDisplayRecovery = { required: false, status: 'not-run', reason: 'Old-binary baseline does not invoke the new trusted command.' };
  } else {
    const before = native.state(main.title);
    main.window.webContents._sendCommand('setVSyncDisplayId', { displayId: -1 });
    await pause(500);
    const invalid = await cadence(main, 'invalid-display-default-cadence', main.window.webContents, 60);
    // This validates a lost/invalid identity, NOT an actual physical unplug.
    main.window.webContents._sendCommand('setVSyncDisplayId', { displayId: before.displayId });
    await pause(500);
    const recovered = await cadence(main, 'trusted-display-identity-recovery');
    report.coverage.invalidDisplayRecovery = { required: true,
      status: [invalid, recovered].every(item => item.status === 'passed') ? 'passed' : 'failed',
      scope: 'Trusted invalid-ID injection and recovery only, not physical display removal.' };
    const cycles = [];
    for (let cycle = 0; cycle < 3; ++cycle) {
      for (const displayId of [-1, before.displayId]) {
        // Change sources while frames are in flight, then restart observation.
        // This catches callback-registration sequence bugs after fallback.
        await evaluate(main.window.webContents, 'vsyncStart(20000)');
        await pause(50);
        main.window.webContents._sendCommand('setVSyncDisplayId', { displayId });
        await pause(100);
        cycles.push(await cadence(main, `active-recovery-${cycle}-${displayId === -1 ? 'fallback' : 'native'}`,
          main.window.webContents, displayId === -1 ? 60 : targetHz(before, {}), 550));
      }
    }
    report.coverage.repeatedDisplayRecovery = { required: true,
      status: cycles.every(item => item.status === 'passed') ? 'passed' : 'failed',
      transitions: cycles.length, scope: 'Repeated invalid/valid source replacement with active native frames.' };
  }
  if (high && low && typeof BrowserView === 'function') {
    const companion = makeWindow('browserview-host');
    await loadOwned(companion);
    await move(main, high.displayId);
    await move(companion, low.displayId);
    const view = new BrowserView({ webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
    const wc = view.webContents;
    contents.add(wc);
    await wc.loadURL(documentURL(`mini-vsync-view-${process.pid}`));
    view.setBounds({ x: 0, y: 0, width: 1000, height: 700 });
    main.window.setBrowserView(view);
    await pause(600);
    const attached = await cadence(main, 'browserview-attach-high-refresh', wc);
    companion.window.setBrowserView(view); // Real NSView reparent; no manual display-ID command.
    await pause(600);
    const reparented = await cadence(companion, 'browserview-reparent-low-refresh', wc);
    main.window.setBrowserView(view);
    await pause(600);
    const returned = await cadence(main, 'browserview-reparent-high-refresh-recovery', wc);
    report.coverage.browserViewReparent = { required: true,
      status: [attached, reparented, returned].every(item => item.status === 'passed') ? 'passed' : 'failed' };
    await idle('static-all-window-and-browserview-framebuffers');
    main.window.removeBrowserView(view);
  } else {
    report.coverage.browserViewReparent = { required: true, status: 'unavailable', reason: 'Requires BrowserView and two usable equal-DPR different-refresh screens.' };
  }
  await optionalDisconnect(main, low);
  await idle('final-static-no-animation');
  report.coverage.staticGeneration = { required: true, status: report.idle.every(item => item.status === 'passed') ? 'passed' : 'failed' };
  check(native.state(main.title).primaryDisplayId === originalPrimary, 'Physical primary display must remain unchanged', native.state(main.title));
  await finish();
}).catch(error => void finish(error));
