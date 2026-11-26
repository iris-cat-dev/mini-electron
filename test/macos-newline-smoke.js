'use strict';

// Run with the rebuilt macOS runtime; no global keyboard/clipboard injection.
const { app, BrowserWindow } = require('electron');
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-newline-'));
const systemPinyin = process.env.MINI_NEWLINE_PINYIN === '1';
const SHIFT = 1 << 17;
const title = `Native newline ${process.pid}`;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
app.setPath('userData', temporary);
let window, keys, ime;
const deadline = setTimeout(() => finish(new Error('Native newline smoke timed out')), 60000);
function finish(error) {
  clearTimeout(deadline);
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: native Return/Shift+Return/keypad newline, selection replacement, DOM cancellation, chat submit and IME Return isolation');
  app.exit(error ? 1 : 0);
}
const evaluate = source => window.webContents.executeJavaScript(source);
const snapshot = () => evaluate('JSON.stringify(window.snapshot())').then(JSON.parse);
async function eventually(label, predicate) {
  let last;
  for (let attempt = 0; attempt < 100; ++attempt) {
    last = await predicate();
    if (last === true) return;
    await pause(20);
  }
  throw new Error(`${label}: ${JSON.stringify(last)}`);
}
async function reset(options = {}) {
  await evaluate(`window.reset(${JSON.stringify(options)})`);
  await eventually('Owned native editor is focused', async () =>
    await evaluate('document.hasFocus() && document.activeElement.id === window.editorId'));
}
async function press(code = 36, modifiers = SHIFT) {
  keys.key(title, code, modifiers);
  await eventually('Native keyup reached the editor', async () => {
    const state = await snapshot();
    return state.events.some(event => event.type === 'keyup') || state;
  });
  return snapshot();
}
function newline(label, state, text = 'ab\nCD', cursor = 3, shift = true) {
  assert.equal(state.text, text, label);
  assert.equal(state.selection, cursor, `${label}: caret follows inserted newline`);
  assert.equal(state.submissions, 0, `${label}: no message submission`);
  assert.equal(state.events.filter(event => event.type === 'input' && event.inputType === 'insertLineBreak').length, 1,
    `${label}: exactly one newline input`);
  const keypress = state.events.find(event => event.type === 'keypress');
  assert.equal(keypress?.key, 'Enter', `${label}: DOM Return semantics`);
  assert.equal(keypress.shift, shift, `${label}: native Shift survives character dispatch`);
  console.log(JSON.stringify({ pass: label, ...state }));
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const root = path.resolve(__dirname, '..');
  const sdk = process.env.SDKROOT || execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--sdk', 'macosx', '--find', 'clang++'], { encoding: 'utf8' }).trim();
  for (const name of ['shortcuts', 'ime-position']) {
    const bundle = path.join(temporary, `${name}.node`);
    execFileSync(compiler, ['-isysroot', sdk, '-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup',
      '-framework', 'AppKit', '-framework', 'Carbon', '-I', path.join(root, 'third_party/libnode/src'),
      path.join(__dirname, `macos-${name}-helper.mm`), '-o', bundle], { stdio: 'inherit', timeout: 30000 });
    if (name === 'shortcuts') keys = require(bundle);
    else ime = require(bundle);
  }
  window = new BrowserWindow({ title, width: 720, height: 540, show: false,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><meta charset="utf-8">
    <style>body{font:20px system-ui;padding:24px}textarea,#rich{display:block;width:520px;height:110px;padding:8px;font:24px/32px monospace;border:2px solid #888;white-space:pre-wrap}</style>
    <h1>Native Shift+Enter</h1><textarea id="plain"></textarea><div id="rich" contenteditable="true"></div>
    <script>
    window.events=[];window.submissions=0;window.options={};window.editorId='plain';
    window.reset=options=>{
      window.options=options;window.editorId=options.rich?'rich':'plain';
      const target=document.getElementById(editorId);const text=options.text??'abCD';
      if(options.rich)target.textContent=text;else target.value=text;
      target.focus();const start=options.start??2,end=options.end??start;
      if(options.rich){const range=document.createRange();range.setStart(target.firstChild,start);range.setEnd(target.firstChild,end);getSelection().removeAllRanges();getSelection().addRange(range)}
      else target.setSelectionRange(start,end);
      window.events=[];window.submissions=0;return true;
    };
    for(const type of ['keydown','keypress','keyup','beforeinput','input','compositionstart','compositionend'])
      document.addEventListener(type,event=>{
        if(type==='keydown'&&event.key==='Enter'){
          if(options.cancelKeyDown)event.preventDefault();
          else if(options.chat&&!event.shiftKey&&!event.isComposing){event.preventDefault();window.submissions++}
        }
        if(type==='keypress'&&options.cancelKeyPress)event.preventDefault();
        events.push({type,key:event.key,code:event.code,location:event.location,shift:event.shiftKey,
          composing:event.isComposing,defaultPrevented:event.defaultPrevented,inputType:event.inputType});
      });
    window.snapshot=()=>{const target=document.getElementById(editorId);return {
      text:target instanceof HTMLTextAreaElement?target.value:target.innerText,
      selection:target instanceof HTMLTextAreaElement?target.selectionStart:getSelection().anchorOffset,
      submissions,events:events.slice()}};
    </script>`));
  window.show();
  window.focus();
  await eventually('Owned window is focused', () => window.isFocused());

  await reset();
  newline('textarea Shift+Return', await press());
  await reset();
  newline('textarea Return', await press(36, 0), 'ab\nCD', 3, false);
  await reset({ chat: true, start: 1, end: 3 });
  newline('chat selected-text replacement', await press(), 'a\nD', 2);
  await reset({ chat: true });
  let state = await press(36, 0);
  assert.equal(state.text, 'abCD');
  assert.equal(state.submissions, 1);
  assert.equal(state.events.filter(event => event.type === 'input').length, 0);
  console.log(JSON.stringify({ pass: 'chat Return submits without inserting a newline', ...state }));
  for (const cancel of ['cancelKeyDown', 'cancelKeyPress']) {
    await reset({ [cancel]: true });
    state = await press();
    assert.equal(state.text, 'abCD', `${cancel}: no text insertion`);
    assert.equal(state.events.filter(event => event.type === 'input').length, 0, `${cancel}: no input event`);
    console.log(JSON.stringify({ pass: cancel, ...state }));
  }
  await reset();
  state = await press(7, 0);
  assert.equal(state.text, 'abxCD', 'Ordinary character input is preserved');
  await reset();
  state = await press(76);
  newline('keypad Shift+Enter', state);
  assert.equal(state.events.find(event => event.type === 'keypress').location, 3);
  await reset({ rich: true });
  state = await press();
  assert.equal(state.text, 'ab\nCD', 'contenteditable Shift+Return inserts a line break');
  assert.equal(state.events.filter(event => event.type === 'input').length, 1);
  console.log(JSON.stringify({ pass: 'contenteditable Shift+Return', ...state }));

  await reset({ chat: true });
  ime.mark(title, 'にほん', 3, 0);
  await eventually('Native marked text reaches Blink', async () => (await snapshot()).text === 'abにほんCD');
  state = await press(36, 0);
  assert.equal(state.text, 'abにほんCD', 'Return must not replace marked text with a newline');
  assert.equal(state.submissions, 0, 'IME Return must not submit a chat');
  ime.commit(title, '日本語');
  await eventually('Native candidate commit is retained', async () => (await snapshot()).text === 'ab日本語CD');
  state = await snapshot();
  assert.equal(state.events.filter(event => event.type === 'compositionend').length, 1);
  console.log(JSON.stringify({ pass: 'IME Return and native commit', ...state }));

  // Confirm marked text from inside interpretation of the first Return, like
  // macOS Pinyin does. The renderer can process the commit before DOM keydown.
  await reset({ chat: true });
  ime.mark(title, 'a bo x', 6, 0);
  await eventually('Pinyin-style marked text reaches Blink', async () =>
    (await snapshot()).text === 'aba bo xCD');
  keys.key(title, 36, 0, 'abox');
  keys.key(title, 36, 0);
  await eventually('Both Returns and the candidate commit reach the editor', async () => {
    const current = await snapshot();
    return (current.events.filter(event => event.type === 'keyup' && event.key === 'Enter').length === 2 &&
      current.events.some(event => event.type === 'compositionend')) || current;
  });
  state = await snapshot();
  const returns = state.events.filter(event => event.type === 'keydown' && event.key === 'Enter');
  assert.deepEqual(returns.map(event => event.defaultPrevented), [false, true],
    'The confirming Return stays with the IME; the immediate second Return sends');
  assert.equal(state.submissions, 1, 'Immediate second Return sends exactly once');
  assert.equal(state.text, 'ababoxCD', 'Sending leaves the committed text intact');

  if (systemPinyin) {
    let lateKeyDowns = 0;
    let checked = 0;
    for (const delay of [0, 6, 40]) {
      for (const text of ['', '中文草稿：']) {
        for (let attempt = 0; attempt < 2; ++attempt) {
          if (!window.isFocused()) window.focus();
          await reset({ chat: true, text, start: text.length });
          // Let WindowServer and the IME observe the window before posting keys.
          await pause(150);
          for (const keyCode of [0, 11, 31, 7]) { // a, b, o, x
            keys.pinyinKey(title, keyCode);
            if (delay) await pause(delay);
          }
          keys.pinyinKey(title, 36);
          // Don't run a renderer probe between the posted keystroke and commit.
          await pause(110);
          await eventually('Pinyin confirms the candidate on the first Return', async () => {
            const current = await snapshot();
            return (current.events.some(event => event.type === 'compositionend') &&
              current.events.some(event => event.type === 'keyup' && event.key === 'Enter')) || current;
          });
          state = await snapshot();
          assert.equal(state.text, text + 'abox', 'First Return commits the Latin text');
          const keyDown = state.events.findIndex(event => event.type === 'keydown' && event.key === 'Enter');
          assert.notEqual(keyDown, -1, 'The native confirming Return reaches the page');
          const end = state.events.findIndex(event => event.type === 'compositionend');
          if (end < keyDown) ++lateKeyDowns;
          assert.equal(state.submissions, 0, `First Return only confirms (DOM commit before keydown: ${end < keyDown})`);
          keys.pinyinKey(title, 36);
          await eventually('The independent second Return sends', async () => {
            const current = await snapshot();
            const releases = current.events.filter(event => event.type === 'keyup' && event.key === 'Enter');
            return (releases.length === 2 && current.submissions === 1 && current.text === text + 'abox') || current;
          });
          ++checked;
        }
      }
    }
    console.log(JSON.stringify({ pass: 'System Pinyin abox confirmations and sends', checked, lateKeyDowns }));
  }
  console.log(JSON.stringify({ pass: 'IME confirmation followed by immediate chat Return' }));

  if (process.env.MINI_NEWLINE_SCREENSHOT) {
    await reset({ chat: true });
    newline('visible chat Shift+Return', await press());
    await evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))');
    await pause(50);
    fs.writeFileSync(process.env.MINI_NEWLINE_SCREENSHOT, (await window.webContents.capturePage()).toPNG());
  }
}).then(() => finish(), finish);
