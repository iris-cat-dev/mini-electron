# Release process

[Documentation index](README.md) · [Project overview](../README.md)

> Run commands from the repository root unless a section explicitly changes directory.

See [version and channel rules](INSTALLATION.md#independent-npm-and-runtime-versions) before publishing.

### Publishing a binary-only update

1. Update the runtime version and platform archive names in root `artifacts.json`
   and the native version constant in `src/runtime/electron/common/atom_version.h`.
   Leave `package.json` unchanged unless npm-shipped inputs also changed.
   Rebuild/package the native distribution so the executable and `version.json`
   carry the new runtime identity and file hashes.
2. Assemble without staging an npm package:

   ```sh
   python build.py --jobs 12 --package-dist
   python scripts/build_runtime_release.py \
     --windows-dist out/windows-x64/runtime-dist \
     --runtime-only --output out/runtime-release
   ```

3. Publish **all three** immutable assets from
   `out/runtime-release/v<runtimeVersion>/` to GitHub Release `v<runtimeVersion>`:
   the declared ZIP, `SHASUMS256.txt`, and `artifacts.json`. The assembler computes
   the ZIP SHA256 in the emitted manifest. Never overwrite a released version's
   assets; fixed versions and caches depend on immutability.
4. Only after those assets are available, upload each emitted channel's
   `artifacts.json` to the matching channel GitHub Release, replacing only that
   channel asset. The Windows-only command above emits
   `out/runtime-release/runtime-v1-win32-x64/artifacts.json`; do not change the
   existing macOS `runtime-v1` release. Create a new channel release once,
   preferably as a prerelease so it is not mistaken for an immutable runtime
   release. Fresh unpinned installs and explicit unpinned updates use this pointer.

For example, after uploading a new version's immutable assets:

```sh
gh release upload runtime-v1-win32-x64 \
  out/runtime-release/runtime-v1-win32-x64/artifacts.json --clobber
```

No `npm publish` is needed for this workflow. Supply `--macos-dist` for a macOS
build, or both distribution arguments for a dual-platform release. Partial
assembly is allowed only when omitted targets use different channels; omitting
a target that shares a selected channel is rejected to avoid dropping support.
Target selection does not depend on npm's `os`/`cpu` declarations.

### Publishing an installer update

Omit `--runtime-only` to also stage `out/runtime-release/npm-package`, then publish
that directory with public access when installer/CLI/types/package inputs change.
The staged npm package excludes `artifacts.json`, so a runtime-only manifest bump
does not change its installer inputs.

The Windows-only installer `0.1.12` update required Windows runtime `0.1.14`
and its new `runtime-v1-win32-x64` channel before npm publication. That update
left macOS runtime `0.1.13` and `runtime-v1` unchanged. Later macOS runtime-only
updates, including `0.1.16`, retain the same npm installer and protocol.

The initial `0.1.11` cutover requires publishing both a protocol-1 runtime and its
channel before publishing the new npm installer. Older `0.1.10` installers remain
pinned to their original runtime; existing protocol-less binaries must be rebuilt
for the new installer. Future incompatible protocol or Electron API changes
require a new npm installer and a separate protocol channel, preserving the old
channel for existing installers.
