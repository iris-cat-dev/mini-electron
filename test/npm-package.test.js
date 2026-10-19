'use strict';

const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const artifacts = require('../artifacts.json');

const ROOT = path.resolve(__dirname, '..');
const PACKAGE_FILES = ['cli.js', 'index.js', 'install.js', 'package.json'];
const WINDOWS_TARGET = {
  platform: 'win32',
  arch: 'x64',
  archive: `mini-electron-v${artifacts.runtimeVersion}-win32-x64.zip`,
  executable: 'electron.exe',
};
const MAC_TARGET = {
  platform: 'darwin',
  arch: 'arm64',
  archive: `mini-electron-v${artifacts.runtimeVersion}-darwin-arm64.zip`,
  executable: 'Electron.app/Contents/MacOS/Electron',
};


function temporaryDirectory(t) {
  const directory = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'mini-electron-test-')));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  return directory;
}

function packageFixture(t, target) {
  const directory = path.join(temporaryDirectory(t), 'package');
  fs.mkdirSync(directory);
  for (const file of PACKAGE_FILES) fs.copyFileSync(path.join(ROOT, file), path.join(directory, file));
  fs.cpSync(path.join(ROOT, 'lib'), path.join(directory, 'lib'), { recursive: true });
  if (target) {
    const manifestFile = path.join(directory, 'package.json');
    const manifest = JSON.parse(fs.readFileSync(manifestFile, 'utf8'));
    manifest.os = [target.platform];
    manifest.cpu = [target.arch];
    manifest.miniElectron.platforms = {
      [`${target.platform}-${target.arch}`]: { executable: target.executable },
    };
    fs.writeFileSync(manifestFile, `${JSON.stringify(manifest)}\n`);
  }
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

function runNodeAsync(args, options = {}) {
  return new Promise((resolve, reject) => {
    const child = childProcess.spawn(process.execPath, args, {
      cwd: options.cwd,
      env: { ...process.env, ...options.env },
      timeout: 15000,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stdout = '';
    let stderr = '';
    child.stdout.setEncoding('utf8').on('data', (chunk) => { stdout += chunk; });
    child.stderr.setEncoding('utf8').on('data', (chunk) => { stderr += chunk; });
    child.once('error', reject);
    child.once('close', (status, signal) => resolve({ status, signal, stdout, stderr }));
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
      installerProtocol: artifacts.installerProtocol,
      runtimeVersion: artifacts.runtimeVersion,
      electronApiVersion: '41.2.0',
      nodeVersion: artifacts.nodeVersion,
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
    installerProtocol: artifacts.installerProtocol,
    runtimeVersion: artifacts.runtimeVersion,
    electronApiVersion: '41.2.0',
    nodeVersion: artifacts.nodeVersion,
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
  for (const [platform, arch] of [['linux', 'x64'], ['darwin', 'x64'], ['win32', 'arm64']]) {
    const fixture = packageFixture(t);
    const result = runNode(['install.js'], {
      cwd: fixture,
      env: { npm_config_platform: platform, npm_config_arch: arch },
    });
    assert.notEqual(result.status, 0, `${platform}-${arch}`);
    assert.equal(fs.existsSync(path.join(fixture, 'path.txt')), false);
    assert.equal(fs.existsSync(path.join(fixture, 'dist')), false);
  }
});

test('skip-download fails clearly when no verified binary exists', (t) => {
  const fixture = packageFixture(t, WINDOWS_TARGET);
  const result = runNode(['install.js'], {
    cwd: fixture,
    env: { ...targetEnvironment, MINI_ELECTRON_SKIP_BINARY_DOWNLOAD: '1' },
  });
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /download was skipped, but no valid win32-x64 runtime is installed/u);
  assert.equal(fs.existsSync(path.join(fixture, 'path.txt')), false);
});

test('offline distribution override installs atomically and require returns the real executable', (t) => {
  const fixture = packageFixture(t, WINDOWS_TARGET);
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
    const fixture = packageFixture(t, WINDOWS_TARGET);
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
  const fixture = packageFixture(t, WINDOWS_TARGET);
  const archive = path.join(temporaryDirectory(t), 'runtime.zip');
  const executable = Buffer.from('verified mini-electron runtime');
  const executableHash = crypto.createHash('sha256').update(executable).digest('hex');
  const version = {
    installerProtocol: artifacts.installerProtocol,
    runtimeVersion: artifacts.runtimeVersion,
    electronApiVersion: '41.2.0',
    nodeVersion: artifacts.nodeVersion,
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
  const fixture = packageFixture(t, MAC_TARGET);
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
  const fixture = packageFixture(t, MAC_TARGET);
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
    const fixture = packageFixture(t, WINDOWS_TARGET);
    const archive = path.join(temporaryDirectory(t), `${scenario.name}.zip`);
    const version = {
      installerProtocol: artifacts.installerProtocol,
      runtimeVersion: artifacts.runtimeVersion,
      electronApiVersion: '41.2.0',
      nodeVersion: artifacts.nodeVersion,
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
  const fixture = packageFixture(t, WINDOWS_TARGET);
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
  const fixture = packageFixture(t, WINDOWS_TARGET);
  const override = path.join(temporaryDirectory(t), 'override');
  fs.mkdirSync(override);
  // Homebrew node needs its real path for @rpath/libnode; Windows needs no symlink privilege.
  const executable = path.join(override, 'electron.exe');
  if (process.platform === 'win32') fs.copyFileSync(process.execPath, executable);
  else fs.symlinkSync(process.execPath, executable);
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
  const fixture = packageFixture(t, WINDOWS_TARGET);
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

function releaseFixture(t, version) {
  const archiveName = `mini-electron-v${version}-win32-x64.zip`;
  const archive = path.join(temporaryDirectory(t), archiveName);
  const executable = `runtime v${version}\n`;
  const metadata = {
    installerProtocol: artifacts.installerProtocol,
    runtimeVersion: version,
    electronApiVersion: artifacts.electronApiVersion,
    nodeVersion: artifacts.nodeVersion,
    nodeModuleAbi: artifacts.nodeModuleAbi,
    chromiumMajorVersion: artifacts.chromiumMajorVersion,
    platform: 'win32',
    arch: 'x64',
    files: { 'electron.exe': crypto.createHash('sha256').update(executable).digest('hex') },
    links: {},
  };
  storedZip(archive, [['electron.exe', executable], ['version.json', JSON.stringify(metadata)]]);
  const manifest = {
    ...artifacts,
    runtimeVersion: version,
    platforms: {
      'win32-x64': { archive: archiveName, executable: 'electron.exe', sha256: hash(archive) },
    },
  };
  return { manifest, archive: fs.readFileSync(archive), executable };
}

async function releaseServer(t, releases) {
  const assets = new Map();
  for (const release of releases) {
    const { runtimeVersion: version, platforms } = release.manifest;
    const artifact = platforms['win32-x64'];
    assets.set(`/v${version}/artifacts.json`, JSON.stringify(release.manifest));
    assets.set(`/v${version}/${artifact.archive}`, release.archive);
    assets.set(`/v${version}/SHASUMS256.txt`, `${artifact.sha256}  ${artifact.archive}\n`);
  }
  const server = http.createServer((request, response) => {
    const body = assets.get(request.url);
    response.writeHead(body === undefined ? 404 : 200);
    response.end(body);
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  t.after(() => new Promise((resolve) => server.close(resolve)));
  const env = {
    ...targetEnvironment,
    MINI_ELECTRON_OFFLINE: '0',
    MINI_ELECTRON_CACHE: temporaryDirectory(t),
    MINI_ELECTRON_MIRROR: `http://127.0.0.1:${server.address().port}`,
    MINI_ELECTRON_DIST_PATH: '',
    MINI_ELECTRON_SKIP_BINARY_DOWNLOAD: '0',
    ELECTRON_SKIP_BINARY_DOWNLOAD: '0',
  };
  return { assets, env };
}

test('Windows target channel installs independently and exact version pins take precedence', async (t) => {
  const channelRelease = releaseFixture(t, '0.1.14');
  const pinnedRelease = releaseFixture(t, '0.1.15');
  const { assets, env } = await releaseServer(t, [channelRelease, pinnedRelease]);
  const legacyManifest = {
    ...channelRelease.manifest,
    runtimeVersion: '0.1.13',
    platforms: {
      'darwin-arm64': {
        archive: 'mini-electron-v0.1.13-darwin-arm64.zip',
        executable: 'Electron.app/Contents/MacOS/Electron',
        sha256: '0'.repeat(64),
      },
    },
  };
  assets.set('/runtime-v1/artifacts.json', JSON.stringify(legacyManifest));
  assets.set('/runtime-v1-win32-x64/artifacts.json', JSON.stringify(channelRelease.manifest));

  const channelFixture = packageFixture(t);
  let result = await runNodeAsync(['install.js'], { cwd: channelFixture, env });
  assert.equal(result.status, 0, result.stderr);
  assert.equal(
    fs.readFileSync(path.join(channelFixture, 'dist', 'electron.exe'), 'utf8'),
    channelRelease.executable,
  );

  const pinnedFixture = packageFixture(t);
  result = await runNodeAsync(['install.js'], {
    cwd: pinnedFixture,
    env: { ...env, MINI_ELECTRON_RUNTIME_VERSION: '0.1.15' },
  });
  assert.equal(result.status, 0, result.stderr);
  assert.equal(
    fs.readFileSync(path.join(pinnedFixture, 'dist', 'electron.exe'), 'utf8'),
    pinnedRelease.executable,
  );
});

test('runtime channel upgrades independently without changing npm or updating on ordinary install', async (t) => {
  const oldRelease = releaseFixture(t, '0.1.12');
  const newRelease = releaseFixture(t, '0.1.13');
  const { assets, env } = await releaseServer(t, [oldRelease, newRelease]);
  const fixture = packageFixture(t, WINDOWS_TARGET);
  const npmManifest = fs.readFileSync(path.join(fixture, 'package.json'), 'utf8');
  assets.set('/runtime-v1/artifacts.json', JSON.stringify(oldRelease.manifest));
  let result = await runNodeAsync(['install.js'], { cwd: fixture, env });
  assert.equal(result.status, 0, result.stderr);
  assert.equal(fs.readFileSync(path.join(fixture, 'dist', 'electron.exe'), 'utf8'), oldRelease.executable);

  assets.set('/runtime-v1/artifacts.json', JSON.stringify(newRelease.manifest));
  result = await runNodeAsync(['install.js'], { cwd: fixture, env });
  assert.equal(result.status, 0, result.stderr);
  assert.equal(fs.readFileSync(path.join(fixture, 'dist', 'electron.exe'), 'utf8'), oldRelease.executable);

  result = await runNodeAsync(['cli.js', '--update-runtime'], { cwd: fixture, env });
  assert.equal(result.status, 0, result.stderr);
  assert.equal(fs.readFileSync(path.join(fixture, 'dist', 'electron.exe'), 'utf8'), newRelease.executable);
  assert.equal(fs.readFileSync(path.join(fixture, 'package.json'), 'utf8'), npmManifest);
});

test('runtime pins override channel selection and mismatched releases leave the installed runtime usable', async (t) => {
  const pinned = releaseFixture(t, '0.1.12');
  const latest = releaseFixture(t, '0.1.13');
  const { assets, env } = await releaseServer(t, [pinned, latest]);
  const fixture = packageFixture(t, WINDOWS_TARGET);
  assets.set('/runtime-v1/artifacts.json', JSON.stringify(latest.manifest));
  let result = await runNodeAsync(['install.js'], {
    cwd: fixture, env: { ...env, MINI_ELECTRON_RUNTIME_VERSION: '0.1.12' },
  });
  assert.equal(result.status, 0, result.stderr);
  assert.equal(fs.readFileSync(path.join(fixture, 'dist', 'electron.exe'), 'utf8'), pinned.executable);
  result = await runNodeAsync(['cli.js', '--update-runtime', '0.1.13'], {
    cwd: fixture, env: { ...env, MINI_ELECTRON_RUNTIME_VERSION: '0.1.12' },
  });
  assert.equal(result.status, 0, result.stderr);
  assert.equal(fs.readFileSync(path.join(fixture, 'dist', 'electron.exe'), 'utf8'), latest.executable);

  assets.set('/v0.1.14/artifacts.json', JSON.stringify(latest.manifest));
  result = await runNodeAsync(['cli.js', '--update-runtime', '0.1.14'], { cwd: fixture, env });
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /runtime manifest version mismatch/u);
  const required = runNode(['-p', 'require("./")'], { cwd: fixture, env });
  assert.equal(required.status, 0, required.stderr);
  assert.equal(required.stdout.trim(), path.join(fixture, 'dist', 'electron.exe'));
  assert.equal(fs.readFileSync(required.stdout.trim(), 'utf8'), latest.executable);
});

test('incompatible manifests and corrupt downloads cannot replace a verified runtime or its marker', async (t) => {
  const release = releaseFixture(t, '0.1.12');
  const { assets, env } = await releaseServer(t, [release]);
  const fixture = packageFixture(t, WINDOWS_TARGET);
  const local = path.join(temporaryDirectory(t), 'runtime');
  createDistribution(local);
  const installed = runNode(['install.js'], {
    cwd: fixture, env: { ...env, MINI_ELECTRON_DIST_PATH: local },
  });
  assert.equal(installed.status, 0, installed.stderr);
  const originalHash = hash(path.join(fixture, 'dist', 'electron.exe'));
  const originalMarker = fs.readFileSync(path.join(fixture, 'path.txt'), 'utf8');
  for (const [changes, error] of [
    [{ installerProtocol: 2 }, /installerProtocol mismatch/u],
    [{ electronApiVersion: '42.0.0' }, /electronApiVersion mismatch/u],
    [{ runtimeVersion: '../escape' }, /invalid runtime version/u],
    [{ schemaVersion: 99 }, /unsupported runtime manifest schema/u],
    [{ platforms: {} }, /no valid artifact/u],
    [{}, /downloaded runtime SHA256 mismatch/u],
  ]) {
    assets.set('/runtime-v1/artifacts.json', JSON.stringify({ ...release.manifest, ...changes }));
    if (Object.keys(changes).length === 0) {
      assets.set(`/v0.1.12/${release.manifest.platforms['win32-x64'].archive}`, 'corrupt ZIP');
    }
    const result = await runNodeAsync(['cli.js', '--update-runtime'], { cwd: fixture, env });
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, error);
    assert.equal(hash(path.join(fixture, 'dist', 'electron.exe')), originalHash);
    assert.equal(fs.readFileSync(path.join(fixture, 'path.txt'), 'utf8'), originalMarker);
  }
});

test('a fresh offline installation can use a version-pinned verified download cache', async (t) => {
  const release = releaseFixture(t, '0.1.12');
  const { assets, env } = await releaseServer(t, [release]);
  assets.set('/runtime-v1/artifacts.json', JSON.stringify(release.manifest));
  const downloaded = await runNodeAsync(['install.js'], { cwd: packageFixture(t, WINDOWS_TARGET), env });
  assert.equal(downloaded.status, 0, downloaded.stderr);
  const fixture = packageFixture(t, WINDOWS_TARGET);
  const offline = await runNodeAsync(['install.js'], {
    cwd: fixture,
    env: {
      ...env, MINI_ELECTRON_OFFLINE: '1', MINI_ELECTRON_MIRROR: 'http://127.0.0.1:1',
      MINI_ELECTRON_RUNTIME_VERSION: '0.1.12',
    },
  });
  assert.equal(offline.status, 0, offline.stderr);
  assert.equal(fs.readFileSync(path.join(fixture, 'dist', 'electron.exe'), 'utf8'), release.executable);
});
