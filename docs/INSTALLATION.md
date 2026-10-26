# Installation and runtime updates

[Documentation index](README.md) · [Project overview](../README.md)

> Run commands from the repository root unless a section explicitly changes directory.

For native release history, see [release notes](RELEASES.md). For publishing assets, see [release process](RELEASING.md).

## npm distribution

The npm package is `@iriscat/mini-electron` and supports Windows x64 and Apple
Silicon macOS. Intel macOS and Windows ARM64 are not supported. Installer
`0.1.12` restores Windows installation; npm and native runtime versions remain
independent. Windows runtime `0.1.27` and macOS runtime `0.1.25` use separate
channels; the Windows update leaves the macOS channel unchanged.

```sh
npm install --save-dev @iriscat/mini-electron@0.1.12
node -p "require('@iriscat/mini-electron')"
```

The npm tarball contains the installer, CLI, types, and package metadata, not the
native executable or a pinned runtime manifest. Fresh installation reads
`runtime-v1-win32-x64/artifacts.json` for Windows or `runtime-v1/artifacts.json`
for macOS, then downloads the selected runtime's ZIP and `SHASUMS256.txt` from
its immutable `v<runtimeVersion>` release.
It verifies the archive against both manifests, validates the complete file/link
inventory, and atomically installs `dist`.
`require('@iriscat/mini-electron')` returns the executable path outside the runtime;
application main/preload code still imports the built-in `electron` API.
The Electron API compatibility version remains `41.2.0`.

### Independent npm and runtime versions

- `package.json.version` versions the npm installer, CLI, types, and package
  contract. Publish npm only when those shipped inputs change.
- Root `artifacts.json.runtimeVersion` versions the native distribution, including
  its embedded runtime JavaScript. Binary-only changes update this manifest and
  GitHub assets; they do **not** require an npm version bump or npm publication.
  For example, npm `0.1.12` can install Windows `0.1.27` or macOS `0.1.25`.
- `installerProtocol` replaces the old npm `packageVersion` equality check in
  runtime `version.json`. The remote manifest uses schema version `2`; protocol
  and Electron API compatibility must match the installer. Never point an
  existing protocol channel at an incompatible runtime.
- Per-target `runtimeChannel` metadata selects the mutable release channel.
  Windows uses `runtime-v1-win32-x64`; macOS keeps `runtime-v1`. This allows
  publishing a Windows-only runtime without rebuilding or changing macOS assets.

Existing valid installations are reused by ordinary postinstall runs. Application
startup and `require()` do not check the network or silently upgrade the runtime.
Update only the local binary explicitly:

```sh
npx mini-electron --update-runtime
# Pin the macOS runtime:
npx mini-electron --update-runtime 0.1.25
# Select an exact runtime that includes your platform (Windows example):
npx mini-electron --update-runtime 0.1.27
node -p "require('@iriscat/mini-electron/dist/version.json').runtimeVersion"
```

For reproducible fresh installations, pin the runtime separately from npm
(`0.1.27` for Windows; `0.1.25` for macOS):

```sh
# macOS arm64:
MINI_ELECTRON_RUNTIME_VERSION=0.1.25 npm ci
# Windows x64:
MINI_ELECTRON_RUNTIME_VERSION=0.1.27 npm ci
```

Keep that environment setting in CI/build configuration: npm's lockfile fixes
the installer, **not** the runtime selected by a mutable channel. An explicit
update-command version overrides the environment pin for that invocation; it does
not persist a new project pin. A failed manifest/download/integrity check leaves
the previous distribution and executable marker intact.

For local/offline installation, set `MINI_ELECTRON_DIST_PATH` to a protocol-1
`runtime-dist` directory, a ZIP accompanied by its `.sha256` file, or an assembled
release directory containing `artifacts.json`, the ZIP, and `SHASUMS256.txt`.
Local inputs bypass remote discovery but still enforce protocol, API, inventory,
and any explicit runtime pin. `MINI_ELECTRON_OFFLINE=1` can also install from a
previously verified download cache; use `MINI_ELECTRON_CACHE` to relocate it.
Set `MINI_ELECTRON_MIRROR` to an asset-server base URL hosting your target's
channel (`runtime-v1-win32-x64/artifacts.json` or `runtime-v1/artifacts.json`) and
`v<version>/` release assets. A version-pinned installation needs only its
`v<version>/` assets, not the channel.
Unset `ELECTRON_SKIP_BINARY_DOWNLOAD` and `MINI_ELECTRON_SKIP_BINARY_DOWNLOAD`
when installing into a fresh package directory or explicitly updating.
The installed executable is `dist/electron.exe` on Windows or
`dist/Electron.app/Contents/MacOS/Electron` on macOS; `npx mini-electron app.js`
and `ELECTRON_RUN_AS_NODE=1 npx mini-electron` launch the selected runtime.

Use this runtime directly for development or as an explicit Electron
distribution when packaging a Windows x64 or Apple Silicon macOS application.
Keep the original `npmRebuild: false` setting and application imports.
Both development packages expose an `electron` command; use the explicit
`mini-electron` CLI rather than depending on which shared command wins.
