# Verification and known boundaries

[Documentation index](README.md) · [Project overview](../README.md)

> Run commands from the repository root unless a section explicitly changes directory.

Each record applies only to its stated source layout, runtime, platform and
workload. Older records migrated from the root README are not evidence of a new
verification run. Release-specific evidence is in [release notes](RELEASES.md).

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
