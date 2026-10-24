'use strict';

const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const { TextDecoder } = require('node:util');

const EOCD = 0x06054b50;
const CENTRAL_FILE = 0x02014b50;
const LOCAL_FILE = 0x04034b50;
const MAX_ENTRIES = 10000;
const MAX_ENTRY_SIZE = 1024 * 1024 * 1024;
const MAX_TOTAL_SIZE = 4 * 1024 * 1024 * 1024;
const MAX_LINK_TARGET_SIZE = 4096;
const RESERVED_WINDOWS_NAME = /^(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$/iu;
const utf8 = new TextDecoder('utf-8', { fatal: true });

let crcTable;
function crc32(buffer) {
  if (!crcTable) {
    crcTable = Array.from({ length: 256 }, (_, index) => {
      let value = index;
      for (let bit = 0; bit < 8; bit += 1) {
        value = (value & 1) === 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
      }
      return value >>> 0;
    });
  }
  let value = 0xffffffff;
  for (const byte of buffer) value = crcTable[(value ^ byte) & 0xff] ^ (value >>> 8);
  return (value ^ 0xffffffff) >>> 0;
}

function decodeName(bytes, utf8Flag) {
  if (utf8Flag) return utf8.decode(bytes);
  if (bytes.some((byte) => byte > 0x7f)) {
    throw new Error('zip entry names must be UTF-8 or ASCII');
  }
  return bytes.toString('ascii');
}

function safeRelativeName(name) {
  const portable = name.replaceAll('\\', '/');
  const isDirectory = portable.endsWith('/');
  const trimmed = isDirectory ? portable.slice(0, -1) : portable;
  if (!trimmed || portable.startsWith('/') || portable.startsWith('//') || portable.includes('\0')) {
    throw new Error(`unsafe zip entry path: ${JSON.stringify(name)}`);
  }
  const parts = trimmed.split('/');
  for (const part of parts) {
    if (
      !part ||
      part === '.' ||
      part === '..' ||
      part.includes(':') ||
      part.endsWith('.') ||
      part.endsWith(' ') ||
      RESERVED_WINDOWS_NAME.test(part)
    ) {
      throw new Error(`unsafe zip entry path: ${JSON.stringify(name)}`);
    }
  }
  return { isDirectory, parts, portable: parts.join('/') };
}

function findEndRecord(archive) {
  const minimum = Math.max(0, archive.length - 22 - 0xffff);
  for (let offset = archive.length - 22; offset >= minimum; offset -= 1) {
    if (archive.readUInt32LE(offset) === EOCD) return offset;
  }
  throw new Error('zip end-of-central-directory record is missing');
}

function readEntries(archive) {
  if (archive.length < 22) throw new Error('zip archive is truncated');
  const end = findEndRecord(archive);
  const disk = archive.readUInt16LE(end + 4);
  const centralDisk = archive.readUInt16LE(end + 6);
  const diskEntries = archive.readUInt16LE(end + 8);
  const entries = archive.readUInt16LE(end + 10);
  const centralSize = archive.readUInt32LE(end + 12);
  const centralOffset = archive.readUInt32LE(end + 16);
  const commentLength = archive.readUInt16LE(end + 20);
  if (end + 22 + commentLength !== archive.length) throw new Error('zip has trailing or truncated data');
  if (disk !== 0 || centralDisk !== 0 || diskEntries !== entries) {
    throw new Error('multi-disk zip archives are not supported');
  }
  if (entries === 0xffff || centralSize === 0xffffffff || centralOffset === 0xffffffff) {
    throw new Error('ZIP64 archives are not supported');
  }
  if (entries > MAX_ENTRIES || centralOffset + centralSize !== end) {
    throw new Error('zip central directory is invalid');
  }

  const result = [];
  const names = new Set();
  let offset = centralOffset;
  let totalSize = 0;
  for (let index = 0; index < entries; index += 1) {
    if (offset + 46 > end || archive.readUInt32LE(offset) !== CENTRAL_FILE) {
      throw new Error('zip central directory entry is invalid');
    }
    const madeBySystem = archive.readUInt16LE(offset + 4) >>> 8;
    const flags = archive.readUInt16LE(offset + 8);
    const method = archive.readUInt16LE(offset + 10);
    const expectedCrc = archive.readUInt32LE(offset + 16);
    const compressedSize = archive.readUInt32LE(offset + 20);
    const size = archive.readUInt32LE(offset + 24);
    const nameLength = archive.readUInt16LE(offset + 28);
    const extraLength = archive.readUInt16LE(offset + 30);
    const commentSize = archive.readUInt16LE(offset + 32);
    const externalAttributes = archive.readUInt32LE(offset + 38);
    const localOffset = archive.readUInt32LE(offset + 42);
    const next = offset + 46 + nameLength + extraLength + commentSize;
    if (next > end) throw new Error('zip central directory entry is truncated');
    if ((flags & 1) !== 0) throw new Error('encrypted zip entries are not supported');
    if (method !== 0 && method !== 8) throw new Error(`unsupported zip compression method ${method}`);
    if (size > MAX_ENTRY_SIZE || totalSize + size > MAX_TOTAL_SIZE) {
      throw new Error('zip uncompressed size exceeds the extraction limit');
    }
    if (compressedSize > 0 && size / compressedSize > 1000) {
      throw new Error('zip entry compression ratio exceeds the extraction limit');
    }
    const rawName = archive.subarray(offset + 46, offset + 46 + nameLength);
    const name = decodeName(rawName, (flags & 0x800) !== 0);
    const safeName = safeRelativeName(name);
    const folded = safeName.portable.toLocaleLowerCase('en-US');
    if (names.has(folded)) throw new Error(`duplicate zip entry path: ${name}`);
    names.add(folded);
    const unixMode = madeBySystem === 3 ? externalAttributes >>> 16 : 0;
    const fileType = unixMode & 0o170000;
    let type;
    if (fileType === 0o120000) {
      if (safeName.isDirectory || size > MAX_LINK_TARGET_SIZE) {
        throw new Error(`invalid zip symlink entry: ${name}`);
      }
      type = 'link';
    } else if (safeName.isDirectory) {
      if ((fileType !== 0 && fileType !== 0o040000) || size !== 0 || compressedSize !== 0) {
        throw new Error(`invalid zip directory entry: ${name}`);
      }
      type = 'directory';
    } else {
      if (fileType !== 0 && fileType !== 0o100000) {
        throw new Error(`unsupported zip entry type: ${name}`);
      }
      type = 'file';
    }
    result.push({
      ...safeName,
      compressedSize,
      expectedCrc,
      flags,
      localOffset,
      method,
      mode: unixMode & 0o777,
      rawName,
      size,
      type,
    });
    totalSize += size;
    offset = next;
  }
  if (offset !== end) throw new Error('zip central directory entry count does not match its size');
  return result;
}

function entryData(archive, entry) {
  const offset = entry.localOffset;
  if (offset + 30 > archive.length || archive.readUInt32LE(offset) !== LOCAL_FILE) {
    throw new Error(`zip local header is invalid: ${entry.portable}`);
  }
  const flags = archive.readUInt16LE(offset + 6);
  const method = archive.readUInt16LE(offset + 8);
  const nameLength = archive.readUInt16LE(offset + 26);
  const extraLength = archive.readUInt16LE(offset + 28);
  const nameStart = offset + 30;
  const dataStart = nameStart + nameLength + extraLength;
  const dataEnd = dataStart + entry.compressedSize;
  if (flags !== entry.flags || method !== entry.method || dataEnd > archive.length) {
    throw new Error(`zip local header disagrees with central directory: ${entry.portable}`);
  }
  const localName = archive.subarray(nameStart, nameStart + nameLength);
  if (!localName.equals(entry.rawName)) throw new Error(`zip local entry name mismatch: ${entry.portable}`);
  const compressed = archive.subarray(dataStart, dataEnd);
  const data = method === 0
    ? Buffer.from(compressed)
    : zlib.inflateRawSync(compressed, { maxOutputLength: Math.max(entry.size, 1) });
  if (data.length !== entry.size || crc32(data) !== entry.expectedCrc) {
    throw new Error(`zip entry integrity check failed: ${entry.portable}`);
  }
  return data;
}

function resolvedLinkTarget(relative, target) {
  if (
    typeof target !== 'string' ||
    !target ||
    target.startsWith('/') ||
    target.startsWith('//') ||
    target.includes('\\') ||
    target.includes('\0') ||
    /^[a-z]:/iu.test(target)
  ) {
    throw new Error(`unsafe symlink target: ${relative} -> ${JSON.stringify(target)}`);
  }
  const resolved = relative.split('/').slice(0, -1);
  for (const part of target.split('/')) {
    if (!part || part === '.' || part.includes(':')) {
      throw new Error(`unsafe symlink target: ${relative} -> ${JSON.stringify(target)}`);
    }
    if (part === '..') {
      if (resolved.length === 0) {
        throw new Error(`symlink target escapes extraction root: ${relative} -> ${target}`);
      }
      resolved.pop();
    } else {
      resolved.push(part);
    }
  }
  if (resolved.length === 0) {
    throw new Error(`unsafe symlink target: ${relative} -> ${JSON.stringify(target)}`);
  }
  return resolved.join('/');
}

function validatedLinks(archive, entries) {
  const metadataEntry = entries.find((entry) => entry.portable === 'version.json');
  if (!metadataEntry || metadataEntry.type !== 'file') {
    throw new Error('zip runtime version metadata is missing or unsafe');
  }
  let metadata;
  try {
    metadata = JSON.parse(utf8.decode(entryData(archive, metadataEntry)));
  } catch (error) {
    throw new Error('zip runtime version metadata is invalid', { cause: error });
  }
  if (
    !metadata ||
    typeof metadata !== 'object' ||
    Array.isArray(metadata) ||
    !metadata.links ||
    typeof metadata.links !== 'object' ||
    Array.isArray(metadata.links)
  ) {
    throw new Error('zip runtime version metadata does not contain a symlink manifest');
  }

  const declared = new Map();
  for (const [relative, target] of Object.entries(metadata.links)) {
    const safeName = safeRelativeName(relative);
    if (safeName.isDirectory || safeName.portable !== relative || typeof target !== 'string') {
      throw new Error(`invalid symlink manifest record: ${relative}`);
    }
    resolvedLinkTarget(relative, target);
    const folded = relative.toLocaleLowerCase('en-US');
    if (declared.has(folded)) throw new Error(`duplicate symlink manifest record: ${relative}`);
    declared.set(folded, { relative, target });
  }

  const archiveLinks = entries.filter((entry) => entry.type === 'link');
  if (archiveLinks.length !== declared.size) {
    throw new Error('zip symlinks do not match the runtime symlink manifest');
  }
  for (const entry of archiveLinks) {
    let target;
    try {
      target = utf8.decode(entryData(archive, entry));
    } catch (error) {
      throw new Error(`invalid zip symlink target: ${entry.portable}`, { cause: error });
    }
    const expected = declared.get(entry.portable.toLocaleLowerCase('en-US'));
    if (!expected || expected.relative !== entry.portable || expected.target !== target) {
      throw new Error(`zip symlink target does not match version metadata: ${entry.portable}`);
    }
    resolvedLinkTarget(entry.portable, target);
    entry.linkTarget = target;
  }

  const nodes = new Map();
  const foldedNodes = new Map();
  function registerNode(portable, type, target) {
    const parts = portable.split('/');
    for (let index = 1; index < parts.length; index += 1) {
      const parent = parts.slice(0, index).join('/');
      const foldedParent = parent.toLocaleLowerCase('en-US');
      const existingName = foldedNodes.get(foldedParent);
      if (existingName && existingName !== parent) {
        throw new Error(`ambiguous zip entry path: ${portable}`);
      }
      foldedNodes.set(foldedParent, parent);
      const existing = nodes.get(parent);
      if (existing && existing.type !== 'directory') {
        throw new Error(`zip entry has a non-directory ancestor: ${portable}`);
      }
      nodes.set(parent, existing || { type: 'directory' });
    }
    const folded = portable.toLocaleLowerCase('en-US');
    const existingName = foldedNodes.get(folded);
    if (existingName && existingName !== portable) {
      throw new Error(`ambiguous zip entry path: ${portable}`);
    }
    foldedNodes.set(folded, portable);
    const existing = nodes.get(portable);
    if (existing && existing.type !== type) {
      throw new Error(`conflicting zip entry path: ${portable}`);
    }
    nodes.set(portable, { type, target });
  }
  for (const entry of entries) registerNode(entry.portable, entry.type, entry.linkTarget);

  function resolveNode(start) {
    let pending = start.split('/');
    let resolved = [];
    let linkCount = 0;
    while (pending.length > 0) {
      const part = pending.shift();
      const candidate = [...resolved, part].join('/');
      const node = nodes.get(candidate);
      if (!node) throw new Error(`dangling zip symlink target: ${start}`);
      if (node.type === 'link') {
        linkCount += 1;
        if (linkCount > archiveLinks.length) throw new Error(`cyclic zip symlink target: ${start}`);
        pending = [...resolvedLinkTarget(candidate, node.target).split('/'), ...pending];
        resolved = [];
      } else {
        if (pending.length > 0 && node.type !== 'directory') {
          throw new Error(`zip symlink traverses a non-directory: ${start}`);
        }
        resolved.push(part);
      }
    }
  }
  for (const entry of archiveLinks) resolveNode(entry.portable);
}

function outputPath(root, entry) {
  const output = path.join(root, ...entry.parts);
  const relative = path.relative(root, output);
  if (relative === '..' || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative)) {
    throw new Error(`zip entry escaped extraction root: ${entry.portable}`);
  }
  return output;
}

function extractZip(archivePath, destination) {
  const archive = fs.readFileSync(archivePath);
  const entries = readEntries(archive);
  validatedLinks(archive, entries);
  if (fs.existsSync(destination)) {
    const status = fs.lstatSync(destination);
    if (!status.isDirectory() || status.isSymbolicLink() || fs.readdirSync(destination).length !== 0) {
      throw new Error(`zip extraction destination must be an empty directory: ${destination}`);
    }
  } else {
    fs.mkdirSync(destination, { recursive: true });
  }
  const root = fs.realpathSync(destination);

  const directories = entries
    .filter((entry) => entry.type === 'directory')
    .sort((left, right) => left.parts.length - right.parts.length);
  for (const entry of directories) {
    entryData(archive, entry);
    const output = outputPath(root, entry);
    fs.mkdirSync(output, { recursive: true, mode: entry.mode || 0o755 });
    if (entry.mode) fs.chmodSync(output, entry.mode);
  }
  for (const entry of entries.filter((candidate) => candidate.type === 'file')) {
    const output = outputPath(root, entry);
    fs.mkdirSync(path.dirname(output), { recursive: true });
    const data = entryData(archive, entry);
    const descriptor = fs.openSync(output, 'wx', entry.mode || 0o644);
    try {
      fs.writeFileSync(descriptor, data);
    } finally {
      fs.closeSync(descriptor);
    }
    if (entry.mode) fs.chmodSync(output, entry.mode);
  }
  for (const entry of entries.filter((candidate) => candidate.type === 'link')) {
    const output = outputPath(root, entry);
    fs.symlinkSync(entry.linkTarget, output);
  }
  for (const entry of entries.filter((candidate) => candidate.type === 'link')) {
    const resolved = fs.realpathSync(outputPath(root, entry));
    const relative = path.relative(root, resolved);
    if (relative === '..' || relative.startsWith(`..${path.sep}`) || path.isAbsolute(relative)) {
      throw new Error(`zip symlink escaped extraction root: ${entry.portable}`);
    }
  }
}

module.exports = { extractZip, resolvedLinkTarget, safeRelativeName };
