#!/usr/bin/env node
'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const https = require('node:https');
const os = require('node:os');
const path = require('node:path');
const artifacts = require('./artifacts.json');
const { executableIn, targetFromEnvironment } = require('./lib/paths');
const { extractZip, resolvedLinkTarget, safeRelativeName } = require('./lib/zip');

const PACKAGE_ROOT = __dirname;
const DIST = path.join(PACKAGE_ROOT, 'dist');
const PATH_MARKER = path.join(PACKAGE_ROOT, 'path.txt');
const HASH_PATTERN = /^[a-f0-9]{64}$/u;
const MAX_ARCHIVE_BYTES = 2 * 1024 * 1024 * 1024;

function truthy(value) {
  return typeof value === 'string' && value !== '' && value !== '0' && value.toLowerCase() !== 'false';
}

function sha256(file) {
  const digest = crypto.createHash('sha256');
  const descriptor = fs.openSync(file, 'r');
  const buffer = Buffer.allocUnsafe(1024 * 1024);
  try {
    let count;
    while ((count = fs.readSync(descriptor, buffer, 0, buffer.length, null)) > 0) {
      digest.update(buffer.subarray(0, count));
    }
  } finally {
    fs.closeSync(descriptor);
  }
  return digest.digest('hex');
}

function readJson(file, label) {
  let parsed;
  try {
    parsed = JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (error) {
    throw new Error(`${label} is missing or invalid: ${file}`, { cause: error });
  }
  if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
    throw new Error(`${label} must contain a JSON object: ${file}`);
  }
  return parsed;
}

function isRegularFile(file) {
  try {
    return fs.statSync(file).isFile();
  } catch {
    return false;
  }
}

function validateDistribution(directory, target) {
  const metadataFile = path.join(directory, 'version.json');
  const metadataStatus = fs.lstatSync(metadataFile);
  if (!metadataStatus.isFile() || metadataStatus.isSymbolicLink()) {
    throw new Error(`runtime version metadata is missing or unsafe: ${metadataFile}`);
  }
  const metadata = readJson(metadataFile, 'runtime version metadata');
  const required = {
    packageVersion: artifacts.packageVersion,
    runtimeVersion: artifacts.runtimeVersion,
    electronApiVersion: artifacts.electronApiVersion,
    nodeVersion: artifacts.nodeVersion,
    nodeModuleAbi: artifacts.nodeModuleAbi,
    chromiumMajorVersion: artifacts.chromiumMajorVersion,
    platform: target.platform,
    arch: target.arch,
  };
  for (const [name, expected] of Object.entries(required)) {
    if (metadata[name] !== expected) {
      throw new Error(
        `runtime metadata ${name} mismatch: expected ${JSON.stringify(expected)}, got ${JSON.stringify(metadata[name])}`,
      );
    }
  }
  if (!metadata.files || typeof metadata.files !== 'object' || Array.isArray(metadata.files)) {
    throw new Error('runtime metadata does not contain file hashes');
  }
  if (!metadata.links || typeof metadata.links !== 'object' || Array.isArray(metadata.links)) {
    throw new Error('runtime metadata does not contain a symlink manifest');
  }

  function manifestPath(relative, recordType) {
    if (typeof relative !== 'string') throw new Error(`runtime metadata contains an invalid ${recordType} record`);
    const safe = safeRelativeName(relative);
    if (safe.isDirectory || safe.portable !== relative) {
      throw new Error(`runtime metadata contains an invalid ${recordType} record: ${relative}`);
    }
    return safe.parts;
  }

  const declaredFiles = new Map();
  for (const [relative, expectedHash] of Object.entries(metadata.files)) {
    const parts = manifestPath(relative, 'file');
    if (typeof expectedHash !== 'string' || !HASH_PATTERN.test(expectedHash)) {
      throw new Error(`runtime metadata contains an invalid file record: ${relative}`);
    }
    const folded = relative.toLocaleLowerCase('en-US');
    if (declaredFiles.has(folded)) throw new Error(`runtime metadata contains a duplicate file record: ${relative}`);
    declaredFiles.set(folded, { expectedHash, parts, relative });
  }

  const declaredLinks = new Map();
  for (const [relative, targetValue] of Object.entries(metadata.links)) {
    manifestPath(relative, 'symlink');
    if (typeof targetValue !== 'string') {
      throw new Error(`runtime metadata contains an invalid symlink record: ${relative}`);
    }
    resolvedLinkTarget(relative, targetValue);
    const folded = relative.toLocaleLowerCase('en-US');
    if (declaredLinks.has(folded)) throw new Error(`runtime metadata contains a duplicate symlink record: ${relative}`);
    declaredLinks.set(folded, { relative, target: targetValue });
  }
  if (target.platform !== 'darwin' && declaredLinks.size !== 0) {
    throw new Error(`runtime symlinks are not allowed for ${target.platform}`);
  }

  const actualFiles = new Map();
  const actualLinks = new Map();
  function addActual(collection, relative, kind) {
    const folded = relative.toLocaleLowerCase('en-US');
    if (collection.has(folded)) throw new Error(`runtime distribution contains duplicate ${kind} paths: ${relative}`);
    collection.set(folded, relative);
  }
  function collectEntries(current, prefix = '') {
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const relative = prefix ? `${prefix}/${entry.name}` : entry.name;
      manifestPath(relative, 'path');
      const file = path.join(current, entry.name);
      const status = fs.lstatSync(file);
      if (status.isSymbolicLink()) {
        addActual(actualLinks, relative, 'symlink');
      } else if (status.isDirectory()) {
        collectEntries(file, relative);
      } else if (status.isFile()) {
        if (relative !== 'version.json') addActual(actualFiles, relative, 'file');
      } else {
        throw new Error(`runtime distribution contains a special file: ${relative}`);
      }
    }
  }
  collectEntries(directory);

  function assertExactManifest(actual, declared, kind) {
    if (actual.size !== declared.size) {
      throw new Error(`runtime distribution ${kind}s do not match version metadata`);
    }
    for (const [folded, relative] of actual) {
      const expected = declared.get(folded);
      if (!expected || expected.relative !== relative) {
        throw new Error(`runtime distribution ${kind}s do not match version metadata`);
      }
    }
  }
  assertExactManifest(actualFiles, declaredFiles, 'file');
  assertExactManifest(actualLinks, declaredLinks, 'symlink');

  const root = fs.realpathSync(directory);
  for (const { relative, target: expectedTarget } of declaredLinks.values()) {
    const link = path.join(directory, ...relative.split('/'));
    const status = fs.lstatSync(link);
    const actualTarget = fs.readlinkSync(link);
    if (!status.isSymbolicLink() || actualTarget !== expectedTarget) {
      throw new Error(`runtime symlink target mismatch: ${relative}`);
    }
    resolvedLinkTarget(relative, actualTarget);
    let resolved;
    try {
      resolved = fs.realpathSync(link);
    } catch (error) {
      throw new Error(`runtime symlink is dangling or cyclic: ${relative}`, { cause: error });
    }
    const fromRoot = path.relative(root, resolved);
    if (fromRoot === '..' || fromRoot.startsWith(`..${path.sep}`) || path.isAbsolute(fromRoot)) {
      throw new Error(`runtime symlink escapes the distribution: ${relative}`);
    }
  }

  for (const { expectedHash, parts, relative } of declaredFiles.values()) {
    const file = path.join(directory, ...parts);
    const status = fs.lstatSync(file);
    if (!status.isFile() || status.isSymbolicLink()) throw new Error(`runtime file is missing or unsafe: ${relative}`);
    if (sha256(file) !== expectedHash) throw new Error(`runtime file SHA256 mismatch: ${relative}`);
  }

  const executable = executableIn(directory, target);
  const status = fs.lstatSync(executable);
  if (!status.isFile() || status.isSymbolicLink() || status.size === 0) {
    throw new Error(`runtime executable is missing, unsafe, or empty: ${executable}`);
  }
  if (!Object.hasOwn(metadata.files, target.executable)) {
    throw new Error(`runtime metadata has no SHA256 for ${target.executable}`);
  }
  if (target.platform === 'darwin' && (status.mode & 0o111) === 0) {
    throw new Error(`runtime executable is not executable: ${executable}`);
  }
  return executable;
}

function parseChecksums(text, archiveName) {
  let match;
  for (const line of text.split(/\r?\n/u)) {
    const fields = /^([a-fA-F0-9]{64})\s+\*?(.+?)\s*$/u.exec(line);
    if (fields && path.basename(fields[2]) === archiveName) {
      match = fields[1].toLowerCase();
    }
  }
  if (!match) throw new Error(`SHA256 manifest does not contain ${archiveName}`);
  return match;
}

function checksumForLocalArchive(archive, env) {
  const configured = env.MINI_ELECTRON_DIST_SHA256;
  if (configured !== undefined) {
    const normalized = configured.trim().toLowerCase();
    if (!HASH_PATTERN.test(normalized)) throw new Error('MINI_ELECTRON_DIST_SHA256 must be 64 hexadecimal characters');
    return normalized;
  }
  const direct = `${archive}.sha256`;
  if (isRegularFile(direct)) {
    const text = fs.readFileSync(direct, 'utf8').trim();
    const token = text.split(/\s+/u)[0].toLowerCase();
    if (!HASH_PATTERN.test(token)) throw new Error(`invalid SHA256 sidecar: ${direct}`);
    return token;
  }
  const manifest = path.join(path.dirname(archive), 'SHASUMS256.txt');
  if (isRegularFile(manifest)) return parseChecksums(fs.readFileSync(manifest, 'utf8'), path.basename(archive));
  throw new Error(
    'offline runtime archive has no SHA256; provide a .sha256 sidecar, SHASUMS256.txt, or MINI_ELECTRON_DIST_SHA256',
  );
}

function defaultCacheDirectory(env) {
  if (env.MINI_ELECTRON_CACHE) return path.resolve(env.MINI_ELECTRON_CACHE);
  if (process.platform === 'win32' && env.LOCALAPPDATA) return path.join(env.LOCALAPPDATA, 'mini-electron', 'Cache');
  return path.join(os.homedir(), '.cache', 'mini-electron');
}

function download(url, destination, redirects = 0, maxBytes = MAX_ARCHIVE_BYTES) {
  return new Promise((resolve, reject) => {
    if (redirects > 5) {
      reject(new Error(`too many redirects while downloading ${url}`));
      return;
    }
    const parsed = new URL(url);
    if (parsed.protocol !== 'https:' && parsed.protocol !== 'http:') {
      reject(new Error(`unsupported download protocol ${parsed.protocol}`));
      return;
    }
    const client = parsed.protocol === 'https:' ? https : http;
    const request = client.get(parsed, { headers: { 'User-Agent': `mini-electron/${artifacts.packageVersion}` } }, (response) => {
      if (response.statusCode >= 300 && response.statusCode < 400 && response.headers.location) {
        response.resume();
        const redirect = new URL(response.headers.location, parsed).toString();
        download(redirect, destination, redirects + 1, maxBytes).then(resolve, reject);
        return;
      }
      if (response.statusCode !== 200) {
        response.resume();
        reject(new Error(`download failed with HTTP ${response.statusCode}: ${url}`));
        return;
      }
      const declaredLength = Number(response.headers['content-length']);
      if (Number.isFinite(declaredLength) && declaredLength > maxBytes) {
        response.resume();
        reject(new Error(`download exceeds the size limit: ${url}`));
        return;
      }
      let received = 0;
      response.on('data', (chunk) => {
        received += chunk.length;
        if (received > maxBytes) response.destroy(new Error(`download exceeds the size limit: ${url}`));
      });
      const output = fs.createWriteStream(destination, { flags: 'wx', mode: 0o600 });
      response.pipe(output);
      output.on('finish', () => output.close(resolve));
      output.on('error', reject);
      response.on('error', (error) => {
        output.destroy();
        reject(error);
      });
    });
    request.setTimeout(30000, () => request.destroy(new Error(`download timed out: ${url}`)));
    request.on('error', reject);
  });
}

async function downloadText(url, temporaryDirectory) {
  const file = path.join(temporaryDirectory, `checksums-${crypto.randomUUID()}.txt`);
  try {
    await download(url, file, 0, 1024 * 1024);
    return fs.readFileSync(file, 'utf8');
  } finally {
    fs.rmSync(file, { force: true });
  }
}

function copyDistribution(source, destination) {
  const sourceRoot = fs.realpathSync(source);
  const directories = [];
  const files = [];
  const links = [];
  function collect(current, prefix = '') {
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const relative = prefix ? `${prefix}/${entry.name}` : entry.name;
      const safe = safeRelativeName(relative);
      if (safe.isDirectory || safe.portable !== relative) {
        throw new Error(`runtime distribution contains an unsafe path: ${relative}`);
      }
      const input = path.join(current, entry.name);
      const status = fs.lstatSync(input);
      const record = { input, relative, status };
      if (status.isSymbolicLink()) {
        links.push(record);
      } else if (status.isDirectory()) {
        directories.push(record);
        collect(input, relative);
      } else if (status.isFile()) {
        files.push(record);
      } else {
        throw new Error(`runtime distribution contains a non-file entry: ${input}`);
      }
    }
  }
  collect(sourceRoot);

  fs.mkdirSync(destination, { recursive: true });
  directories.sort((left, right) => left.relative.split('/').length - right.relative.split('/').length);
  for (const entry of directories) {
    fs.mkdirSync(path.join(destination, ...entry.relative.split('/')), { mode: entry.status.mode & 0o777 });
  }
  for (const entry of files) {
    const currentStatus = fs.lstatSync(entry.input);
    if (!currentStatus.isFile() || currentStatus.isSymbolicLink()) {
      throw new Error(`runtime source changed while copying: ${entry.relative}`);
    }
    const output = path.join(destination, ...entry.relative.split('/'));
    fs.copyFileSync(entry.input, output, fs.constants.COPYFILE_EXCL);
    fs.chmodSync(output, currentStatus.mode & 0o777);
  }
  for (const entry of links) {
    const target = fs.readlinkSync(entry.input);
    resolvedLinkTarget(entry.relative, target);
    const output = path.join(destination, ...entry.relative.split('/'));
    fs.symlinkSync(target, output);
  }
}

async function acquireArchive(target, env, temporaryDirectory) {
  const overrideValue = env.MINI_ELECTRON_DIST_PATH;
  if (overrideValue) {
    const override = path.resolve(overrideValue);
    const status = fs.statSync(override);
    if (status.isDirectory()) {
      const directExecutable = executableIn(override, target);
      if (isRegularFile(directExecutable)) return { directory: override };
      const archive = path.join(override, target.archive);
      if (!isRegularFile(archive)) {
        throw new Error(`MINI_ELECTRON_DIST_PATH has neither ${target.executable} nor ${target.archive}: ${override}`);
      }
      return { archive, expectedHash: checksumForLocalArchive(archive, env) };
    }
    if (!status.isFile()) throw new Error(`MINI_ELECTRON_DIST_PATH is not a file or directory: ${override}`);
    return { archive: override, expectedHash: checksumForLocalArchive(override, env) };
  }

  const cache = defaultCacheDirectory(env);
  const archive = path.join(cache, target.archive);
  const checksumFile = `${archive}.sha256`;
  fs.mkdirSync(cache, { recursive: true });
  if (isRegularFile(archive) && isRegularFile(checksumFile)) {
    const expectedHash = fs.readFileSync(checksumFile, 'utf8').trim().toLowerCase();
    if (HASH_PATTERN.test(expectedHash) && sha256(archive) === expectedHash) return { archive, expectedHash };
    fs.rmSync(archive, { force: true });
    fs.rmSync(checksumFile, { force: true });
  }
  if (truthy(env.MINI_ELECTRON_OFFLINE)) {
    throw new Error(`offline mode is enabled and no verified cached runtime exists at ${archive}`);
  }

  const base = (env.MINI_ELECTRON_MIRROR || env.ELECTRON_MIRROR || artifacts.downloadBaseUrl).replace(/\/+$/u, '');
  if (!base) throw new Error('no mini-electron download mirror is configured');
  const release = `${base}/v${artifacts.runtimeVersion}`;
  const checksumText = await downloadText(`${release}/SHASUMS256.txt`, temporaryDirectory);
  const expectedHash = parseChecksums(checksumText, target.archive);
  const temporaryArchive = path.join(temporaryDirectory, target.archive);
  await download(`${release}/${encodeURIComponent(target.archive)}`, temporaryArchive);
  const actualHash = sha256(temporaryArchive);
  if (actualHash !== expectedHash) throw new Error(`downloaded runtime SHA256 mismatch: expected ${expectedHash}, got ${actualHash}`);
  const cacheTemporary = `${archive}.${process.pid}.${crypto.randomUUID()}.tmp`;
  fs.copyFileSync(temporaryArchive, cacheTemporary, fs.constants.COPYFILE_EXCL);
  try {
    fs.renameSync(cacheTemporary, archive);
  } catch (error) {
    fs.rmSync(cacheTemporary, { force: true });
    if (!isRegularFile(archive) || sha256(archive) !== expectedHash) throw error;
  }
  const checksumTemporary = `${checksumFile}.${process.pid}.${crypto.randomUUID()}.tmp`;
  fs.writeFileSync(checksumTemporary, `${expectedHash}\n`, { mode: 0o600 });
  try {
    fs.renameSync(checksumTemporary, checksumFile);
  } finally {
    fs.rmSync(checksumTemporary, { force: true });
  }
  return { archive, expectedHash };
}

function writePathMarker(relative) {
  const temporary = `${PATH_MARKER}.${process.pid}.${crypto.randomUUID()}.tmp`;
  try {
    fs.writeFileSync(temporary, `${relative}\n`, { mode: 0o600 });
    fs.rmSync(PATH_MARKER, { force: true });
    fs.renameSync(temporary, PATH_MARKER);
  } finally {
    fs.rmSync(temporary, { force: true });
  }
}


function installAtomically(staged, target) {
  validateDistribution(staged, target);
  const backup = `${DIST}.previous-${process.pid}-${crypto.randomUUID()}`;
  let movedExisting = false;
  let installedNew = false;
  try {
    if (fs.existsSync(DIST)) {
      fs.renameSync(DIST, backup);
      movedExisting = true;
    }
    fs.renameSync(staged, DIST);
    installedNew = true;
    const executable = validateDistribution(DIST, target);
    const relative = path.relative(PACKAGE_ROOT, executable);
    writePathMarker(relative);
    if (movedExisting) fs.rmSync(backup, { recursive: true, force: true });
  } catch (error) {
    fs.rmSync(PATH_MARKER, { force: true });
    if (installedNew && fs.existsSync(DIST)) fs.rmSync(DIST, { recursive: true, force: true });
    if (movedExisting && fs.existsSync(backup)) fs.renameSync(backup, DIST);
    throw error;
  }
}

async function install(env = process.env) {
  const target = targetFromEnvironment(env);
  try {
    validateDistribution(DIST, target);
    const relative = path.join('dist', ...target.executable.split('/'));
    writePathMarker(relative);
    return;
  } catch {
    fs.rmSync(PATH_MARKER, { force: true });
  }

  if (truthy(env.MINI_ELECTRON_SKIP_BINARY_DOWNLOAD) || truthy(env.ELECTRON_SKIP_BINARY_DOWNLOAD)) {
    throw new Error(
      `mini-electron binary download was skipped, but no valid ${target.key} runtime is installed; ` +
        'set MINI_ELECTRON_DIST_PATH to a built distribution or reinstall without the skip flag',
    );
  }

  const temporary = fs.mkdtempSync(path.join(PACKAGE_ROOT, '.mini-electron-install-'));
  const staged = path.join(temporary, 'dist');
  try {
    const source = await acquireArchive(target, env, temporary);
    if (source.directory) {
      validateDistribution(source.directory, target);
      copyDistribution(source.directory, staged);
    } else {
      if (fs.statSync(source.archive).size > MAX_ARCHIVE_BYTES) {
        throw new Error(`runtime archive exceeds the ${MAX_ARCHIVE_BYTES}-byte size limit`);
      }
      const actualHash = sha256(source.archive);
      if (actualHash !== source.expectedHash) {
        throw new Error(`runtime archive SHA256 mismatch: expected ${source.expectedHash}, got ${actualHash}`);
      }
      extractZip(source.archive, staged);
    }
    installAtomically(staged, target);
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

if (require.main === module) {
  install().catch((error) => {
    console.error(`mini-electron installation failed: ${error.message}`);
    process.exitCode = 1;
  });
}
