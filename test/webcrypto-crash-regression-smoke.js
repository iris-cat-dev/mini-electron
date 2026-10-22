'use strict';

// Run with mini-electron test/webcrypto-crash-regression-smoke.js, not Node.
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { app, BrowserWindow } = require('electron');

// Public test material only. A fixed key makes PKCS#1 signatures reproducible.
const rsa = { kty: 'RSA',
  n: 'nmbiATdQSNt_wFjs1e_RKET8c65uSzXY1p39t7fy7Y45l5rMUaapT0rphtMSMn8TtX7LZ_HAJOdqLWklaNJK6s0PpGdRfCt5d53r0vOLAU1mrQ0nrFSomEfExs2v2LXhioa6PYeo7PZQ4uT_5Yg30Gq68TXl4Hd_U-nR6HsR4fs', e: 'AQAB',
  d: 'WSIvntuPU9AgDQ59Bs2R9oULQdUvvyanw-zXbmAWVNKpWcQDoRaVQ0c2LWXsYXncj06wCuegZaPKvY7jPB7JhH_xEvwbCYwCB6TSadINIkAuYyrrJnl9PMyKbX4OYxGPhFRWXw7F6mI_rGEDiuEF4BWUdW9slV32VdC4SdE7E5E',
  p: 'z2NNCCVV3bcPe3iFnRqiNeaCEHxdnsN3uJe7JknDdfN7PUdLgvS-TOWrctZBuwEexliErHPMeBVWWevExE2k_w',
  q: 'w4gVsa7BVbe4A9YtP9f7l7mtqhZTm2So-zIj_Q-_Mw5FPVn8oaMxz-57WI_S8kqjEW--xTbmkRU_FeAKdoVXBQ',
  dp: 'LcbbgArNs0zvC_AEfjMczwz8dKse9yZqTMKhOshZbrW55ELSdiuTveGMQKLui1qp8lojnvVIktES6HJUUohY_Q',
  dq: 'PgG2130SxIsZaOYPvXJRz-DLKOghpWK7j8Vr0kGfIaLB5F3YFq45CraXGROrErkGk237cnEhlxrEwbwltUY9vQ',
  qi: 'fqgFqFd1gGvrcLYsl9f7vz0iXZ9EbHYmARZrsvfcPY9xFF8lvriJQqeATrsA1jHBSQ_ujJB_eN3XvsRk6g0KSA' };
const ecdh = crypto.createECDH('prime256v1');
ecdh.setPrivateKey(Buffer.from('01'.padStart(64, '0'), 'hex'));
const point = ecdh.getPublicKey();
const ec = { kty: 'EC', crv: 'P-256', d: Buffer.from('01'.padStart(64, '0'), 'hex').toString('base64url'),
  x: point.subarray(1, 33).toString('base64url'), y: point.subarray(33).toString('base64url') };
const message = Buffer.from('native WebCrypto regression');
const cases = [];
for (const bits of [128, 256]) {
  for (const tagLength of [32, 64, 96, 104, 112, 120, 128]) {
    for (const ivLength of [12, 16]) {
      for (const data of [Buffer.alloc(0), message]) {
        const key = Buffer.alloc(bits / 8), iv = Buffer.alloc(ivLength, 7), aad = Buffer.from('authenticated');
        const cipher = crypto.createCipheriv(`aes-${bits}-gcm`, key, iv, { authTagLength: tagLength / 8 });
        cipher.setAAD(aad);
        const expected = Buffer.concat([cipher.update(data), cipher.final(), cipher.getAuthTag()]);
        cases.push({ key: [...key], iv: [...iv], aad: [...aad], data: [...data], tagLength, expected: expected.toString('hex') });
      }
    }
  }
}
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-webcrypto-regression-'));
app.setPath('userData', path.join(temporary, 'user-data'));
app.on('window-all-closed', () => {});
let window;
let finished = false;
const deadline = setTimeout(() => finish(new Error('WebCrypto regression timed out')), 45000);
function finish(error) {
  if (finished) return;
  finished = true;
  clearTimeout(deadline);
  if (window && !window.isDestroyed()) window.destroy();
  fs.rmSync(temporary, { recursive: true, force: true });
  if (error) console.error(error.stack || error);
  else console.log('PASS: native AES-GCM vectors/authentication, RFC5869 HKDF, EC/RSA formats and signatures, malformed DER rejection');
  app.exit(error ? 1 : 0);
}

async function rendererTests(input) {
  const subtle = crypto.subtle;
  const bytes = value => new Uint8Array(value);
  const hex = value => Array.from(new Uint8Array(value), byte => byte.toString(16).padStart(2, '0')).join('');
  const fromHex = value => bytes(value.match(/../g).map(byte => parseInt(byte, 16)));
  function check(value, label) { if (!value) throw new Error(label); }
  async function rejects(operation, name) {
    try { await operation(); } catch (error) { check(error.name === name, `expected ${name}, got ${error.name}`); return; }
    throw new Error(`expected ${name} rejection`);
  }
  for (const item of input.cases) {
    const key = await subtle.importKey('raw', bytes(item.key), 'AES-GCM', false, ['encrypt', 'decrypt']);
    const algorithm = { name: 'AES-GCM', iv: bytes(item.iv), additionalData: bytes(item.aad), tagLength: item.tagLength };
    const encrypted = await subtle.encrypt(algorithm, key, bytes(item.data));
    check(hex(encrypted) === item.expected, 'AES-GCM ciphertext differs from independent Node EVP oracle');
    check(hex(await subtle.decrypt(algorithm, key, encrypted)) === hex(bytes(item.data)), 'AES-GCM plaintext mismatch');
    const tampered = new Uint8Array(encrypted); tampered[tampered.length - 1] ^= 1;
    await rejects(() => subtle.decrypt(algorithm, key, tampered), 'OperationError');
    await rejects(() => subtle.decrypt(algorithm, key, bytes([])), 'OperationError');
  }
  const aes = await subtle.importKey('raw', new Uint8Array(16), 'AES-GCM', false, ['encrypt']);
  await rejects(() => subtle.encrypt({ name: 'AES-GCM', iv: new Uint8Array(12), tagLength: 40 }, aes, bytes([])), 'OperationError');
  const kdf = await subtle.importKey('raw', new Uint8Array(22).fill(11), 'HKDF', false, ['deriveBits']);
  const hkdf = { name: 'HKDF', hash: 'SHA-256', salt: fromHex('000102030405060708090a0b0c'), info: fromHex('f0f1f2f3f4f5f6f7f8f9') };
  check(hex(await subtle.deriveBits(hkdf, kdf, 336)) === '3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865', 'RFC5869 HKDF vector');
  check((await subtle.deriveBits(hkdf, kdf, 0)).byteLength === 0, 'HKDF zero length');
  const emptyKdf = await subtle.importKey('raw', bytes([]), 'HKDF', false, ['deriveBits']);
  check(hex(await subtle.deriveBits({ ...hkdf, salt: bytes([]), info: bytes([]) }, emptyKdf, 256)) ===
    'eb70f01dede9afafa449eee1b1286504e1f62388b3f7dd4f956697b0e828fe18', 'HKDF empty input key, salt and info');
  await rejects(() => subtle.deriveBits(hkdf, kdf, 7), 'OperationError');
  await rejects(() => subtle.deriveBits(hkdf, kdf, 256 * 32 * 8), 'OperationError');
  const output = {};
  for (const [label, jwk, algorithm, signAlgorithm] of [
    ['ec', input.ec, { name: 'ECDSA', namedCurve: 'P-256' }, { name: 'ECDSA', hash: 'SHA-256' }],
    ['rsa', input.rsa, { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' }, { name: 'RSASSA-PKCS1-v1_5' }],
  ]) {
    const publicJwk = label === 'ec' ? { kty: jwk.kty, crv: jwk.crv, x: jwk.x, y: jwk.y } : { kty: jwk.kty, n: jwk.n, e: jwk.e };
    const privateKey = await subtle.importKey('jwk', jwk, algorithm, true, ['sign']);
    const publicKey = await subtle.importKey('jwk', publicJwk, algorithm, true, ['verify']);
    const privateExport = await subtle.exportKey('jwk', privateKey);
    const publicExport = await subtle.exportKey('jwk', publicKey);
    for (const property of Object.keys(jwk)) check(privateExport[property] === jwk[property], `${label} private JWK ${property}`);
    for (const property of Object.keys(publicJwk)) check(publicExport[property] === publicJwk[property], `${label} public JWK ${property}`);
    const pkcs8 = await subtle.exportKey('pkcs8', privateKey);
    const spki = await subtle.exportKey('spki', publicKey);
    const importedPrivate = await subtle.importKey('pkcs8', pkcs8, algorithm, true, ['sign']);
    const importedPublic = await subtle.importKey('spki', spki, algorithm, true, ['verify']);
    const signature = await subtle.sign(signAlgorithm, importedPrivate, bytes(input.message));
    check(await subtle.verify(signAlgorithm, importedPublic, signature, bytes(input.message)), `${label} verification`);
    check(!await subtle.verify(signAlgorithm, importedPublic, signature, bytes([1])), `${label} altered message accepted`);
    check(!await subtle.verify(signAlgorithm, importedPublic, bytes([1]), bytes(input.message)), `${label} short signature accepted`);
    for (const format of ['spki', 'pkcs8']) {
      const usage = format === 'spki' ? ['verify'] : ['sign'];
      await rejects(() => subtle.importKey(format, bytes([48, 0]), algorithm, true, usage), 'DataError');
      const der = new Uint8Array(format === 'spki' ? spki : pkcs8);
      const trailing = new Uint8Array(der.length + 1); trailing.set(der);
      await rejects(() => subtle.importKey(format, trailing, algorithm, true, usage), 'DataError');
    }
    if (label === 'ec') {
      const raw = await subtle.exportKey('raw', publicKey);
      check(hex(raw) === input.ecPoint, 'EC uncompressed point export');
      const rawKey = await subtle.importKey('raw', raw, algorithm, true, ['verify']);
      check(await subtle.verify(signAlgorithm, rawKey, signature, bytes(input.message)), 'EC raw roundtrip');
    }
    output[label] = { signature: hex(signature), pkcs8: hex(pkcs8), spki: hex(spki) };
  }
  return output;
}

app.whenReady().then(async () => {
  const page = path.join(temporary, 'index.html');
  fs.writeFileSync(page, '<!doctype html><title>WebCrypto regression</title>');
  window = new BrowserWindow({ show: false, webPreferences: { sandbox: true, contextIsolation: true, nodeIntegration: false } });
  await window.loadFile(page);
  const result = await window.webContents.executeJavaScript(`(${rendererTests.toString()})(${JSON.stringify({ cases, rsa, ec, message: [...message], ecPoint: point.toString('hex') })})`);
  for (const label of ['ec', 'rsa']) {
    const publicKey = crypto.createPublicKey({ key: Buffer.from(result[label].spki, 'hex'), format: 'der', type: 'spki' });
    const privateKey = crypto.createPrivateKey({ key: Buffer.from(result[label].pkcs8, 'hex'), format: 'der', type: 'pkcs8' });
    assert.equal(privateKey.asymmetricKeyType, label);
    assert.equal(crypto.verify('sha256', message, { key: publicKey, dsaEncoding: 'ieee-p1363' }, Buffer.from(result[label].signature, 'hex')), true);
  }
  const expectedSignature = crypto.sign('sha256', message, crypto.createPrivateKey({ key: rsa, format: 'jwk' }));
  assert.equal(result.rsa.signature, expectedSignature.toString('hex'));
  finish();
}).catch(finish);
