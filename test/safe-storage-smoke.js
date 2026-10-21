'use strict';

// Run in application mode, not ELECTRON_RUN_AS_NODE:
// mini-electron --app=test/safe-storage-smoke.js
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, safeStorage } = require('electron');

const readback = process.env.MINI_SAFE_STORAGE_READBACK;
const temporary = readback || fs.mkdtempSync(path.join(os.tmpdir(), 'mini-safe-storage-'));
app.setPath('userData', path.join(temporary, readback ? 'restart-profile' : 'profile'));

function verifyUnavailable() {
  assert.equal(safeStorage.isEncryptionAvailable(), false);
  assert.throws(() => safeStorage.encryptString('synthetic probe'), /Encryption is not available/);
  assert.throws(() => safeStorage.decryptString(Buffer.alloc(0)), /Decryption is not available/);
}

async function main() {
  assert.equal(app.isReady(), false);
  verifyUnavailable();
  await app.whenReady();

  if (process.platform !== 'win32') {
    verifyUnavailable();
    safeStorage.setUsePlainTextEncryption(true);
    verifyUnavailable();
    safeStorage.setUsePlainTextEncryption(false);
    console.log('PASS: unsupported safeStorage backend fails closed, including plaintext opt-in');
    return;
  }

  assert.equal(safeStorage.isEncryptionAvailable(), true, 'Windows user DPAPI must be operational');
  if (readback) {
    for (const record of JSON.parse(fs.readFileSync(path.join(temporary, 'ciphertexts.json'), 'utf8'))) {
      assert.equal(safeStorage.decryptString(Buffer.from(record.encrypted, 'base64')), record.text);
    }
    console.log('PASS: persisted DPAPI data decrypts after restart with a separate application profile');
    return;
  }

  const samples = [
    'mini-electron-safe-storage-probe',
    '虚构测试-不含任何密钥',
    '',
    'before\0after\0中文🔐',
    'synthetic-中文🔐'.repeat(4096),
  ];
  const records = samples.map(text => {
    const encrypted = safeStorage.encryptString(text);
    assert.ok(Buffer.isBuffer(encrypted));
    assert.equal(safeStorage.decryptString(encrypted), text);
    return { text, encrypted: encrypted.toString('base64'), plain: Buffer.from(text).toString('base64') };
  });
  assert.equal(safeStorage.decryptString(Buffer.alloc(0)), '');
  assert.throws(() => safeStorage.decryptString('not a buffer'), /buffer/);
  const encrypted = Buffer.from(records[0].encrypted, 'base64');
  const damaged = Buffer.from(encrypted);
  damaged[damaged.length - 1] ^= 1;
  const legacyKey = Buffer.from('AtomEncryptionKey');
  const legacyXor = Buffer.from(samples[0]);
  for (let i = 0; i < legacyXor.length; ++i) legacyXor[i] ^= legacyKey[i % legacyKey.length];
  for (const invalid of [Buffer.from('plaintext'), Buffer.from('v10'), Buffer.from('v11'),
    legacyXor, encrypted.subarray(0, encrypted.length - 1), damaged]) {
    assert.throws(() => safeStorage.decryptString(invalid), /Error while decrypting/);
  }
  safeStorage.setUsePlainTextEncryption(true);
  assert.equal(safeStorage.decryptString(safeStorage.encryptString(samples[1])), samples[1]);
  safeStorage.setUsePlainTextEncryption(false);

  // Independent OS implementation: a round trip alone cannot distinguish DPAPI
  // from the old fixed-key XOR. Exchange only synthetic bytes with .NET DPAPI.
  const script = `
    $ErrorActionPreference = 'Stop'
    Add-Type -AssemblyName System.Security
    $scope = [System.Security.Cryptography.DataProtectionScope]::CurrentUser
    $records = [Console]::In.ReadToEnd() | ConvertFrom-Json
    $results = @($records | ForEach-Object {
      $plain = [Security.Cryptography.ProtectedData]::Unprotect([Convert]::FromBase64String($_.encrypted), $null, $scope)
      $encrypted = [Security.Cryptography.ProtectedData]::Protect([Convert]::FromBase64String($_.plain), $null, $scope)
      @{ plain = [Convert]::ToBase64String($plain); encrypted = [Convert]::ToBase64String($encrypted) }
    })
    [Console]::Out.Write((ConvertTo-Json -InputObject $results -Compress))
  `;
  const external = spawnSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-EncodedCommand',
    Buffer.from(script, 'utf16le').toString('base64')], {
    input: JSON.stringify(records.map(({ plain, encrypted }) => ({ plain, encrypted }))),
    encoding: 'utf8', windowsHide: true, timeout: 30000, maxBuffer: 4 * 1024 * 1024,
  });
  assert.ifError(external.error);
  assert.equal(external.status, 0, external.stderr);
  const exchanged = JSON.parse(external.stdout.trim());
  assert.equal(exchanged.length, records.length);
  exchanged.forEach((record, index) => {
    assert.equal(record.plain, records[index].plain, '.NET DPAPI must decrypt runtime output');
    assert.equal(safeStorage.decryptString(Buffer.from(record.encrypted, 'base64')), samples[index],
      'Runtime must decrypt independently protected DPAPI bytes');
  });

  fs.writeFileSync(path.join(temporary, 'ciphertexts.json'), JSON.stringify(records));
  const childEnvironment = { ...process.env, MINI_SAFE_STORAGE_READBACK: temporary };
  delete childEnvironment.ELECTRON_RUN_AS_NODE;
  const child = spawnSync(process.execPath, [`--app=${__filename}`], {
    env: childEnvironment, encoding: 'utf8', windowsHide: true, timeout: 30000,
  });
  assert.ifError(child.error);
  assert.equal(child.status, 0, `${child.stdout}\n${child.stderr}`);
  assert.match(child.stdout, /PASS: persisted DPAPI data decrypts after restart/);
  console.log('PASS: Windows DPAPI interoperability, ASCII/Unicode/empty/NUL round trips, malformed ciphertext rejection, and restart persistence');
}

main().then(() => finish(), finish);
function finish(error) {
  if (!readback) fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  app.exit(error ? 1 : 0);
}
