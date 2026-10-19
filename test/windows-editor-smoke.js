'use strict';
// Interactive Windows desktop; Microsoft Pinyin must already be active. Never changes the OS input source/theme.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow, BrowserView, Menu, clipboard } = require('electron');
const { startNativeInput } = require('./windows-native-input-driver');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-windows-editor-'));
const title = `mini-windows-editor-${process.pid}`;
const artifacts = process.env.MINI_NATIVE_INPUT_ARTIFACTS;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const results = [];
let window, driver, finished = false;
const request = (...args) => driver.request(...args);
const key = (...keys) => request('key', { keys });
const deadline = setTimeout(() => finish(new Error('Windows editor smoke timed out')), 150000);
app.on('window-all-closed', () => {});
async function evaluate(source, contents = window.webContents) {
  return contents.executeJavaScript(source);
}
const value = (source, contents) => evaluate(`JSON.stringify(${source})`, contents).then(JSON.parse);
async function eventually(read, predicate, label) {
  let last;
  const until = Date.now() + 5000;
  do { last = await read(); if (predicate(last)) return last; await pause(30); } while (Date.now() < until);
  assert.fail(`${label}: ${JSON.stringify(last)}`);
}
async function capture(name, inactive = false) {
  if (!artifacts) return;
  fs.mkdirSync(artifacts, { recursive: true });
  await request('capture', { path: path.join(artifacts, name + '.png'), inactive });
}
async function finish(error) {
  if (finished) return;
  finished = true; clearTimeout(deadline);
  if (error && window && !window.isDestroyed()) {
    try { console.error('EDITOR_FAILURE_STATE ' + JSON.stringify(await value('({active:document.activeElement.id,events,pastes})'))); }
    catch (diagnosticError) { console.error(diagnosticError); }
  }
  if (driver) { try { await driver.close(); } catch (cleanupError) { error = error || cleanupError; } }
  if (window && !window.isDestroyed()) window.destroy();
  try { fs.rmSync(temporary, { recursive: true, force: true }); }
  catch (cleanupError) { error = error || cleanupError; }
  if (artifacts) {
    fs.mkdirSync(artifacts, { recursive: true });
    fs.writeFileSync(path.join(artifacts, 'windows-editor-results.json'), JSON.stringify({ passed: !error, results, error: error ? String(error.stack || error) : null }, null, 2));
  }
  if (error) console.error(error.stack || error);
  else console.log('PASS: Windows native active/inactive selection contrast, copied/mixed image Files, Pinyin composition/cancel/UTF-16 replacement, caret scroll/zoom/move and BrowserView routing');
  app.exit(error ? 1 : 0);
}
async function focus(id, contents = window.webContents, offset = { x: 0, y: 0 }) {
  const rect = await value(`(() => { const r=document.getElementById(${JSON.stringify(id)}).getBoundingClientRect(); return {x:r.x+20,y:r.y+20,dpr:devicePixelRatio}; })()`, contents);
  const geometry = await request('geometry');
  await request('click', { x: Math.round(geometry.clientX + offset.x + rect.x * rect.dpr), y: Math.round(geometry.clientY + offset.y + rect.y * rect.dpr) });
  await evaluate(`(() => { const e=document.getElementById(${JSON.stringify(id)}); e.focus();
    if ('setSelectionRange' in e) e.setSelectionRange(e.value.length,e.value.length);
    else { const r=document.createRange();r.selectNodeContents(e);r.collapse(false);getSelection().removeAllRanges();getSelection().addRange(r); } return true; })()`, contents);
}
function luminance(rgb) {
  const c = rgb.map(v => v / 255).map(v => v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4);
  return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
}
function contrast(a, b) { const v = [luminance(a), luminance(b)].sort((x, y) => x - y); return (v[1] + 0.05) / (v[0] + 0.05); }
function cfHtml(fragment) {
  const before = '<html><body><!--StartFragment-->', after = '<!--EndFragment--></body></html>';
  const template = 'Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\nStartFragment:0000000000\r\nEndFragment:0000000000\r\n';
  const start = Buffer.byteLength(template), fragmentStart = start + Buffer.byteLength(before), fragmentEnd = fragmentStart + Buffer.byteLength(fragment);
  const numbers = [start, fragmentEnd + Buffer.byteLength(after), fragmentStart, fragmentEnd];
  let index = 0;
  return Buffer.from(template.replace(/0000000000/g, () => String(numbers[index++]).padStart(10, '0')) + before + fragment + after + '\0').toString('base64');
}
const nativeText = text => ({ name: 'UnicodeText', data: Buffer.from(text + '\0', 'utf16le').toString('base64') });
async function paste(id) {
  await focus(id); await evaluate('pastes=[];true'); await key(0x11, 0x56);
  const pasted = await eventually(() => value('pastes'), rows => rows.length === 1, 'native paste');
  assert.equal(pasted[0].error, undefined); return pasted[0];
}
async function caret(id, contents = window.webContents, offset = { x: 0, y: 0 }) {
  const pair = await eventually(async () => {
    const dom = await value(`(() => { const e=document.getElementById(${JSON.stringify(id)}),r=e.getBoundingClientRect();
      let selected=null; if(e.isContentEditable){const s=getSelection();if(s.rangeCount){const range=s.getRangeAt(0).cloneRange();
        range.collapse(false);if(range.startContainer.nodeType===1){const walker=document.createTreeWalker(e,NodeFilter.SHOW_TEXT);let node,last;while(node=walker.nextNode())last=node;if(last)range.setStart(last,last.length);range.collapse(true);}
        const c=range.getBoundingClientRect();selected={x:c.x,y:c.y,width:c.width,height:c.height};}}
      return {dpr:devicePixelRatio,rect:{x:r.x,y:r.y,width:r.width,height:r.height},selected};})()`, contents);
    return { dom, native: await request('guiState') };
  }, ({ dom, native }) => {
    if (native.caret !== native.window || native.bottom <= native.top) return false;
    const x = (native.left - offset.x) / dom.dpr, y = (native.top - offset.y) / dom.dpr;
    return x >= dom.rect.x && x < dom.rect.x + dom.rect.width && y >= dom.rect.y && y < dom.rect.y + dom.rect.height
      && (!dom.selected || dom.selected.height <= 0 || (Math.abs(native.left - offset.x - dom.selected.x * dom.dpr) <= 3
        && Math.abs(native.top - offset.y - dom.selected.y * dom.dpr) <= 3));
  }, `native/DOM caret geometry ${id}`);
  return pair.native;
}
async function preedit(id, contents, offset) {
  await evaluate('events=[];true', contents); await key(0x4e); await key(0x49);
  await eventually(() => value('events', contents), events => events.some(e => e.type === 'compositionupdate' && e.data === 'ni'), 'Pinyin preedit');
  return caret(id, contents, offset);
}
async function cancel(contents = window.webContents) {
  await key(0x1b);
  await eventually(() => value('events', contents), events => events.some(e => e.type === 'compositionend'), 'native IME cancel');
  await eventually(() => request('guiState'), state => state.caret === 0, 'native caret cleared');
}
app.whenReady().then(async () => {
  assert.equal(process.platform, 'win32');
  driver = await startNativeInput(temporary, title);
  await request('clipboardSave');
  const images = await request('images');
  window = new BrowserWindow({ x: 300, y: 80, width: 1120, height: 850, title,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  Menu.setApplicationMenu(Menu.buildFromTemplate([{ label: 'Edit', submenu: ['selectAll','copy','cut','paste','undo','redo'].map(role => ({role})) }]));
  const listeners = `window.events=[];window.pastes=[];
    for(const type of ['compositionstart','compositionupdate','compositionend','beforeinput','input'])addEventListener(type,e=>events.push({type,data:e.data,inputType:e.inputType,target:e.target.id,isComposing:e.isComposing}));
    addEventListener('paste',async e=>{e.preventDefault();const row={target:e.target.id,types:Array.from(e.clipboardData.types),text:e.clipboardData.getData('text/plain'),html:e.clipboardData.getData('text/html')};
      try{row.files=await Promise.all(Array.from(e.clipboardData.files).map(async file=>{const bytes=new Uint8Array(await file.arrayBuffer()),bitmap=await createImageBitmap(file),canvas=document.createElement('canvas');
        canvas.width=bitmap.width;canvas.height=bitmap.height;const context=canvas.getContext('2d');context.drawImage(bitmap,0,0);const pixel=Array.from(context.getImageData(0,0,1,1).data);bitmap.close();
        return{type:file.type,size:file.size,header:Array.from(bytes.slice(0,8)),width:canvas.width,height:canvas.height,pixel};}));}catch(error){row.error=String(error)}pastes.push(row)});`;
  await window.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(`<!doctype html><meta charset="utf-8"><style>
    html,body{margin:0;background:#d9d9d9;font:22px Consolas,monospace}body{height:1900px}
    textarea,.editor{position:absolute;box-sizing:border-box;width:400px;height:90px;padding:8px;border:2px solid #666;font:32px/42px Consolas,monospace;white-space:pre-wrap}
    #light{left:30px;top:35px;color-scheme:light;background:#fff;color:#111}#dark{left:480px;top:35px;color-scheme:dark;background:#121212;color:#eee}
    #custom{left:30px;top:145px;background:#fff;color:#111}#custom::selection{background:#ffb300;color:#102030}
    #rich-dark{left:30px;top:650px;color-scheme:dark;background:#232323;color:#eee}
    #rich{left:480px;top:250px;height:120px;background:#fff;font:22px/32px Consolas,monospace}
    #plain{left:480px;top:420px;height:140px;font:22px/32px Consolas,monospace}
    #image{position:absolute;left:70px;top:280px;width:64px;height:64px}
    #paste-plain{left:30px;top:410px;height:70px;font:22px/30px Consolas,monospace}#paste-rich{left:30px;top:530px;height:90px;background:white;font:22px/30px Consolas,monospace}
    </style><textarea id="light">MMMM MMMM selected</textarea><textarea id="dark">MMMM MMMM selected</textarea><textarea id="custom">MMMM MMMM selected</textarea>
    <div id="rich-dark" class="editor" contenteditable>MMMM MMMM selected</div>
    <div id="rich" class="editor" contenteditable>editor 😀 </div><textarea id="plain">textarea 😀 </textarea>
    <img id="image" src="data:image/png;base64,${images.png}"><textarea id="paste-plain"></textarea><div id="paste-rich" class="editor" contenteditable></div><script>${listeners}</script>`));
  window.show(); window.focus();
  for (const id of ['light','dark','custom','rich-dark']) {
    await focus(id); await key(0x11, 0x41);
    const geometry = await request('geometry');
    const rect = await value(`(() => {const r=document.getElementById('${id}').getBoundingClientRect();return{x:r.x,y:r.y,dpr:devicePixelRatio};})()`);
    const region = { x: Math.round(geometry.clientX + (rect.x + 12) * rect.dpr), y: Math.round(geometry.clientY + (rect.y + 12) * rect.dpr), width: Math.round(320 * rect.dpr), height: Math.round(40 * rect.dpr) };
    for (const inactive of [false,true]) {
      if (inactive) await request('focusSource');
      await pause(180);
      const colors = await request('sample', { ...region, inactive });
      const ratio = contrast(colors[0].rgb, colors[1].rgb);
      assert(ratio >= 4.5, `${id} ${inactive ? 'inactive' : 'active'} contrast ${ratio}: ${JSON.stringify(colors)}`);
      if (id === 'custom' && !await evaluate('matchMedia("(forced-colors: active)").matches')) {
        assert.deepEqual(colors[0].rgb, [255,179,0]); assert.deepEqual(colors[1].rgb, [16,32,48]);
      }
      results.push({ name:`${id}-${inactive ? 'inactive' : 'active'}-selection`, colors:colors.slice(0,2), contrast:ratio });
      if (id === 'dark' && inactive) await capture('editor-dark-inactive', true);
    }
  }
  await request('clipboardWrite', { formats: [nativeText('sentinel before web image copy')] });
  const imagePoint = await value('(() => {const r=document.getElementById("image").getBoundingClientRect();return{x:Math.floor(r.x+r.width/2),y:Math.floor(r.y+r.height/2)};})()');
  await window.webContents.copyImageAt(imagePoint.x, imagePoint.y);
  await eventually(() => Promise.resolve(clipboard.has('PNG')), Boolean, 'web image copied to native Windows PNG clipboard format');
  const assertImage = row => {
    assert.equal(row.files.length, 1); const image = row.files[0]; assert.equal(image.type,'image/png');
    assert.deepEqual(image.header,[137,80,78,71,13,10,26,10]); assert.equal(image.width,16);assert.equal(image.height,16);assert.deepEqual(image.pixel,[203,44,55,255]);
  };
  for (const id of ['paste-plain','paste-rich']) { const row=await paste(id);assertImage(row);results.push({name:`web-copy-image-${id}`,paste:row}); }
  const url = 'https://example.invalid/native-image.png', html = '<a href="' + url + '"><img src="' + url + '"></a>';
  const web = [nativeText(url), { name:'HTML Format',data:cfHtml(html) }, { name:'UniformResourceLocatorW',data:Buffer.from(url+'\0','utf16le').toString('base64') }];
  for (const [name,image] of [['PNG',images.png],['DIB',images.dib]]) {
    await request('clipboardWrite',{formats:[...web,{name,data:image}]});
    for (const id of ['paste-plain','paste-rich']) { const row=await paste(id);assertImage(row);assert.equal(row.text,url);assert(row.html.includes(url));results.push({name:`mixed-${name}-${id}`,paste:row}); }
  }
  for (const [name,formats] of [['html-url-only',web],['malformed-png',[...web,{name:'PNG',data:Buffer.from('not a PNG').toString('base64')}]]]) {
    await request('clipboardWrite',{formats});
    for (const id of ['paste-plain','paste-rich']) {const row=await paste(id);assert.equal(row.files.length,0);assert.equal(row.text,url);assert(row.html.includes(url));results.push({name:`${name}-${id}`,paste:row});}
  }
  await focus('light');await key(0x11,0x41);await key(0x11,0x43);
  const textOnly = await paste('paste-plain');assert.equal(textOnly.files.length,0);assert.equal(textOnly.text,'MMMM MMMM selected');
  results.push({name:'text-copy-clears-stale-image',paste:textOnly});
  const initialGeometry = await request('geometry');
  assert.equal(parseInt(initialGeometry.keyboardLayout,16)&0xffff,0x0804,'Run with Chinese Microsoft Pinyin already selected; test never changes the OS keyboard layout');
  await focus('rich');const richBase=await evaluate('document.getElementById("rich").textContent');
  const startCaret=await preedit('rich');await capture('editor-ime-preedit');await key(0x20);
  const commit=await eventually(()=>value('events'),events=>events.some(e=>e.type==='compositionend'),'native IME commit');
  const ends=commit.filter(e=>e.type==='compositionend');assert.equal(ends.length,1);assert(/\p{Script=Han}/u.test(ends[0].data));
  assert.equal(await evaluate('document.getElementById("rich").textContent'),richBase+ends[0].data);
  results.push({name:'native-pinyin-commit-once-after-surrogate',caret:startCaret,events:commit});
  const committedText=await evaluate('document.getElementById("rich").textContent');
  await preedit('rich');await cancel();assert.equal(await evaluate('document.getElementById("rich").textContent'),committedText);
  results.push({name:'native-pinyin-cancel-without-insertion'});
  for (const zoom of [1.2,1.5]) {
    window.webContents.setZoomFactor(zoom);await pause(120);await focus('rich');
    const scaled=await preedit('rich');await capture('editor-ime-zoom-'+zoom);await cancel();results.push({name:`native-caret-zoom-${zoom}`,caret:scaled});
  }
  window.webContents.setZoomFactor(1);await pause(100);await focus('rich');await preedit('rich');
  const beforeScroll=await request('guiState');
  await evaluate('window.editorScrollEnded=false;addEventListener("scrollend",()=>{editorScrollEnded=true},{once:true});true');
  await request('wheel',{x:initialGeometry.clientX+20,y:initialGeometry.clientY+20,delta:-120});
  await eventually(()=>evaluate('scrollY>0&&window.editorScrollEnded'),Boolean,'real page wheel scroll completed');
  const scrolled=await caret('rich');assert(scrolled.top<beforeScroll.top);await capture('editor-ime-page-scroll');
  const beforeMove=await request('geometry');await request('move',{x:beforeMove.left+70,y:beforeMove.top+50});
  const moved=await caret('rich');assert.equal(moved.screenX-scrolled.screenX,70);assert.equal(moved.screenY-scrolled.screenY,50);
  await evaluate('events=[];true');await key(0x48);
  const movedEvents=await eventually(()=>value('events'),events=>events.some(e=>e.type==='compositionupdate'),'preedit after scroll and move');
  assert.equal(await evaluate('document.getElementById("rich").textContent'),committedText+movedEvents.find(e=>e.type==='compositionupdate').data);
  const movedPreedit=await caret('rich');assert(movedPreedit.left>moved.left);await capture('editor-ime-window-moved');await cancel();
  results.push({name:'native-caret-page-scroll-and-window-move',before:beforeScroll,scrolled,moved,movedPreedit});
  await evaluate('scrollTo(0,0);true');await focus('plain');await preedit('plain');await capture('editor-ime-textarea');await cancel();
  const multiline=Array.from({length:18},(_,index)=>`line ${index} 😀 caret`).join('\n');
  await evaluate(`document.getElementById('plain').value=${JSON.stringify(multiline)};true`);await focus('plain');await key(0x11,0x24);
  for(let row=0;row<8;++row)await key(0x28);
  const innerSelectionStart=await evaluate('document.getElementById("plain").selectionStart');
  const beforeInner=await preedit('plain'),innerTop=await evaluate('document.getElementById("plain").scrollTop');
  const innerPoint=await value('(() => {const r=document.getElementById("plain").getBoundingClientRect();return{x:r.x+20,y:r.y+20,dpr:devicePixelRatio};})()'),innerGeometry=await request('geometry');
  await request('wheel',{x:Math.round(innerGeometry.clientX+innerPoint.x*innerPoint.dpr),y:Math.round(innerGeometry.clientY+innerPoint.y*innerPoint.dpr),delta:-120});
  await eventually(()=>evaluate('document.getElementById("plain").scrollTop'),top=>top>innerTop,'real textarea wheel scroll');
  const innerCaret=await caret('plain');assert(innerCaret.top<beforeInner.top);
  await evaluate('events=[];true');await key(0x48);
  const innerEvents=await eventually(()=>value('events'),events=>events.some(e=>e.type==='compositionupdate'),'preedit after textarea scroll');
  const innerPreedit=innerEvents.find(e=>e.type==='compositionupdate').data;
  assert.equal(await evaluate('document.getElementById("plain").value'),multiline.slice(0,innerSelectionStart)+innerPreedit+multiline.slice(innerSelectionStart));
  const resumedCaret=await caret('plain');assert(resumedCaret.left>innerCaret.left);await capture('editor-ime-textarea-scroll');await cancel();
  assert.equal(await evaluate('document.getElementById("plain").value'),multiline);results.push({name:'native-caret-textarea-scroll',before:beforeInner,after:innerCaret});
  const plainBefore=await evaluate('document.getElementById("plain").value');await key(0x11,0x41);await preedit('plain');await key(0x20);
  const replacement=await eventually(()=>value('events'),events=>events.some(e=>e.type==='compositionend'),'IME selection replacement');
  const replacementText=replacement.find(e=>e.type==='compositionend').data;
  assert(/\p{Script=Han}/u.test(replacementText));assert.equal(await evaluate('document.getElementById("plain").value'),replacementText);
  assert.notEqual(plainBefore,replacementText);results.push({name:'textarea-native-ime-replaces-UTF16-selection-once',events:replacement});
  await focus('rich');await preedit('rich');await request('focusSource');await eventually(()=>request('guiState'),s=>s.caret===0,'blur clears native caret');
  await focus('rich');await preedit('rich');await cancel();results.push({name:'blur-closes-ime-and-refocus-uses-fresh-caret'});
  await focus('rich');await preedit('rich');const bodyGeometry=await request('geometry');
  await request('click',{x:bodyGeometry.clientX+20,y:bodyGeometry.clientY+20});
  await eventually(()=>evaluate('document.activeElement.tagName'),tag=>tag==='BODY','noneditable native focus');
  await eventually(()=>request('guiState'),state=>state.caret===0,'noneditable focus clears native caret');
  await focus('rich');await preedit('rich');await cancel();results.push({name:'noneditable-focus-clears-ime-and-refocus-recovers'});
  const view=new BrowserView({webPreferences:{sandbox:true,contextIsolation:true,nodeIntegration:false}});
  window.addBrowserView(view);view.setBounds({x:100,y:180,width:600,height:260});
  await view.webContents.loadURL('data:text/html;charset=utf-8,'+encodeURIComponent('<!doctype html><meta charset="utf-8"><style>body{margin:0;background:#dbeafe}#view-editor{position:absolute;left:40px;top:35px;width:430px;height:100px;background:white;font:22px/32px Consolas}</style><div id="view-editor" contenteditable>view 😀 </div><script>'+listeners+'</script>'));
  const viewOffset={x:100*initialGeometry.dpi/96,y:180*initialGeometry.dpi/96};
  await focus('view-editor',view.webContents,viewOffset);const viewCaret=await preedit('view-editor',view.webContents,viewOffset);await capture('editor-ime-browserview');
  window.removeBrowserView(view);await eventually(()=>request('guiState'),s=>s.caret===0,'detaching focused BrowserView clears native caret');
  await focus('rich');await preedit('rich');await cancel();
  results.push({name:'browserview-native-ime-offset-focus-and-detach',caret:viewCaret,offset:viewOffset});
}).then(()=>finish(),finish);
