'use strict';
// Interactive Windows desktop; Microsoft Pinyin must already be active. This test
// does not change the input source, theme, high-contrast mode, or clipboard.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');
const { startNativeInput } = require('./windows-native-input-driver');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-windows-composition-colors-'));
app.setPath('userData', path.join(temporary, 'user-data'));
const title = `mini-windows-composition-colors-${process.pid}`;
const artifacts = process.env.MINI_NATIVE_INPUT_ARTIFACTS;
const results = [];
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let window;
let driver;
let finished = false;
const watchdog = setTimeout(() => finish(new Error('Windows composition color smoke exceeded 60s watchdog')), 60000);
app.on('window-all-closed', () => {});

const request = (action, values = {}) => driver.request(action, values);
const key = (...keys) => request('key', { keys });
async function evaluate(source) {
  const result = await window.webContents.executeJavaScript(source);
  return result && typeof result === 'object' && 'value' in result ? result.value : result;
}
const value = source => evaluate(`JSON.stringify(${source})`).then(JSON.parse);
async function eventually(read, predicate, label) {
  let last;
  const end = Date.now() + 5000;
  do {
    last = await read();
    if (predicate(last)) return last;
    await pause(30);
  } while (Date.now() < end);
  assert.fail(`${label}: ${JSON.stringify(last)}`);
}
async function capture(name) {
  if (!artifacts) return;
  fs.mkdirSync(artifacts, { recursive: true });
  await request('capture', { path: path.join(artifacts, name + '.png') });
}
async function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  if (error && window && !window.isDestroyed()) {
    try { console.error('COMPOSITION_COLOR_FAILURE_STATE ' + JSON.stringify(await value('window.smokeState()'))); }
    catch (diagnosticError) { console.error(diagnosticError); }
  }
  if (driver) {
    try { await driver.close(); }
    catch (cleanupError) { error = error || cleanupError; }
  }
  if (window && !window.isDestroyed()) window.destroy();
  try { fs.rmSync(temporary, { recursive: true, force: true }); }
  catch (cleanupError) { error = error || cleanupError; }
  if (artifacts) {
    fs.mkdirSync(artifacts, { recursive: true });
    fs.writeFileSync(path.join(artifacts, 'windows-composition-colors-results.json'),
      JSON.stringify({ passed: !error, results, error: error ? String(error.stack || error) : null }, null, 2));
  }
  if (error) console.error(error.stack || error);
  else console.log('PASS: native Windows Pinyin marked text uses a neutral readable fallback in light and dark editors without selection styling');
  app.exit(error ? 1 : 0);
}
function luminance(rgb) {
  const channels = rgb.map(value => value / 255).map(value =>
    value <= 0.04045 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4);
  return 0.2126 * channels[0] + 0.7152 * channels[1] + 0.0722 * channels[2];
}
function contrast(left, right) {
  const values = [luminance(left), luminance(right)].sort((a, b) => a - b);
  return (values[1] + 0.05) / (values[0] + 0.05);
}
function distance(left, right) {
  return Math.max(...left.map((component, index) => Math.abs(component - right[index])));
}
async function focusEditor(id) {
  const point = await value(`(() => { const r=document.getElementById(${JSON.stringify(id)}).getBoundingClientRect();
    return {x:r.left+24,y:r.top+r.height/2,dpr:devicePixelRatio}; })()`);
  const geometry = await request('geometry');
  await request('click', {
    x: Math.round(geometry.clientX + point.x * point.dpr),
    y: Math.round(geometry.clientY + point.y * point.dpr),
  });
  await eventually(() => value(`({active:document.activeElement.id,collapsed:getSelection().isCollapsed})`),
    state => state.active === id && state.collapsed, `${id}: collapsed native click focus`);
}
async function beginPinyin(id) {
  await focusEditor(id);
  await evaluate(`window.events=[];document.getElementById(${JSON.stringify(id)}).textContent='';true`);
  await key(0x4e);
  await key(0x49);
  await eventually(() => value('window.events'), events =>
    events.some(event => event.target === id && event.type === 'compositionstart') &&
      events.some(event => event.target === id && event.type === 'compositionupdate' && event.data === 'ni'),
  `${id}: real Microsoft Pinyin compositionstart/update for ni`);
  return eventually(() => value(`window.markedRect(${JSON.stringify(id)})`), rect =>
    rect && rect.text === 'ni' && rect.collapsed && rect.width >= 20 && rect.height >= 20,
  `${id}: marked text DOM range`);
}
async function sampleMarkedText(id, fixture) {
  const rect = await beginPinyin(id);
  const geometry = await request('geometry');
  // Range coordinates are CSS pixels and the native helper is per-monitor-DPI aware.
  // Keep the sample within the range so ordinary editor/selection backgrounds cannot
  // dominate the histogram.
  const inset = Math.max(1, Math.round(rect.dpr));
  const region = {
    x: Math.round(geometry.clientX + rect.left * rect.dpr) + inset,
    y: Math.round(geometry.clientY + rect.top * rect.dpr) + inset,
    width: Math.max(1, Math.round(rect.width * rect.dpr) - inset * 2),
    height: Math.max(1, Math.round(rect.height * rect.dpr) - inset * 2),
  };
  const colors = await request('sample', region);
  await capture(`composition-${id}-preedit`);
  const candidates = colors.filter(color => color.count >= 4
    && distance(color.rgb, fixture.base) > 6 && distance(color.rgb, fixture.foreground) > 24);
  assert(candidates.length, `${id}: native pixels did not contain a marked-text background: ${JSON.stringify(colors)}`);
  const background = candidates[0];
  const spread = Math.max(...background.rgb) - Math.min(...background.rgb);
  const ratio = contrast(fixture.foreground, background.rgb);
  assert(spread <= 12,
    `${id}: marked-text fallback must be neutral, not the yellow default: ${JSON.stringify({ background, colors })}`);
  assert(!colors.some(color => color.count >= 4 && color.rgb[0] - color.rgb[1] > 15 && color.rgb[1] - color.rgb[2] > 45),
    `${id}: native marked-text pixels contain the yellow fallback: ${JSON.stringify(colors)}`);
  assert(ratio >= 4.5,
    `${id}: explicit editor foreground is unreadable on marked text (contrast ${ratio}): ${JSON.stringify(background)}`);
  results.push({ id, rect, region, colors, markedBackground: background, foreground: fixture.foreground, contrast: ratio });
}

app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32', 'This smoke exercises native Windows IMM composition rendering');
  driver = await startNativeInput(temporary, title);
  window = new BrowserWindow({
    x: 320, y: 120, width: 900, height: 560, title, show: false, alwaysOnTop: true,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false },
  });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><meta charset="utf-8">
    <style>
      html,body{margin:0;background:#707070;font:48px/64px Consolas,monospace}
      h1{margin:18px 32px 8px;color:white;font:20px sans-serif}
      .editor{box-sizing:border-box;width:720px;height:108px;margin:0 32px 28px;padding:18px 20px;border:2px solid #606060;white-space:pre;outline:none}
      #light{color-scheme:light;background:#fafafa;color:#000}
      #dark{color-scheme:dark;background:#121212;color:#fff}
      .editor::selection{background:#0067c0;color:#fff}
    </style>
    <h1>Light editor — explicit black foreground</h1><div id="light" class="editor" contenteditable spellcheck="false"></div>
    <h1>Dark editor — explicit white foreground</h1><div id="dark" class="editor" contenteditable spellcheck="false"></div>
    <script>
      window.events=[];
      for(const type of ['compositionstart','compositionupdate','compositionend','beforeinput','input'])
        addEventListener(type,event=>events.push({type,data:event.data,target:event.target.id,isComposing:event.isComposing,inputType:event.inputType}));
      window.markedRect=id=>{const editor=document.getElementById(id),node=editor.firstChild;if(!node)return null;
        const range=document.createRange();range.setStart(node,0);range.setEnd(node,node.length);const rect=range.getBoundingClientRect();
        return{left:rect.left,top:rect.top,width:rect.width,height:rect.height,dpr:devicePixelRatio,text:editor.textContent,collapsed:getSelection().isCollapsed};};
      window.smokeState=()=>({active:document.activeElement.id,texts:{light:light.textContent,dark:dark.textContent},events});
    </script>`));
  window.show();
  window.focus();
  await eventually(() => request('geometry'), geometry => geometry.clientWidth > 0 && geometry.clientHeight > 0,
    'native window geometry');
  const geometry = await request('geometry');
  assert.equal(parseInt(geometry.keyboardLayout, 16) & 0xffff, 0x0804,
    `Microsoft Pinyin must already be active (expected keyboard layout 0x0804, got ${geometry.keyboardLayout}); the test never changes input sources`);
  assert.equal(await evaluate('matchMedia("(forced-colors: active)").matches'), false,
    'Disable Windows high-contrast/forced-colors before running; the test never changes that system setting');

  await sampleMarkedText('light', { base: [250, 250, 250], foreground: [0, 0, 0] });
  await key(0x1b);
  await eventually(() => value('window.smokeState()'), state =>
    state.texts.light === '' && state.events.some(event => event.target === 'light' && event.type === 'compositionend'),
  'Escape cancels the sampled light composition without inserting text');

  await sampleMarkedText('dark', { base: [18, 18, 18], foreground: [255, 255, 255] });
  await key(0x20);
  const committed = await eventually(() => value('window.smokeState()'), state => {
    const end = state.events.find(event => event.target === 'dark' && event.type === 'compositionend');
    return end && /\p{Script=Han}/u.test(end.data) && state.texts.dark === end.data && state;
  }, 'Space commits the sampled dark composition exactly once');
  assert.equal(committed.events.filter(event => event.target === 'dark' && event.type === 'compositionend').length, 1);
  results.push({ id: 'dark-commit', text: committed.texts.dark, events: committed.events });
}).then(() => finish(), finish);
