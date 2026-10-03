'use strict';

const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const ROOT = path.resolve(__dirname, '..');
const PACKAGE_FILES = ['artifacts.json', 'cli.js', 'index.js', 'install.js', 'package.json'];

function temporaryDirectory(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'mini-electron-test-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  return directory;
}

function packageFixture(t) {
  const directory = path.join(temporaryDirectory(t), 'package');
  fs.mkdirSync(directory);
  for (const file of PACKAGE_FILES) fs.copyFileSync(path.join(ROOT, file), path.join(directory, file));
  fs.cpSync(path.join(ROOT, 'lib'), path.join(directory, 'lib'), { recursive: true });
  return directory;
}

function runNode(args, options = {}) {
  return childProcess.spawnSync(process.execPath, args, {
    cwd: options.cwd,
    env: { ...process.env, ...options.env },
    encoding: 'utf8',
    timeout: 15000,
  });
}

function hash(file) {
  return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
}

function createDistribution(directory) {
  fs.mkdirSync(directory, { recursive: true });
  const executable = path.join(directory, 'electron.exe');
  fs.copyFileSync(process.execPath, executable);
  fs.chmodSync(executable, 0o755);
  fs.writeFileSync(
    path.join(directory, 'version.json'),
    `${JSON.stringify({
      packageVersion: '1.3.3',
      runtimeVersion: '1.3.3',
      electronApiVersion: '41.2.0',
      nodeVersion: '24.0.0',
      nodeModuleAbi: 134,
      chromiumMajorVersion: 132,
      platform: 'win32',
      arch: 'x64',
      files: { 'electron.exe': hash(executable) },
      links: {},
    })}\n`,
  );
}

let crcTable;
function crc32(buffer) {
  crcTable ||= Array.from({ length: 256 }, (_, index) => {
    let value = index;
    for (let bit = 0; bit < 8; bit += 1) value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
    return value >>> 0;
  });
  let value = 0xffffffff;
  for (const byte of buffer) value = crcTable[(value ^ byte) & 0xff] ^ (value >>> 8);
  return (value ^ 0xffffffff) >>> 0;
}

function storedZip(file, entries) {
  const localParts = [];
  const centralParts = [];
  let offset = 0;
  for (const [name, contents, mode = 0o100644] of entries) {
    const nameBytes = Buffer.from(name);
    const data = Buffer.from(contents);
    const crc = crc32(data);
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(0x800, 6);
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(data.length, 18);
    local.writeUInt32LE(data.length, 22);
    local.writeUInt16LE(nameBytes.length, 26);
    localParts.push(local, nameBytes, data);

    const central = Buffer.alloc(46);
    central.writeUInt32LE(0x02014b50, 0);
    central.writeUInt16LE(0x0314, 4);
    central.writeUInt16LE(20, 6);
    central.writeUInt16LE(0x800, 8);
    central.writeUInt32LE(crc, 16);
    central.writeUInt32LE(data.length, 20);
    central.writeUInt32LE(data.length, 24);
    central.writeUInt16LE(nameBytes.length, 28);
    central.writeUInt32LE((mode << 16) >>> 0, 38);
    central.writeUInt32LE(offset, 42);
    centralParts.push(central, nameBytes);
    offset += local.length + nameBytes.length + data.length;
  }
  const centralDirectory = Buffer.concat(centralParts);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(entries.length, 8);
  end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(centralDirectory.length, 12);
  end.writeUInt32LE(offset, 16);
  fs.writeFileSync(file, Buffer.concat([...localParts, centralDirectory, end]));
}

const targetEnvironment = {
  npm_config_platform: 'win32',
  npm_config_arch: 'x64',
  MINI_ELECTRON_OFFLINE: '1',
};

const macTargetEnvironment = {
  npm_config_platform: 'darwin',
  npm_config_arch: 'arm64',
  MINI_ELECTRON_OFFLINE: '1',
};

function macVersion(files, links) {
  return {
    packageVersion: '1.3.3',
    runtimeVersion: '1.3.3',
    electronApiVersion: '41.2.0',
    nodeVersion: '24.0.0',
    nodeModuleAbi: 134,
    chromiumMajorVersion: 132,
    platform: 'darwin',
    arch: 'arm64',
    files,
    links,
  };
}

function createMacDistribution(directory) {
  const executableRelative = 'Electron.app/Contents/MacOS/Electron';
  const framework = 'Electron.app/Contents/Frameworks/Squirrel.framework';
  const binaryRelative = `${framework}/Versions/A/Squirrel`;
  const shipItRelative = `${framework}/Versions/A/Resources/ShipIt`;
  const executable = Buffer.from('mini-electron mac runtime');
  const frameworkBinary = Buffer.from('Squirrel framework binary');
  const shipIt = Buffer.from('ShipIt executable');
  for (const [relative, contents, mode] of [
    [executableRelative, executable, 0o755],
    [binaryRelative, frameworkBinary, 0o755],
    [shipItRelative, shipIt, 0o755],
  ]) {
    const output = path.join(directory, ...relative.split('/'));
    fs.mkdirSync(path.dirname(output), { recursive: true });
    fs.writeFileSync(output, contents, { mode });
  }
  fs.mkdirSync(path.join(directory, ...`${framework}/Versions/A/Headers`.split('/')));
  const links = {
    [`${framework}/Headers`]: 'Versions/Current/Headers',
    [`${framework}/Resources`]: 'Versions/Current/Resources',
    [`${framework}/Squirrel`]: 'Versions/Current/Squirrel',
    [`${framework}/Versions/Current`]: 'A',
  };
  for (const [relative, target] of Object.entries(links)) {
    fs.symlinkSync(target, path.join(directory, ...relative.split('/')));
  }
  const files = {
    [executableRelative]: crypto.createHash('sha256').update(executable).digest('hex'),
    [binaryRelative]: crypto.createHash('sha256').update(frameworkBinary).digest('hex'),
    [shipItRelative]: crypto.createHash('sha256').update(shipIt).digest('hex'),
  };
  fs.writeFileSync(path.join(directory, 'version.json'), `${JSON.stringify(macVersion(files, links))}\n`);
  return { framework, links };
}

test('installer rejects unsupported targets before creating an install marker', (t) => {
  const fixture = packageFixture(t);
  const result = runNode(['install.js'], {
    cwd: fixture,
    env: { npm_config_platform: 'linux', npm_config_arch: 'x64' },
  });
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /does not provide a runtime for linux-x64/u);
  assert.equal(fs.existsSync(path.join(fixture, 'path.txt')), false);
  assert.equal(fs.existsSync(path.join(fixture, 'dist')), false);
});

test('skip-download fails clearly when no verified binary exists', (t) => {
  const fixture = packageFixture(t);
  const result = runNode(['install.js'], {
    cwd: fixture,
    env: { ...targetEnvironment, MINI_ELECTRON_SKIP_BINARY_DOWNLOAD: '1' },
  });
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /download was skipped, but no valid win32-x64 runtime is installed/u);
  assert.equal(fs.existsSync(path.join(fixture, 'path.txt')), false);
});

test('offline distribution override installs atomically and require returns the real executable', (t) => {
  const fixture = packageFixture(t);
  const distribution = path.join(temporaryDirectory(t), 'runtime-dist');
  createDistribution(distribution);
  const installed = runNode(['install.js'], {
    cwd: fixture,
    env: { ...targetEnvironment, MINI_ELECTRON_DIST_PATH: distribution },
  });
  assert.equal(installed.status, 0, installed.stderr);
  const required = runNode(['-e', 'process.stdout.write(require(process.argv[1]))', fixture], {
    env: targetEnvironment,
  });
  assert.equal(required.status, 0, required.stderr);
  assert.equal(required.stdout, path.join(fixture, 'dist', 'electron.exe'));
  assert.equal(fs.statSync(required.stdout).size > 0, true);
});

test('installer rejects stale and unlisted runtime resources', (t) => {
  for (const scenario of ['stale', 'unlisted']) {
    const fixture = packageFixture(t);
    const distribution = path.join(temporaryDirectory(t), `runtime-${scenario}`);
    createDistribution(distribution);
    const resource = path.join(distribution, 'resources', 'mini-electron', 'lib', 'browser', 'init.js');
    fs.mkdirSync(path.dirname(resource), { recursive: true });
    fs.writeFileSync(resource, 'authenticated runtime resource\n');
    const versionFile = path.join(distribution, 'version.json');
    const version = JSON.parse(fs.readFileSync(versionFile, 'utf8'));
    version.files['resources/mini-electron/lib/browser/init.js'] = hash(resource);
    fs.writeFileSync(versionFile, `${JSON.stringify(version)}\n`);
    if (scenario === 'stale') fs.appendFileSync(resource, 'tampered\n');
    else fs.writeFileSync(path.join(distribution, 'unlisted.js'), 'unlisted\n');

    const result = runNode(['install.js'], {
      cwd: fixture,
      env: { ...targetEnvironment, MINI_ELECTRON_DIST_PATH: distribution },
    });
    assert.notEqual(result.status, 0, scenario);
    assert.match(result.stderr, /do not match version metadata|SHA256 mismatch/u);
    assert.equal(fs.existsSync(path.join(fixture, 'path.txt')), false);
    assert.equal(fs.existsSync(path.join(fixture, 'dist')), false);
  }
});

test('verified offline zip installs through the same contract as release archives', (t) => {
  const fixture = packageFixture(t);
  const archive = path.join(temporaryDirectory(t), 'runtime.zip');
  const executable = Buffer.from('verified mini-electron runtime');
  const executableHash = crypto.createHash('sha256').update(executable).digest('hex');
  const version = {
    packageVersion: '1.3.3',
    runtimeVersion: '1.3.3',
    electronApiVersion: '41.2.0',
    nodeVersion: '24.0.0',
    nodeModuleAbi: 134,
    chromiumMajorVersion: 132,
    platform: 'win32',
    arch: 'x64',
    files: { 'electron.exe': executableHash },
    links: {},
  };
  storedZip(archive, [
    ['electron.exe', executable],
    ['version.json', `${JSON.stringify(version)}\n`],
  ]);
  const result = runNode(['install.js'], {
    cwd: fixture,
    env: {
      ...targetEnvironment,
      MINI_ELECTRON_DIST_PATH: archive,
      MINI_ELECTRON_DIST_SHA256: hash(archive),
    },
  });
  assert.equal(result.status, 0, result.stderr);
  assert.deepEqual(fs.readFileSync(path.join(fixture, 'dist', 'electron.exe')), executable);
  assert.equal(fs.readFileSync(path.join(fixture, 'path.txt'), 'utf8').trim(), path.join('dist', 'electron.exe'));
});

test('authenticated macOS framework symlinks survive ZIP installation', { skip: process.platform === 'win32' }, (t) => {
  const fixture = packageFixture(t);
  const archive = path.join(temporaryDirectory(t), 'mac-runtime.zip');
  const executableRelative = 'Electron.app/Contents/MacOS/Electron';
  const framework = 'Electron.app/Contents/Frameworks/Squirrel.framework';
  const binaryRelative = `${framework}/Versions/A/Squirrel`;
  const shipItRelative = `${framework}/Versions/A/Resources/ShipIt`;
  const executable = Buffer.from('mini-electron mac runtime');
  const frameworkBinary = Buffer.from('Squirrel framework binary');
  const shipIt = Buffer.from('ShipIt executable');
  const links = {
    [`${framework}/Headers`]: 'Versions/Current/Headers',
    [`${framework}/Resources`]: 'Versions/Current/Resources',
    [`${framework}/Squirrel`]: 'Versions/Current/Squirrel',
    [`${framework}/Versions/Current`]: 'A',
  };
  const files = {
    [executableRelative]: crypto.createHash('sha256').update(executable).digest('hex'),
    [binaryRelative]: crypto.createHash('sha256').update(frameworkBinary).digest('hex'),
    [shipItRelative]: crypto.createHash('sha256').update(shipIt).digest('hex'),
  };
  storedZip(archive, [
    ...Object.entries(links).map(([relative, target]) => [relative, target, 0o120777]),
    [`${framework}/Versions/A/Headers/`, '', 0o040755],
    [executableRelative, executable, 0o100755],
    [binaryRelative, frameworkBinary, 0o100755],
    [shipItRelative, shipIt, 0o100755],
    ['version.json', `${JSON.stringify(macVersion(files, links))}\n`],
  ]);
  const result = runNode(['install.js'], {
    cwd: fixture,
    env: {
      ...macTargetEnvironment,
      MINI_ELECTRON_DIST_PATH: archive,
      MINI_ELECTRON_DIST_SHA256: hash(archive),
    },
  });
  assert.equal(result.status, 0, result.stderr);
  for (const [relative, target] of Object.entries(links)) {
    assert.equal(fs.readlinkSync(path.join(fixture, 'dist', ...relative.split('/'))), target);
  }
  assert.deepEqual(fs.readFileSync(path.join(fixture, 'dist', ...shipItRelative.split('/'))), shipIt);
});

test('authenticated macOS framework symlinks survive a local distribution override', { skip: process.platform === 'win32' }, (t) => {
  const fixture = packageFixture(t);
  const distribution = path.join(temporaryDirectory(t), 'mac-runtime');
  fs.mkdirSync(distribution);
  const { framework, links } = createMacDistribution(distribution);
  const result = runNode(['install.js'], {
    cwd: fixture,
    env: { ...macTargetEnvironment, MINI_ELECTRON_DIST_PATH: distribution },
  });
  assert.equal(result.status, 0, result.stderr);
  for (const [relative, target] of Object.entries(links)) {
    assert.equal(fs.readlinkSync(path.join(fixture, 'dist', ...relative.split('/'))), target);
  }
  assert.equal(
    fs.readFileSync(path.join(fixture, 'dist', ...`${framework}/Versions/A/Resources/ShipIt`.split('/')), 'utf8'),
    'ShipIt executable',
  );
});

test('installer rejects unsafe, unlisted, altered, dangling, and cyclic archive symlinks', (t) => {
  const executable = Buffer.from('verified mini-electron runtime');
  const executableHash = crypto.createHash('sha256').update(executable).digest('hex');
  const scenarios = [
    {
      name: 'absolute',
      declared: { pivot: '/tmp/mini-electron-escaped-marker' },
      archived: { pivot: '/tmp/mini-electron-escaped-marker' },
      error: /unsafe symlink target/u,
    },
    {
      name: 'drive',
      declared: { pivot: 'C:/mini-electron-escaped-marker' },
      archived: { pivot: 'C:/mini-electron-escaped-marker' },
      error: /unsafe symlink target/u,
    },
    {
      name: 'UNC',
      declared: { pivot: '//server/share/mini-electron-escaped-marker' },
      archived: { pivot: '//server/share/mini-electron-escaped-marker' },
      error: /unsafe symlink target/u,
    },
    {
      name: 'escaping',
      declared: { pivot: '../..' },
      archived: { pivot: '../..' },
      extraEntries: [['pivot/escaped-marker', 'must not leave staging']],
      error: /escapes extraction root/u,
    },
    {
      name: 'dangling',
      declared: { pivot: 'missing' },
      archived: { pivot: 'missing' },
      error: /dangling zip symlink target/u,
    },
    {
      name: 'cyclic',
      declared: { a: 'b', b: 'a' },
      archived: { a: 'b', b: 'a' },
      error: /cyclic zip symlink target/u,
    },
    {
      name: 'unlisted',
      declared: {},
      archived: { pivot: 'electron.exe' },
      error: /do not match the runtime symlink manifest/u,
    },
    {
      name: 'altered',
      declared: { pivot: 'electron.exe' },
      archived: { pivot: 'other.exe' },
      extraEntries: [['other.exe', 'other']],
      error: /does not match version metadata/u,
    },
  ];

  for (const scenario of scenarios) {
    const fixture = packageFixture(t);
    const archive = path.join(temporaryDirectory(t), `${scenario.name}.zip`);
    const version = {
      packageVersion: '1.3.3',
      runtimeVersion: '1.3.3',
      electronApiVersion: '41.2.0',
      nodeVersion: '24.0.0',
      nodeModuleAbi: 134,
      chromiumMajorVersion: 132,
      platform: 'win32',
      arch: 'x64',
      files: { 'electron.exe': executableHash },
      links: scenario.declared,
    };
    storedZip(archive, [
      ...Object.entries(scenario.archived).map(([relative, target]) => [relative, target, 0o120777]),
      ...(scenario.extraEntries || []),
      ['electron.exe', executable],
      ['version.json', `${JSON.stringify(version)}\n`],
    ]);
    const result = runNode(['install.js'], {
      cwd: fixture,
      env: {
        ...targetEnvironment,
        MINI_ELECTRON_DIST_PATH: archive,
        MINI_ELECTRON_DIST_SHA256: hash(archive),
      },
    });
    assert.notEqual(result.status, 0, scenario.name);
    assert.match(result.stderr, scenario.error, `${scenario.name}: ${result.stderr}`);
    assert.equal(fs.existsSync(path.join(fixture, 'path.txt')), false, scenario.name);
    assert.equal(fs.existsSync(path.join(fixture, 'dist')), false, scenario.name);
    assert.equal(fs.existsSync(path.join(fixture, 'escaped-marker')), false, scenario.name);
  }
});

test('installer rejects traversal archives without leaving a binary or escaped file', (t) => {
  const fixture = packageFixture(t);
  const archive = path.join(temporaryDirectory(t), 'malicious.zip');
  storedZip(archive, [['../escaped.exe', 'not an executable']]);
  const result = runNode(['install.js'], {
    cwd: fixture,
    env: {
      ...targetEnvironment,
      MINI_ELECTRON_DIST_PATH: archive,
      MINI_ELECTRON_DIST_SHA256: hash(archive),
    },
  });
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /unsafe zip entry path/u);
  assert.equal(fs.existsSync(path.join(fixture, 'path.txt')), false);
  assert.equal(fs.existsSync(path.join(fixture, 'dist')), false);
  assert.equal(fs.existsSync(path.join(fixture, 'escaped.exe')), false);
});

test('electron bin preserves argv, stdout, stderr, and exit code', (t) => {
  const fixture = packageFixture(t);
  const override = path.join(temporaryDirectory(t), 'override');
  fs.mkdirSync(override);
  const executable = path.join(override, 'electron.exe');
  fs.copyFileSync(process.execPath, executable);
  fs.chmodSync(executable, 0o755);
  const child = path.join(temporaryDirectory(t), 'child.js');
  fs.writeFileSync(child, "process.stdout.write(process.argv[2]); process.stderr.write(process.argv[3]); process.exit(23);\n");
  const result = runNode([path.join(fixture, 'cli.js'), child, 'stdout-value', 'stderr-value'], {
    env: { ...targetEnvironment, ELECTRON_OVERRIDE_DIST_PATH: override },
  });
  assert.equal(result.status, 23);
  assert.equal(result.stdout, 'stdout-value');
  assert.equal(result.stderr, 'stderr-value');
});

test('electron bin forwards termination signals and reports the child signal', { skip: process.platform === 'win32' }, async (t) => {
  const fixture = packageFixture(t);
  const override = path.join(temporaryDirectory(t), 'override');
  fs.mkdirSync(override);
  fs.symlinkSync(process.execPath, path.join(override, 'electron.exe'));
  const childScript = path.join(temporaryDirectory(t), 'signal-child.js');
  fs.writeFileSync(childScript, "process.once('SIGTERM', () => { process.removeAllListeners('SIGTERM'); process.kill(process.pid, 'SIGTERM'); }); console.log('ready'); setInterval(() => {}, 1000);\n");
  const cli = childProcess.spawn(process.execPath, [path.join(fixture, 'cli.js'), childScript], {
    env: { ...process.env, ...targetEnvironment, ELECTRON_OVERRIDE_DIST_PATH: override },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  await new Promise((resolve, reject) => {
    cli.stdout.once('data', resolve);
    cli.once('error', reject);
  });
  cli.kill('SIGTERM');
  const result = await new Promise((resolve) => cli.once('close', (code, signal) => resolve({ code, signal })));
  assert.deepEqual(result, { code: null, signal: 'SIGTERM' });
});
