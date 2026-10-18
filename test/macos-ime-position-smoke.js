'use strict';

// Invoke: /path/to/mini-electron --app=test/macos-ime-position-smoke.js
// Requires a graphical macOS session and Xcode command-line tools. This queries
// the real AppKit candidate-anchor API; the system IME candidate panel itself
// remains a manual visual check. No keyboard input-source changes are made.
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-ime-position-'));
const title = `mini-ime-position-${process.pid}`;
const windows = [];
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
let window, native, finished = false;
const watchdog = setTimeout(() => finish(new Error('Native IME smoke exceeded 90s watchdog')), 90000);
app.on('window-all-closed', () => {});
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(watchdog);
  for (const item of windows) if (!item.isDestroyed()) item.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: native NSTextInputClient click focus, caret/selection, page/editor scroll, window movement, backing scale, blur reset and textarea/contenteditable composition/update/Unicode commit without duplicates');
  app.exit(error ? 1 : 0);
}
const evaluate = async source => (await window.webContents.executeJavaScript(source)).value;
const snapshot = id => evaluate(`JSON.stringify(window.imeSnapshot(${JSON.stringify(id)}))`).then(JSON.parse);
async function eventually(label, predicate) {
  let last;
  for (let attempt = 0; attempt < 100; ++attempt) {
    last = await predicate();
    if (last === true) return;
    await pause(30);
  }
  throw new Error(`${label}: ${JSON.stringify(last)}`);
}
const near = (a, b, tolerance) => Number.isFinite(a) && Math.abs(a - b) <= tolerance;
function matchesCaret(page, state) {
  return page.active && state.firstResponder && state.keyWindow &&
    state.selected.location === page.selected.location && state.selected.length === page.selected.length &&
    state.actual.location === page.selected.location && state.actual.length === page.selected.length &&
    state.rect.every(Number.isFinite) && state.rect[2] >= 0 && state.rect[3] > 10 &&
    near(state.rect[0], state.expectedScreen[0], 4) &&
    near(state.rect[1], state.expectedScreen[1], 6) &&
    near(state.rect[3], state.expectedScreen[3], 8);
}
async function caret(label, id) {
  let evidence;
  await eventually(label, async () => {
    const page = await snapshot(id);
    const state = native.state(title, page.caret);
    evidence = { label, id, page, native: state };
    return matchesCaret(page, state) || evidence;
  });
  assert.ok(near(evidence.page.dpr, evidence.native.scale, 0.01), 'Blink DPR must match AppKit backing scale');
  assert.ok(near(evidence.native.expectedBacking[3], evidence.page.caret[3] * evidence.native.scale, 0.1),
    'Page logical caret and native physical backing pixels must have exactly one scale conversion');
  console.log(JSON.stringify({ pass: label, id, selected: evidence.native.selected,
    screenRect: evidence.native.rect, expectedScreen: evidence.native.expectedScreen,
    backingRect: evidence.native.expectedBacking, scale: evidence.native.scale }));
  return evidence;
}
async function clickOffset(id, offset) {
  const rect = JSON.parse(await evaluate(`JSON.stringify(window.imeCaretAt(${JSON.stringify(id)}, ${offset}))`));
  native.click(title, rect[0] + 2, rect[1] + rect[3] / 2);
  await eventually(`Native click focuses ${id} at UTF-16 offset ${offset}`, async () => {
    const page = await snapshot(id);
    return (page.active && page.selected.location === offset && page.selected.length === 0) || page;
  });
}
async function resetState(label) {
  let state;
  await eventually(label, () => {
    state = native.state(title, [0, 0, 0, 0]);
    return (state.selected.location === null && state.actual.location === null &&
      !state.hasMarked && state.marked.location === null && state.rect.every(value => value === 0)) || state;
  });
  console.log(JSON.stringify({ pass: label, native: state }));
}
async function composition(id) {
  await evaluate(`(() => {
    const target = document.getElementById(${JSON.stringify(id)});
    if (target instanceof HTMLTextAreaElement) { target.value = 'abCD'; target.scrollTop = 0; }
    else target.textContent = 'abCD';
    window.imeEvents = []; return true;
  })()`);
  await clickOffset(id, 2);
  await caret(`${id}: composition insertion caret`, id);
  native.mark(title, 'にほん', 1, 1);
  await eventually(`${id}: first marked text reaches Blink`, async () => {
    const page = await snapshot(id), state = native.state(title, page.caret);
    return (page.text === 'abにほんCD' && page.selected.location === 3 && page.selected.length === 1 &&
      state.selected.location === 3 && state.selected.length === 1 &&
      state.hasMarked && state.marked.location === 2 && state.marked.length === 3) || { page, state };
  });
  await caret(`${id}: selected marked-text candidate anchor`, id);
  native.mark(title, 'にほんご', 3, 1);
  await eventually(`${id}: marked text update replaces prior composition`, async () => {
    const page = await snapshot(id), state = native.state(title, page.caret);
    return (page.text === 'abにほんごCD' && page.selected.location === 5 && page.selected.length === 1 &&
      state.selected.location === 5 && state.selected.length === 1 &&
      state.hasMarked && state.marked.location === 2 && state.marked.length === 4) || { page, state };
  });
  await caret(`${id}: updated marked-text candidate anchor`, id);
  native.commit(title, '日本語😀');
  await eventually(`${id}: native Unicode commit replaces marked text exactly once`, async () => {
    const page = await snapshot(id), state = native.state(title, page.caret);
    return (page.text === 'ab日本語😀CD' && page.selected.location === 7 && page.selected.length === 0 &&
      !state.hasMarked && state.marked.location === null) || { page, state };
  });
  await caret(`${id}: committed Unicode caret`, id);
  native.commit(title, '!');
  await eventually(`${id}: independent native insertion`, async () => {
    const page = await snapshot(id);
    return (page.text === 'ab日本語😀!CD' && page.selected.location === 8 && page.selected.length === 0) || page;
  });
  // Let queued native/renderer work drain before checking for duplicate commits.
  await pause(150);
  const page = await snapshot(id);
  assert.equal(page.text, 'ab日本語😀!CD');
  const events = page.events.filter(event => event.target === id);
  const lifecycle = events.filter(event => event.type.startsWith('composition'));
  const sequence = lifecycle.map(event => [event.type, event.data]);
  assert.deepEqual(sequence.slice(0, 3), [
    ['compositionstart', ''], ['compositionupdate', 'にほん'], ['compositionupdate', 'にほんご'],
  ], `${id}: native marked-text calls must generate ordered DOM composition updates`);
  assert.deepEqual(sequence.at(-1), ['compositionend', '日本語😀']);
  // Blink may report the changed commit string as one final update before end.
  assert.ok(sequence.length === 4 || sequence.length === 5, `${id}: no duplicate composition lifecycle`);
  if (sequence.length === 5) assert.deepEqual(sequence[3], ['compositionupdate', '日本語😀']);
  for (const text of ['にほん', 'にほんご']) {
    assert.ok(events.some(event => event.type === 'beforeinput' && event.data === text && event.composing),
      `${id}: composition beforeinput ${text}`);
    assert.ok(events.some(event => event.type === 'input' && event.data === text && event.composing),
      `${id}: composition input ${text}`);
  }
  assert.equal(events.filter(event => event.type === 'input' && event.data === '!').length, 1,
    `${id}: independent insertText must produce one input, not a duplicate char path`);
  console.log(JSON.stringify({ pass: `${id}: native composition/update/commit`, text: page.text, events }));
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'darwin');
  const root = path.resolve(__dirname, '..');
  const sdk = execFileSync('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], { encoding: 'utf8' }).trim();
  const compiler = execFileSync('xcrun', ['--find', 'clang++'], { encoding: 'utf8' }).trim();
  const helper = path.join(temporary, 'ime-position.node');
  const includes = ['', 'gen', 'third_party/abseil-cpp', 'base/allocator/partition_allocator/src',
    'gen/base/allocator/partition_allocator/src', 'third_party/libnode/src'];
  execFileSync(compiler, ['-std=c++20', '-fobjc-arc', '-bundle', '-undefined', 'dynamic_lookup', '-isysroot', sdk,
    ...includes.map(include => `-I${path.join(root, include)}`), '-framework', 'AppKit',
    path.join(__dirname, 'macos-ime-position-helper.mm'), '-o', helper], { stdio: 'inherit', timeout: 30000 });
  native = require(helper);
  window = new BrowserWindow({ title, width: 820, height: 720, show: true,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  windows.push(window);
  const blocker = new BrowserWindow({ title: `${title}-blocker`, width: 180, height: 120, show: false,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  windows.push(blocker);
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html>
    <title>${title}</title><style>
    html,body{margin:0;min-height:1500px;background:white;color:black}
    textarea,#rich{position:absolute;left:180px;width:450px;box-sizing:border-box;
      font:24px/32px monospace;letter-spacing:0;padding:8px;border:2px solid #555;
      white-space:pre;overflow:auto;resize:none}
    textarea{top:190px;height:128px}#rich{top:390px;height:104px}
    #noneditable{position:absolute;left:190px;top:560px;font:20px sans-serif}
    </style><textarea id="plain" spellcheck="false">abcdefghijklmnopqrstuvwxyz</textarea>
    <div id="rich" contenteditable="true" spellcheck="false">abcdefghijklmnopqrstuvwxyz</div>
    <button id="noneditable">Non-editable focus target</button><script>
    window.imeEvents = [];
    for (const type of ['compositionstart','compositionupdate','compositionend','beforeinput','input'])
      document.addEventListener(type, event => window.imeEvents.push({type, target:event.target.id,
        data:event.data, inputType:event.inputType || '', composing:!!event.isComposing}));
    window.imeCaretAt = (id, offset) => {
      const target = document.getElementById(id), box = target.getBoundingClientRect();
      if (target instanceof HTMLTextAreaElement) {
        // A hidden layout mirror is only an independent test oracle for the
        // textarea's otherwise inaccessible shadow-tree caret. Never passed to
        // an engine input/IME API; native state must arrive from Blink itself.
        const style = getComputedStyle(target), mirror = document.createElement('div');
        for (const name of ['font','letterSpacing','padding','border','boxSizing','tabSize'])
          mirror.style[name] = style[name];
        Object.assign(mirror.style, {position:'fixed',left:box.x+'px',top:box.y+'px',
          width:box.width+'px',height:'auto',whiteSpace:'pre',visibility:'hidden'});
        mirror.append(document.createTextNode(target.value.slice(0, offset)));
        const marker = document.createElement('span'); marker.textContent = '\\u200b'; mirror.append(marker);
        document.body.append(mirror); const rect = marker.getBoundingClientRect();
        const result = [rect.x-target.scrollLeft,rect.y-target.scrollTop,0,rect.height];
        mirror.remove(); return result;
      }
      const range = document.createRange(), walker = document.createTreeWalker(target, NodeFilter.SHOW_TEXT);
      let node = walker.nextNode(), remaining = offset;
      while (node && remaining > node.length) { remaining -= node.length; node = walker.nextNode(); }
      if (node) range.setStart(node, remaining);
      else range.setStart(target, target.childNodes.length);
      range.collapse(true);
      const rect = range.getBoundingClientRect(); return [rect.x,rect.y,rect.width,rect.height];
    };
    window.imeSnapshot = id => {
      const target = document.getElementById(id), rect = target.getBoundingClientRect();
      const plain = target instanceof HTMLTextAreaElement;
      let start = 0, end = 0;
      if (plain) { start=target.selectionStart; end=target.selectionEnd; }
      else {
        const selection = getSelection();
        if (selection.rangeCount && target.contains(selection.anchorNode) && target.contains(selection.focusNode)) {
          const offset = (node, position) => { const range=document.createRange();
            range.selectNodeContents(target); range.setEnd(node,position); return range.toString().length; };
          const anchor=offset(selection.anchorNode,selection.anchorOffset), focus=offset(selection.focusNode,selection.focusOffset);
          start=Math.min(anchor,focus); end=Math.max(anchor,focus);
        }
      }
      return { active:document.activeElement===target, text:plain?target.value:target.textContent,
        selected:{location:start,length:end-start}, caret:window.imeCaretAt(id,start),
        editor:[rect.x,rect.y,rect.width,rect.height], dpr:devicePixelRatio,
        scroll:[scrollX,scrollY,target.scrollLeft,target.scrollTop], events:window.imeEvents.slice() };
    };</script>`));
  window.show();
  app.focus();
  window.focus();
  await eventually('Native test window focused', async () => window.isFocused() && await evaluate('document.hasFocus()'));
  assert.equal(await evaluate("typeof require === 'undefined' && typeof process === 'undefined'"), true,
    'Fixture must run in the actual sandboxed renderer');
  await clickOffset('plain', 3);
  const first = await caret('Native mouse click anchors textarea far from view origin', 'plain');
  assert.ok(first.page.caret[0] > 180 && first.page.caret[1] > 190, 'Fixture must not be near the view origin');
  await clickOffset('plain', 17);
  const second = await caret('Native click moves caret within textarea', 'plain');
  assert.ok(second.native.rect[0] - first.native.rect[0] > 100, 'Candidate anchor must follow the changed caret');
  await evaluate("document.getElementById('plain').setSelectionRange(4, 11); true");
  await eventually('Native selectedRange tracks nonempty DOM selection', async () => {
    const state = native.state(title, [0, 0, 0, 0]);
    return (state.selected.location === 4 && state.selected.length === 7) || state;
  });
  await clickOffset('plain', 9);
  const beforeScroll = await caret('Collapsed caret after selection', 'plain');
  await evaluate('scrollTo(0, 90); true');
  const afterScroll = await caret('Page scroll updates native screen anchor', 'plain');
  assert.ok(near(afterScroll.native.rect[1] - beforeScroll.native.rect[1], 90, 3),
    'AppKit screen coordinates must move up when the page scrolls down');
  await evaluate(`(() => { const target=document.getElementById('plain');
    target.value=Array.from({length:12}, (_,i)=>'row'+i+' abcdefghijklmnop').join('\\n');
    const offset=target.value.indexOf('row8')+6; target.setSelectionRange(offset,offset);
    target.scrollTop=224; return true; })()`);
  const internal = await caret('Scrolled textarea uses visible caret, not unscrolled text offset', 'plain');
  assert.ok(internal.page.scroll[3] > 100, 'Textarea fixture must really be internally scrolled');
  assert.ok(internal.page.caret[1] >= internal.page.editor[1] &&
    internal.page.caret[1] + internal.page.caret[3] <= internal.page.editor[1] + internal.page.editor[3],
    'Scrolled textarea caret must remain visible');
  await clickOffset('rich', 6);
  const beforeMove = await caret('Native click anchors contenteditable', 'rich');
  const movement = native.move(title, 37, 29);
  assert.ok(Math.abs(movement[0]) + Math.abs(movement[1]) > 20, 'Window must actually move');
  const afterMove = await caret('Window movement reconverts cached caret to screen coordinates', 'rich');
  assert.ok(near(afterMove.native.rect[0] - beforeMove.native.rect[0], movement[0], 2));
  assert.ok(near(afterMove.native.rect[1] - beforeMove.native.rect[1], movement[1], 2));
  console.log(`PASS: logical/physical scale ${afterMove.native.scale} (${afterMove.native.scale > 1 ? 'Retina exercised' : '1x display; Retina hardware not available'})`);
  for (const id of ['plain', 'rich']) await composition(id);
  const button = JSON.parse(await evaluate(`(() => { const r=document.getElementById('noneditable').getBoundingClientRect();
    return JSON.stringify([r.x+r.width/2,r.y+r.height/2]); })()`));
  native.click(title, ...button);
  await eventually('Native click focuses a non-editable page control', async () =>
    await evaluate("document.activeElement.id === 'noneditable'"));
  await resetState('Non-editable focus clears stale candidate anchor');
  await clickOffset('rich', 2);
  await caret('Native editable refocus restores anchor', 'rich');
  blocker.show(); blocker.focus();
  await eventually('Native window blur', async () => !window.isFocused() && !await evaluate('document.hasFocus()'));
  await resetState('Native blur clears stale candidate anchor');
  finish();
}).catch(finish);
