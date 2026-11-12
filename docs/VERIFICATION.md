# Verification and known boundaries

[Documentation index](README.md) · [Project overview](../README.md)

> Run commands from the repository root unless a section explicitly changes directory.

Each record applies only to its stated source layout, runtime, platform and
workload. Older records migrated from the root README are not evidence of a new
verification run. Release-specific evidence is in [release notes](RELEASES.md).

## Windows runtime 0.1.41 (2026-11-13)

Built normal `Release|x64` with `python build.py --jobs 8 --package-dist`.
Assembled only Windows with `scripts/build_runtime_release.py --runtime-only`,
using the exact symbol leaf printed by the build:
`out/windows-x64/symbols/mini-electron.pdb/869E453D751DFEA14C4C44205044422E1`.
The runtime ZIP was extracted to `out/verification/windows-0.1.41/runtime/`;
that extracted `electron.exe` was used for the final native regressions.

Runtime identity: `0.1.41`, Electron API `41.2.0`, Node `24.0.0-pre`, ABI `134`,
installer protocol `1`, `win32`/`x64`. All 53 declared file hashes matched.
Native clipboard, asynchronous context and storage capabilities remain enabled.
EXE SHA-256:
`a2dddafb20584dbc16ffcbfff280d2cebcc0646ce8793ebc881d41274994e42f`.

The matching full PDB has GUID `869E453D751DFEA14C4C44205044422E`, Age `1`,
SHA-256 `0ee6d0d9c0af7dadb13391aa4084e67817b6e580591256341b89566b349b6979`.
`llvm-pdbutil dump -summary` reports debug info, types, IDs, globals and publics,
no conflicting types, and `Is stripped: false`. The symbols ZIP contains only
`mini-electron.pdb` and `symbols.json`; streaming the archived PDB reproduced its
hash. The installable ZIP contains no PDB or symbol metadata.

| Immutable asset | Bytes | SHA-256 |
| --- | ---: | --- |
| `mini-electron-v0.1.41-win32-x64.zip` | `39647022` | `e601dd377d81ab25bd20fb9210175e0f2be23f559d86cdf434200d7ec758300f` |
| `mini-electron-v0.1.41-win32-x64-symbols.zip` | `258993274` | `b2f20e3cdc1d2a601bc6c695ba593b2fc334d9f8aa9c5038894e34c7c673d091` |
| `artifacts.json` | `528` | `546d20a060ac5509e6b1ca5af0ef6b7947c478755f9165eccc104a416991c362` |
| `SHASUMS256.txt` | `214` | `efd9461125b03b4d8e4420aba349b2c451197e6e8a5c731e5f07f7bdd7695392` |

Passed against the final extracted executable:

- HTTPS navigation and native WSS echo/clean `1000` close, in default sandbox
  and `--single-process` modes.
- HTTPS and WSS rejection of untrusted/self-signed, expired and wrong-host
  certificates in both modes. The matching-SAN local self-signed fixture
  received neither an HTTP application request nor a WebSocket upgrade.
- Unchanged WS handshake, fragmented text, binary, ping/pong and clean-close
  regression in both process modes.
- Node/Worker native build layout and Node-only startup; SQLite/FTS5, transactions,
  backup, changesets and Worker binding; application async-context/Promise ordering.
- Build/package qualification also passed Node-only async-context/Worker ordering
  and all six isolated broker-storage durability/error phases.

TLS verification retained `webSecurity`, sandboxing, context isolation and
disabled renderer Node integration. No root certificate was installed and no TLS
validation bypass was used. WSS used a loopback HTTP initiator because this
workstation's Fake-IP DNS triggers existing private-endpoint authorization from a
custom-scheme origin; that separate authorization policy remains unchanged.

Evidence is under `out/verification/windows-0.1.41/`: `qualification.json`,
`build.log`, `pdb-summary.txt`, and isolated native result/row JSON files.
The public-service certificate matrix is session evidence, not a new
network-dependent permanent test. macOS and npm are not part of this binary update.

Published [v0.1.41](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.41)
from source `f01a4acbb7601dedae89d5389f1275a95c184b40`.
GitHub reports all four immutable assets uploaded, with exact sizes and SHA-256
digests matching the table. Anonymous full downloads of both ZIPs, both release
metadata files and the Windows channel manifest returned HTTP `200` and reproduced
their hashes. The public release page displayed both languages, 12 flat list
items, no nested lists and all four uploaded download links.

A fresh unpinned public npm `0.1.13` installation selected Windows `0.1.41`.
All 53 installed file hashes and the EXE hash matched the qualified archive;
the package import returned that executable. The public CLI passed application
mode with `miniElectron=0.1.41`, Electron API `41.2.0` and ABI `134`; Node-only
mode passed Node `24.0.0-pre`, ABI/platform/architecture and executable identity.

The Windows `runtime-v1-win32-x64` manifest asset is now `741521157`, SHA-256
`546d20a060ac5509e6b1ca5af0ef6b7947c478755f9165eccc104a416991c362`.
The macOS `runtime-v1` asset remains `737495293`, `541` bytes, SHA-256
`8d960c21ee6fa7ac4c015929aa601fa05cffb23127bca99fe2d8b5fb58aa64a6`;
its update timestamp is unchanged. Public download, page, channel and installation
evidence is retained alongside the local qualification JSON. Temporary probe
scripts, profiles and test private keys were removed. No npm was published and
no installed Desktop application was replaced.

## Windows native HTTPS/WSS CA trust (2026-11-12)

Built normal `Release|x64` with `python build.py --jobs 8`, using the existing local
GN, Ninja and CMake installations. Qualified `out/windows-x64/mini-electron.exe`,
SHA-256 `73352b4d07688d78697f8a1bf7e659e0cb94dc5325a20b13cc24f770992303aa`.
Its existing `0.1.40` version metadata was not advanced; this is an unreleased
source build, not the immutable published `0.1.40` archive.

The original `0.1.37` runtime ZIP was extracted into an isolated directory.
Baseline EXE SHA-256:
`d47e87c38500a928960109551f2db73ce73b7c39276d6cd2d969e9b2ac720578`.
It rejected `https://example.com/` with the webRequest error
`SSL peer certificate or SSH remote key was not OK`; a renderer connection to
`wss://ws.postman-echo.com/raw` failed before opening and closed with `1006`.

A throwaway native probe linked the runtime's actual `support.lib` and
`openssl.lib`, reporting curl `8.5.0-DEV`, TLS `BoringSSL`, CAfile `(none)` and
CApath `(none)`. With peer verification `1` and hostname verification `2`:

| Configuration / endpoint | curl result | TLS verify result | HTTP result |
| --- | --- | --- | --- |
| Default CA configuration, `https://example.com/` | `60` | `20` (local issuer unavailable) | No response |
| Windows native CA enabled, same endpoint | `0` | `0` | `200` |
| Windows native CA enabled, Postman WSS | `0` | `0` | Upgrade `101` |
| Windows native CA enabled, `wrong.host.badssl.com` | `60` | `1`, explicit hostname-mismatch diagnostic | No response |
| Windows native CA enabled, `expired.badssl.com` | `60` | `10` (expired) | No response |

The rebuilt runtime passed the following actual renderer scenarios in both the
default sandbox process model and `--single-process`:

- HTTPS navigation loaded the expected `Example Domain` page without webRequest
  errors.
- WSS opened, echoed `mini-native-tls-trust`, and closed cleanly with `1000`.
- HTTPS and WSS rejected a locally generated, currently valid self-signed
  certificate with a matching `127.0.0.1` SAN. Neither an HTTP request nor a
  WebSocket upgrade reached that fixture's application handlers.
- HTTPS and WSS rejected the expired and wrong-host public certificate fixtures.
- The unchanged `test/websocket-smoke.js` passed handshake, fragmented text,
  binary, ping/pong and clean close in both process models.

The TLS scenarios retained `webSecurity`, sandboxing, context isolation and
disabled Node integration. No test root was installed in Windows, no CA bundle
was injected, and no certificate-validation bypass was used. Temporary profiles
and generated application names isolated the runs from installed applications.

Environment boundary: Windows DNS on this workstation returned Fake-IP addresses
in `198.18.0.0/15` and `2001:2::` for the external test hosts. An initial
custom-scheme-origin run restored HTTPS but WSS still failed the existing private
endpoint authorization check. The final WSS certificate scenarios used an allowed
loopback HTTP page as their initiator, without changing that authorization policy.
This repair does not claim to change Fake-IP/private-network authorization.

Evidence is under `out/verification/windows-tls-trust/`: native curl diagnostic
logs, `build.log`, baseline `isolated-tls-LUYwuf/result.json`, default-process
`isolated-tls-krh8td/{result.json,native-rows.json}`, single-process
`isolated-tls-jrw1ez/{result.json,native-rows.json}`, and WS regression result
directories. The external-service checks are session evidence, not hermetic
permanent tests; the throwaway probes and certificate private key were removed.
No release was published, installed Desktop was not replaced, and macOS was not
built or exercised for this change.

## Windows runtime 0.1.40 (2026-11-12)

Built normal `Release|x64` with `python build.py --jobs 8 --package-dist`,
including the Windows ownership fixes and shared macOS `0.1.39` changes.
The final runtime ZIP was extracted to `out/verification/windows-0.1.40/runtime/`;
only its `electron.exe` was used for the final archive regression.

Runtime/metadata identity: `0.1.40`, Electron API `41.2.0`, Node `24.0.0-pre`,
ABI `134`, installer protocol `1`, `win32`/`x64`. All 53 declared file hashes
matched, with native clipboard, async-context and async-storage capabilities.
EXE SHA-256:
`7914daa9f1e9c5406880e9fdaf357034c060e027bd8e3eea7ce5f3acdf77eaa3`.

The full PDB's GUID is `903A1B48D09BAF894C4C44205044422E`, Age `1`, SHA-256
`a849bfad61b2bc4e228feaaf9d499bc48614297df2f9c45f0265c7a5a3f9e3e0`.
`llvm-pdbutil dump -summary` reports debug info, types, IDs, globals and publics,
no conflicting types, and `Is stripped: false`. The symbol store retains the
exact archived EXE/PDB pair under
`out/windows-x64/symbols/mini-electron.pdb/903A1B48D09BAF894C4C44205044422E1`.
Assembly validated the renamed distribution EXE against this pair. The final
symbols ZIP contains only the PDB and `symbols.json`; streaming the archived PDB
reproduced its hash. The installable runtime ZIP contains no symbols.

Passed against the final extracted executable:

- Runtime identity/platform/architecture/ABI/protocol/features.
- IPC frame/listener lifetime, native protocol identity/forged-header isolation,
  Range `206`/`416`, binary resources and document navigation lifetime, each in
  default sandbox and `--single-process` modes.
- Full Windows native file-dialog matrix: exact owner, Node progress, native
  open/save acceptance/cancel, destroyed-owner rejection, active and queued
  close/destroy cancellation, callback reentry and exactly-once completion.
- Pending and queued `app.exit`/`app.quit`, including exit-listener-created dialogs.
- Async-context and Promise-hook ordering, native build/heap layout, SQLite/FTS5
  and Worker regressions in Node-only and application modes.
- All six isolated broker-storage phases, including deliberately failing
  persistence with expected exit status `1`.

The four local release assets:

| Asset | Bytes | SHA-256 |
| --- | ---: | --- |
| `mini-electron-v0.1.40-win32-x64.zip` | 39,647,458 | `73c3f13c2c205361e985cf3365d20689d5c4556084b57c55a92ccb4f81d6f07f` |
| `mini-electron-v0.1.40-win32-x64-symbols.zip` | 258,990,059 | `196c6ca35ed5c7a5314503c0dbbbc698c20318e9e64e4e91d8eeccbce779bede` |
| `artifacts.json` | 528 | `0e6280e1a25714e3ae8203925997fe58c19eb7b82f5ae69108d04af7e9160eb6` |
| `SHASUMS256.txt` | 214 | `dc0d5d28d7a084672ac8f5ea429b9c4e4364a61ba1d41a8058e15982fe5a9830` |

Assets are assembled under `out/runtime-release-0.1.40/v0.1.40/`; evidence is
under `out/verification/windows-0.1.40/`. npm installer `0.1.13`, macOS binaries,
installed Desktop and the macOS `runtime-v1` channel were not changed.
Windows 11, physical human file-picker interaction and macOS were not exercised.

Published [v0.1.40](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.40)
from source `4c3d1cc47f54d33b28930dd02f742cc60215134b`. GitHub's sizes and SHA-256
digests match all four assets above. The bilingual public release body rendered
its flat lists correctly. An anonymous full runtime ZIP download reproduced its
SHA-256; anonymous PDB HEAD access returned `200` and 258,990,059 bytes.

Only after immutable assets became public, the Windows
`runtime-v1-win32-x64/artifacts.json` pointer was replaced. Its anonymous download
selects `0.1.40`, Windows x64 only, with the qualified runtime ZIP hash.
The macOS channel asset retains ID `737495293`, size 541, SHA-256
`8d960c21ee6fa7ac4c015929aa601fa05cffb23127bca99fe2d8b5fb58aa64a6`,
timestamp, title and prerelease state.

A fresh public `npm install @iriscat/mini-electron@0.1.13` used dedicated npm and
runtime caches, no local distribution override and no runtime pin. Package import
resolved its installed `dist/electron.exe`, runtime `0.1.40`/ABI `134`/Windows x64;
that EXE's SHA-256 matches the final archive. The installed CLI launched native
Node `24.0.0-pre`, ABI `134`, Windows x64. Evidence includes
`public-install-identity.json`, `anonymous-assets.json`, channel snapshots and
the uploaded/public release metadata under the verification directory.

## npm installer 0.1.13 (2026-11-12)

Assembled `out/npm-release-0.1.13/npm-package` using the existing release
assembler and the macOS `0.1.39` distribution, without `--runtime-only`.
`npm pack` produced `iriscat-mini-electron-0.1.13.tgz`: nine package files,
including `src/npm/electron.d.ts`; no native runtime or root `artifacts.json`.
Runtime release assets and channel manifests were not uploaded or replaced.

Passed:

- All 16 existing `node --test test/npm-package.test.js` regressions, including
  atomic installation, channel/version precedence, integrity failures, framework
  symlinks, CLI argv/status/signals and preservation of valid installed runtimes.
- Real installation of the packed tarball with `MINI_ELECTRON_DIST_PATH` selecting
  the already-qualified macOS `0.1.39` distribution. Package import resolved the
  executable under package-root `dist`, outside the new `src/npm/` layout.
- The packaged CLI launched that executable in Node-only mode and reported Node
  `24.0.0-pre`, ABI `134` and arm64.
- A strict TypeScript `7.0.2` consumer imported `WebFrameMain`,
  `NativeProtocolRequestIdentity` and `ProtocolFetchRequest`, accessed
  `WebContents.mainFrame`, narrowed optional native request identity and enforced
  its readonly fields. Compilation used `--noEmit --strict --module NodeNext
  --moduleResolution NodeNext --target ES2022`.

Local consumption evidence is under `out/verification/npm-0.1.13/local-install/`.

Published `@iriscat/mini-electron@0.1.13` with public access and `latest`.
The public registry integrity matches the tested tarball:
`sha512-8zLj4oxA70w9zyyZjE4dhbeS0KFponEwsNLJWnn7nyClGVeRcq9zHkdCnlGmfrRrUJ11vmWu01rcwYZWMq4PvQ==`.
Fresh public installation with no local distribution override or runtime pin
installed macOS runtime `0.1.39` through the existing channel. Package import
resolved package-root `dist`; the installed `node_modules/.bin/mini-electron`
launched the runtime and reported Node `24.0.0-pre`, ABI `134` and arm64.
Public consumption evidence is under `out/verification/npm-0.1.13/public-install/`.

## Windows source parity with macOS 0.1.39 (2026-11-12)

Fast-forwarded `main` from `8be282e56` to `155dd50c5`, then supplemented Windows
asynchronous file-dialog ownership/shutdown handling. IPC listener lifetime,
frame/document identity, protocol identity and request-header transport already
use shared sources; no duplicate Windows implementation was added.

Built normal `Release|x64` with `python build.py --jobs 8`, LLVM 22, the existing
MSVC/SDK, and the repository's GN/Ninja/CMake tools. Source-verification executable:
`out/windows-x64/mini-electron.exe`, SHA-256
`c66055e2a363da118eb9b2b34ee1fd301af27d77268086f9c48066f7951acc70`.
Application mode reported runtime `0.1.39`, Electron API `41.2.0`, Node
`24.0.0-pre`, ABI `134`, `win32`/`x64`. This is a local source candidate, not a
published Windows `0.1.39` distribution: the Windows archive/channel remains
`0.1.37`, and npm, installed Desktop and existing release assets were unchanged.

Observed against the old executable before rebuilding:

- A removed `ipcMain` listener still consumed renderer messages.
- Open-dialog requests naming a destroyed owner stayed pending instead of
  rejecting.

An intermediate ownership implementation exposed a native close deadlock.
CDB showed the UI thread joining the dialog worker while the worker remained
inside `comdlg32`'s modal loop. `IFileDialog::Close` returned `S_OK` without
dismissing that dialog during owner destruction; posting `WM_CLOSE` to the actual
owned dialog released the wait. The final implementation requests that native
closure on the dialog's STA and rechecks canceled state through file-dialog
initialization events, rather than relying on `Close` or timer polling.

Passed on the final executable:

| Scenario | Exercised result |
| --- | --- |
| `ipc-frame-smoke.js`, default sandbox and `--single-process` | Listener removal/once, frame authority/identity, detachment and targeted delivery |
| `protocol-identity-smoke.js`, both process modes | Frozen native identity, distinct windows/subframes, forged-header isolation, headers, binary resources, Range `206`/`416`, document replacement |
| `same-document-navigation-smoke.js`, both process modes | History preserves document/preload/frame identity; custom-protocol and HTTP reload replace it |
| `windows-owned-dialog-smoke.js`, full matrix | Exact owner HWND, Node progress, open/save native acceptance/cancel, active and queued owner close/destroy, destroyed-owner rejection, once-only and reentrant completion, ownerless dialog |
| Dialog launcher shutdown phases | Pending and queued `app.exit`/`app.quit` exit successfully, including a dialog created by an exit listener |
| External Win32 synchronous open/save smoke | Both actual dialogs cancel and return `undefined`; Node timers do not run before the synchronous call returns |
| `broker-storage-persistence-smoke.js`, all six phases | Ordering, no-op suppression, flush/restart/quit/exit durability; deliberate persistence failure retains expected exit code `1` |

Evidence is under `out/verification/windows-mac-parity/`: final frame/navigation
results, `protocol-final-*`, `dialogs-verified`, `sync-dialogs/final-result.json`,
`storage-final`, candidate identity and before/after/close-diagnostic records.
The isolated launchers preserve Windows known-folder environment variables;
overriding them made `SHGetFolderPath` fail before main. They redirect native
paths before sessions/windows and remove only the fresh empty startup namespace.
The native helper recognizes both classic-open and DirectUI-save filename
controls, validates fixture PID/HWND/title, and checks the returned Unicode path.

No Windows release packaging/publishing, Windows 11 qualification, physical
human file-picker interaction or macOS rerun was performed.

## macOS runtime 0.1.39 (2026-11-12)

The plugin runtime changes in `adca57273` were fast-forwarded into `main`.
Built with `/opt/homebrew/bin/python3.13 build.py --jobs 12 --package-dist`
and assembled with `scripts/build_runtime_release.py --macos-dist
out/mac-arm64/runtime-dist --runtime-only --output out/runtime-release-mac-0.1.39`.
The final ZIP was extracted with `ditto` to `out/verification/mac-0.1.39/runtime/`.

Runtime ZIP SHA-256:
`edfc61df3687388a83d322429d8d9f95941c2a7ade646a657ea0bdb95855cdbe`.
The extracted app passed `codesign --verify --deep --strict`; `file` identified
Mach-O arm64. Node-only startup reported Node `24.0.0-pre`, ABI `134` and arm64.
Application startup reported runtime `0.1.39`, Electron `41.2.0` and ABI `134`.

Passed using the extracted executable and dedicated HOME/user-data fixtures:

- Complete `ipc-frame-smoke.js`: listener removal/once, frame authorization,
  stable identity, navigation detachment and target-only delivery.
- Complete `protocol-identity-smoke.js`: readonly identity across windows
  and subframes, forged-header isolation, transported headers, Range `206`/`416`,
  binary resources and document replacement.
- Focused real AppKit open/save sheets: exact native owner, options, Node timer
  progress, owner movement, close/destroy cancellation, exactly one completion,
  native dismissal, close/destroy before queued presentation and invalid-owner
  rejection. The existing native observer only read panel state; no synthetic
  acceptance, Escape or watchdog dismissal was used.
- `macos-owned-dialog-smoke.js --case=exit-pending` and `--case=quit-pending`
  passed with a visible standalone panel before shutdown, zero late callbacks
  and zero remaining windows. Their observations/logs are copied under
  `out/verification/mac-0.1.39/{exit-pending,quit-pending}/`.

Evidence: `out/verification/mac-0.1.39/smoke-results.json`, per-scenario
`result.json`/logs and `dialog-smoke/results.json`. The first launcher inherited
`ELECTRON_RUN_AS_NODE=1` from the agent host and never entered application mode;
the passing runs explicitly removed it from the child environment.

Published [v0.1.39](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.39)
from source `155dd50c5`. The three immutable assets are publicly available;
the macOS `runtime-v1` manifest selects `0.1.39` with the ZIP hash above and
retains its existing release title, notes and prerelease label.
An isolated public `npm install @iriscat/mini-electron@0.1.12` pinned to
runtime `0.1.39` downloaded and installed the release through the existing
archive/inventory verification path. The installed executable reported Node
`24.0.0-pre`, ABI `134` and arm64. An explicit `--update-runtime` with no version
pin also installed `0.1.39` through the normal channel URL. Installed identity
is recorded in `out/verification/mac-0.1.39/public-install-identity.json`.

The existing [dialog qualification limits](MACOS.md) remain: real file acceptance,
Escape delivery and remote-panel opacity are unqualified. The older full cursor
failure was not rerun. Developer ID signing, notarization and a multi-hour soak
were not run. Windows and npm were not published by this runtime-only release.

## macOS runtime 0.1.38 (2026-11-10)

Built using `/opt/homebrew/bin/python3.13 build.py --jobs 12 --package-dist`
from shared source `8be282e56` plus the macOS-only runtime version update.
The system `python3` is 3.9.6 and lacks `hashlib.file_digest`; the documented
Python 3.12+ prerequisite is satisfied by Python 3.13.

Release assembly used `scripts/build_runtime_release.py --macos-dist
out/mac-arm64/runtime-dist --runtime-only --output
out/runtime-release-mac-0.1.38`. Its final ZIP was extracted with `ditto` to
`out/verification/mac-0.1.38/runtime/`; every scenario below uses that extracted
executable. `file` identifies Mach-O arm64, and `codesign --verify --deep
--strict` passed for the app and bundled frameworks.

Runtime ZIP SHA-256:
`75eec013df8448e94f457e4806862dd7281ff1d9c1d904a7874e7144e3c0648f`.
Installer protocol `1`, Electron API `41.2.0`, Node `24.0.0-pre`, Node ABI
`134`, and Chromium major `132` are unchanged. Windows runtime/channel remains
`0.1.37`; npm installer remains `0.1.12`.

Published [v0.1.38](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.38)
from source `79ed2d3d8f8e1dcb83f5496650fbbd1b4cfae800`. All three immutable
assets are uploaded. Anonymous downloads of the ZIP, version manifest and
`SHASUMS256.txt` succeeded; the downloaded ZIP's SHA-256 matches the value above.
The public macOS `runtime-v1/artifacts.json` now selects `0.1.38` and the same
archive hash. Only that channel's manifest was replaced; its prerelease label
and notes remain unchanged. Windows runtime/channel remains `0.1.37`.

Passed:

- Application runtime identity `0.1.38`/Electron `41.2.0`/ABI `134`, and
  Node-only arm64/ABI identity. Node-only mode intentionally has no Electron
  version fields; the initial probe's contrary assertion was corrected.
- `node-async-context-smoke.js` in application and Node-only modes, including
  Workers, foreign Promise realms, nested scopes and once-ordered Promise hooks.
- `node-build-config-smoke.js` in both modes and Workers: native tagged slots
  measured 4 bytes; pointer-compression/31-bit-SMI/sandbox flags were `1/1/0`.
- `node-sqlite-smoke.js` in both modes, including forwarding builtin hooks,
  FTS5 MATCH/ranking/update/delete/reopen, transactions, WAL, backup and Workers.
- `macos-storage-mutation-smoke.js`: repeated set/delete/clear acknowledgements
  and reload retain the final value without a renderer crash.
- `macos-exit-lifecycle-smoke.js`: native menus and preload-backed windows exit.
- All six broker-storage phases: write, restart, exit-write, exit-read,
  quit-read, and exit-error with expected status `1`. The temporary copy changed
  only the Windows platform assertion to macOS; all behavioral checks remained.
  This covers no-op file timestamps, existing-format loading, origin/partition
  isolation, persistence failures/recovery, clearing and shutdown durability.

Evidence: per-scenario JSON under `out/verification/mac-0.1.38/`, with broker
results under `mini-broker-storage-O6QdCj/`. Only the external `node` driver's
actual six-phase result counts; an earlier invocation incorrectly used the
Desktop host executable and did not run the storage driver.

The complete `macos-cursor-smoke.js` did not pass: its pressed-drag assertion
observed `default` rather than `grabbing`. A diagnostic later observed `grab`
and recorded injected `pointerdown`/`mousedown` with buttons `1`, followed by
`pointerup`/`mouseup`, `lostpointercapture`, and real no-button mouse moves.
The final DOM had no held class/capture and CSS `grab`. Native injection does
not hold the system mouse button; ordinary movement reconciles the capture
owner from AppKit's actual pressed buttons. This result does not qualify the
full physical drag/cursor matrix. The captured native fixture rendered correctly
in `cursor-diagnostic.png`; no cursor behavior was changed to bypass the check.

Developer ID signing, notarization and a multi-hour desktop soak were not run.


## Windows runtime 0.1.37 — broker storage responsiveness

Built and qualified `out/windows-x64/runtime-dist` with runtime identity
`0.1.37` from the repaired local source checkout, using the normal release
configuration. No installed Desktop was replaced.
EXE SHA-256:
`d47e87c38500a928960109551f2db73ce73b7c39276d6cd2d969e9b2ac720578`.
The distribution declares `asyncStoragePersistence: 1` alongside the existing
clipboard and async-context capabilities, only after staged executable checks.

The matching PDB has GUID `D87D19E62F0962DF4C4C44205044422E`, Age `1`,
and SHA-256
`023a057c12153270503fa01dfc48796f2c14380ef3422106ff5d2f79857f3aea`.
`llvm-pdbutil dump -summary` reports debug information, types, IDs, globals and
publics, no conflicting types, and `Is stripped: false`.

Published [v0.1.37](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.37)
from source `2845171129b1188a63368eaf7362822ae1ae3ecb`.
The exact saved bilingual notes were reopened and the public rendered body has
12 flat list items and no nested lists. GitHub's uploaded sizes and SHA-256
digests match these local files:

| Asset | Bytes | SHA-256 |
| --- | ---: | --- |
| `mini-electron-v0.1.37-win32-x64.zip` | 39,639,810 | `e462daf33fe2f7f15517f60b0a859fc4e8fbd5bcba1e959b61b74930f1874cac` |
| `mini-electron-v0.1.37-win32-x64-symbols.zip` | 258,934,311 | `3a38fe3935d44c050e9633794b168904ccdd754c80bb0799910deec2381aa7eb` |
| `artifacts.json` | 528 | `e15872c494f5aadd898f0b76bd3d7734edacad7328056dfb3d1798192de3bd0a` |
| `SHASUMS256.txt` | 214 | `c2b13461b37547313bd9f6ca45751ab77a621b79977af032235983d90b60d72a` |

The Windows channel manifest was updated only after immutable assets were
published. Its anonymous download selects `0.1.37` and the matching runtime ZIP
hash. Anonymous HEAD access to the PDB asset returns `200` and 258,934,311 bytes.
The macOS channel's asset ID `698404550`, size and SHA-256 remain unchanged.
The Desktop checkout's explicit public install,
`npx mini-electron --update-runtime 0.1.37`, completed successfully.


- The real native async-context probe passed during distribution staging.
- All six isolated broker-storage phases passed: write, restart, exit-write,
  exit-read, quit-read, and the deliberately failing exit-error phase (expected
  status `1`). These cover existing-format loading, no-op file timestamps,
  origin/partition isolation, quota rejection, set/remove/clear ordering,
  persistence errors and recovery, and retained-session shutdown draining.
- The regression exposed a renderer AV while clearing nonempty storage:
  Blink had replaced its cache before the backend used a borrowed map pointer.
  The obsolete registration bridge is removed; ordered clear acknowledgements
  now use broker-owned `wasNonempty`, without dereferencing a renderer cache.

An additional visible standalone native window used a fresh owned profile,
synthetic data, no daemon, and 32 calls per case: 40 characters per value,
waiting 200 ms after each call. All times below are milliseconds:

| Case | Median | P95 | Maximum | Maximum rAF interval |
| --- | ---: | ---: | ---: | ---: |
| Small persistent store, changed value | 0.2 | 0.4 | 0.5 | 13.9 |
| Small persistent store, same value | 0.2 | 0.3 | 0.4 | 13.3 |
| Additional 500,000-character synthetic cache, changed value | 0.2 | 0.4 | 0.6 | 8.2 |

No case recorded an rAF interval above 50 ms. Explicit flush and disk reads
verified the complete latest draft and 500,000 cache characters. The small
store was 88 bytes; the larger file was 500,109 bytes.

With continued 200 ms writes during eight native maximize/unmaximize
transitions, all final renderer dimensions matched the native content size.
Operation-before-renderer-resize timestamps ranged 17.6–37.9 ms; adding two
rAFs ranged 23.6–61.4 ms. The concurrent writes had median 0.2 ms and maximum
32.1 ms, so this is not a claim that every write stays below a frame during a
window transition. The final concurrent value was also flushed and checked.

Evidence in the sibling Desktop checkout:
`.tmp/issue-139-fixed-kernel/result.json`, `maximized.png`, and `restored.png`.
Staging's native regression evidence is under
`%TEMP%/mini-broker-storage-v5spd9/`, with per-phase JSON records.
Renderer events and `capturePage()` do not prove DWM presentation latency.
No long-session Windows 11 soak, forced-power-loss guarantee, or macOS
build/runtime verification is implied.

## Windows runtime 0.1.36 (2026-11-07)

Status: normally built, assembled and
[published as v0.1.36](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.36)
from source `a9c594ee163a27cafdad83e2e3f846e425bdd345`. All four GitHub asset
sizes and SHA-256 digests match the local release. The Windows channel now
selects `0.1.36`; anonymous runtime and channel-manifest downloads were checked.
The macOS channel's asset ID, digest, size and update time are unchanged.
The final runtime ZIP was extracted into a fresh directory and all 53 declared
file hashes were checked before execution.

- **16/16 final-artifact scenarios passed**, with isolated HOME, profile,
  application-data and temporary directories. GUI scenarios ran serially.
- Runtime identity reported `0.1.36`, Electron API `41.2.0`, Node
  `24.0.0-pre`, Node ABI `134` and Chromium major `132`.
- Async-context propagation, actual V8 heap layout/configuration and SQLite
  FTS5/forwarding CommonJS hooks passed in both Node and application modes.
- Hardware composition passed in independent-renderer and single-process modes;
  software composition and mixed GPU/CPU guests also passed. These exercised
  native desktop pixels, alpha, capture, resize, teardown and real hardware VSync.
- Unset, zero and invalid snapshot settings retained responsive renderer
  JavaScript with no heap snapshot. Explicit opt-in produced a genuine,
  once-only V8 heap graph; fatal renderer OOM still produced a symbol-identifiable
  exception dump with the production default.
- Release assembly tests: 21 passed, one skipped. Windows symbol tests:
  three passed. Final symbols ZIP contains only the full PDB and `symbols.json`;
  runtime ZIP contains no PDB. EXE RSDS and independent LLVM PDB inspection agree,
  with debug info, types, IDs, globals and publics present and stripping disabled.

Final asset SHA-256:

- Runtime ZIP (`39,632,780` bytes):
  `7ce2fdd8a814cdd590cbbbfb729b82a14bba56d4107a2c5056a0885c3ae4e218`.
- Symbols ZIP (`258,867,775` bytes):
  `ec1a7985ff73e487b701b9e309b1e5b5272f1ed3f78b9620890b87466e2e0e11`.
- EXE: `028f01ecef9926706c2568e7519dc9fef0eac5909b97dea8f88685f4fe3ebf83`.
- PDB: `59ef63eff062f7512ed7d22494faae9b0a63437d4f542c7051cbb2906e48b967`.
- PDB GUID `4059B752CC9D58CA4C4C44205044422E`, Age `1`.

Evidence: `out/verification/windows-0.1.36/runtime-checks.json`, per-scenario
logs and PNGs, and `llvm-pdb-summary.log`. npm installer and macOS were not
rebuilt or published. The [long-session investigation limits](#windows-long-session-responsiveness-2026-11-07-source-build)
still apply; these focused runs do not substitute for a multi-hour Desktop soak
or GPU fault injection. No installed Desktop was replaced.

## Windows long-session responsiveness (2026-11-07, source build)

Built the normal Windows x64 runtime with the existing cached GN, Ninja, CMake,
ANGLE and Dawn toolchains, four jobs, and no diagnostic compiler flags.
`out/windows-x64/mini-electron.exe` SHA-256:
`e6d6911539a3806dbed625ea1fd4ec4bedbdea9e3d522b32175689967a252eb4`.
The installed OMP Desktop, published runtime assets and channels were not changed.

- Replaced blocking producer-fence and first-present worker tasks with
  cancellable `ObjectWatcher` notifications. Explicit readback defers on an
  incomplete external fence instead of blocking Viz, and fails after a
  two-second retry deadline without treating that texture as released.
- Automatic pre-OOM heap snapshots are disabled unless the trusted host
  explicitly supplies a threshold from 1 through 65. V8/Job pressure sampling
  and crash minidumps remain enabled.
- Real hardware compositor smoke passed in independent-renderer and
  `--single-process` modes on Windows 10 x64 / NVIDIA RTX 3090. Native desktop
  pixels, transparent alpha, BrowserViews, six active-view teardowns, resize,
  minimize/restore, captions, explicit captures and active hardware VSync were
  exercised. Software composition also passed.
- The real webview guest smoke passed GPU/GPU, GPU/CPU, CPU/GPU and CPU/CPU
  combinations, including simultaneous crops, capture, movement and detach.
- Fresh isolated runtime probes passed unset, explicit-zero and invalid
  snapshot configuration. Disabled cases retained usable JavaScript and
  produced no heap snapshot. The existing positive-threshold smoke parsed a
  real once-only V8 heap graph; the existing renderer OOM smoke still produced
  a symbol-identifiable exception dump with the new production default.

An initial single-process smoke rejected `source: unknown` before the first
clock tick. That state is explicitly declared by `WindowsVSyncInfo`; the
generic startup-source assertion was removed, not re-pinned to another initial
value. The animation phase still requires an actual DXGI/DWM clock, increasing
ticks and a bounded tick rate. Its serial rerun passed. Graphical smoke runs
must not contend for the foreground window.

Native PNG evidence is retained under `out/verification/long-run/gpu`,
`gpu-single-process` and `software`. In the associated Desktop checkout,
269 focused server tests, server typecheck and server library build passed.
A real 6,000-file rename/delete workload verified every expected path and empty
post-unsubscribe inventory; its 10 ms heartbeat's maximum observed gap was
34.8 ms. `out/verification/long-run/observer-result.json` records those counters.

Limits: no multi-hour Desktop soak, stuck-fence/device-loss fault injection,
electrical power measurement or macOS build was performed. The deadline bounds
the external-ownership retry, not arbitrary driver readback or healthy-device
teardown. This work does not establish the cause of a previously unrecorded UI
freeze or impose a hard memory limit on live conversation state.

## macOS lost mouse-release recovery (2026-11-05)

The source runtime rebuild and local runtime-distribution build passed. AppKit
ordinary mouse moves now reconcile the native held-button mask even for the
capture owner. Renderer guest routing also clears its held-button/capture cache
after forwarding a no-button move to the former owner. Ordered down/drag/up
transitions remain unchanged.

Before the native repair, the extended `macos-cursor-smoke.js` failed because a
no-button move still reached the captured drag target with `buttons=1`. The
native-only repair exposed the second cache: lost right/middle releases kept
later input routed away from a newly attached webview. The final repair covers
both layers; the regression retains those subsequent guest-input assertions.

A throwaway owned-AppKit-input smoke ran against
`out/mac-arm64/pointer-recovery/runtime-dist/Electron.app/Contents/MacOS/Electron`.
All nine combinations of window/BrowserView/webview and left/right/middle
lost releases passed: the held-button snapshot was retained during dragging,
the no-button move released DOM capture, and the first subsequent click reached
the other control. This used process-owned NSViews without global mouse or
keyboard injection.

Desktop `npm run build:mac`, with `MINI_ELECTRON_DIST_PATH` selecting that local
distribution, produced the arm64 `OMP Desktop.app`, DMG and ZIP under Desktop's
`packages/desktop/release/`. The `OMP Desktop Local` signature passed strict/deep
verification; notarization was not performed. The same nine owned-input cases
also passed against the final branded application executable. Launching the
real bundle without `--app`, with an isolated profile/home, created its main
window and built-in daemon without model requests. Window screenshot capture
was unavailable while the console was locked. The installed `/Applications`
bundle was not replaced.

The final unlocked-console cursor suite stopped at its console-lock
prerequisite after the system locked. The owned-input smoke does not establish
physical cursor/raster observations or a packaged Desktop visual acceptance
run. This is an unpublished local runtime build; no runtime channel was updated.

## Windows runtime 0.1.35 (2026-11-05)

Status: normally built, packaged, independently symbol-verified and
[published as v0.1.35](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.35),
with all four asset digests verified on GitHub.
The Windows manifest/archive and native runtime version are `0.1.35`; npm
installer `0.1.12`, installer protocol `1`, Electron API `41.2.0`, Node ABI `134`,
the macOS manifest entry and both channel names are unchanged.
The published macOS channel continues to serve `0.1.31`.

### Application-entry SQLite diagnosis

The previously recorded official `0.1.34` and FTS5 development application
failures both report builtin loading through `lib/browser/init.js:44`, with
`ERR_INVALID_RETURN_PROPERTY_VALUE` because the load hook received `null` source.
This is not evidence that SQLite executed or that application-mode FTS5 passed.
The official executable SHA256 is
`f11e9761e05bcb9bd0ce5e944ccf4235f5b353ea79e4da3c9e3f8ecc669e62b4`;
the development executable SHA256 recorded with the failure is
`96ffb52375a84b7492737b77dafca371c3b9edf684312489b5fcfef7ae30c20d`.

A minimal diagnosis used only that existing development EXE in Node-only mode,
with empty `NODE_OPTIONS`/`NODE_PATH` and throwaway home/profile/temp directories.
It registered ordinary forwarding `resolve`/`load` hooks and required
`node:fs` followed by `node:sqlite`, without changing either hook's result:

```text
node:fs     -> resolve URL node:fs -> load format builtin -> required
node:sqlite -> resolve URL sqlite  -> load format builtin -> ERR_INVALID_RETURN_PROPERTY_VALUE
```

The cause is `resolveForCJSWithHooks()` in the canonical Node CommonJS loader:
`defaultResolveImpl()` correctly recognizes `node:sqlite` and returns normalized
filename `sqlite`, but the hook default resolver reconverts that filename and
overwrites the valid `node:sqlite` URL. `sqlite` is deliberately excluded from
unprefixed builtin names, so conversion leaves it bare. The default builtin
loader correctly returns `null`; `validateLoad()` correctly rejects that source
for the now non-`node:` URL. The decoded checked-in js2c resource contains the
same resolution and validation logic, excluding stale embedded script contents
as the cause. This also explains why the hook-free Node-only SQLite entry passed.

The narrow source repair preserves an explicitly requested `node:` URL after
normal builtin validation. It does not change builtin visibility, provide fake
source, relax source validation or skip hooks. The existing source-to-js2c
refresh now includes the CommonJS loader so the next native build embeds it.
The SQLite smoke adds a forwarding-hook regression and confirms unprefixed
`require('sqlite')` is not accepted as a builtin; its application route now uses
the same readiness/exit convention as the other Node consumer smokes.

### Final-archive verification

`out/verification/windows-0.1.34-merged/runtime-checks.json` supplied the exact
fourteen historical executable/argument arrays. Its SQLite case was Node-only,
not `--app=...`; it did not establish application-entry SQLite coverage.
The normal `0.1.35` release was assembled before these checks. Only
`out/verification/windows-0.1.35/extracted-final/electron.exe`, extracted from
the final runtime ZIP, was used for the new fifteen-case run:

| Scenario | Result |
| --- | --- |
| Runtime identity/platform/architecture/ABI/protocol/features | PASS |
| Async-context Node-only | PASS |
| Async-context application | PASS |
| Native build configuration/heap layout/Workers Node-only | PASS |
| Native build configuration/heap layout/Workers application | PASS |
| SQLite Node-only | PASS |
| SQLite application (additional fifteenth case) | PASS |
| Fresh Desktop core SDK `18.14.0` artifact/Eval | PASS |
| Windows native input sandbox | PASS |
| Windows native input single-process | PASS |
| Windows cursor/capture sandbox | PASS |
| Windows cursor/capture single-process | PASS |
| Public clipboard image | PASS |
| Console forwarding sandbox | PASS |
| Console forwarding single-process | PASS |

All fifteen processes exited `0`. Both SQLite entries passed the unchanged
forwarding CJS hook assertions (`node:sqlite`, builtin `null` source and rejected
bare `sqlite`) and native SQLite `3.49.1` FTS5 MATCH/phrase/BM25/rank,
update/delete, read-only durable reopen and Worker checks, together with the
existing transaction, binding, backup, changeset and extension restrictions.
Both configuration entries measured four-byte tagged slots and flags `1/1/0`
for pointer compression/31-bit Smis/sandbox; this does not remove the 4 GiB cage.
The SDK probe used the freshly rebuilt current
`D:/Project/omp-desktop/packages/desktop/sdk-dist/node_modules/@oh-my-pi/pi-coding-agent`
stage, version `18.14.0`, and a new `sdk-artifact` output directory. Real Eval
passed sync/await output, final object expression and explicit error delivery;
no paid model request was made.

Node-only cases explicitly set `ELECTRON_RUN_AS_NODE=1`; application cases
removed it. Every case isolated HOME, USERPROFILE, APPDATA, LOCALAPPDATA, TEMP,
TMP, OMP_AGENT_DIR, PI_CODING_AGENT_DIR and PASEO_HOME and removed NODE_OPTIONS
and NODE_PATH. GUI cases ran serially with per-case MINI_NATIVE_INPUT_ARTIFACTS;
the native scripts completed their normal pointer/clipboard restoration.
An initial harness layout used independent profile and AppData directories:
Win32 `SHGetFolderPathW(CSIDL_APPDATA)` returned `0x80070003`, so application
startup failed before the smoke scripts loaded. Fresh-process diagnosis and a
conventional private `USERPROFILE/AppData/Roaming` and `Local` layout resolved
that harness issue without changing runtime code or weakening assertions.
The initial logs and diagnosis remain beside the successful final results.

All **53** `version.json.files` SHA256 entries matched the extracted files.
Metadata and live identity agreed on runtime `0.1.35`, `win32`, `x64`, ABI `134`,
installer protocol `1`, Electron API `41.2.0` and
`features.asyncContextPropagation: 1` (also `nativeClipboardFiles: 1`).
The Windows-only schema-v2 `artifacts.json` and both SHASUMS entries matched
the exact final assets:

| Asset | Bytes | SHA256 |
| --- | ---: | --- |
| `mini-electron-v0.1.35-win32-x64.zip` | 39,633,967 | `ff36c793ba04bfe8439f36811320f5f2b657674a6a70f7bb49720b3a64e942d8` |
| `mini-electron-v0.1.35-win32-x64-symbols.zip` | 258,897,224 | `87fef7f29cce48344999b6441a34347026609ff48d2d3ceba841fe9734b7973f` |
| `artifacts.json` | 528 | `bd229751958d723a901732c2e6a210d7231efedb1966c3ede43738ca1322b4e2` |
| `SHASUMS256.txt` | 214 | `e37369815d634bdeef15ddb764b7478f79a11edcfd1e9b3245ed8476ecb388fa` |

The extracted EXE is 100,568,576 bytes, SHA256
`ed0327bf7a62c392ae1b55f51fb2ae144d4a921e241d0d17cc1a8140ccb85f3e`.
Its parsed PE RSDS record independently matches the extracted full PDB:
GUID `59E0A05A338C1A424C4C44205044422E`, Age `1`, PDB name
`mini-electron.pdb`. The PDB is 1,308,106,752 bytes, SHA256
`9555ce59002166f9dac562aaf4d2beabc1fcb400cde890497e2231748f988185`.
LLVM `llvm-pdbutil dump -summary` independently reports that GUID/Age, debug
info/types/IDs/globals/publics present, `Is stripped: false`, no incremental
linking and no conflicting types. PDB and EXE hashes also match the exact leaf
`out/windows-x64/symbols/mini-electron.pdb/59E0A05A338C1A424C4C44205044422E1`.
The symbols ZIP contains exactly `mini-electron.pdb` and `symbols.json`, no EXE;
the runtime ZIP contains no PDB. Symbol metadata hashes and identity matched.

Evidence retained under `out/verification/windows-0.1.35/`:
`runtime-checks.json` with all commands/exit codes/timings and per-case logs,
`asset-checks.json`, `llvm-pdb-summary.log`, `profile-layout-diagnosis.json`,
`initial-profile-layout/`, per-case native-input success PNGs and cursor JSON,
the new SDK artifact output, and `extracted-final/` plus `symbols-final/`.
Throwaway runner/identity scripts and private profiles were removed.

Boundaries: the execution evidence is local Windows x64 verification. The
published runtime and matching PDB do not establish an installed Desktop update,
macOS build or macOS SQLite/js2c repair. Existing Desktop, its daemon,
configuration, credentials and frozen `0.1.34` installation packages were not
used or modified. The prior `0.1.34`
artifacts are not relabeled as FTS5-capable. No native rebuild or assertion
relaxation was performed during final-archive verification.

## macOS source capture repair (2026-11-04)

Built the current arm64 source with Homebrew Python 3.14 and four workers.
The application and `ELECTRON_RUN_AS_NODE=1` entries both passed
`test/node-build-config-smoke.js`, including Workers: pointer compression `1`,
31-bit Smis `1`, sandbox `0`, and measured tagged-slot width four bytes.
This corrects configuration reporting; it does not enlarge the V8 cage.

The unlocked-console native regressions also passed:
- `test/macos-cursor-smoke.js`: real AppKit cursor/raster observations,
  BrowserView non-owner release protection, guest buttons
  `1 → 3 → 7 → 6 → 4 → 0`, out-of-bounds middle-button dragging, and exactly
  one final `pointerup`.
- `test/macos-pointer-screen-coordinates-smoke.js`: native mouse/wheel desktop
  DIPs, negative display positions, moved windows, BrowserView offsets and
  stationary captured input.
- `test/macos-precise-wheel-smoke.js`: both axes/directions, standard/legacy
  DOM deltas, one-pixel input and ordinary mouse-wheel compatibility.
- `test/mac-pre-oom-snapshot-smoke.js` (capture case): a real owning-renderer
  heap graph, restricted local sidecar, once-only capture and frozen exit
  metadata. The existing low diagnostic threshold safely triggered capture;
  the test's signal exit was not reported as V8 OOM and produced no fake dump.

The merged source build also passed `test/node-async-context-smoke.js` on macOS
arm64 in application and Node-only modes, including default/frame-based ALS
children, Workers, foreign Promise realms and once-only Promise hook ordering.
This post-merge run is not Windows verification.

The cursor smoke retained a compositor PNG and native JSON observations.
Only new test processes/profiles were used; installed applications and
runtime channels were unchanged. Windows was not run for this source repair.

## Windows runtime 0.1.34 (2026-11-04)

Windows x64-only normal `Release|x64` rebuild of the async-context repair in
`79af817edffa0a96678e8c6854069f22f726008b`, integrated with remote main
`679cd8e2c` and its platform-owned capture repair before the final rebuild.
Four workers and the existing LLVM/ANGLE/Dawn tool cache were used, without
diagnostic compiler flags or a default ALS implementation change. npm
installer `0.1.12`, installer protocol `1`, Electron API `41.2.0`, Node ABI
`134` and the macOS runtime/channel `0.1.31` are unchanged.

The exact executable extracted from the assembled runtime ZIP passed fourteen
real-runtime scenarios:
- Runtime/application version, Electron API, Node ABI and capability identity.
- `node-async-context-smoke.js` in Node-only and application modes, including
  default/frame-based ALS children, Workers, foreign realms, concurrent/nested
  scopes, microtasks, timers, bound callbacks, rejection and single ordered
  Promise notifications.
- `node-build-config-smoke.js` in both entry modes with native heap-layout,
  frozen configuration and Worker consistency checks.
- Native SQLite transaction/binding, rollback, lock isolation, WAL and Worker
  checks.
- Real Desktop core SDK artifact smoke, including native I/O, SQLite, image
  handling, Koffi, shared workers, synchronous/post-await Eval output, object
  final expressions and explicit errors.
- Complete native input in sandboxed and explicit single-process modes.
- Real native cursor resources, RGBA/hotspots, stationary updates, chrome,
  inactive windows, BrowserView and chorded guest capture in sandboxed and
  explicit single-process modes.
- Public clipboard image format/pixel/fallback/rejection checks.
- Console forwarding in sandboxed and explicit single-process modes.

Release-assembly regressions ran 22 cases (21 passed, one skipped); all three
Windows symbol regressions passed. Assembly and extracted-ZIP validation
checked all runtime file hashes. The symbols ZIP contains only
`mini-electron.pdb` and `symbols.json`; its decompressed PDB stream hash matched
the archived production pair. Independent LLVM tools reported identical
EXE/PDB GUID/Age, debug/type/global/public records, no incremental linking
and an unstripped PDB.

Executable SHA256:
`f11e9761e05bcb9bd0ce5e944ccf4235f5b353ea79e4da3c9e3f8ecc669e62b4`.
PDB GUID `B6FD71D98B49FC954C4C44205044422E`, Age `1`; PDB SHA256:
`a3c0dc93b7bb9561f87bc0bd4f9b5b3cc69da4f43bb1f5835dd0f84a3cb42e64`.
Runtime ZIP: 39,553,485 bytes, SHA256
`3bf107154eae8d9dba38254f1ca6c2fa49b176405cdc341a10f9b7baa051c9f5`.
Symbols ZIP: 258,737,502 bytes, SHA256
`ad04c002af2a9587ab89e66099f72413e56a800d67759d83c7d6edd5d611d34a`.

Distribution: `out/windows-x64/runtime-dist-0.1.34-merged/`.
Immutable assembly: `out/runtime-release-0.1.34-merged/v0.1.34/`.
Logs/results/native screenshots:
`out/verification/windows-0.1.34-merged/`.
The tests used isolated profiles and saved/restored native clipboard/pointer
state. No installed Desktop or user profile was replaced. Original Windows 11
Desktop sessions, the real Figma tool catalog, physical mixed-DPI transitions
and a full-day soak were not exercised.

### Publication and public installation

Published [Windows runtime `v0.1.34`](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.34)
as the latest stable release at source commit
`9f0ce3033d30aec3533c10d5dc945db187e642b7`. All four GitHub assets were in the
uploaded state and their server-reported sizes/SHA256 digests matched the
local assembly. The saved/reopened bilingual notes matched exactly after
GitHub form newline normalization; the live body had 22 flat items and no
nested lists.

Two fresh projects installed the registry-published npm installer `0.1.12`
with its published SHA512 integrity, separate empty npm/runtime caches and
no local distribution, mirror or offline override. The first explicitly
pinned `0.1.34`; after that passed, only the Windows mutable channel's
`artifacts.json` was replaced, and the second installed without a pin.
Both downloaded executable hashes matched the final EXE above and passed
six runtime scenarios each: identity, ALS and native V8 layout in both entry
modes, plus SQLite.

The Windows channel remains a pre-release with the same release ID and
target commit. Its new manifest SHA256 is
`9d26ba42043e57ac52aea5a94f05409bb7c514152e7b761a6abbfa7b1004b39a`.
The macOS channel's release settings, asset ID `698404550`, asset update
timestamp, size and SHA256 digest remained unchanged; it still serves
runtime `0.1.31`. Existing valid npm installations still require an explicit
runtime update.

Public installation logs and per-case results are under
`out/verification/windows-0.1.34/public-{pinned,channel}/`;
GitHub release/tag/asset/channel snapshots are under
`out/verification/windows-0.1.34-merged/`. No installed Desktop was replaced.


## Windows async context source repair (2026-11-04)

Built `Release|x64` from the repaired source and staged it in
`out/async-context-verification/runtime-dist`. The actual `electron.exe` SHA-256:

```text
5185671fb5444b6c5faf4cfe1f13b94b49772c3bbd3c034f5cba1ac5506dee13
```

`test/node-async-context-smoke.js` passed in application and Node-only modes
against this binary, including default/frame-based ALS children and Workers. Concurrent/nested
scopes, foreign Promise realms, microtasks, timers, bound callbacks and rejection
retained the correct store. The explicit Promise continuation produced one
init/before/resolve/after sequence; the probe caught and blocked an intermediate
build with duplicate resolve notifications.

The same binary passed Desktop's runtime async-context probe and the real
Desktop core SDK artifact smoke: native read/write, SQLite, image handling,
Koffi, shared workers, synchronous/post-await Eval output, object final
expression, and explicit errors. Native CLI `browser-relay` status/serve on an
isolated loopback port distinguished an absent service from a serving relay
without an extension, then reused that service without replacing it.

The staged metadata includes `asyncContextPropagation: 1`. Runtime version
remains `0.1.33` for this unpublished local artifact; the hash above, not the
version, identifies the repaired binary. No installed Desktop was replaced,
no GitHub runtime assets were published, and Windows 11, a real Chrome extension
handshake, and the real Figma tool catalog were not exercised.


## Windows runtime 0.1.33 (2026-11-04)

Windows x64-only runtime update carrying brokered native clipboard files and
the native V8 build-configuration reporting correction. npm installer `0.1.12`,
installer protocol `1`, Electron API `41.2.0` and Node ABI `134` are unchanged;
the macOS runtime/channel remains `0.1.31`.

Built the normal `Release|x64` distribution with four workers and the existing
local ANGLE/Dawn/tool cache, without diagnostic compiler flags. GN, Ninja and
CMake were explicitly selected from `.build-tools`; initial attempts lacking
their tool paths stopped before a complete native build.

The final packaged executable passed nine runtime scenarios:
- Application identity: native runtime `0.1.33`, Electron API `41.2.0`, ABI `134`
  and `features.nativeClipboardFiles: 1`.
- `node-build-config-smoke.js` in application and Node-only modes, including
  Workers and cross-entry consistency. Native tagged slots measured four bytes;
  compression/31-bit-SMI/sandbox reported `1/1/0`; `process.config` stayed deeply
  frozen. This does not remove V8's compressed-heap 4 GiB cage.
- Native SQLite, including transaction state, mixed numbered bindings,
  savepoints, rollback recovery, writer-lock isolation, WAL and Workers.
- Complete native-input checks in sandboxed and explicit single-process modes:
  Unicode/rich/empty text, PNG/DIB/DIBV5, single/mixed Explorer-file paste,
  canceled and nested/script authorization, child-frame focus, File lifetime,
  native private-byte retention, keyboard policy, OLE and rejection boundaries.
  Real ancestor-reparse races returned only authorized bytes or rejection.
  Private-byte samples were `326053888 -> 326057984` in sandboxed mode and
  `365924352 -> 360464384` in single-process mode.
- Public `clipboard.readImage()` format/fallback/pixel/rejection checks.
- Console-message forwarding in sandboxed and single-process modes.

The original native-input fixture stopped at its `srcdoc` child:
`contentDocument` was null. A readiness guard alone still timed out. The
engine's existing `about:srcdoc` navigation rewrites to an independent data URL,
so the regression now serves both real documents from one loopback HTTP origin.
All paste/security/lifetime assertions and existing deadlines remain intact;
both complete native-input runs then passed. No runtime `srcdoc` repair,
macOS execution, application-specific attachment UI, mixed-monitor transition
or full-day soak is claimed.

Release-assembly tests ran 22 cases (21 passed, one skipped); all three symbol tests passed.
Assembly validated the archived EXE/PDB identity and hashes. The runtime ZIP
contains no PDB or symbol metadata; the symbols ZIP contains only the full
`mini-electron.pdb` and `symbols.json`. ZIP-member EXE and PDB hashes were also
checked against that metadata.

Executable SHA256:
`76be49183fb5167a72558d905af0fc6d6073fe11723453264e90159238ba4b4d`.
PDB GUID `C27221B57099F8054C4C44205044422E`, Age `1`; PDB SHA256:
`b8c467707160d428071d14697dd3584cb132ac171500c3c9b17708d7ed245c15`.
Runtime ZIP SHA256:
`875f96d6bd284e51965f18b964e3f43394f20678a4d34e96c664b70504a879b9`.
Symbols ZIP SHA256:
`80d0b6e522eb2f3642ac791b32531e8749afa3fe29aa011b76e79014f1bd4cdb`.

Distribution: `out/windows-x64/runtime-dist-0.1.33/`.
Immutable assembly: `out/runtime-release-0.1.33/v0.1.33/`.
Logs, results and native screenshots: `out/verification/windows-0.1.33/`.
No installed Desktop executable or user application profile was replaced.

Published [GitHub Release `v0.1.33`](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.33)
from source commit `51d584617965bb40d1e09b770330ca392bafdfe9`, with bilingual
release notes and only the Windows runtime ZIP, matching full-PDB ZIP,
`artifacts.json` and `SHASUMS256.txt`. GitHub's SHA256 digests and byte sizes
matched all four local assets before the Windows channel was changed.

The registry-published npm installer `0.1.12`, verified against registry integrity
`sha512-GRxg5hZnklfaqCmMje/rZ9NSuBC6J3Ac8zjBGIEsKJst37IOM7HmpnGQpDwz2XLTPA3qX1Oy6K9RxVMISTxmEg==`,
installed the public release in two isolated packages with separate fresh
caches and no local distribution, mirror or offline override. The pinned
installation reported `v0.1.33`, then passed application identity, native V8
layout/Worker consistency and SQLite. After channel cutover, the unpinned
installation independently selected `0.1.33` and passed those same checks plus
console-message forwarding. Both installed EXEs matched the executable hash above.

Windows `runtime-v1-win32-x64/artifacts.json` is asset ID `707805325`, size `528`,
updated `2026-11-04T01:38:44Z`, digest
`sha256:a9e797b907d873992e6216a57e9a9c5f54fa198c06c8a4b701b8084b70fac85d`.
The channel retains its prerelease classification and has only that asset.
macOS `runtime-v1` stayed unchanged: asset ID `698404550`, size `541`, updated
`2026-11-01T13:20:40Z`, digest
`sha256:c492596d5bb3379d4a84f729dfedb42eb19816507fb55fb65f5b60a6a14cb499`.
No macOS asset, older immutable runtime asset or npm package was republished.

## Windows runtime 0.1.32 (2026-11-02)

Synchronized the clean Windows checkout from `1dac123b9` to `249bf1cfa`,
then completed the Windows cursor/input and late-D3D-owner counterparts.
macOS-only Seatbelt, POSIX-FD and time-zone repairs were not generalized to
Windows. npm installer `0.1.12`, installer protocol `1`, Electron API `41.2.0`
and Node ABI `134` are unchanged; the macOS runtime/channel remains `0.1.31`.

Built the normal Windows x64 Release with Clang 22, MSVC `14.44.35207`,
Windows SDK `10.0.26100.0`, Python 3.14 and four workers. Native output was
rebuilt in `out/windows-x64`, followed by incremental rebuilds for the defects
found by real-runtime verification. ANGLE/Dawn dependencies used the existing
local tool/source cache. This is not a clean-room dependency bootstrap.

Before-fix observations:
- Immutable Windows `0.1.28` lacks `DatabaseSync.isTransaction`, rejects mixed
  numbered/named bindings with `column index out of range`, and lacks embedded
  cursor resource `alias` (`45000`).
- Native diagnostics showed a captured BrowserView receiving `(-2,-73)` with
  `buttons:1` and DOM pointer capture, while a stationary CSS update left the
  Win32 cursor at `grabbing`. Blink's timer path ignored the capture target.
- In single-process mode, offscreen native focus/capture calls cleared the
  BrowserWindow's HWND capture; the out-of-view move reached the host instead.
- At 150% display scaling, a correctly positioned physical pointer failed to
  reach the guest because Win32 pixels were compared with logical guest bounds.

The final packaged executable passed **33 runtime scenarios**:
- 17 shared checks: native SQLite, plus console-message, creation reentrancy,
  runtime event/destruction, IndexedDB transaction lifetime, body-loader,
  preload scope, same-document navigation and IPC-frame checks in sandboxed
  and explicit single-process modes.
- Eight native checks: cursor semantics and desktop coordinates in both
  process modes; complete hardware compositor checks in both modes; the
  software compositor check; and all four GPU/CPU host/guest combinations.
  Cursor observations use real SendInput and GetCursorInfo/GetIconInfo/GetDIBits,
  including RGBA/hotspots, stationary captured updates, chorded guest input,
  native caption/border precedence, inactive hover and hidden/ignored wakeups.
  Both coordinate runs passed 55 native/DOM assertions. GPU checks retained
  their hardware-clock assertions, native pixel/capture checks and six active
  GPU-owner teardown cycles per hardware run.
- Eight serial memory checks: live JS heap above 2 GiB, real cache reclamation
  and redraw, renderer OOM/minidump and dump refusal, and pre-OOM capture,
  file refusal, storage budget and near-heap-limit refusal. Real heap graphs
  and symbol-identifiable exception dumps were inspected by the regressions.

A separate disposable runtime probe passed in both modes at emulated renderer
scales `1.5` and `2`: native-message input hit a bounded guest target at `(50,50)`,
preserved DOM buttons `1 -> 3 -> 2 -> 0`, and retained the captured guest at
`(-230,-40)` after left release while right remained held. This exercises the
non-unit Win32-to-guest conversion without changing OS display settings; it is
not a physical mixed-DPI monitor transition. The final native matrix ran on a
single 100%-scale display; earlier candidate diagnostics used a 150%-scale
display. Earlier timer-clock prerequisite and fixture-initialization failures
remain recorded; no hardware assertion or deadline was weakened to pass them.
The exact missing-owner D3D interleaving was not forced deterministically.
No macOS run, mixed-monitor transition, original Desktop-session replay or
full-day soak is claimed.

Final executable SHA256:
`0daab4201c9a48386619235fc79ca2d78d4fa8c2d0e0e2be5c8f47ea25f3b3dd`.
Matching PDB GUID `E476DFFD226220F04C4C44205044422E`, Age `1`; PDB SHA256:
`b58e3e14f2745ac8b2c78727b4774f1015d39606851026e38126e8ea74c3ea3a`.
Runtime ZIP SHA256:
`891574cc63a0da559f45bafc84bfd465795c2a5ff43a4aa4c07854b40f85ee2c`.
Separate symbols ZIP SHA256:
`88bc42c11b006b4a08689c4b04e5e9ecd9023d6d77a9b514d8d9ccb5b6b8cd91`.
Release assembly validates the matching EXE/PDB pair and excludes symbols from
the installable ZIP. The integrity-checked registry npm installer `0.1.12`
installed the assembled final release in an isolated package; its CLI reported
`v0.1.32` and its executable passed the complete native SQLite regression.

Distribution: `out/windows-x64/runtime-dist-0.1.32/`.
Immutable assembly: `out/runtime-release-0.1.32/v0.1.32/`.
Logs, JSON results and native PNGs:
`out/verification/windows-0.1.32/`, with final matrices under `release-matrix-*`.
No installed Desktop executable or user application profile was replaced.

Published immutable [GitHub Release `v0.1.32`](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.32)
from source commit `e22b23b483da98b631a10f5d6815259039d37efa`, with only the
Windows runtime ZIP, matching symbols ZIP, `artifacts.json` and `SHASUMS256.txt`.
GitHub's SHA256 digests matched all four local assets. Downloaded the public
runtime ZIP, manifest and checksum file without credentials and matched the
runtime hash above before changing the Windows channel.

The registry-published npm installer `0.1.12`, integrity
`sha512-GRxg5hZnklfaqCmMje/rZ9NSuBC6J3Ac8zjBGIEsKJst37IOM7HmpnGQpDwz2XLTPA3qX1Oy6K9RxVMISTxmEg==`,
installed the public release into two isolated packages with separate fresh
caches and no local distribution/mirror override. The pinned installation
reported `v0.1.32` and ABI `134`, then passed native SQLite and the complete
native cursor/captured-guest regression. After the channel update, the unpinned
installation independently selected `0.1.32`, reported the same version/ABI,
and passed native SQLite and console-message regressions. Both installed EXEs
matched the final executable SHA256 above.

Windows `runtime-v1-win32-x64/artifacts.json` is asset ID `700412260`, size `528`,
updated `2026-11-02T03:43:15Z`, digest
`sha256:7ea70fd7503139429e0a378462e00688919b238bf41d1312eace80b296944621`.
macOS `runtime-v1` stayed unchanged: asset ID `698404550`, size `541`, updated
`2026-11-01T13:20:40Z`, digest
`sha256:c492596d5bb3379d4a84f729dfedb42eb19816507fb55fb65f5b60a6a14cb499`.
No macOS asset, old immutable runtime asset or npm package was republished.

## macOS runtime 0.1.31 (2026-11-01)

Apple Silicon macOS-only runtime update carrying the native SQLite binding fixes
committed as `71eea8427`. Windows runtime/channel remains `0.1.28`; npm installer
remains `0.1.12`. Installer protocol `1`, Electron API `41.2.0` and Node ABI `134`
are unchanged. No Blink renderer, cursor or browser surface changed relative to
`0.1.30`.

Rebuilt the Release engine with Xcode Clang 17, Python 3.14 and GN/Ninja. The
first relink failed with `Undefined symbols: _SSL_get0_group_name` because the
cached static curl archive in `.build-tools` had been compiled against OpenSSL
3.2+ headers while the linked `libmini_electron_openssl.a` implements the
repository's OpenSSL 3.0.13, which never defines that symbol. Deleting the stale
`curl-8.23.0-darwin-arm64` install and rebuilding with the archive-referenced
headers restored the link; no source or build-script change was needed. The
distribution was stripped and ad-hoc signed; the executable reported `v0.1.31`,
`version.json` reported runtime `0.1.31`, platform `darwin`, arch `arm64`, ABI
`134`, and `codesign --verify --deep --strict` passed.

Packaged-runtime checks passed:
- Native SQLite regression: OAuth-style filtering, WAL, `isTransaction` state
  including closed-connection `ERR_INVALID_STATE`, nested savepoints, failed
  `BEGIN` writer-lock isolation, rollback recovery, mixed numbered/named
  positional bindings, read-only access, column metadata, backup, changesets,
  Worker binding and extension-loading restrictions (SQLite 3.49.1).
- Native pointer/wheel screen DIPs, negative positions, window movement,
  BrowserView offsets and stationary pointer capture.
- Sandboxed local `Date`/`Intl` labels in system `Asia/Shanghai`.

The cursor regression still requires an unlocked console and was not re-run;
the physical hover, native-border precedence and captured-cursor presentation
results recorded for `0.1.30` are neither repeated nor contradicted here.
Windows binaries were not compiled or run. No Developer ID signing or
notarization is claimed.

Distribution: `out/mac-arm64/runtime-dist-0.1.31/`.
Release assembly: `out/runtime-release-0.1.31/`.
Runtime ZIP SHA256:
`0638f4022bd42883488ef5276464ae2f6a799d8720d86ffcdbeb30663123899e`.

Published immutable [GitHub Release `v0.1.31`](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.31)
from source commit `a04474f98`, uploading only the macOS runtime ZIP,
`artifacts.json` and `SHASUMS256.txt`. Downloaded the public ZIP without
credentials and matched the SHA256 above before updating macOS `runtime-v1`
(asset ID `698404550`, digest
`sha256:c492596d5bb3379d4a84f729dfedb42eb19816507fb55fb65f5b60a6a14cb499`).

Two isolated installations of the registry-published npm installer `0.1.12`,
each with its own fresh `MINI_ELECTRON_CACHE`, selected `0.1.31`: one with an
explicit `0.1.31` runtime pin, one unpinned after the channel update. Both
installed executables reported `v0.1.31` and ABI `134`, and both passed the
native SQLite regression including numbered bindings — the case that fails on
`v0.1.30` with `column index out of range`. The pinned installation also passed
deep/strict signature verification; the channel installation also passed native
desktop coordinates and sandboxed system local time.

Windows `runtime-v1-win32-x64` stayed unchanged at `0.1.28`, asset ID `681966016`,
size `528`, updated `2026-10-27T15:15:07Z`, digest
`793ac248f1d1764ebbe5f82edb152105ea3dae4f4752109a99ef9164995fef0c`.
No Windows asset, npm package or `v0.1.30` asset was rebuilt, republished or
overwritten.

## macOS runtime 0.1.30 (2026-11-01)

Apple Silicon macOS-only runtime update from the native cursor cutover in
`edab61e96`, including the already committed renderer time-zone initialization
before Seatbelt lockdown. Windows runtime/channel remains `0.1.28`; npm installer
remains `0.1.12`. Installer protocol `1`, Electron API `41.2.0` and Node ABI `134`
are unchanged.

Packaged the current Release engine with Xcode Clang 17, Python 3.14, GN/Ninja and
cached dependencies. The build graph regenerated and required no further native
compilation. The app was stripped and ad-hoc signed; its actual executable
reported `v0.1.30` and ABI `134`, and `codesign --verify --deep --strict` passed.
The default system Python could not run packaging because it lacks
`hashlib.file_digest`; the supported Homebrew Python 3.14 completed packaging.
No build-script compatibility workaround was added.

Packaged-runtime checks passed:
- Native pointer/wheel screen DIPs, negative positions, window movement,
  BrowserView offsets and stationary pointer capture.
- Sandboxed local `Date`/`Intl` labels in system `Asia/Shanghai`, explicit
  `America/New_York` (winter/summer DST offsets), and explicit `UTC`.

Before packaging, real AppKit-view event injection passed Blink-to-binary-IPC-to-
retained-NSCursor mapping for ordinary and hidden cursors, stationary CSS updates,
grab transitions, a 16×16-point premultiplied sRGB custom cursor with hotspot
`(5,7)`, BrowserView, navigation reset and captured webview guest routing.
DOM buttons progressed `1 → 3 → 2 → 0`; the captured guest received `clientX=-208`
with the right button still held. These are native-object/transport observations,
not evidence of the physical cursor on the screen. Temporary diagnostics were removed.

The physical-cursor regression was blocked by the explicit locked-console
precondition. The existing packaged mouse-modifier fixture stopped at
`Native window focused: false`; it is not counted as passing. Full unlocked hover,
native-border precedence, inactive-window hover, captured BrowserView cursor
presentation and late cross-window updates remain unverified. Windows cursor
code was not compiled or run on Windows; a portable harness exercised only the
actual captured-view routing method. No Developer ID signing or notarization is claimed.

Distribution: `out/mac-arm64/runtime-dist-0.1.30/`.
Release assembly: `out/runtime-release-0.1.30/`.
Runtime ZIP SHA256:
`55b16110c6af985a56efa2f606773bdd71b6e87d95a30bc32d9f30ddc64bca51`.

Published immutable [GitHub Release `v0.1.30`](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.30)
from source commit `3918ca830`, uploading only the macOS runtime ZIP,
`artifacts.json` and `SHASUMS256.txt`. Downloaded the public ZIP without credentials
and matched the SHA256 above before updating macOS `runtime-v1`.

The registry-published npm installer `0.1.12` installed the immutable `0.1.30`
release with an explicit pin, no local distribution override and a fresh cache.
The installed CLI reported `v0.1.30`, its native executable reported ABI `134`,
and deep/strict signature verification, native desktop coordinates and sandboxed
system local time passed. After the channel update, a second isolated unpinned
installation with another fresh cache selected `0.1.30`; its CLI, ABI and
deep/strict signature checks passed.

Windows `runtime-v1-win32-x64` stayed unchanged at `0.1.28`. Its channel asset
remained ID `681966016`, size `528`, updated `2026-10-27T15:15:07Z`, digest
`793ac248f1d1764ebbe5f82edb152105ea3dae4f4752109a99ef9164995fef0c`.
No Windows asset or npm package was rebuilt or published, and no immutable
release asset was overwritten.


## macOS runtime 0.1.29 (2026-10-30)

Incrementally rebuilt Apple Silicon Release with Xcode Clang 17, Python 3.14,
GN/Ninja, eight workers and cached native dependencies. The final distribution
was stripped and ad-hoc signed; `codesign --verify --deep --strict` passed.
This is not a clean-room dependency build or a Windows rebuild.

The 18-scenario packaged-runtime matrix passed:
- IndexedDB transaction lifetime, console-message, and creation reentrancy in
  both default independent-renderer and explicit single-process modes.
- Native desktop coordinates, including negative monitor positions, moved
  windows, BrowserView offsets, ordinary/precise wheels, and stationary capture.
  The old `0.1.25` reproduced `112 != 316`; the new PNG visibly renders the
  pointer/mouse event log with desktop coordinates instead of a blank/error page.
- High-FD renderer spawn and six pre-OOM cases: actual capture, disabled/invalid
  settings, four in-flight files, refused destination, and early close.
  Capture parsed a real `MacPreOomMarker` object node and `retainedMarker` edge,
  checked file bytes/0600 mode, and preserved the frozen outcome on a later
  actual SIGKILL exit. Early close joined cleanup with no late file recreation.
- Events, body-loader, WebSocket receive, and preload scope.

Native lifetime output was `live=64 retained-after-client-destruction=0`,
`frame-notifiers: retained-after-client-destruction=0`, and
`standalone-loader: destroyed-after-peer-disconnect=1`. The allocator/pressure
regression reported `PASS: renderer discardable memory and pressure policy`.
Actual idle-visibility and event-driven AppKit/libuv pump regressions also passed.
The registry-published npm installer `0.1.12` installed the assembled ZIP into an
isolated package using its SHA256 manifest. Its CLI reported `v0.1.29` and ABI
`134`; installed-runtime IndexedDB, console, creation, high-FD, native coordinates,
and real pre-OOM capture passed. One initial parallel console run missed the five
reload-iframe logs within the unchanged five-second observer; isolation and four
subsequent concurrent console/IndexedDB/creation rounds passed the original test.
The failed log remains recorded; this does not establish that no intermittent
reload issue is possible. No exception, lock assertion, or permanent test deadline
was suppressed or relaxed. The runtime ZIP SHA256 is
`6e17211e382966c5a078c8357b8bee91e3a7cb96b591611d68d9d90dd3949aea`.


Snapshot cases passed in isolated fresh processes. Concurrent runs sometimes
correctly refused when sampled free-page headroom fell below graph-plus-reserve;
those refusals were not changed into successes or masked by larger heap limits.
No synthetic global macOS critical-pressure event, real fatal V8 OOM/minidump,
full-day soak, original Desktop-session replay, complete WebGL resize matrix,
Developer ID signing, or notarization is claimed. Automatic single-process
snapshots are unsupported. The extra IPC-frame focus regression timed out at
`focus blocker` on both old `0.1.25` and new `0.1.29`, including a LaunchServices
launch of the new app; it is not counted as passing or claimed repaired here.

Logs, result JSON, and native coordinate PNG are under
`out/verification/macos-0.1.29/`; release assembly is under
`out/runtime-release-0.1.29/`.

Published immutable [GitHub Release `v0.1.29`](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.29)
from source commit `b5b28b8a7`, with only the macOS ZIP, `artifacts.json`, and
`SHASUMS256.txt`. Downloaded the published ZIP and matched the SHA256 above before
updating macOS `runtime-v1`. Windows `runtime-v1-win32-x64` remained at `0.1.28`:
its asset ID, size, update time and manifest digest stayed unchanged
(`793ac248f1d1764ebbe5f82edb152105ea3dae4f4752109a99ef9164995fef0c`).
A second fresh npm `0.1.12` package installed from the public, unpinned default
channel with no local ZIP override; it reported `v0.1.29` and ABI `134`, then
passed the actual IndexedDB and console-message regressions. The installed
bundle's deep/strict code-signature verification also passed.
No Windows asset or npm package was rebuilt or published, and no released
immutable asset was overwritten.

## Windows runtime 0.1.28 packaging (2026-10-27)

Incrementally rebuilt the Windows x64 Release with runtime identity `0.1.28`,
Clang 22, MSVC 14.44, Windows SDK 10.0.26100.0, cached dependencies and two
build workers. The immutable runtime ZIP and separate full-PDB ZIP were
assembled from the same build; no npm package or macOS runtime was included.

Release executable SHA-256:
`c119de32aa5b364c1c7628864cdf389fcd8562413ab1192644344ac2c06114a5`.
Matching PDB GUID: `C7DD7EF7035DE0494C4C44205044422E`, Age `1`;
PDB SHA-256:
`a92722535b883a95e28e508c36b5130c0b3a9715a46d86a14bded14d5f127434`.
Both archive hashes are in `SHASUMS256.txt`. The installable ZIP excludes the
PDB; the symbols ZIP contains only `mini-electron.pdb` and `symbols.json`.

The actual versioned runtime passed the IndexedDB transaction lifetime smoke
in sandboxed and explicit `--single-process` modes, the IPC-frame regression,
and a real pre-OOM snapshot followed by a V8 OOM/minidump. The published npm
installer `0.1.12`, downloaded from the registry and integrity-checked, installed
the assembled local release in an isolated package. Its CLI reported `v0.1.28`
and ABI `134`; the resulting executable also passed the IndexedDB smoke.

Build, local installer and runtime evidence are under
`out/runtime-release-0.1.28-investigation/`; assembled assets are under
`out/runtime-release-0.1.28/v0.1.28/`. The matching EXE/PDB pair is archived at
`out/memory-repair-native/symbols/mini-electron.pdb/C7DD7EF7035DE0494C4C44205044422E1/`.
These local-asset checks did not replace the user's Desktop installation.
No macOS run, original-session replay or full-day soak is claimed.

## IndexedDB transaction lifetime repair (2026-10-27)

The production pre-OOM graph identified the embedded IndexedDB's database
transaction list as the strong root for 1369 finished transactions and
approximately 2412 MiB of historical cache strings. The source repair ports
fake-indexeddb's deferred transaction removal on both completion and abort.
It does not change V8/Job limits, the application's cache budget, or native
IndexedDB service support.

The new `test/indexeddb-transaction-lifetime-smoke.js` failed against the
installed, unmodified runtime: after 32 overwrites the database still rooted
35 finished transactions. It passed against the rebuilt Windows x64 Release
in both sandboxed-renderer and explicit `--single-process` modes. Each run
verified 96 large same-key overwrites with one final record, no settled
transaction history, explicit and constraint-error rollback, cancellation
before start, overlapping/reentrant writes, explicit commit, aborted and
committed upgrade event ordering, and delete/reopen.

A separate disposable renderer performed **512 overwrites**, each containing a
524288-character payload. All eight settled batch samples had zero retained
transactions; the database contained one record with sequence 512. Only after
the writes, a known **1024-MiB** live Smi-array anchor crossed the probe's
host-only 20% snapshot threshold. The real owning-isolate profiler collected
and serialized the heap successfully:

- Snapshot self bytes: **1025.504 MiB**, including exactly **1024 MiB** of
  deliberately retained array storage.
- **Zero** live `FDBTransaction` objects after collection.
- All string self bytes: **0.591 MiB**; the only large string was the latest
  **0.5-MiB** database value, not the 512 historical versions.

The existing native IPC-frame, same-document-navigation and preload-scope
regressions also passed. These are actual executable runs, not Node mocks.
The 20% capture override applied only to the owned diagnostic process;
the production default remains 65%.

The normal incremental build reused `out/memory-repair-native`, Clang 22,
MSVC 14.44 and Windows SDK 10.0.26100.0 with two workers. GN/Ninja/CMake were
selected from the existing local tool cache. The distribution and extracted
ZIP passed exact manifest/hash checks, and the matching full PDB was validated.
Executable SHA-256:
`9162260373a00699dd80372199393d699ca8666e99dad256de99273af75dfb2f`.
PDB GUID: `633E631D480A635B4C4C44205044422E`, Age 1.

Evidence, the local runtime distribution and ZIP are under
`out/indexeddb-lifetime-fix/`; the PDB pair is under
`out/memory-repair-native/symbols/mini-electron.pdb/633E631D480A635B4C4C44205044422E1/`.
The local build retains metadata version `0.1.27`; no public release/channel,
installed Desktop executable, or real user profile was replaced. No macOS
execution, original-session replay, or full-day soak is claimed.

## Windows runtime 0.1.27 packaging (2026-10-27)

Rebuilt the normal Windows x64 Release distribution with runtime identity
`0.1.27` in `out/memory-repair-native/runtime-dist-0.1.27`, using the same
Clang 22/MSVC 14.44/Windows SDK 10.0.26100.0 toolchain and two workers as the
memory-policy candidate below. Assembled the Windows-only immutable release
and its Windows channel manifest with `scripts/build_runtime_release.py
--runtime-only`; no npm package or macOS runtime was published by that step.

Release executable SHA-256:
`f86a3c11fad2f18dccd3250f6d93e201624cb1f78bb4aa9135bdf261a776b9de`.
The matched full PDB is **1,307,623,424 bytes**, RSDS GUID
`989678E6BE12D8284C4C44205044422E`, Age `1`. The **258,673,710-byte** symbols
ZIP contains only the PDB and `symbols.json`; its decompressed PDB SHA-256
matched the metadata. The **39,493,778-byte** runtime ZIP contains no debug
symbols. Both ZIP hashes are recorded in `SHASUMS256.txt`.

The actual packaged executable reported `v0.1.27`. Its sandboxed renderer
passed the 2.125-GiB held-JS-heap responsiveness regression and the real
pre-OOM object/reference graph capture followed by a fatal V8 OOM/minidump.
The published npm installer `0.1.12`, extracted into an isolated directory,
installed the assembled local release successfully; its CLI reported
`v0.1.27` and ABI `134`. This is local-asset installation verification, not a
claim that the remote channel was already updated at build time.

Build/assembly logs, release smoke output, installer results and publication
evidence are under `out/runtime-release-0.1.27-investigation/`. Immutable
artifacts are under `out/runtime-release-0.1.27/v0.1.27/`; the matching symbol
store is under `out/memory-repair-native/symbols/mini-electron.pdb/989678E6BE12D8284C4C44205044422E1/`.
The existing Desktop installation was not replaced. No macOS execution,
14-hour soak, or unique retained-object cause is claimed.

## Windows memory policy candidate (2026-10-27)

Incrementally built Windows x64 Release in `out/memory-repair-native` using
Clang 22, MSVC `14.44.35207`, Windows SDK `10.0.26100.0`, and two build workers.
Dependencies/tooling were cached; this was not a clean-room build or release.
The initial shell invocation could not resolve CMake; the successful build
used the existing native-build virtual environment on the process PATH.

Final executable SHA-256:
`28c68ae6342303d45b963c3be6b77803236c6a512d13f2138c8e503f9a4d87d1`.
Its matched EXE/PDB pair is archived under
`out/native-memory-policy-check/symbols/mini-electron.pdb/D23C8730F7B913804C4C44205044422E1/`.
Installed Desktop binaries, production daemon and published version/channels
were not replaced or restarted.

Observed behavior in disposable, sandboxed native renderer processes:

- The headroom regression retained **2,281,701,376 bytes (2.125 GiB)** of Smi
  array slots, verified their sentinels and a subsequent DOM response, then
  released them and exited normally. `performance.memory` reported the
  privacy-rounded heap class, not exact byte-level limits.
- Native diagnostics measured a **8,589,934,592-byte (8-GiB)** Job budget and
  **4,294,705,152-byte** machine-default V8 heap limit. No `/2` custom V8
  budget remained.
- The production-linked C++ pressure regression passed independent heap/process
  hysteresis, source transitions, cooldowns and missing-Job samples, alongside
  the existing discardable-memory accounting/race cases.
- The real software-compositor pressure workload reached **6,069,043,200 bytes**
  peak private commit and reclaimed **66,732,032 bytes (63.64 MiB)** of warmed
  memory while preserving held buffers and subsequent composed colors.
  The post-pressure 2283x2130 surface was inspected. Its reported Job threshold
  remains formula-derived; the separate crash diagnostics query the actual Job.
- A real owning-isolate heap snapshot was JSON-parsed and checked for the
  `PreOomSnapshotMarker` object node and its retaining property edge. It was
  **1,807,291 bytes**. The smoke lowers the trigger to **1%**, using approximately
  96.6 MiB of heap, rather than profiling a 2.6-GiB application heap.
- A later deliberate V8 OOM preserved the completed snapshot status and still
  produced a valid exception minidump. Existing minidump/metadata identity,
  exception-context and unwritable-output contract checks passed. Restored V8
  defaults can fail at `CALL_AND_RETRY_LAST`; tests no longer require a specific
  internal V8 error-location phrase.
- Unwritable snapshot destinations reported `failed` / `file-io`; four existing
  automatic snapshots reported `skipped` / `storage-budget`, preserving those
  files. Neither condition retried or published partial snapshots.
- A single task retaining approximately 3.5 GiB of Smi slots reached
  **3,758,673,964 used heap bytes** before sampling. The delayed capture was
  refused with `skipped` / `insufficient-headroom`, zero serialized bytes,
  and no temporary snapshot file.

The first pressure run completed its memory/pixel assertions but hung during
profile cleanup. An unnecessary `armed` sidecar was being persisted on close;
the candidate now avoids unattempted/unchanged sidecar writes. The same full
pressure scenario then exited with code 0. Earlier failing logs are retained.
A double-array near-limit probe exhausted V8 before yielding; it is not counted
as guard coverage. The passing guard scenario uses Smi arrays to avoid that
allocation/fragmentation path.

Evidence is under `out/native-memory-policy-check/`: build/link logs, native
policy output, headroom/pressure reports and PNGs, snapshot/refusal reports,
and runtime check results. Temporary build overrides and profiles were removed.

Limits: no 14-hour soak, original-session replay, macOS runtime verification,
or proof that V8/TS/native roots no longer retain growing object graphs.
Default-threshold multi-GiB snapshot performance is not measured. Snapshot
graph allocation and UI pause remain risks; see the
[capture guards and privacy policy](WINDOWS.md#renderer-memory-diagnostics-and-reclamation).

## Windows runtime 0.1.26 (2026-10-25)

Built the normal Windows x64 Release distribution in `out/memory-repair-native`
with Clang 22, MSVC `14.44.35207`, Windows SDK `10.0.26100.0`, and two build
workers. GN, Ninja, CMake, and Python came from the existing local tool cache.
The build used cached dependencies; it was not a clean-room build.

The inherited/default Dawn toolset initially selected MSVC `14.38` and encountered
a compiler internal error. Explicit `VCToolsVersion=14.44.35207`, bounded
`CL_MPCount=2`, and disabled MSBuild node reuse produced a successful dependency
and application build. These were process-local build-environment settings.

Packaged executable SHA-256:
`855fb92a83a6fcbcfc922e8ed8eccf7ba18436aeb8cb2bfedec8ce05ad5adb78`.

Verified on the actual packaged executable in fresh, owned profiles:

- `ipc-frame-smoke.js`: frame authorization, identity, detachment, and targeted
  delivery.
- `same-document-navigation-smoke.js`: document/preload/frame identity survives
  same-document history and is replaced on reload.
- `runtime-body-loader-smoke.js`: empty HTTP responses, redirected uploads,
  streamed request bodies, and blob/file reads.
- `runtime-event-contract-smoke.js`: generic event/reply separation and
  reentrant-safe destruction.
- `navigation-lifetime-smoke.cc`, linked against the final production objects:
  64 live frame registrations, zero retained entries/notifiers after client
  destruction, and exactly one standalone-loader destruction on disconnect.
- The integrity-verified, published npm installer `0.1.12` installed the actual
  versioned runtime ZIP. Its CLI reported `v0.1.26` and ABI `134`; Run-as-Node
  reported Windows x64, Node `24.0.0-pre`, and ABI `134`.

The full PDB is 1,307,582,464 bytes; its separate symbols ZIP is 258,673,279 bytes.
The ZIP was decompressed and the PDB SHA-256 matched the archived production
pair. Independent LLVM tools reported GUID
`25CD7A79-B426-853D-4C4C-44205044422E`, Age `1` for both EXE and PDB, with full
debug/type/global information and no stripping or conflicting types. Runtime
ZIP inspection found no PDB or symbol metadata. Temporary Blink heap-capture
instrumentation is absent from this production executable.

Release assembly tests: 19 passed, one macOS executable-permission case skipped
on Windows; all three symbol-identity tests passed. The skip is not macOS runtime
verification. The existing cross-document-back blank-document failure remains
outside the passing coverage. This release does not establish a unique cause
for the separately supplied JavaScript heap-limit OOM.

Raw smoke logs, symbol/installer checks, and publication evidence are under
`out/runtime-release-0.1.26-investigation/`. Existing user installations and
production daemons were not replaced or restarted.

## Frame and navigation object lifetimes (2026-10-25)

The Windows x64 Release source candidate was built with
`python build.py --out out/memory-repair-native --jobs 8`, configured GN/Ninja,
Clang 22 and MSVC 14.44.35207. The installed Desktop application and running
production daemon were not replaced or restarted.

Candidate executable SHA-256:
`68a3b976bd2e7c5e278f7be3176ce7a15fbb0833b8c0f6c8a408b48513816fa4`.

Frame-registry retirement now uses the retained frame token after
`FrameDetached` clears the frame pointer. Resource-load notifiers belong to the
frame client, and their weak references expire with it. Standalone navigation
loaders retire on receiver disconnect; factory-owned loaders retain their
existing parent ownership. Policy-container implementations belong to their
frame client or self-owned receiver rather than an unowned allocation.

Verified against the actual candidate:

- `test/navigation-lifetime-smoke.cc`, linked with the production objects and
  libraries, passed: all 64 registered frames were live before client
  destruction; zero entries and zero notifier weak references remained after;
  a standalone loader was destroyed when its Mojo consumer disconnected.
- A real single-process iframe workload created and removed 1,152 frames.
  All six sampled, exactly representable IDs were registered while live and
  absent after detachment. The original runtime retained all five live-ID
  samples in its 832-frame workload. Sampling is not the deterministic regression.
- Existing `ipc-frame-smoke.js` and `same-document-navigation-smoke.js` passed
  in the isolated candidate, including frame detachment, targeted IPC, preserved
  same-document identity and replacement on reload.
- A local HTTP workload verified the rendered URL, body marker and title for
  all 20 ordinary navigations in both binaries. The following cross-document
  back navigation exposed a pre-existing blank-document failure in both.
  After window destruction the original retained one of 21 URLLoader objects;
  the candidate released all 21. Cross-document back navigation is **not**
  recorded as a passing feature test.

Evidence, including failing probes, is under `out/memory-repair-investigation/`:
`native-repair-verification.json`, `http-navigation-content-evidence.json`,
frame-registry results and CDB allocation/destruction logs.

These fixes do not establish the cause of the separately supplied V8
heap-limit OOM. Its 2 GiB heap sample cannot identify JavaScript retaining
owners without an actual renderer heap snapshot; native object counts and
process commit totals are not substitutes for that evidence.

## macOS runtime 0.1.25 (2026-10-25)

Built and packaged on Apple Silicon with:

```sh
python3.14 -B build.py --jobs 8 --package-dist --dist-output out/mac-arm64/runtime-dist-0.1.25
python3.14 -B scripts/build_runtime_release.py --macos-dist out/mac-arm64/runtime-dist-0.1.25 --runtime-only --output out/runtime-release-0.1.25
```

GN generation, 10,409 Ninja build steps, native linking, stripping, and ad-hoc
bundle signing completed with cached dependencies. This was not a clean-room
dependency build. The packaged executable reported `v0.1.25` and ABI `134`;
`codesign --verify --deep --strict` succeeded.

Actual packaged-runtime smoke runs passed:

- `macos-precise-wheel-smoke.js`: both axes/directions, standard and legacy DOM
  deltas, one-logical-pixel Retina input, and ordinary-wheel compatibility.
- `macos-scroll-smoke.js`: 80-pixel mouse notches, multi-notch distances,
  proportional trackpad movement, high-frequency/fractional input, and nested
  scroll targeting. The window is shown without activation; hidden renderer
  input is suspended. Renderer `capturePage()` PNG evidence was retained.
- `runtime-event-contract-smoke.js` and `runtime-body-loader-smoke.js`:
  reentrant window destruction, native event/reply separation, empty responses,
  redirected uploads, streamed request bodies, and blob/file reads.
- `webcrypto-crash-regression-smoke.js`: cryptographic vectors, authentication,
  key formats/signatures, and malformed DER rejection.
- `websocket-smoke.js`, `websocket-backpressure-smoke.js`,
  `websocket-receive-lifecycle-smoke.js`, and
  `websocket-frame-retirement-smoke.js`: native transport, bounded receives,
  independent renderer progress, cancellation, and credit retirement.
- `macos-exit-lifecycle-smoke.js`: native menus and preload-backed window exit.

The published npm installer `0.1.12` installed the assembled local release,
checking its archive and file/link inventory. The installed runtime reported
`v0.1.25` and ABI `134` outside the checkout; Run-as-Node reported
`darwin`/`arm64`, Node `24.0.0-pre`, and ABI `134`.

Limits: `macos-gpu-compositing-smoke.js` could not obtain composed WindowServer
pixels (`SCStreamErrorDomain -3811`), so no new native GPU-composition visual
claim is made. The generic `webgl-smoke.js` reached a resize step requiring
`BrowserWindow.setContentSize`, which this macOS host does not implement; that
suite did not pass. Developer ID signing, notarization, and Windows builds were
not performed.

## Renderer memory forensics and receive backpressure (2026-10-24)

The Windows 10 x64 source candidate was incrementally built and staged with
`python build.py --package-dist --jobs 8 --dist-output out/verification/renderer-memory-fixes-20261024/runtime`,
using the installed GN/Ninja/CMake toolchain and cached dependencies. This was
not a clean-room rebuild, a new published release, or a replay of the original
OMP Desktop incident.

Verified executable SHA-256:
`1858762d7d4a1ff1c2803b4ba8c88d42920aa62875bb5e159c3b7d3e5a30a249`.
Its matching PDB has GUID `FCD654A3AAFA0A544C4C44205044422E`, age `1`, and SHA-256
`e7a00317c950280a19cbc551b8fa0186139e6ef80ff011c238674afd2e6b47ec`.
The archived identity is under
`out/windows-x64/symbols/mini-electron.pdb/FCD654A3AAFA0A544C4C44205044422E1/`.

### Actual runtime and native checks

- **21 isolated packaged-runtime executions passed with external exit code 0.**
  This includes WebSocket basic, receive/cancel and frame-retirement checks in
  both process modes; bounded receive flow; real pressure/repaint; heap, mixed,
  native-Blob and Worker OOM; unwritable dump output; ordinary native crash;
  native crash after a recoverable Job refusal; transport/control memory curves;
  and immediate-exit/continue capture cancellation in both process modes.
  No owned child needed forced cleanup.
- **Six real exception minidumps** matched the archived EXE/PDB identities and
  were loaded in CDB with private symbols. Main/Worker V8 failures resolved
  through `blink::ReportV8OOMError` and `V8::FatalProcessOutOfMemory`; native Blob
  exhaustion resolved through `WTF::Partitions::HandleOutOfMemory` and
  PartitionAlloc, while ordinary native exceptions retained their actual thread.
  The dump contract also checked AMD64 control registers, exception stream,
  module RSDS GUID/age and embedded diagnostic evidence.
- The mixed workload recorded peak commit **4,294,950,912 bytes** against an
  actually queried **4,294,967,296-byte** Job process limit, a real Job refusal,
  V8 failure location `CALL_AND_RETRY_LAST`, `reason: "oom"` and unsigned
  `exitCode: 0xe0000008`. Heap-limit OOM retained `null`/`unknown` Job evidence.
  Worker failures preserved their failing thread and explicitly labeled
  last-main-isolate samples rather than presenting those samples as worker data.
- Native Blob OOM retained a **67,112,960-byte** failed allocation; unspecified
  allocation size stayed `null`. A native breakpoint after a recovered refusal
  remained `crashed`/unknown-source despite its historical Job evidence.
  Replacing the dump directory with a file retained OOM diagnostics and the
  exact Win32 `ERROR_FILE_EXISTS` (`80`) without claiming a dump was written.
- Two production-linked native executables passed: discardable live-byte
  accounting, locked-data preservation, purge/relock races, allocation dumps and
  pressure hysteresis/cooldowns; and deterministic delayed fatal OOM/native
  exit observation with a still-live broken-IPC control. Three symbol-archive
  Python contract tests also passed.

The final run fixed two additional observed failures rather than weakening
their assertions. IPC closing after a captured fatal exception could beat the
old 250 ms process observation and report `ipc-failure`; bounded capture/exit
coordination now waits for the actual OS status. Pending capture cancellation
could leave V8 promises in tasks destroyed after isolate teardown; owner-state
cancellation now releases them before shutdown. The before-fix CDB host stack
ran through `PromiseBase::~PromiseBase`, `GlobalHandles::NodeSpace::Free` and
task-queue destruction. Both immediate exit and continued replacement-renderer
scenarios passed after the fix.

### Measured receive and pressure behavior

For 1,024 ordered 128 KiB messages with JavaScript blocked for six seconds,
renderer private growth while blocked was **10.94 MiB**, versus **161.02 MiB**
in the original released-runtime investigation. The final one-message control
grew **0.50 MiB**. At the blocked sample the producer had completed **86/1,024**
messages; it resumed and delivered every message after the consumer resumed.
These are finite workload measurements, not a process-memory ceiling.

The broader WebSocket smoke also passed exact-60-MiB fragmented binary with an
empty final continuation, minimum-charge empty messages, aggregate sockets,
independent renderer budgets, unread stream cancellation and prompt disposal.
The raw test peers now complete TCP half-closes, and flow tests await clean
WebSocket close events before navigation rather than treating teardown resets
as a transport failure.

The real visible software-compositor pressure workload retained all touched
live buffer pages at **3,042,627,584 bytes** private commit and measured
**74,084,352 bytes (70.65 MiB)** reclaimed from warmed memory.
The smoke's 4 GiB threshold is explicitly formula-derived, not a queried Job
sample; the production monitor queries the actual Job limit. The workload stayed
in the moderate band, preserved renderer identity and verified post-purge colors.
Before/after **2283x2130** composed PNGs and numeric measurements were retained;
the post-pressure surface was visually inspected.

Evidence is under `out/verification/renderer-memory-fixes-20261024/`:
`final-measurements.json`, `final-capture-validation.json`,
`serial-suite-results.json`, native compile/link/run logs, and each
`runs/accepted-*/` directory's samples, metadata, dump, symbolized stack and
pressure captures. Earlier failing runs and debugger logs remain as before-fix
evidence; temporary probe/driver source files were removed after verification.

Limits: no native macOS build/run, Windows 11 replay, or live Desktop operation.
The original incident still lacks its historical dump and memory curve, so these
repairs do not identify its unique allocation cause. The pressure smoke uses a
visible surface; initial hidden software capture progression is not fixed or
claimed fixed. A non-yielding Blink-thread script can still delay pressure polls.
See [Windows behavior](WINDOWS.md#renderer-memory-diagnostics-and-reclamation)
and the [receive-credit contract](DEVELOPMENT.md#websocket-receive-backpressure).

## Source directory relocation (2026-10-23)

Project-owned code now lives under `src/runtime/`, `src/platform/` and `src/npm/`.
Windows/MSBuild and macOS/GN manifests, native includes, resource manifests,
embedded JavaScript inputs and npm packaging use the new locations without
old-root forwarding files. Upstream source roots and generated output paths
remain unchanged.

Verification on Windows x64:

- `python -B build.py --jobs 12 --electron`, with the installed GN/Ninja/CMake
  executables configured, completed dependency builds, native compilation,
  linking and staging. The built executable displayed `mini-electron API Demo`
  and the smoke command exited successfully. This was an incremental build using
  cached dependencies, not a clean-room rebuild.
- 584 moved Windows/macOS source-list references and 1,181 migrated native
  includes resolved to existing inputs. The Windows executable manifest paths
  were also migrated after the initial link exposed their old locations.
- The relocated WebGL generator executed with a temporary output directory and
  reproduced the checked-in forwarding implementation.
- Release assembly tests passed 7 cases; Dawn bootstrap tests passed 2.
  npm package tests passed 13 cases with 3 platform-dependent skips.
- An actual `npm pack` tarball installed into an isolated consumer with the
  assembled `v0.1.23` release as its offline runtime source. Package resolution
  returned the executable under package-root `dist/`; `path.txt` stayed at the
  package root. The `mini-electron` CLI printed `v0.1.23`, the `electron` CLI
  printed ABI `134`, and explicit `--update-runtime 0.1.23` succeeded.
- The release assembler staged the real npm source tree with resolvable package
  entrypoints. All 70 local documentation links and anchors checked successfully.

The pre-existing loose `out/windows-x64/runtime-dist` was rejected because its
file inventory did not match its metadata; the offline npm smoke used the
assembled immutable release instead. No release assets or versions were changed.
macOS manifests were checked for moved inputs, but no native macOS build ran.

## Verification and remaining boundaries

The renamed tree was built and exercised on Windows 10 x64 with
`python build.py --electron --jobs 16`. Checks included:

- Building all 18 static-library targets and linking `mini-electron.exe`.
- Actual native-window startup, readiness, registry, `loadFile()`, URL, Unicode
  title, and bounds round-tripping.
- Rendering and aligned native mouse input at 150% scaling, followed by clean
  process exit.
- A real Node HTTP/2 loopback request, embedded UTF-8 loading and `lstatSync()`
  byte lengths, and SHA-256 verification of a packaged PNG.
- `nativeImage.toDataURL()` producing a decodable 48×32 JPEG with expected pixel
  values, browser rendering of that data URL, and empty-image behavior. The
  existing JPEG format is retained; encoding no longer uses an obsolete SDK lookup.
- Profile migration preserving bytes, destination collisions preserving both
  directories without merging, and a locked source retaining the old profile
  without creating a partial destination.
- Native archive inspection finding 203 `mini_electron_*` symbols and no old
  `mb*` C entry points; duplicate-basename and long-filename MSBuild scenarios.

The consolidated IPC tree was built with `python build.py --jobs 16`.
Before/after utility-process smoke runs both connected a real child process,
sent `[7, 12, -2]`, and received the computed sum `17` plus intact Unicode text.
A separate context-isolated renderer invoked a main-process computation and
rendered its result through a resolved contextBridge Promise. Canonical Listener
and ChannelProxy headers also compiled together. The Windows source list keeps
13 IPC implementation files; the macOS manifest keeps the same 12 common files,
without adding a platform channel backend. Evidence is retained in
`out/verification/ipc-merge-results.json` and `ipc-merge-renderer.png`.

The Android cleanup and context-menu change were built together with
`python build.py --jobs 16`. The rebuilt runtime rendered a loopback HTTP page at
150% scaling, canonicalized a Unicode host/path, decoded a 4x3 PNG to the expected
RGBA pixel `[40, 120, 210, 255]`, and returned native checkbox input through an
isolated preload/IPC bridge. Actual right-click inspection found no popup on a
blank page and exactly the normal image-copy/save entries over an image.
V8's device-free Windows command runner executed a real child, and its performance
runner read a nested working-directory resource and produced three scores of
`17`. All 29 remaining performance-runner regression tests passed. The macOS
source manifests were checked for unchanged compilation entries and missing
files; no native macOS build ran on this Windows host. Evidence is in
`out/verification/android-cleanup-results.json`, `context-menu-cleanup.png`,
and `android-cleanup-desktop.png`.

OMP Desktop 0.3.11 was built with
`python build.py --omp-desktop --omp-source ../omp-desktop --jobs 16`.
Runtime checks included:

- The original packaged frontend reaching its home screen and settings through
  isolated preload/contextBridge IPC, not a replacement frontend.
- A desktop-managed daemon supervisor and worker started by the packaged
  `OMP Desktop.exe`, with a separate home and listen target `127.0.0.1:6798`;
  the bundled CLI reported `running`, `reachable`, and version `0.3.11`.
- Frontend WebSocket hello from `omp-desktop://app` to that daemon.
- Standalone Node child/fork/worker execution, including Unicode and spaces in
  argv and a child exit status of 17; bundled CLI `--version` returned `0.3.11`.
- Real Blink custom-protocol responses preserving status 418, headers, UTF-8,
  and an empty 204 response; file fetching returned HTML and missing-file 404.
- Out-of-order concurrent invokes on one IPC channel retaining their own
  results, replaced handlers taking effect, and rejected handlers reaching the
  main world without leaving the contextBridge Promise pending.
- Native tray bounds and valid/missing/empty image checks.
- Windows chrome at 144 DPI: caption-button actions and hover, runtime overlay
  color/height changes, drag/no-drag handling, and double-click maximize.
- Auto-hidden menus revealed by Alt, dispatched a real menu callback, and hid
  again; explicit visibility, replacement, null clearing, and framed windows.
- Disabled window capabilities, repeated prevented close events, and fullscreen
  restoring both normal placement and maximized monitor-work-area bounds.

OMP evidence is under `out/verification/omp-desktop-windows.png`,
`omp-desktop-settings.png`, `omp-ui.log`, and `omp-desktop-home/daemon.log`.
Window-chrome evidence is retained in `out/verification/window-chrome-results.json`
and `window-chrome-native.png`; the OMP screenshots include the integrated dark
title bar without a persistent native menu strip.
That older check received HTTP 404 for the release's missing `latest.yml`; it did
not verify an automatic-update download.

### OMP Desktop 0.4.0 Windows contract verification

The original 0.4.0 packaged frontend reached its home screen and settings through
the real isolated OMP preload. Native mouse input opened settings; physical
Ctrl+Shift+N opened one additional original application window. Screenshots are
under `out/windows-x64/verification/omp-desktop-windows-final.png` and
`omp-desktop-settings-final.png`.
The final uninstrumented packaged run showed a green connected host, resolved an
isolated directory through the daemon, and reached its new-conversation composer.
Direct packaged `--version`, `--help`, and positional `status --json` exited zero;
the latter reported the desktop-managed 0.4.0 daemon running and reachable.
`omp-desktop-final-gui.json` identifies the packaged executable and those results.

Actual Windows checks covered:

- `browser-window-created` before constructor return, one first-painted-frame
  `ready-to-show` notification, and restored bounds across maximize, fullscreen,
  minimize, and restore.
- Native popup dismissal and owner destruction invoking the close callback once;
  selection invokes `click` before that callback. Physical accelerators honor
  disabled/hidden items and application-menu replacement/removal. Ctrl/Shift
  state is transported to the renderer rather than sampled from its OS thread.
- Real custom/default/cancel message buttons and checkbox state; native file
  multiselect with `*.png;*.jpg`; asynchronous and synchronous directory cancel.
- Real default/persistent Session directories, `null` for memory sessions, and
  the original retired-profile cleanup preserving the default profile.
- `shell.openPath` resolving the native error string or `''`, and
  `shell.openExternal` resolving `undefined` or rejecting a real OS failure.
- PNG/JPEG/ICO decoding, including the original 256×256 packaged ICO and rejected
  truncated ICO data. Main-to-renderer IPC survives nested payload delivery on
  the Blink thread.
- Per-Monitor V2 awareness on the 144-DPI monitor: DPR 1.5, a 920×600 DIP viewport
  and matching 1380×900 physical client/bitmap. A physical click reached CSS
  coordinates (400, 150) exactly; page zoom 1.25 produced DPR 1.875 without
  resizing the bitmap, and reset/reload retained DPR 1.5.
- Real CSS-caption dragging, overlapping no-drag holes, non-resizable dragging,
  `movable: false`, native double-click maximize, maximized drag-to-restore,
  fullscreen exclusion, and clearing an empty drag-region list. The unmodified
  packaged OMP titlebar also moved its window by the physical mouse delta.
- Actual renderer WebSocket Unicode and 70,000-byte binary round trips with a
  clean close; Windows libcurl now compiles its WebSocket protocol handlers.

Windows `autoUpdater` is an EventEmitter consumed by the original
`electron-updater` NSIS implementation. An isolated original-builder NSIS probe
installed 0.3.11, downloaded and verified 0.4.0, called `quitAndInstall`, observed
`before-quit-for-update` before application shutdown, and launched the installed
0.4.0 replacement. The probe used native input
`a3b7d9131bfd20c2da2997401452a35b1d51f6199e2431a5c0dab3a7c0c06da6`;
its versioned evidence is `nsis-updater-proof-f71c8bfac1.json` and matching
`nsis-updater-*.jsonl` in the same verification directory. The official installed
OMP application and its registry identity were untouched.

Current release hashes/feed consumer checks are in `native-input.json` and
`package-proof.json`. Actual packaged Node mode, CLI, keyring, ConPTY, and ZIP
extraction were also exercised; their retained reports identify the native
input used. The public 0.3.11 release still lacks `latest.yml`; producing a local
feed does not publish it to GitHub.

The final DPI/input/drag smoke is retained in `windows-dpi-drag-final.json`.
It identifies the native executable used and covers a single 3840×2160 display
at 150%; a real cross-monitor DPI transition was not exercised. The obsolete
Mac-private device-scale entry point was replaced by the shared native API.
Packaged runtime discovery selects `resources/app.asar` before `resources/app`
and preserves application argument casing. Command/flag precedence belongs to
the original application's argument parser, not the runtime's app-path resolver.

Inspector, DevTools, and development React DevTools remain disabled on Windows.
No extension system is implemented. Existing renderer job restrictions, including
direct OS clipboard read/write denial, remain unchanged; the native main-process
clipboard API is separate from those restrictions.

Local evidence is retained in `out/verification/rename-runtime-result.json`,
`rename-runtime-ready.png`, and `rename-runtime-input.png`.
The runtime smoke kept a Node timer active while awaiting `loadFile()`; one run
without it rendered the page but did not finish the main-script report within
five seconds. Idle promise scheduling is not established by this verification.

Windows project source paths and macOS manifests were checked after migration.
The macOS native build, AppKit runtime, and macOS profile migration were **not**
executed on this Windows machine; they require an Apple Silicon Mac.

Windows embedded resources under the virtual `mini-electron-resources` path
support UTF-8 loading and synchronous `lstat`, but `statSync()` and binary
`fs.readFileSync()` currently fail with `ENOENT`.
Normal files packaged under `resources/app` do not have those virtual-path
restrictions. This is not full Electron API or filesystem compatibility.
The existing Node `url.parse()` deprecation warning also remains visible.

### Cleanup cutover verification

The renderer uses the canonical native IPC transport and browser-owned file,
network, storage, download, and popup brokers. Frame identity and origin metadata
are not supplied by renderer JavaScript. File access is limited to application
roots, trusted `loadFile` roots, and opaque capabilities issued by native file
selection; profile directories are not application roots. The obsolete renderer
bootstrap, compatibility aliases, and incomplete extension surface were removed.
Windows no longer stages disabled DevTools resources. CDP/DevTools and Node
Inspector are disabled at compile time in the macOS host and Windows
`Release|x64` build; macOS retains its updater framework inputs.

Windows runtime evidence is under `out/windows-x64/verification/cleanup-*.json`
and matching PNGs. Reports identify the particular native input exercised; older
reports are not evidence that every later executable was rerun through every case.
The exercised paths include:

- Genuine Unicode file and directory selection, DOM `File` bytes and upload
  bodies, plus rejection of forged native file identity.
- Credentialless private-network preflights, denied and allowed requests,
  frame-origin storage isolation, and continued execution after a failed preload.
- Browser-owned streamed downloads, including cancellation, interruption, and a
  completed 70 MiB file with matching SHA-256; denied/allowed popup creation and
  rejection of a pending load when its WebContents is destroyed.
- BrowserView bounds, real click/wheel input and detach at 150% DPI, physical
  window dragging, and opaque, half-alpha, and fully transparent desktop
  composition. Clipboard image probes covered malformed and valid top-down DIBs.
- Native `loadURL` completion for Unicode, Base64, and empty `data:` documents.
  Renderer-initiated top-level `data:` navigation remains blocked, and the new
  opaque document does not inherit the previous document's local-file permission.
- Real SQLite cookie and LevelDB Local Storage imports without source mutation;
  a mixed valid/unsupported database does not publish partial BrokerStorage.
- Real runtime-directory, checksummed ZIP, and verified offline-cache npm
  installation, plus rejection of bad checksums and unlisted release files.
  `require('@iriscat/mini-electron')` remains a path resolver, not a per-call inventory scan.

Packed ESM source is read directly from ASAR rather than extracted into temporary
files. Windows extraction for real file handles and native modules uses bounded
GUID-based reservation and removes owned files when sharing rules allow it.
Historical temporary files of unknown ownership are not removed by this cleanup.
The final original OMP Desktop 0.4.0 package passed its unchanged CLI shim's
version/help/status commands, reached its own desktop-managed daemon, and
created, wrote to, captured, and killed a real ConPTY terminal. Original packaged
main/preload bytes matched the source build. The retained consumer report is
`cleanup-omp-consumer-proof.json`; `cleanup-final-native-asar-proof.json` covers
direct packed ESM loading, real file handles, and the native keyring operation.
`cleanup-final-release-install-proof.json` identifies the final ZIP and installed
native executable used by the npm CLI smoke.
The DPI-correct 1800-by-1200 original application home surface is retained as
`cleanup-omp-consumer-physical.png`, with its scope and correction recorded in
`cleanup-omp-consumer-physical-proof.json`. The earlier logical-coordinate
consumer capture and input attempt are not accepted as GUI interaction proof;
the extra original-application click probe was not validated and was stopped.
SQLite, LevelDB, the renderer sandbox, and the live Squirrel/Mantle/ReactiveObjC
updater dependencies and licenses remain in the product closure.

On an Apple Silicon Mac (macOS 27, Xcode clang 17), `python build.py --run`,
`--gui`, `--electron`, and `--package-dist` built and ran. The smoke executable
reported `result: 42` with ICU zh-CN formatting; the GUI demo wrote its Blink
snapshot; the shared example reached a visible window and exited 0. The
assembled two-target release was served from a local `MINI_ELECTRON_MIRROR`;
`npm install` of the packed tarball verified and installed the darwin ZIP,
`codesign --verify --deep --strict` passed, `require()` returned
`dist/Electron.app/Contents/MacOS/Electron`, `npx electron --version`/`--abi`
printed `v0.1.0`/`134`, and an installed app loaded a file and exited with its
`app.exit()` code. Notarization and Developer ID signing were not performed.
