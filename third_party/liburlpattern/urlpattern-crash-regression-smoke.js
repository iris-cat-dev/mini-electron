'use strict';

// Run with out/mac-arm64/mini-electron third_party/liburlpattern/urlpattern-crash-regression-smoke.js.
// These assertions execute Blink's URLPattern, not Node's independent implementation.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-urlpattern-regression-'));
app.setPath('userData', path.join(temporary, 'user-data'));
app.on('window-all-closed', () => {});
let window;
let finished = false;
const deadline = setTimeout(() => finish(new Error('URLPattern regression timed out')), 30000);

function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: Blink URLPattern string/object inputs, matching, groups, base URLs, and invalid-pattern errors');
  app.exit(error ? 1 : 0);
}

app.whenReady().then(async () => {
  const page = path.join(temporary, 'index.html');
  fs.writeFileSync(page, '<!doctype html><title>URLPattern regression</title>');
  window = new BrowserWindow({ show: false,
    webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  window.webContents.on('render-process-gone', (_event, details) => {
    finish(new Error(`URLPattern renderer terminated: ${JSON.stringify(details)}`));
  });
  await window.loadFile(page);
  const result = await window.webContents.executeJavaScript(`(() => {
    const absolute = new URLPattern('https://example.com/*');
    const object = new URLPattern({ protocol: 'https', hostname: 'example.com', pathname: '/users/:name' });
    const regex = new URLPattern({ pathname: '/items/:id([0-9]+)' });
    const relative = new URLPattern('../items/:id', 'https://example.com/root/nested/');
    const components = new URLPattern('https://user:123@example.com:8443/users/:name?q=:query#section-:part');
    const componentMatch = components.exec('https://user:123@example.com:8443/users/alice?q=hello#section-intro');
    const objectMatch = object.exec({ protocol: 'https', hostname: 'example.com', pathname: '/users/alice' });
    const regexMatch = regex.exec('https://example.com/items/123');
    const errors = [];
    // Repeat to exercise Status error construction, formatting and destruction,
    // not just the first tokenizer failure.
    const invalid = ['https://example.com/(', { pathname: '/:id(' },
      { pathname: '/:id/:id' }, { pathname: '/{unterminated' },
      { pathname: '/(a(b))' }, { protocol: '(' }];
    for (let round = 0; round < 8; ++round) {
      for (const pattern of invalid) {
        try {
          new URLPattern(pattern);
          errors.push('NO_ERROR');
        } catch (error) {
          errors.push(error.name);
        }
      }
    }
    return {
      isolated: typeof require === 'undefined' && typeof process === 'undefined',
      absolute: [absolute.test('https://example.com/path'), absolute.test('http://example.com/path'),
        absolute.test('https://other.example/path'), absolute.exec('https://example.com/a/b').pathname.groups['0']],
      object: [objectMatch.pathname.groups.name, object.test('https://example.com/users/alice'),
        object.test('https://example.com/users/alice/more'), object.exec('https://other.example/users/alice')],
      regex: [regex.hasRegExpGroups, regexMatch.pathname.groups.id,
        regex.test('https://example.com/items/abc'), absolute.hasRegExpGroups, object.hasRegExpGroups],
      relative: [relative.test('https://example.com/root/items/42'),
        relative.exec('/root/items/42', 'https://example.com').pathname.groups.id,
        relative.test('https://example.com/items/42')],
      components: [componentMatch.username.input, componentMatch.password.input,
        componentMatch.port.input, componentMatch.pathname.groups.name,
        componentMatch.search.groups.query, componentMatch.hash.groups.part,
        components.test('https://user:wrong@example.com:8443/users/alice?q=hello#section-intro')],
      invalidInput: [absolute.test('not an absolute URL'), absolute.exec('not an absolute URL')],
      errors,
      recovery: new URLPattern('https://example.com/recovered/:value').exec('https://example.com/recovered/ok').pathname.groups.value,
    };
  })()`);
  assert.equal(result.isolated, true);
  assert.deepEqual(result.absolute, [true, false, false, 'a/b']);
  assert.deepEqual(result.object, ['alice', true, false, null]);
  assert.deepEqual(result.regex, [true, '123', false, false, false]);
  assert.deepEqual(result.relative, [true, '42', false]);
  assert.deepEqual(result.components, ['user', '123', '8443', 'alice', 'hello', 'intro', false]);
  assert.deepEqual(result.invalidInput, [false, null]);
  for (const error of result.errors) {
    assert.equal(error, 'TypeError');
  }
  assert.equal(result.recovery, 'ok');
  finish();
}).catch(finish);
