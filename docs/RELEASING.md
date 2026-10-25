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
2. Assemble without staging an npm package. On Windows, `--package-dist` prints
   `[windows] Matching executable and PDB: ...`; pass that exact archived leaf
   directory to `--windows-symbols` (replace the placeholders below):

   ```sh
   python build.py --jobs 12 --package-dist
   python scripts/build_runtime_release.py \
     --windows-dist out/windows-x64/runtime-dist \
     --windows-symbols "out/windows-x64/symbols/<pdbName>/<GUIDAGE>" \
     --runtime-only --output out/runtime-release
   ```

3. Publish every immutable asset from
   `out/runtime-release/v<runtimeVersion>/` to GitHub Release `v<runtimeVersion>`:
   the declared runtime ZIP, `SHASUMS256.txt`, `artifacts.json`, and, for Windows,
   `mini-electron-v<runtimeVersion>-win32-x64-symbols.zip`. The symbols ZIP contains
   the full matching PDB and `symbols.json` with its RSDS GUID/Age, original image
   and PDB names, and both SHA256 hashes. Its image hash also identifies the
   renamed `runtime-dist/electron.exe`; the duplicate EXE is not in the symbols
   ZIP. Both ZIP checksums appear in `SHASUMS256.txt`; only the installable runtime
   ZIP appears in the installer/channel manifest. Never overwrite a released
   version's assets; fixed versions and caches depend on immutability.
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

Windows assembly requires `--windows-symbols`; it rejects missing files,
GUID/Age mismatches, altered metadata, and archived EXEs whose bytes differ from
the distribution's executable, even if their RSDS identities match. Supply the
leaf directory containing `symbols.json`, the archived EXE, and PDB, not the
parent symbol store. The leaf is normally
`out/windows-x64/symbols/<pdbName>/<GUIDAGE>`, where GUID is uppercase without
hyphens and Age is uppercase hexadecimal. Preserve it for later crash analysis.
Use symbols from the normal release build, never a temporary diagnostic build.
PDBs and symbol metadata must stay out of `runtime-dist`, the installable ZIP,
and npm. Assembly validates symbols before replacing any prior release output
and checks the PDB hash again while compressing it. macOS-only assembly needs
only `--macos-dist`, creates no symbols ZIP, and requires no Windows symbol path.

### Using published Windows symbols

Download and extract the symbols ZIP from the same runtime release as the crash.
Match the crash module's RSDS GUID/Age to `symbols.json`; matching filenames or
version labels alone are insufficient. A newer release's PDB cannot symbolize an
older binary. Renaming the runtime to an application's EXE name does not change
its RSDS identity.

In WinDbg, add the extracted directory to the symbol path, reload symbols, and
analyze the dump:

```text
.sympath+ C:\symbols\mini-electron-v<runtimeVersion>-win32-x64
.reload /f
!analyze -v
```

PDBs symbolize native stacks. JavaScript retaining paths still require a
renderer heap snapshot; a small Crashpad minidump and PDB do not contain that
object graph.

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
