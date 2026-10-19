#!/usr/bin/env node
'use strict';

const fs = require('node:fs');
const fsp = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const http = require('node:http');
const crypto = require('node:crypto');
const { spawn, execFile } = require('node:child_process');

const ROOT = path.resolve(__dirname, '../..');
const SCENARIOS = ['raf-baseline', 'dom-create', 'layout-update', 'canvas-paint', 'transform-animation', 'programmatic-scroll'];
const MAX_RESULT_BYTES = 8 * 1024 * 1024;
const HELP = `Usage: node benchmarks/rendering/run.js [options]

Compare this mini-electron build with locally installed, headed Chrome on macOS.
No browser is downloaded or updated. Do not interact with either window during a run.

  --runtime PATH       Runtime executable (default: out/mac-arm64/mini-electron)
  --chrome PATH        Chrome executable (default: /Applications/Google Chrome.app/Contents/MacOS/Google Chrome)
  --trials N           Trials per engine (default: 3; order alternates)
  --duration-ms N      Duration of each animation measurement (default: 2000)
  --warmup-ms N        Per-scenario warmup (default: 500)
  --samples N          Operation samples (default: 20)
  --width N            Comparison content viewport width (default: 1000)
  --height N           Comparison content viewport height (default: 700)
  --output DIR         Artifacts (default: out/rendering-benchmark/<timestamp>)
  --serve              Only serve the manual page until interrupted; launch no engines
  --help               Show this help

Requires Node.js 22+ (global WebSocket), and a graphical macOS login session for comparison.
Reports contain rAF callback cadence, not measured display/presentation FPS.
`;

function options(argv) {
  const stamp = new Date().toISOString().replace(/[:.]/g, '-');
  const result = {
    runtime: path.join(ROOT, 'out/mac-arm64/mini-electron'),
    chrome: '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    trials: 3, durationMs: 2000, warmupMs: 500, samples: 20, width: 1000, height: 700,
    output: path.join(ROOT, 'out/rendering-benchmark', stamp), serve: false,
  };
  const names = { '--runtime': 'runtime', '--chrome': 'chrome', '--trials': 'trials', '--duration-ms': 'durationMs', '--warmup-ms': 'warmupMs', '--samples': 'samples', '--width': 'width', '--height': 'height', '--output': 'output' };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--help') return { help: true };
    if (arg === '--serve') { result.serve = true; continue; }
    const key = names[arg];
    if (!key || i + 1 === argv.length || argv[i + 1].startsWith('--')) throw new Error(`Unknown option or missing value: ${arg}\nUse --help for usage.`);
    const value = argv[++i];
    if (['runtime', 'chrome', 'output'].includes(key)) result[key] = path.resolve(value);
    else {
      const number = Number(value);
      if (!/^\d+$/.test(value) || !Number.isSafeInteger(number) || number < (key === 'warmupMs' ? 0 : 1)) throw new Error(`${arg} must be a ${key === 'warmupMs' ? 'nonnegative' : 'positive'} safe integer`);
      result[key] = number;
    }
  }
  return result;
}

function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  // A result or process exit can arrive before the launch sequence starts awaiting it.
  promise.catch(() => {});
  return { promise, resolve, reject };
}

function sleep(ms, signal) {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) return reject(signal.reason);
    const done = () => { clearTimeout(timer); signal?.removeEventListener('abort', abort); resolve(); };
    const abort = () => { clearTimeout(timer); signal.removeEventListener('abort', abort); reject(signal.reason); };
    const timer = setTimeout(done, ms);
    signal?.addEventListener('abort', abort, { once: true });
  });
}

function deadline(promise, ms, label, signal) {
  return new Promise((resolve, reject) => {
    const finish = (fn, value) => { clearTimeout(timer); signal?.removeEventListener('abort', abort); fn(value); };
    const abort = () => finish(reject, signal.reason);
    const timer = setTimeout(() => finish(reject, new Error(`${label} timed out after ${ms} ms`)), ms);
    if (signal?.aborted) abort();
    else signal?.addEventListener('abort', abort, { once: true });
    promise.then(value => finish(resolve, value), error => finish(reject, error));
  });
}

function sha256(bytes) { return crypto.createHash('sha256').update(bytes).digest('hex'); }
function saveJSON(file, value) { fs.writeFileSync(file, `${JSON.stringify(value, null, 2)}\n`); }
function errorText(error) { return error?.stack || String(error); }

async function makeServer(files) {
  const active = new Map();
  const server = http.createServer((request, response) => {
    const send = (status, message) => { response.writeHead(status, { 'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store' }); response.end(message); };
    let url;
    try { url = new URL(request.url, 'http://127.0.0.1'); } catch { return send(400, 'Bad URL'); }
    if (request.method === 'GET' && files.has(url.pathname)) {
      response.writeHead(200, { 'Content-Type': url.pathname.endsWith('.js') ? 'text/javascript; charset=utf-8' : 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
      return response.end(files.get(url.pathname));
    }
    if (request.method !== 'POST' || url.pathname !== '/result') return send(404, 'Not found');
    const token = url.searchParams.get('token');
    const slot = token && active.get(token);
    if (!slot || slot.received) return send(403, 'No active result token');
    if (!/^application\/json(?:\s*;|$)/i.test(request.headers['content-type'] || '')) return send(415, 'Expected application/json');
    let bytes = 0;
    const chunks = [];
    let excessive = false;
    request.on('data', chunk => {
      bytes += chunk.length;
      if (bytes > MAX_RESULT_BYTES) {
        if (!excessive) { excessive = true; slot.reject(new Error(`Result exceeds ${MAX_RESULT_BYTES} bytes`)); send(413, 'Result too large'); }
      } else if (!excessive) chunks.push(chunk);
    });
    request.on('error', error => slot.reject(error));
    request.on('end', () => {
      if (excessive) return;
      try {
        const result = JSON.parse(Buffer.concat(chunks).toString('utf8'));
        slot.received = true;
        send(200, 'Accepted');
        slot.resolve(result);
      } catch (error) { slot.reject(new Error(`Invalid result JSON: ${error.message}`)); send(400, 'Invalid JSON'); }
    });
  });
  server.requestTimeout = 15000;
  await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
  return { server, active, origin: `http://127.0.0.1:${server.address().port}` };
}

class CDP {
  constructor(socket) {
    this.socket = socket;
    this.next = 0;
    this.pending = new Map();
    socket.addEventListener('message', event => {
      let message;
      try { message = JSON.parse(event.data); } catch { return; }
      const entry = this.pending.get(message.id);
      if (!entry) return;
      clearTimeout(entry.timer);
      this.pending.delete(message.id);
      if (message.error) entry.reject(new Error(`CDP ${entry.method}: ${message.error.message}`));
      else entry.resolve(message.result);
    });
    const closed = () => {
      for (const entry of this.pending.values()) { clearTimeout(entry.timer); entry.reject(new Error(`CDP closed during ${entry.method}`)); }
      this.pending.clear();
    };
    socket.addEventListener('close', closed);
    socket.addEventListener('error', closed);
  }
  static async connect(url, signal) {
    const socket = new WebSocket(url);
    try {
      await deadline(new Promise((resolve, reject) => {
        socket.addEventListener('open', resolve, { once: true });
        socket.addEventListener('error', () => reject(new Error(`Cannot connect to Chrome DevTools: ${url}`)), { once: true });
      }), 15000, 'Chrome DevTools connection', signal);
      return new CDP(socket);
    } catch (error) { socket.close(); throw error; }
  }
  send(method, params = {}, sessionId) {
    const id = ++this.next;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.pending.delete(id); reject(new Error(`CDP ${method} timed out`)); }, 15000);
      this.pending.set(id, { resolve, reject, timer, method });
      try { this.socket.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) })); }
      catch (error) { clearTimeout(timer); this.pending.delete(id); reject(error); }
    });
  }
  close() { this.socket.close(); }
}

function launch(executable, args, env, prefix) {
  const stdoutPath = `${prefix}.stdout.log`;
  const stderrPath = `${prefix}.stderr.log`;
  const stdout = fs.openSync(stdoutPath, 'w');
  const stderr = fs.openSync(stderrPath, 'w');
  let child;
  try { child = spawn(executable, args, { env, cwd: ROOT, detached: true, stdio: ['pipe', stdout, stderr] }); }
  finally { fs.closeSync(stdout); fs.closeSync(stderr); }
  const failure = deferred();
  const exited = deferred();
  const processInfo = { child, failure: failure.promise, exited: exited.promise, stopping: false, cdp: null, stdoutPath, stderrPath, exit: null };
  child.stdin.on('error', () => {});
  child.once('error', error => { failure.reject(error); exited.resolve(); });
  child.once('exit', (code, signal) => {
    processInfo.exit = { code, signal };
    exited.resolve();
    if (!processInfo.stopping) failure.reject(new Error(`Owned process exited early (code=${code}, signal=${signal}); see ${stderrPath}`));
  });
  return processInfo;
}

function groupAlive(pid) {
  if (!pid) return false;
  try { process.kill(-pid, 0); return true; } catch (error) {
    if (error.code === 'ESRCH') return false;
    // A permission-denied probe still means the group exists. Chrome can pass
    // through this state while exiting; wait for disappearance, not permission.
    if (error.code === 'EPERM') return true;
    throw error;
  }
}

async function stopOwned(owned, engine) {
  if (!owned) return;
  owned.stopping = true;
  const pid = owned.child.pid;
  if (engine === 'chrome' && owned.cdp) {
    await Promise.race([owned.cdp.send('Browser.close').catch(() => {}), sleep(2000)]);
  } else if (owned.child.stdin.writable) owned.child.stdin.end('quit\n');
  const waitGroup = async ms => {
    const until = Date.now() + ms;
    while (groupAlive(pid) && Date.now() < until) await sleep(100);
    return !groupAlive(pid);
  };
  if (!await waitGroup(4000)) {
    try { process.kill(-pid, 'SIGTERM'); } catch (error) { if (error.code !== 'ESRCH') throw error; }
    if (!await waitGroup(2000)) {
      try { process.kill(-pid, 'SIGKILL'); } catch (error) { if (error.code !== 'ESRCH') throw error; }
      if (!await waitGroup(2000)) throw new Error(`Owned process group ${pid} did not terminate`);
    }
  }
  owned.cdp?.close();
}

async function prepareChrome(owned, profile, url, config, signal, metadata) {
  const devtoolsFile = path.join(profile, 'DevToolsActivePort');
  const until = Date.now() + 45000;
  let endpoint;
  while (Date.now() < until) {
    try {
      const lines = (await fsp.readFile(devtoolsFile, 'utf8')).trim().split(/\r?\n/);
      if (/^\d+$/.test(lines[0]) && lines[1]?.startsWith('/devtools/browser/')) {
        endpoint = `ws://127.0.0.1:${lines[0]}${lines[1]}`;
        break;
      }
    } catch (error) { if (error.code !== 'ENOENT') throw error; }
    await sleep(100, signal);
  }
  if (!endpoint) throw new Error('Chrome did not publish DevToolsActivePort within 45 seconds');
  const cdp = await CDP.connect(endpoint, signal);
  owned.cdp = cdp;
  metadata.version = await cdp.send('Browser.getVersion');
  try { metadata.systemInfo = await cdp.send('SystemInfo.getInfo'); }
  catch (error) { metadata.systemInfoError = error.message; }
  let target;
  while (Date.now() < until) {
    const { targetInfos } = await cdp.send('Target.getTargets');
    target = targetInfos.find(candidate => candidate.type === 'page' && candidate.url === url);
    if (target) break;
    await sleep(100, signal);
  }
  if (!target) throw new Error('Cannot locate the owned Chrome benchmark app page');
  const { sessionId } = await cdp.send('Target.attachToTarget', { targetId: target.targetId, flatten: true });
  signal.throwIfAborted();
  const evaluate = async expression => {
    signal.throwIfAborted();
    const reply = await cdp.send('Runtime.evaluate', { expression, returnByValue: true }, sessionId);
    if (reply.exceptionDetails) throw new Error(`Chrome page evaluation failed: ${reply.exceptionDetails.text}`);
    return reply.result.value;
  };
  let ready = false;
  while (Date.now() < until) {
    ready = await evaluate('document.readyState === "complete" && typeof window.renderBenchmark?.run === "function"');
    if (ready) break;
    await sleep(100, signal);
  }
  if (!ready) {
    metadata.notReadyPage = await evaluate(`({
      url: location.href, readyState: document.readyState, visibility: document.visibilityState,
      focused: document.hasFocus(), title: document.title, text: document.body?.innerText.slice(0, 2000),
      scripts: [...document.scripts].map(script => script.src),
      resources: performance.getEntriesByType('resource').map(entry => ({name:entry.name, duration:entry.duration, transferSize:entry.transferSize}))
    })`);
    throw new Error('Chrome benchmark page did not become ready: ' + JSON.stringify(metadata.notReadyPage));
  }
  const { windowId } = await cdp.send('Browser.getWindowForTarget', { targetId: target.targetId });
  await cdp.send('Browser.setWindowBounds', { windowId, bounds: { windowState: 'normal' } });
  // Match the native host's primary-display placement, not the last key screen.
  await cdp.send('Browser.setWindowBounds', { windowId, bounds: { left: 40, top: 40 } });
  let viewport;
  for (let attempt = 0; attempt < 12; attempt++) {
    viewport = await evaluate('({width:innerWidth,height:innerHeight,outerWidth,outerHeight,dpr:devicePixelRatio,visibility:document.visibilityState})');
    if (viewport.width === config.width && viewport.height === config.height && viewport.visibility === 'visible') break;
    const { bounds } = await cdp.send('Browser.getWindowBounds', { windowId });
    // Adjust the actual native window, preserving physical device scale and browser rendering configuration.
    await cdp.send('Browser.setWindowBounds', { windowId, bounds: {
      width: (bounds.width || viewport.outerWidth) + config.width - viewport.width,
      height: (bounds.height || viewport.outerHeight) + config.height - viewport.height,
    } });
    await sleep(150, signal);
  }
  if (viewport.width !== config.width || viewport.height !== config.height || viewport.visibility !== 'visible') {
    throw new Error(`Chrome actual viewport cannot match ${config.width}x${config.height}: ${JSON.stringify(viewport)}. Check the available display area.`);
  }
  metadata.preRunViewport = viewport;
  metadata.windowBounds = (await cdp.send('Browser.getWindowBounds', { windowId })).bounds;
  await cdp.send('Page.bringToFront', {}, sessionId);
  // This is the last DevTools command until the page posts its complete result.
  await evaluate('void window.renderBenchmark.run()');
  return metadata;
}

function validateResult(result, engine, trial, config) {
  if (!result || result.schemaVersion !== 1 || result.engine !== engine || result.trial !== trial) throw new Error('Result identity/schema does not match active trial');
  if (result.status !== 'complete') throw new Error(`${engine} trial ${trial}: ${result.error || 'page returned an error'}`);
  for (const [key, value] of Object.entries(config)) if (result.config?.[key] !== value) throw new Error(`Result config mismatch for ${key}`);
  const env = result.environment;
  if (env?.viewport?.width !== config.width || env?.viewport?.height !== config.height) throw new Error(`${engine} viewport mismatch: ${JSON.stringify(env?.viewport)}; requested ${config.width}x${config.height}`);
  if (env.visibilityState !== 'visible') throw new Error(`${engine} was not visible: ${env.visibilityState}`);
  if (!(Number.isFinite(env.devicePixelRatio) && env.devicePixelRatio > 0)) throw new Error(`${engine} returned invalid devicePixelRatio`);
  if (!Array.isArray(result.scenarios) || result.scenarios.length !== SCENARIOS.length) throw new Error('Incomplete scenario list');
  result.scenarios.forEach((scenario, index) => {
    if (scenario.id !== SCENARIOS[index] || !['operation', 'animation'].includes(scenario.kind)) throw new Error(`Unexpected scenario at index ${index}`);
    if (!scenario.metrics || !Object.keys(scenario.metrics).length || Object.values(scenario.metrics).some(value => !Number.isFinite(value))) throw new Error(`Invalid metrics in ${scenario.id}`);
    const requiredMetrics = scenario.kind === 'animation' ? ['rafFps', 'frameP50Ms', 'frameP95Ms', 'over25MsPct'] : ['workP50Ms', 'workP95Ms', 'opportunityP50Ms', 'opportunityP95Ms'];
    for (const name of requiredMetrics) if (!Number.isFinite(scenario.metrics[name])) throw new Error(`Missing ${scenario.id}.${name}`);
    const requiredRaw = scenario.kind === 'animation' ? ['frameIntervalsMs'] : ['workMs', 'opportunityMs'];
    for (const name of requiredRaw) if (!Array.isArray(scenario.raw?.[name]) || !scenario.raw[name].length || scenario.raw[name].some(value => !Number.isFinite(value) || value < 0)) throw new Error(`Missing or invalid ${scenario.id}.${name}`);
    for (const [key, values] of Object.entries(scenario.raw || {})) if (!Array.isArray(values) || values.some(value => !Number.isFinite(value))) throw new Error(`Invalid raw samples ${scenario.id}.${key}`);
  });
}

function median(values) {
  const sorted = [...values].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

function aggregate(trials) {
  return SCENARIOS.map(id => {
    const engines = {};
    for (const engine of ['mini-electron', 'chrome']) {
      const scenarios = trials.filter(trial => trial.engine === engine).map(trial => trial.result.scenarios.find(item => item.id === id));
      const metrics = {};
      for (const key of Object.keys(scenarios[0].metrics)) {
        const values = scenarios.map(scenario => scenario.metrics[key]);
        if (values.some(value => !Number.isFinite(value))) throw new Error(`Inconsistent metric ${id}.${key}`);
        metrics[key] = { median: median(values), min: Math.min(...values), max: Math.max(...values), perTrial: values };
      }
      engines[engine] = { metrics, sampleCounts: scenarios.map(scenario => Object.fromEntries(Object.entries(scenario.raw).map(([key, values]) => [key, values.length]))) };
    }
    const ratios = {};
    for (const key of Object.keys(engines['mini-electron'].metrics)) {
      const mini = engines['mini-electron'].metrics[key].median;
      const chrome = engines.chrome.metrics[key]?.median;
      if (!Number.isFinite(chrome)) throw new Error(`Chrome metric missing: ${id}.${key}`);
      ratios[key] = { miniOverChrome: chrome === 0 ? null : mini / chrome, higherIsBetter: key === 'rafFps' };
    }
    return { id, label: trials[0].result.scenarios.find(item => item.id === id).label, engines, ratios };
  });
}

function number(value) {
  if (value === null) return 'n/a (Chrome median is zero)';
  if (value === 0) return '0';
  return Number(value.toPrecision(3)).toString();
}

function reportMarkdown(report) {
  const lines = ['# Rendering comparison', '', `Status: **${report.status}**`, '', `Started: ${report.startedAt}`, `Finished: ${report.finishedAt}`, '', '## Configuration', '', '```json', JSON.stringify(report.config, null, 2), '```', '',
    '- Same localhost page source, fixed actual content viewport, page-managed per-scenario warmups, headed windows, fresh owned session/profile per trial.',
    '- Engines run sequentially; the first engine alternates by trial. The runner does not override the native rendering backend. Current macOS builds try Metal composition with CPU tile rasterization; CPU fallback/disablement is possible. Runtime logs identify the selected backend.',
    '- Both native windows are placed at global top-left (40,40) on the primary display; actual host/CDP window bounds are retained per trial. Matching DPR alone is insufficient when attached screens have different refresh rates.',
    '- Work timings measure synchronous JavaScript/DOM/canvas submission cost, not completed GPU execution.',
    '- Operation opportunity timings run from immediately before the synchronous operation through two subsequent rAF callbacks, including the operation itself; this is a rendering opportunity proxy, not proof of presentation.',
    '- rAF metrics measure callback cadence: `rafFps` is derived from callback intervals and is **not actual presented FPS**. `frameP50Ms`/`frameP95Ms` are frame-interval quantiles; `over25MsPct` is the percentage of intervals over 25 ms, a fixed threshold based on a 60 Hz reference, not a display-adaptive dropped-frame count. A 120 Hz display can expose distinct engine rAF caps.',
    '- `workP50Ms`/`workP95Ms` and `opportunityP50Ms`/`opportunityP95Ms` are sample quantiles in milliseconds.',
    '- Aggregates are medians of per-trial metrics, not pooled sample quantiles. Ranges below are min–max of per-trial metrics. Ratios are mini-electron / Chrome; higher is worse for time/cost metrics and better for `rafFps`. Zero denominators have no ratio. There is no composite score.',
    '- GPU information is a capability/configuration snapshot from Chrome CDP where available; it does not prove that a particular workload used the GPU.',
    '- Browser/OS scheduling, display refresh rate, thermal state, occlusion and background system load affect results. Keep both apps visible and leave the machine idle; repeat runs to assess variability.', '',
    '## Provenance', '', `- Host: ${report.host.osType} ${report.host.osRelease}, ${report.host.arch}; Node ${report.host.node}`, `- CPU: ${report.host.cpuModel}; logical cores: ${report.host.logicalCpus}; RAM: ${number(report.host.totalMemoryBytes / 1024 ** 3)} GiB`,
    `- Runtime executable: \`${report.executables.runtime.path}\` (${report.executables.runtime.sizeBytes} bytes; modified ${report.executables.runtime.mtime})`,
    `- Chrome executable: \`${report.executables.chrome.path}\` (${report.executables.chrome.sizeBytes} bytes; modified ${report.executables.chrome.mtime})`,
    '- Runtime package declarations are **not a verified binary version**. Actual host `process.versions` / executable metadata and native version-like log lines, when available, are retained separately.',
    '- Chrome Browser.getVersion and SystemInfo.getInfo responses are retained per trial in results.json.',
    `- Chrome products reported by CDP: ${[...new Set(report.trials.map(trial => trial.chrome?.version?.product).filter(Boolean))].join(', ') || 'unavailable'}.`,
    `- Runtime host-reported process.versions: ${JSON.stringify(report.trials.find(trial => trial.nativeHost)?.nativeHost?.versions || null)}.`,
    '- Display and power snapshots from system_profiler / pmset are captured before measurements and retained in results.json (or their errors if unavailable).', '', 'Page source SHA-256:', '', ...Object.entries(report.source.files).map(([file, hash]) => `- \`${file}\`: \`${hash}\``), '', `Combined page source SHA-256: \`${report.source.combinedSha256}\``, ''];
  if (report.error) lines.push('## Failure', '', 'No comparison ratios are produced for an incomplete or invalid run.', '', '```text', report.error, '```', '');
  lines.push('## Trial order and artifacts', '', '| Trial | Order | Engine | Status | DPR | Viewport | Raw result | Logs |', '| --- | --- | --- | --- | --- | --- | --- | --- |');
  for (const trial of report.trials) {
    const env = trial.result?.environment;
    lines.push(`| ${trial.trial} | ${trial.order + 1} | ${trial.engine} | ${trial.status} | ${env?.devicePixelRatio ?? 'n/a'} | ${env?.viewport ? `${env.viewport.width}×${env.viewport.height}` : 'n/a'} | [JSON](${trial.resultFile}) | [stdout](${trial.stdoutFile}) / [stderr](${trial.stderrFile}) |`);
  }
  for (const scenario of report.comparison || []) {
    lines.push('', `## ${scenario.label || scenario.id}`, '', '| Metric | mini-electron median [range] | Chrome median [range] | mini / Chrome | Direction |', '| --- | --- | --- | --- | --- |');
    for (const [key, ratio] of Object.entries(scenario.ratios)) {
      const format = engine => { const metric = scenario.engines[engine].metrics[key]; return `${number(metric.median)} [${number(metric.min)}–${number(metric.max)}]`; };
      lines.push(`| ${key} | ${format('mini-electron')} | ${format('chrome')} | ${number(ratio.miniOverChrome)} | higher ${ratio.higherIsBetter ? 'better' : 'worse'} |`);
    }
    lines.push('', 'Raw sample counts per trial:', '', ...Object.entries(scenario.engines).map(([engine, value]) => `- ${engine}: ${value.sampleCounts.map(counts => Object.entries(counts).map(([key, count]) => `${key}=${count}`).join(', ')).join('; ')}`));
  }
  lines.push('', 'All individual trial metrics, raw sample arrays, page environment, Chrome metadata and process exit information are retained in results.json and trial-*.json.', '');
  return lines.join('\n');
}

async function executableInfo(file) {
  await fsp.access(file, fs.constants.X_OK);
  const stat = await fsp.stat(file);
  if (!stat.isFile()) throw new Error(`Not an executable file: ${file}`);
  return { path: file, realPath: await fsp.realpath(file), sizeBytes: stat.size, mtime: stat.mtime.toISOString() };
}

async function hostCommand(executable, args, json = false) {
  return new Promise(resolve => {
    execFile(executable, args, { timeout: 15000, maxBuffer: 8 * 1024 * 1024, encoding: 'utf8' }, (error, stdout, stderr) => {
      const result = { executable, args, capturedAt: new Date().toISOString() };
      if (error) result.error = error.message;
      if (stderr.trim()) result.stderr = stderr.trim();
      if (json && stdout.trim()) {
        try { result.data = JSON.parse(stdout); } catch { result.stdout = stdout; }
      } else result.stdout = stdout;
      resolve(result);
    });
  });
}

async function main() {
  const opts = options(process.argv.slice(2));
  if (opts.help) { console.log(HELP); return; }
  if (Number(process.versions.node.split('.')[0]) < 22 || typeof WebSocket !== 'function') throw new Error('Rendering benchmark requires Node.js 22+ with the global WebSocket API.');
  if (!opts.serve && process.platform !== 'darwin') throw new Error('Automated rendering comparison requires macOS. Use --serve for the manual page.');
  const files = new Map([
    ['/page.html', await fsp.readFile(path.join(__dirname, 'page.html'))],
    ['/page.js', await fsp.readFile(path.join(__dirname, 'page.js'))],
  ]);
  const server = await makeServer(files);
  const controller = new AbortController();
  const onSignal = signal => { if (!controller.signal.aborted) controller.abort(new Error(`Interrupted by ${signal}`)); };
  const sigint = () => onSignal('SIGINT');
  const sigterm = () => onSignal('SIGTERM');
  process.on('SIGINT', sigint);
  process.on('SIGTERM', sigterm);
  const config = { width: opts.width, height: opts.height, durationMs: opts.durationMs, warmupMs: opts.warmupMs, samples: opts.samples };
  const pageURL = (engine, trial, token, autostart) => {
    const url = new URL('/page.html', server.origin);
    for (const [key, value] of Object.entries({ ...config, engine, trial, ...(token ? { token } : {}), autostart })) url.searchParams.set(key, String(value));
    return url.href;
  };
  let report;
  let owned;
  let profile;
  let currentEngine;
  let failure;
  try {
    if (opts.serve) {
      const url = new URL(pageURL('manual', 1, null, 0));
      url.searchParams.delete('width');
      url.searchParams.delete('height');
      console.log(`Manual benchmark (no engines launched; uses this window's viewport):\n${url.href}\nPress Ctrl-C to stop. Call window.renderBenchmark.run() or use the page's Run control.`);
      await new Promise(resolve => controller.signal.addEventListener('abort', resolve, { once: true }));
      return;
    }
    const executables = { runtime: await executableInfo(opts.runtime), chrome: await executableInfo(opts.chrome) };
    await fsp.access(path.join(__dirname, 'electron-main.js'));
    const packageJSON = JSON.parse(await fsp.readFile(path.join(ROOT, 'package.json'), 'utf8'));
    await fsp.mkdir(opts.output, { recursive: true });
    report = {
      schemaVersion: 1, status: 'running', startedAt: new Date().toISOString(), config: { ...config, trials: opts.trials },
      host: { osType: os.type(), osRelease: os.release(), osVersion: os.version(), arch: os.arch(), node: process.version, cpuModel: os.cpus()[0]?.model || 'unknown', logicalCpus: os.cpus().length, totalMemoryBytes: os.totalmem() },
      executables, runtimePackageDeclarations: { packageVersion: packageJSON.version, miniElectron: packageJSON.miniElectron },
      source: {
        files: Object.fromEntries([...files].map(([name, bytes]) => [name.slice(1), sha256(bytes)])),
        combinedSha256: sha256(Buffer.concat([...files].flatMap(([name, bytes]) => [Buffer.from(`${name}\0`), bytes, Buffer.from('\0')]))),
      },
      trials: [], comparison: null,
    };
    [report.host.displays, report.host.power] = await Promise.all([
      hostCommand('/usr/sbin/system_profiler', ['SPDisplaysDataType', '-json'], true),
      hostCommand('/usr/bin/pmset', ['-g', 'batt']),
    ]);
    console.log(`Artifacts: ${opts.output}\nViewport: ${opts.width}x${opts.height}; ${opts.trials} trials per engine. Leave the visible benchmark window unobstructed.`);
    let expectedDpr;
    for (let trial = 1; trial <= opts.trials; trial++) {
      const order = trial % 2 ? ['mini-electron', 'chrome'] : ['chrome', 'mini-electron'];
      for (let index = 0; index < order.length; index++) {
        currentEngine = order[index];
        if (controller.signal.aborted) throw controller.signal.reason;
        const token = crypto.randomBytes(24).toString('hex');
        const slot = deferred();
        server.active.set(token, slot);
        const base = `trial-${String(trial).padStart(2, '0')}-${currentEngine}`;
        const prefix = path.join(opts.output, base);
        const trialRecord = { engine: currentEngine, trial, order: index, engineOrder: order, config, startedAt: new Date().toISOString(), status: 'running', resultFile: `${base}.json`, stdoutFile: `${base}.stdout.log`, stderrFile: `${base}.stderr.log` };
        report.trials.push(trialRecord);
        profile = await fsp.mkdtemp(path.join(os.tmpdir(), `mini-render-${currentEngine}-`));
        const trialController = new AbortController();
        const trialSignal = AbortSignal.any([controller.signal, trialController.signal]);
        console.log(`Trial ${trial}/${opts.trials}, ${index + 1}/2: ${currentEngine}`);
        try {
          const url = pageURL(currentEngine, trial, token, currentEngine === 'chrome' ? 0 : 1);
          const env = { ...process.env, HOME: profile, CFFIXED_USER_HOME: profile, XDG_CONFIG_HOME: path.join(profile, 'config'), XDG_CACHE_HOME: path.join(profile, 'cache') };
          if (currentEngine === 'mini-electron') {
            Object.assign(env, { MINI_RENDER_BENCH_URL: url, MINI_RENDER_BENCH_WIDTH: String(opts.width), MINI_RENDER_BENCH_HEIGHT: String(opts.height), PASEO_ELECTRON_USER_DATA_DIR: profile });
            owned = launch(opts.runtime, [`--app=${path.join(__dirname, 'electron-main.js')}`], env, prefix);
          } else {
            owned = launch(opts.chrome, [`--app=${url}`, '--remote-debugging-port=0', '--remote-debugging-address=127.0.0.1', `--user-data-dir=${profile}`, '--no-first-run', '--no-default-browser-check', '--disable-extensions'], env, prefix);
            trialRecord.chrome = {};
            await deadline(Promise.race([prepareChrome(owned, profile, url, config, trialSignal, trialRecord.chrome), owned.failure]), 90000, 'Chrome preparation', trialSignal);
          }
          const suiteTimeout = Math.min(2147483647, Math.max(120000, opts.durationMs * 10 + opts.warmupMs * 10 + opts.samples * 2000 + 30000));
          trialRecord.result = await deadline(Promise.race([slot.promise, owned.failure]), suiteTimeout, `${currentEngine} benchmark suite`, trialSignal);
          // Persist even invalid results for diagnosis, then reject mismatched/hidden/error runs.
          saveJSON(`${prefix}.json`, trialRecord);
          validateResult(trialRecord.result, currentEngine, trial, config);
          const dpr = trialRecord.result.environment.devicePixelRatio;
          if (expectedDpr === undefined) expectedDpr = dpr;
          if (dpr !== expectedDpr) throw new Error(`Device pixel ratio mismatch: ${currentEngine} returned ${dpr}, first trial returned ${expectedDpr}`);
          trialRecord.status = 'complete';
        } catch (error) {
          trialRecord.status = 'error';
          trialRecord.error = errorText(error);
          throw error;
        } finally {
          trialController.abort(new Error('Trial ended'));
          server.active.delete(token);
          let cleanupError;
          try { await stopOwned(owned, currentEngine); }
          catch (error) {
            cleanupError = error;
            trialRecord.status = 'error';
            trialRecord.cleanupError = errorText(error);
          }
          trialRecord.finishedAt = new Date().toISOString();
          trialRecord.processExit = owned?.exit;
          if (currentEngine === 'mini-electron' && owned) {
            const logs = [owned.stdoutPath, owned.stderrPath].map(file => fs.readFileSync(file, 'utf8')).join('\n');
            trialRecord.nativeVersionLogLines = logs.split(/\r?\n/).filter(line => /(?:version|chromium|electron|v8|node\.js)/i.test(line)).slice(0, 80);
            const hostLine = logs.split(/\r?\n/).find(line => line.startsWith('MINI_RENDER_HOST '));
            if (hostLine) {
              try { trialRecord.nativeHost = JSON.parse(hostLine.slice('MINI_RENDER_HOST '.length)); }
              catch { trialRecord.nativeHostParseError = 'Invalid MINI_RENDER_HOST JSON'; }
            }
          }
          saveJSON(`${prefix}.json`, trialRecord);
          // Retain ownership on failure so the outer cleanup can retry before deleting the profile.
          if (cleanupError) throw cleanupError;
          owned = null;
          await fsp.rm(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 });
          profile = null;
        }
      }
    }
    report.comparison = aggregate(report.trials);
    report.status = 'complete';
  } catch (error) {
    failure = error;
    if (report) { report.status = 'error'; report.error = errorText(error); report.comparison = null; }
  } finally {
    try { await stopOwned(owned, currentEngine); }
    catch (error) { failure ||= error; if (report) { report.status = 'error'; report.error ||= errorText(error); report.comparison = null; } }
    if (profile) {
      try { await fsp.rm(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 }); }
      catch (error) { failure ||= error; if (report) { report.status = 'error'; report.error ||= errorText(error); report.comparison = null; } }
    }
    server.server.closeAllConnections();
    await new Promise(resolve => server.server.close(resolve));
    process.off('SIGINT', sigint);
    process.off('SIGTERM', sigterm);
    if (report) {
      report.finishedAt = new Date().toISOString();
      saveJSON(path.join(opts.output, 'results.json'), report);
      fs.writeFileSync(path.join(opts.output, 'report.md'), reportMarkdown(report));
      console.log(`JSON: ${path.join(opts.output, 'results.json')}\nReport: ${path.join(opts.output, 'report.md')}`);
    }
  }
  if (failure) throw failure;
}

main().catch(error => { console.error(errorText(error)); process.exitCode = 1; });
