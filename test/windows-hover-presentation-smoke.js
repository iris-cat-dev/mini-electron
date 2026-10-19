'use strict';

// Run with the native Windows runtime on an unlocked desktop.
// Measures real pointer-to-display latency; samples caption backgrounds under load.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, screen } = require('electron');
const { startNativeInput } = require('./windows-native-input-driver');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-hover-presentation-'));
const title = `mini-hover-presentation-${process.pid}`;
const artifacts = process.env.MINI_NATIVE_INPUT_ARTIFACTS;
const background = [24, 30, 38];
const highlight = [61, 118, 160];
const evidence = { hover: [], captions: [] };
const pause = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
let window, driver, finished = false;
const deadline = setTimeout(() => finish(new Error('Hover presentation smoke timed out')), 60000);
app.on('window-all-closed', () => {});

async function evaluate(source) {
  return window.webContents.executeJavaScript(source);
}
async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (error && window && !window.isDestroyed()) {
    try {
      evidence.failureState = await window.webContents._getCompositorInfo();
      evidence.hoveredRows = await evaluate(`Array.from(document.querySelectorAll('.row')).map((row,index)=>
        ({index,hovered:row.matches(':hover'),background:getComputedStyle(row).backgroundColor})).filter(row=>row.hovered)`);
      const image = await Promise.race([window.webContents.capturePage(),
        pause(8000).then(() => { throw new Error('diagnostic capture timed out'); })]);
      const size = image.getSize(), bitmap = image.toBitmap();
      const offset = (Math.floor(101 * size.height / 850) * size.width + Math.floor(320 * size.width / 1400)) * 4;
      evidence.capturedFirstRow = [bitmap[offset + 2], bitmap[offset + 1], bitmap[offset], bitmap[offset + 3]];
      if (artifacts) fs.writeFileSync(path.join(artifacts, 'hover-failure.png'), image.toPNG());
    } catch (failure) { evidence.diagnosticError = String(failure); }
  }
  evidence.passed = !error;
  evidence.error = error ? String(error.stack || error) : null;
  if (artifacts) {
    fs.mkdirSync(artifacts, { recursive: true });
    fs.writeFileSync(path.join(artifacts, `windows-hover-presentation-${process.pid}.json`), JSON.stringify(evidence, null, 2));
  }
  if (driver) { try { await driver.close(); } catch (cleanupError) { error ||= cleanupError; } }
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error, JSON.stringify(evidence));
  else console.log('PASS: native file-row hover presentation, stable translucent caption backgrounds, and clearing under continuous renderer publication');
  app.exit(error ? 1 : 0);
}
const composite = (foreground, alpha, base) => foreground.map((value, index) =>
  Math.round((value * alpha + base[index] * (255 - alpha)) / 255));

app.whenReady().then(async () => {
  const area = screen.getPrimaryDisplay().workArea;
  window = new BrowserWindow({ x: area.x + 20, y: area.y + 20,
    width: Math.min(1400, area.width - 40), height: Math.min(850, area.height - 40),
    minWidth: 0, minHeight: 0, title, frame: false, show: false,
    titleBarOverlay: { color: '#d0206080', symbolColor: '#ffffff', height: 52 },
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadURL('data:text/html,' + encodeURIComponent(`<!doctype html><meta charset="utf-8"><title>${title}</title>
    <style>html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#181e26}
    #work{position:fixed;inset:0;width:100%;height:100%;pointer-events:none}
    #tree{position:absolute;left:20px;top:90px;width:320px;background:#181e26}
    .row{height:22px;line-height:22px;color:#bbc6d6;padding-left:12px;font:14px/22px sans-serif}
    .row:hover{background:#3d76a0}</style><canvas id="work"></canvas><div id="tree">${Array.from({ length: 18 }, (_, index) => `<div class="row">file-${index}.txt</div>`).join('')}</div>
    <script>const canvas=document.querySelector('canvas'),ctx=canvas.getContext('2d');
    let workload=false,tick=0;function paint(){canvas.width=Math.ceil(innerWidth*devicePixelRatio);canvas.height=Math.ceil(innerHeight*devicePixelRatio);ctx.fillStyle='#181e26';ctx.fillRect(0,0,canvas.width,canvas.height);ctx.fillStyle=(++tick%2)?'#18202a':'#19212b';ctx.fillRect(0,canvas.height-2,2,2);if(workload)requestAnimationFrame(paint)}
    window.startWork=()=>{workload=true;requestAnimationFrame(paint)};window.stopWork=()=>{workload=false};paint();</script>`));
  window.show();
  driver = await startNativeInput(temporary, title);
  await driver.request('focusSource');
  await driver.request('focus');
  const geometry = await driver.request('geometry');
  evidence.geometry = geometry;
  const dip = value => Math.floor(value * geometry.dpi / 96 + 0.5);
  const point = (x, y) => ({ x: geometry.clientX + dip(x), y: geometry.clientY + dip(y) });
  // Load completion precedes the first GPU tile presentation. Measure steady
  // hover latency only after actual file text is visible, not during startup.
  await driver.request('pointerMove', point(500, 200));
  const readyDeadline = Date.now() + 5000;
  let fileTextVisible = false;
  do {
    const colors = await driver.request('sample', { ...point(32, 90), width: dip(45), height: dip(22) });
    fileTextVisible = colors.some(({ rgb }) => rgb.every((value, index) => Math.abs(value - [187, 198, 214][index]) <= 4));
    if (!fileTextVisible) await pause(20);
  } while (!fileTextVisible && Date.now() < readyDeadline);
  assert.ok(fileTextVisible, 'initial file text must reach the actual desktop before latency sampling');
  // Alternate rows so each sample requires a new native hover presentation.
  for (const row of [0, 7, 2, 11, 4, 15, 1, 9, 3, 13, 5, 16]) {
    const target = point(280, 90 + row * 22 + 11);
    const sample = { ...point(320, 90 + row * 22 + 11), rgb: highlight };
    const result = await driver.request('samplePointerPresentation', { point: target, samples: [sample], durationMs: 160, tolerance: 4 });
    evidence.hover.push({ row, ...result });
    assert.ok(result.firstMatchMs >= 0, `row ${row} must reach the native display`);
    assert.equal(result.mismatchAfterMatch, 0, `row ${row} must not revert to stale pixels`);
  }
  const times = evidence.hover.map(sample => sample.firstMatchMs).sort((left, right) => left - right);
  evidence.latency = { medianMs: times[Math.floor(times.length / 2)], p95Ms: times[Math.ceil(times.length * 0.95) - 1], maximumMs: times.at(-1) };
  console.log('Native pointer-to-display latency:', JSON.stringify(evidence.latency));

  const baseline = composite([208, 32, 96], 128, background);
  const hovered = composite([255, 255, 255], 31, baseline);
  const buttonWidth = dip(46), inset = dip(1);
  const caption = index => ({ x: geometry.clientX + geometry.clientWidth - buttonWidth * index + dip(7),
    y: geometry.clientY + inset + dip(7) });
  await evaluate('startWork(); true');
  await pause(100);
  evidence.afterWork = await driver.request('pointerProbe', point(450, 90));
  for (const index of [3, 2, 3, 2, 3, 2]) {
    const target = caption(index);
    const samples = [2, 3].map(button => ({ ...caption(button), rgb: button === index ? hovered : baseline }));
    const result = await driver.request('samplePointerPresentation', { point: target, samples, durationMs: 450, tolerance: 5 });
    evidence.captions.push({ button: index, ...result });
    assert.ok(result.firstMatchMs >= 0, 'hovered caption must reach its once-composited color');
    assert.equal(result.mismatchAfterMatch, 0, 'caption backgrounds must not compound alpha or flash under producer contention');
  }
  const target = point(450, 90);
  const samples = [2, 3].map(button => ({ ...caption(button), rgb: baseline }));
  const cleared = await driver.request('samplePointerPresentation', { point: target, samples, durationMs: 450, tolerance: 5 });
  evidence.captions.push({ cleared: true, ...cleared });
  assert.ok(cleared.firstMatchMs >= 0, 'caption hover must clear on the native display');
  assert.equal(cleared.mismatchAfterMatch, 0, 'cleared caption must remain once-composited');
  await evaluate('stopWork(); true');
  await finish();
}).catch(finish);
