'use strict';

const fs = require('node:fs');
const childProcess = require('node:child_process');
const path = require('node:path');
const artifacts = require('../artifacts.json');

function targetFromEnvironment(env = process.env, platform = process.platform, arch = process.arch) {
  const selectedPlatform = env.npm_config_platform || platform;
  let selectedArch = env.npm_config_arch || arch;
  if (
    selectedPlatform === 'darwin' &&
    selectedArch === 'x64' &&
    env.npm_config_arch === undefined &&
    process.platform === 'darwin'
  ) {
    try {
      if (childProcess.execFileSync('/usr/sbin/sysctl', ['-in', 'sysctl.proc_translated'], { encoding: 'utf8' }).trim() === '1') {
        selectedArch = 'arm64';
      }
    } catch {
      // Native Intel macOS remains unsupported and is rejected below.
    }
  }
  const key = `${selectedPlatform}-${selectedArch}`;
  const target = artifacts.platforms[key];
  if (!target) {
    throw new Error(
      `mini-electron does not provide a runtime for ${selectedPlatform}-${selectedArch}; ` +
        'supported targets are win32-x64 and darwin-arm64',
    );
  }
  return { key, platform: selectedPlatform, arch: selectedArch, ...target };
}

function executableIn(directory, target) {
  return path.resolve(directory, ...target.executable.split('/'));
}

function assertExecutable(executable, label = 'mini-electron executable') {
  let status;
  try {
    status = fs.statSync(executable);
  } catch (error) {
    throw new Error(`${label} is missing: ${executable}`, { cause: error });
  }
  if (!status.isFile() || status.size === 0) {
    throw new Error(`${label} is not a non-empty file: ${executable}`);
  }
  return executable;
}

function resolveExecutable(options = {}) {
  const env = options.env || process.env;
  const target = targetFromEnvironment(env, options.platform, options.arch);
  const override = env.MINI_ELECTRON_OVERRIDE_DIST_PATH || env.ELECTRON_OVERRIDE_DIST_PATH;
  if (override) {
    return assertExecutable(executableIn(override, target), 'overridden mini-electron executable');
  }

  const packageRoot = options.packageRoot || path.resolve(__dirname, '..');
  const marker = path.join(packageRoot, 'path.txt');
  let relative;
  try {
    relative = fs.readFileSync(marker, 'utf8').trim();
  } catch (error) {
    throw new Error(
      'mini-electron failed to install: path.txt is missing; reinstall without skipping the binary download',
      { cause: error },
    );
  }
  if (!relative || path.isAbsolute(relative) || relative.split(/[\\/]+/u).includes('..')) {
    throw new Error('mini-electron failed to install: path.txt contains an unsafe executable path');
  }
  const expected = path.join('dist', ...target.executable.split('/'));
  if (path.normalize(relative) !== path.normalize(expected)) {
    throw new Error(
      `mini-electron failed to install: path.txt selects ${relative}, expected ${expected} for ${target.key}`,
    );
  }
  return assertExecutable(path.resolve(packageRoot, relative));
}

module.exports = {
  assertExecutable,
  executableIn,
  resolveExecutable,
  targetFromEnvironment,
};
