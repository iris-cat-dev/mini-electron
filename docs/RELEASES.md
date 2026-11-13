# Release notes

[Documentation index](README.md) · [Project overview](../README.md)

> Run commands from the repository root unless a section explicitly changes directory.

These are historical release and verification records, not a claim that every platform ships every change. npm installer and native runtime versions are independent. See [installation](INSTALLATION.md) and [publishing](RELEASING.md).

### macOS runtime 0.1.42 — native multiline Return input

Apple Silicon macOS-only runtime update. Windows runtime/channel `0.1.41`,
npm installer `0.1.13`, installer protocol `1`, Electron API `41.2.0`,
Node ABI `134` and Chromium major `132` are unchanged.

- Fixes Return and Shift+Return in multiline editors by forwarding AppKit's
  newline commands through the existing character-event path. Native Shift and
  keypad location survive; DOM keydown/keypress cancellation remains effective.
- Leaves Return during marked-text composition to the native IME path, preserving
  candidate text and avoiding an accidental newline or chat submission.
- The executable extracted from `mini-electron-v0.1.42-darwin-arm64.zip` passed
  `test/macos-newline-smoke.js`: textarea/contenteditable newline insertion,
  selected-text replacement, keypad Enter, DOM cancellation, chat Enter submit,
  ordinary character input and native marked-text commit. Its visible newline
  capture is `out/verification/macos-0.1.42/newline.png`.
- The system IME candidate panel was not exercised. Installed Desktop
  applications are unchanged by this runtime-only release.

Published [v0.1.42](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.42)
from [`dba87300e`](https://github.com/iris-cat-dev/mini-electron/commit/dba87300e90d19d4a2e4c76d654eb6c831dbead0).
The macOS `runtime-v1` channel now selects `0.1.42`.

See [final assets and qualification](VERIFICATION.md#macos-runtime-0142-2026-11-13).

### Windows runtime 0.1.41 — HTTPS/WSS system CA trust

Windows x64-only runtime update. npm installer `0.1.13`, installer protocol `1`,
Electron API `41.2.0`, Node ABI `134` and macOS runtime/channel `0.1.39` remain
unchanged.

- Native Session HTTPS and WSS connections share TLS initialization that imports
  Windows ROOT certificates with `CURLSSLOPT_NATIVE_CA`; certificate-chain and
  hostname verification remain enabled. Configuration failure rejects the handle.
- Fixes the missing-CA failure in the original Windows `0.1.37` runtime without
  disabling TLS validation or changing private-network authorization.
- Ships a separate matching full, unstripped PDB archive with RSDS GUID/Age and
  EXE/PDB SHA-256 metadata. Debug symbols remain outside the installable runtime.
- See the [original root-cause qualification](VERIFICATION.md#windows-native-httpswss-ca-trust-2026-11-12)
  and [final release evidence](VERIFICATION.md#windows-runtime-0141-2026-11-13).

Published as [v0.1.41](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.41)
from source `f01a4acbb7601dedae89d5389f1275a95c184b40`. All four uploaded asset
sizes/SHA-256 digests match the qualified local archives. Anonymous full runtime
and PDB ZIP downloads reproduced their hashes; the bilingual public release page
rendered correctly. The Windows channel now selects `0.1.41`; a fresh unpinned
public npm `0.1.13` installation selected it and passed package import and CLI
checks. The macOS channel asset is unchanged. No npm was published and no
installed Desktop application was replaced.

### Windows runtime 0.1.40 — native identity, owned dialogs and full symbols

Windows x64-only runtime update. npm installer `0.1.13`, installer protocol `1`,
Electron API `41.2.0`, Node ABI `134` and macOS runtime/channel `0.1.39` remain
unchanged.

- Carries the shared IPC listener removal/once, native frame/document identity
  and protocol request identity/header/binary-response changes to Windows.
- Completes Windows asynchronous open/save ownership: destroyed-owner rejection,
  active/queued owner-close cancellation, exactly-once callbacks and shutdown
  cleanup. Native cancellation avoids an owner-destruction modal-loop deadlock.
- Includes a separate matching full, unstripped PDB ZIP with RSDS GUID/Age and
  executable/PDB SHA-256 metadata; no PDB is included in the installable runtime.
- The final runtime ZIP's extracted executable passed native identity, IPC,
  protocol identity/Range and navigation in both process modes, the full native
  dialog/exit matrix, Node/Worker configuration, SQLite/FTS5 and storage durability.

Published as [v0.1.40](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.40)
from source `4c3d1cc47f54d33b28930dd02f742cc60215134b`. All four uploaded
sizes/server SHA-256 digests match the qualified local assets. Anonymous runtime
download and PDB access passed; a fresh public npm `0.1.13` installation selected
Windows runtime `0.1.40`, and its package import/CLI passed. The Windows channel
now selects `0.1.40`; the macOS channel asset is unchanged. No npm was published
and no installed Desktop was replaced.

See [exact final assets and qualification](VERIFICATION.md#windows-runtime-0140-2026-11-12).

### npm installer 0.1.13 — native frame and protocol identity types

Installer/types update; macOS runtime/channel remains `0.1.39` and Windows
runtime/channel remains `0.1.37`. Installer protocol `1`, Electron API `41.2.0`
and Node ABI `134` are unchanged.

- Includes `WebFrameMain`, `WebContents.mainFrame`, readonly
  `NativeProtocolRequestIdentity` and `ProtocolFetchRequest.nativeRequest` types.
- Ships the repository's `src/npm/` layout with package-root-aware installer
  and executable resolution. Existing package imports and CLI entry points remain.
- The packed tarball passed real local-distribution installation, package import,
  CLI execution and a strict TypeScript consumer. All 16 existing installer
  regressions passed. See [qualification](VERIFICATION.md#npm-installer-0113-2026-11-12).

Published [npm `0.1.13`](https://www.npmjs.com/package/@iriscat/mini-electron/v/0.1.13)
with public access and the `latest` tag. The registry integrity matches the
tested tarball. Fresh public installation, package import and the installed
`mini-electron` CLI passed using macOS runtime `0.1.39`.

### macOS runtime 0.1.39 — plugin IPC, protocol identity and owned dialogs

Apple Silicon macOS-only runtime update from `adca57273`, merged into `main`,
with runtime identity `0.1.39`. Windows runtime/channel remains `0.1.37`;
npm installer remains `0.1.12`. Installer protocol `1`, Electron API `41.2.0`,
Node ABI `134`, and Chromium major `132` are unchanged.

- Preserves EventEmitter listener removal/once semantics and authorizes IPC
  against its owning frame, document lifetime and navigation identity.
- Exposes readonly browser-owned `nativeRequest` identity to protocol handlers;
  transports request headers, binary resources and byte-range responses.
- Presents asynchronous open/save sheets on their exact native owner, cancels
  them on owner close/destroy, rejects invalid owners and retires shutdown callbacks.
- The final ZIP's extracted executable passed IPC frame/listener regression,
  protocol identity and Range `206`/`416` regression, and a focused native dialog
  smoke covering ownership/options, Node progress, close/destroy, queued
  cancellation and destroyed-owner rejection. Ad-hoc signature validation passed.
- Real file acceptance, Escape delivery and remote-panel opacity retain the
  existing [qualification limits](MACOS.md); they were not rerun or marked passing.

Published as [v0.1.39](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.39)
from source `155dd50c5`, with the runtime ZIP, `artifacts.json` and
`SHASUMS256.txt`. The macOS `runtime-v1` channel selects `0.1.39`; the channel
retains its prerelease label. Public installation with npm installer `0.1.12`
and an explicit unpinned runtime update both succeeded. Windows remains `0.1.37`.

See [release verification](VERIFICATION.md#macos-runtime-0139-2026-11-12).

### macOS runtime 0.1.38

Apple Silicon macOS-only runtime update, built from shared source
`8be282e56` with the `0.1.38` runtime identity. Windows runtime/channel remains
`0.1.37`; npm installer remains `0.1.12`. Installer protocol `1`, Electron API
`41.2.0`, Node ABI `134`, and Chromium major `132` are unchanged.

- Includes asynchronous broker storage persistence, no-op write suppression,
  ordered clearing, explicit flush errors, and macOS quit/exit draining.
- Includes the shared SQLite FTS5 and forwarding CommonJS builtin-loader
  repairs, plus native V8 configuration reporting and async-context changes.
- The final ZIP's extracted executable passed application/Node identity,
  async-context, heap-layout and SQLite/FTS5 checks, native storage mutations,
  preload-backed exit lifecycle, and all six isolated broker-storage phases.
- The full native cursor smoke stopped at the pressed-drag assertion. A
  diagnostic recorded an injected down followed by a no-button move,
  synthesized release, and lost capture; it is not counted as passing.

Published as [v0.1.38](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.38)
from source `79ed2d3d8f8e1dcb83f5496650fbbd1b4cfae800`, with the runtime ZIP,
`artifacts.json`, and `SHASUMS256.txt`. Anonymous runtime download matched the
published SHA-256. The macOS `runtime-v1` channel now selects `0.1.38`; the
Windows channel remains `0.1.37`. The bundle is ad-hoc signed, not Developer ID
signed or notarized. See [exact verification and limits](VERIFICATION.md#macos-runtime-0138-2026-11-10).

### Windows runtime 0.1.37 — asynchronous broker storage

Windows x64-only binary update. npm installer `0.1.12`, installer protocol `1`,
Electron API `41.2.0`, Node ABI `134`, and the existing published macOS
runtime/channel remain unchanged.

- Persistent Web Storage keeps immediate in-memory visibility while serializing
  and atomically replacing `BrokerStorage.json` on an ordered background sequence.
  Same-value sets and other no-op mutations do not rewrite the file.
- Clear acknowledgements use broker-owned state instead of a borrowed Blink
  cache. This removes the stale-map dereference when Blink replaces its cache
  before sending `DeleteAll`.
- Explicit flush, storage clearing and normal host shutdown drain accepted
  mutations. Persistence failures retain dirty data and are observable through
  stderr, explicit flush errors and unsuccessful final shutdown status.
- Windows runtime staging requires the isolated native storage regression before
  advertising `asyncStoragePersistence: 1`; Desktop packaging checks that marker
  as well as the executable hash. Existing published `0.1.36` assets are unchanged.
- See [storage semantics and qualification](DEVELOPMENT.md#broker-storage-persistence).
  The macOS shutdown consumer is updated in source; no macOS verification is
  implied by the Windows capability.

Published as [v0.1.37](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.37)
from source `2845171129b1188a63368eaf7362822ae1ae3ecb`.
All four uploaded asset sizes and server SHA-256 digests match the local release.
LLVM confirms the matching PDB has debug information, types, globals and publics
and is not stripped. The Windows channel now selects `0.1.37`; anonymous channel
and PDB-asset access passed. The macOS channel retains the same asset ID, size
and digest. See [final asset evidence](VERIFICATION.md#windows-runtime-0137--broker-storage-responsiveness).


### Windows runtime 0.1.36

- Producer-fence and first-present completion use cancellable native event
  notifications instead of blocking worker tasks. Explicit capture yields while
  a shared texture remains externally owned and bounds those retries to two
  seconds; it never frees GPU resources merely because a capture timed out.
- Pre-OOM heap snapshots are now explicitly opt-in through
  `MINI_ELECTRON_PRE_OOM_SNAPSHOT_PERCENT=1..65`; unset/zero keeps ordinary
  sampling, pressure handling and crash dumps without synchronous heap profiling.
- Windows source verification passed hardware composition in sandboxed and
  single-process modes, software composition, mixed-backend guests,
  snapshot opt-in/configuration and real renderer-OOM diagnostics.
  The activity-phase hardware VSync assertions remain in place; the smoke no
  longer assumes a newly created clock already has a known first-tick source.
- Published as [v0.1.36](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.36)
  from source `a9c594ee163a27cafdad83e2e3f846e425bdd345`. All four asset sizes
  and SHA-256 digests match the verified local release. The final ZIP passed
  16/16 isolated runtime scenarios and all 53 declared file hashes.
- The Windows `runtime-v1-win32-x64` channel now selects `0.1.36`; anonymous
  downloads of the runtime and channel manifest were verified. The macOS
  channel's asset identity, hash, size and update time are unchanged.
  npm was not published and no installed Desktop was replaced. See
  [final asset evidence and limits](VERIFICATION.md#windows-runtime-0136-2026-11-07).


### Windows runtime 0.1.35

Windows x64-only binary update. npm installer `0.1.12`, installer protocol `1`,
Electron API `41.2.0`, Node ABI `134` and the published macOS runtime/channel
`0.1.31` remain unchanged.

- Includes the existing FTS5 compile configuration and SQLite phrase/ranking,
  index-maintenance, durable-reopen and Worker regression coverage.
- Preserves mandatory `node:` URLs for scheme-only builtins through CommonJS
  resolution hooks. This repairs the established application-entry
  `node:sqlite` load failure without fake source or a hook bypass.
- Extends the SQLite consumer smoke to exercise pass-through hook resolution,
  native builtin `null` source and rejection of unprefixed `require('sqlite')`
  in both entry modes, retaining normal application readiness/exit handling.
- Preserves prior async-context and native-input behavior. The assembled release
  contains the runtime ZIP, separate matching full-PDB symbols ZIP,
  `artifacts.json` and `SHASUMS256.txt`, using only the Windows runtime channel.

Published as [v0.1.35](https://github.com/iris-cat-dev/mini-electron/releases/tag/v0.1.35)
from source `624558d63ea4e6bc15232f4e79a8036e21551696`, with all four
assets uploaded and their server-side digests verified. The Windows
`runtime-v1-win32-x64` manifest now selects this immutable runtime;
the macOS channel's asset identities and contents are unchanged.

The final ZIP's extracted executable passed **15/15** scenarios: identity;
async-context and native heap configuration in Node/application entry modes;
SQLite in both entry modes, including forwarding hooks, FTS5 and Workers;
fresh Desktop core SDK `18.14.0` Eval; sandbox/single-process native input,
cursor/capture and console; and public clipboard images.

EXE SHA256: `ed0327bf7a62c392ae1b55f51fb2ae144d4a921e241d0d17cc1a8140ccb85f3e`.
The independently paired full, unstripped PDB has GUID
`59E0A05A338C1A424C4C44205044422E`, Age `1`, SHA256
`9555ce59002166f9dac562aaf4d2beabc1fcb400cde890497e2231748f988185`.
No installed Desktop or frozen `0.1.34` package was replaced; macOS was not
built or tested for the SQLite/js2c repair. See the
[source diagnosis, final asset hashes and verification limits](VERIFICATION.md#windows-runtime-0135-2026-11-05).

### Source verification: platform-owned mouse capture

- Restricts HWND focus/capture and title-bar dragging to Win32. macOS keeps
  its existing AppKit capture owner; the shared renderer no longer releases
  another native surface's capture.
- Preserves all held buttons when synthesizing a left-button release and
  covers BrowserView non-owner releases plus three-button guest dragging.
- The macOS source build, native V8 layout in application/Node-only modes
  and Workers, cursor/capture, desktop pointer coordinates and precise-wheel
  regressions passed on 2026-11-04. The merged Windows source then passed the
  final runtime `0.1.34` scenarios below. No installed Desktop was replaced.

### Windows runtime 0.1.34

Windows x64-only runtime update. macOS runtime/channel remains `0.1.31`;
npm installer remains `0.1.12`. Installer protocol `1`, Electron API
compatibility `41.2.0` and Node ABI `134` are unchanged.

- Restores `AsyncLocalStorage` across `await`, foreign Promise realms,
  microtasks, timers and Workers in the existing Windows snapshot. Concurrent
  and nested scopes stay isolated without switching ALS implementations.
- Preserves one context resolve notification around the actual Promise
  continuation; it does not double-report thenable assimilation.
- Advertises `features.asyncContextPropagation: 1` only after the actual staged
  executable passes the native semantic probe.
- Keeps mouse capture and final release on the owning Win32 window/UI thread,
  preserves other held buttons during synthetic left-button releases and
  routes double-click presses through the existing focus/capture path.
- Ships the matching full, unstripped PDB in a separate symbols ZIP; the
  installable runtime ZIP contains no debug symbols.

The executable extracted from the final ZIP passed fourteen real-runtime
scenarios, including both ALS entry modes, heap layout/Workers, SQLite,
Desktop core SDK Eval, native input, native cursor/capture and console
forwarding in both process modes, and public clipboard images. See the
[Windows verification record](VERIFICATION.md#windows-runtime-0134-2026-11-04)
for exact hashes and remaining platform boundaries.


### Windows runtime 0.1.33

Windows x64-only runtime update. The macOS runtime/channel remains `0.1.31`;
npm installer remains `0.1.12`. Installer protocol `1`, Electron API compatibility
`41.2.0` and Node ABI `134` are unchanged.

- Adds authenticated Explorer `CF_HDROP` file paste for genuine DOM Files,
  including Unicode names and mixed image/document entries. Grants remain
  bound to the focused main-frame document, native paste and clipboard sequence;
  canceled shortcuts, nested/script paste and child-frame focus cannot reuse them.
- Moves authorized bytes into frame-bound Blob/File storage without retaining
  another complete native copy. Rejects size/count limits, directories and
  reparse paths, including a concurrently replaced ancestor. Packaged Windows
  distributions advertise `features.nativeClipboardFiles: 1`.
- Corrects `process.config.variables.v8_enable_pointer_compression` and the
  existing 31-bit-SMI/sandbox fields from actual C++ compilation flags instead
  of stale vendored `config.gypi` values. The shared bootstrap stays deeply
  frozen across application, Node-only and Worker startup; both native build
  backends refresh its generated js2c resource.
- Adds `test/node-build-config-smoke.js`, an executable native heap-layout and
  Worker/entry-mode consistency regression. Verified locally on Windows x64 in
  application and Node-only modes, including Workers: tagged slots measured
  4 bytes and compression/31-bit-SMI/sandbox flags reported `1/1/0`. macOS has
  not been exercised for this change.
- This is a reporting correction, not removal of the current compressed
  heap's 4 GiB cage boundary. `--max-old-space-size` cannot bypass that cage.
  A separate noncompressed Node engine remains an explicit architecture
  decision. See [the shared runtime boundary](DEVELOPMENT.md#node-build-configuration-and-heap-boundary).

The final packaged runtime passed nine scenarios covering identity, native V8
layout in both entry modes with Workers, SQLite, full native input in sandboxed
and single-process modes, public clipboard images and console forwarding in
both process modes. The input fixture now uses real same-origin HTTP pages
instead of `srcdoc`; runtime `srcdoc` behavior is unchanged. See the
[Windows verification record](VERIFICATION.md#windows-runtime-0133-2026-11-04).


### Windows runtime 0.1.32

Windows x64-only runtime update from the shared source through macOS `0.1.31`.
The macOS runtime/channel remains `0.1.31`; npm installer remains `0.1.12`.
Installer protocol `1`, Electron API compatibility `41.2.0` and Node ABI `134`
are unchanged.

- Ships typed Blink-to-Win32 cursor updates for BrowserWindow, BrowserView and
  embedded webview guests, including stock/embedded cursor resources,
  hidden/custom RGBA cursors, hotspots and DPI scaling.
- Re-hit-tests native chrome for queued cursor updates and consumes stale
  updates from non-hovered, hidden, disabled or mouse-ignored windows instead
  of resetting another window's cursor.
- Preserves held-button capture routing, including chorded guest dragging,
  stationary CSS cursor changes outside captured view bounds, and native
  capture/focus ownership for offscreen views in single-process mode.
- Converts Win32 physical input to the guest's logical embedding coordinates
  before hit-testing and forwarding, and derives held-button masks from native
  left/right/middle messages instead of assuming a JavaScript button string.
- Applies the late-frame live-owner guard to D3D: a missing view has already
  released the registry lock and must not be unlocked twice.
- Includes shared native SQLite transaction-state and numbered-parameter
  binding repairs, including savepoints, automatic rollback and writer-lock
  isolation.
- Includes the shared lifecycle, console source-location and renderer-pressure
  changes. Windows retains its native Job/crash/pre-OOM implementation;
  macOS-only Seatbelt, POSIX-FD and time-zone repairs are not Windows features.

The installable runtime ZIP and separate matching full-PDB ZIP are assembled
from the same Release build. See the [Windows verification record](VERIFICATION.md#windows-runtime-0132-2026-11-02)
for exercised scenarios and remaining hardware/display limits.

### macOS runtime 0.1.31

Apple Silicon macOS-only runtime update. Windows runtime/channel remains
`0.1.28`; npm installer remains `0.1.12`. Installer protocol `1`, Electron API
compatibility `41.2.0` and Node ABI `134` are unchanged. No renderer or browser
surface changed relative to `0.1.30`; this release carries the native SQLite
binding corrections that were committed after `0.1.30` was published.

- Adds read-only `DatabaseSync.isTransaction`, reporting SQLite's actual
  autocommit state across explicit transactions, nested savepoints, statement
  errors and automatic rollback, and rejecting a closed connection with
  `ERR_INVALID_STATE`.
- Fixes positional binding of numbered `?NNN` slots: anonymous parameters no
  longer skip an existing numbered index, so numbered slots keep their SQLite
  indices when mixed with named parameters, sparse slots or repetitions.
- Extends `test/node-sqlite-smoke.js` with database/transaction state, nested
  savepoints, failed-`BEGIN` writer-lock isolation, rollback recovery and mixed
  numbered/named binding behavior.

The packaged runtime passed the isolated native SQLite regression, including
WAL, read-only access, column metadata, backup, changesets, Worker binding and
extension-loading restrictions. Runtime `0.1.30` and its immutable assets are
unchanged; its numbered-binding gap is superseded by this release.

### macOS runtime 0.1.30

Apple Silicon macOS-only runtime update. Windows runtime/channel remains
`0.1.28`; npm installer remains `0.1.12`. Installer protocol `1`, Electron API
compatibility `41.2.0` and Node ABI `134` are unchanged. Windows cursor adaptation
is included in source, not in a newly published Windows binary.

- Routes Blink cursor types and custom image/hotspot/scale data through renderer
  IPC to Electron BrowserWindow, BrowserView and active webview hosts; no CSS
  injection or application-level pointer override.
- Adds native cursor ownership, caching, hidden/custom cursor lifetime and native
  chrome/resize precedence on macOS and Windows. Removes Windows' fixed-arrow
  cursor-info reset and embeds the existing Chromium cursor resources.
- Preserves every held mouse button in AppKit and shared Blink input snapshots,
  preventing premature pointerup/capture release during chorded guest dragging.
- Includes the macOS renderer system time-zone cache initialization before
  Seatbelt lockdown, preserving local `Date`/`Intl` semantics.
- Adds an unlocked-console native cursor regression with actual AppKit cursor
  and raster observations, stationary updates, guest/view transitions and reset.

The macOS source build and packaged runtime identity, ABI, deep/strict signature,
native pointer-screen coordinates and three local-time/DST scenarios passed.
A real AppKit-view transport inspection passed ordinary/custom cursor
mapping, stationary updates, BrowserView, navigation and multi-button captured
guest routing. It inspected held native objects, not the physical pointer.
The packaged mouse-modifier fixture stopped at `Native window focused: false`
under the locked console; it is not counted as passing.
Final physical hover verification is pending an unlocked console. Windows cursor
compilation/native verification was not run; a portable harness exercised the
actual captured-view routing method, not Win32 resources or cursor presentation.
This is a macOS runtime-only update; no npm publication or Windows-channel update.

### macOS runtime 0.1.29

Apple Silicon macOS-only update. Windows runtime/channel remains `0.1.28` and
npm installer remains `0.1.12`; installer protocol `1`, Electron API compatibility
`41.2.0`, and Node ABI `134` are unchanged.

- Includes the IndexedDB finished-transaction/history release and frame/standalone
  loader ownership repairs already shipped in the shared source.
- Repairs DevTools-free console severity/source forwarding and early
  `web-contents-created`, including destroy/throw owner cleanup.
- Preserves native desktop mouse/wheel coordinates across moved windows,
  negative display positions, BrowserViews, guests, and pointer capture.
- Adds native macOS pressure sampling and coordinated base/Blink, discardable,
  Skia, and all-isolate reclaim, without artificial process/heap hard limits.
- Adds once-only local pre-OOM snapshots with browser-owned byte-stream storage,
  explicit configuration/refusal/error results, bounded serialization, and
  cancellation-before-shutdown. Renderer graph contents are explicitly untrusted.
- Fixes high-FD spawn remapping, Darwin page-rounded shared-memory validation,
  datagram `ENOBUFS` backpressure, and Metal missing-owner double unlocks.

See [verification and existing boundaries](VERIFICATION.md#macos-runtime-0129-2026-10-30)
and [snapshot policy/privacy](MACOS.md#renderer-memory-pressure-and-local-pre-oom-diagnostics).

### Windows runtime 0.1.28

- Ports the embedded fake-indexeddb transaction-finalization fix from
  [upstream #171](https://github.com/dumbmatter/fakeIndexedDB/pull/171). Successful
  and aborted transactions release the database's historical transaction root
  on a subsequent task, preserving upgrade/abort event ordering.
- Fixes same-key cache rewrites retaining historical JSON through transaction
  listeners and rollback closures. No heap/Job limit increase or application
  cache reduction is used to mask the leak.
- Adds a real-runtime lifecycle regression and verifies collection in an
  owning-renderer heap snapshot after 512 large overwrites.

This Windows x64-only runtime update preserves macOS runtime/channel `0.1.25`,
npm installer `0.1.12`, installer protocol `1`, Electron API compatibility
`41.2.0`, and Node ABI `134`. See the
[source-fix verification](VERIFICATION.md#indexeddb-transaction-lifetime-repair-2026-10-27)
for the local pre-release candidate and its exact identity.

### Windows runtime 0.1.27

This Windows x64-only runtime update preserves macOS runtime/channel `0.1.25`,
npm installer `0.1.12`, installer protocol `1`, Electron API compatibility
`41.2.0`, and Node ABI `134`.

- Restores machine-derived V8 heap defaults instead of limiting each renderer
  isolate to half the Job budget. Coordinates the x64 Job's native headroom
  with those defaults, up to 8 GiB; unrelated sandbox restrictions are unchanged.
- Combines independent renderer-commit and main-isolate V8-utilization pressure
  signals, rather than waiting for total process commit alone.
- Adds once-only, local pre-OOM main-isolate heap snapshots at 65% utilization,
  with sandboxed file capabilities, memory/disk guards, bounded serialization,
  explicit failure status, and a sensitive-data/disable policy.
- Fixes unnecessary sidecar rewrites during renderer close that raced profile
  cleanup. Extends real-runtime boundary, capture/refusal and OOM coverage.

The runtime ZIP and separate compressed matching PDB symbols are assembled from
the same release build. See [candidate verification](VERIFICATION.md#windows-memory-policy-candidate-2026-10-27)
and [memory behavior](WINDOWS.md#renderer-memory-diagnostics-and-reclamation).
The original incident's retaining object owner remains unproven; the changed
budget is not proof that long-running heap growth has been eliminated.

### Windows runtime 0.1.26

This Windows x64-only update preserves macOS runtime/channel `0.1.25`,
npm installer `0.1.12`, installer protocol `1`, Electron API compatibility
`41.2.0`, and Node ABI `134`.

- Retires frame-registry entries using the retained token after frame detachment.
  Resource-load notifiers and policy-container implementations now follow their
  frame-client or receiver ownership; standalone navigation loaders release on
  consumer disconnect.
- Publishes a separate, compressed full-PDB attachment for the exact runtime
  executable. Release assembly verifies RSDS GUID/Age and EXE/PDB hashes, rejects
  missing or mismatched symbols, and includes both ZIPs in `SHASUMS256.txt`.
  Symbols stay out of the installable runtime ZIP and npm package.
- The packaged runtime passed frame/IPC, same-document navigation, request-body
  loading, and reentrant event/destruction smokes. The production-object lifetime
  regression and the published npm `0.1.12` installer also passed.

See [release verification](VERIFICATION.md#windows-runtime-0126-2026-10-25)
and [symbol download/debugging instructions](RELEASING.md#using-published-windows-symbols).
The separate OMP Desktop JavaScript cache/closure/ref repairs belong to the
application release, not this native runtime. The previously observed
cross-document-back blank-document issue is not claimed fixed.

### macOS runtime 0.1.25

This Apple Silicon macOS-only runtime update leaves Windows release assets, the
Windows channel, and npm installer `0.1.12` unchanged. Installer protocol `1`,
Electron API compatibility `41.2.0`, and Node ABI `134` are retained.

- Native precise-wheel input supplies both pixel deltas and Blink wheel ticks.
  Legacy DOM `wheelDeltaX/Y` now track logical pixels, including one-pixel Retina
  input, so JavaScript scrollers such as xterm can consume trackpad movement.
  Ordinary mouse-wheel distances remain 80 logical pixels per vertical notch.
- Restores macOS source builds after the source-tree reorganization: Mojo
  platform/system handles and net/URL helpers compile in separate object
  namespaces, and the media promise implementation uses its relocated path.
- Native scroll coverage shows its window without activating it, matching
  visibility-aware renderer input. The WebSocket disposal regression accepts
  macOS broken-pipe errors only after intentional renderer destruction while
  retaining its prompt-close and independent-renderer assertions.

The packaged runtime and the published npm installer were exercised locally.
See [macOS verification and limits](VERIFICATION.md#macos-runtime-0125-2026-10-25).
The app uses ad-hoc signing, not Developer ID signing or notarization.

### Windows runtime 0.1.24

This Windows x64-only runtime update leaves the macOS release/channel and npm
installer `0.1.12` unchanged. Installer protocol `1`, Electron API compatibility
`41.2.0` and Node ABI `134` are retained.

- Windows renderer crashes use a preallocated exception channel and browser-side
  minidump/JSON writing, preserving the actual exception thread and context even
  when normal IPC is blocked. Capture/output errors retain their Win32 codes.
  Builds archive matching EXE/PDB identities and hashes for symbolized stacks.
  Fatal capture and process exit are coordinated before classifying IPC loss;
  the runtime still reads the actual OS exit status rather than synthesizing it.
- OOM diagnostics retain the enforced Job limit, process/Job commit peaks,
  system availability and V8 failure location. Missing Job refusal evidence stays
  `null`/`unknown`; a historical refusal does not override the cause of a later
  unrelated crash. Main-isolate heap samples are explicitly labeled and aged,
  including when the failing thread belongs to a Worker.
- WebSocket receive credit spans native assembly, every task/IPC queue and
  Blink consumption/cancellation. Per-WebContents accounting uses an 8 MiB
  ordinary window, a 68 MiB hard aggregate ceiling and a 4 KiB minimum message
  charge while preserving the 60 MiB individual-message limit. Frame retirement,
  stream cancellation and disposal release only their own outstanding payloads.
- The bundled curl WebSocket decoder preserves FIN, continuation message type
  and interleaved control frames, fixing fragmented text followed by binary data.
- Windows pressure monitoring reaches Blink/V8, discardable storage and Skia
  caches without increasing the existing Job budget. The production discardable
  allocator accounts/frees live storage exactly, preserves locked data, and
  supplies non-null live/discarded allocation dumps to decode-cache consumers.
- Renderer cancellation keeps pending screenshot/reply callbacks on their owner
  sequence and releases their V8 promises before isolate teardown, fixing a host
  access violation when destroying a window and immediately exiting.

Windows build/runtime evidence and limits are recorded in
[verification](VERIFICATION.md#renderer-memory-forensics-and-receive-backpressure-2026-10-24).
Shared WebSocket/allocator code has not been built or run on macOS for this repair.

### Windows runtime 0.1.23

This Windows x64-only update fixes the desktop-coordinate contract behind
[issue #7](https://github.com/iris-cat-dev/mini-electron/issues/7). The macOS
release/channel and npm installer `0.1.12` remain unchanged.

- Mouse/pointer and wheel input retain the browser process's event-time desktop
  DIP coordinates across IPC and input queues; renderer HWNDs no longer stand in
  for the BrowserWindow host when computing native event screen positions.
- Window/widget screen rectangles follow native creation, movement, resizing,
  maximize/restore and display/DPI updates without resetting their origins.
- Windows `setPosition()` and `getPosition()` use desktop DIP consistently with
  `getBounds()`, including non-100% display scaling.
- Adds a PID/HWND-guarded native regression for positive/negative positions,
  move-only updates, framed client offsets, BrowserViews and captured input.

The issue-7 physical acceptance at 150% scaling reused the actual OMP Desktop
drag hook, WindowChromeProvider, preload and window-manager handlers. With a
stationary, physically held pointer, the old runtime produced 49 adjacent
window-position feedback pairs; the fix produced zero. Native dragging and
capture/release remained functional. Cross-display mixed-DPI transitions and
physical negative-origin monitors were not available for verification; this
does not claim to resolve every compositing artifact in the original recording.

### Windows runtime 0.1.22

This release publishes Windows x64 assets only. The macOS release/channel and
npm installer `0.1.12` are unchanged. It includes the shared `v0.1.20`/`0.1.21`
crash hardening and the Windows source-review fixes below.

- Invalid window options and WebContents adoption are rejected before allocating
  a native window. Argument-type diagnostics use the calling V8 isolate, and
  native Mojo handle transfer no longer unconditionally enters a V8 stack tracer.
- Alternate Mojo dispatch validates message headers before delivery and preserves
  transferred-handle ownership. Closed Blob/BytesProvider endpoints complete
  without dereferencing a missing receiver or waiting indefinitely.
- Frame hosts resolve the live view and exact Blink frame token, including during
  frame creation, instead of retaining detached frame-client pointers.
- WebContents uses mutation-safe observer notification and retains its wrapper
  during renderer-event dispatch. A `ready-to-show` callback can synchronously
  destroy its window without invalidating an active observer iterator.
- Hidden startup painting is separate from `ready-to-show` delivery. Navigation
  painting stays leased through its matching commit/paint; unattached BrowserViews
  start logically hidden before Blink creation rather than briefly starting a
  rendering clock.
- D3D12 and Metal offscreen outputs let Viz derive the Display size from the
  submitted root frame before its damage. Resizing only after the frame's ACK
  could leave a static page and its capture permanently using the old backing.
- The build uses the canonical serializers, generated providers and bundled
  dependency implementations rather than duplicate partial providers. Reviewed
  entrypoints without a real backend still fail explicitly.

The stamped `0.1.22` distribution was rebuilt with
`python build.py --jobs 12 --package-dist` and passed 22 actual packaged-runtime
executions across both process modes, including Notification lifecycle checks.
The published npm `0.1.12` installer consumed the assembled release locally,
verified its inventory, and ran native version/ABI and reentrant-event checks.
The release candidate's real D3D12 desktop window was also inspected. Candidate
and installer evidence is under `out/verification/runtime-release-0.1.22/`.

The following source-review results precede the `0.1.22` release version stamp.

On Windows 10 x64 build 19045, `python build.py --jobs 12` completed the native
incremental build, link and staging. This reused cached target libraries; it was
not a clean-room rebuild. The staged `out/windows-x64/stage/mini-electron.exe`
then passed 24 actual process executions across default sandboxed-renderer and
`--single-process` modes, with every exit code checked. Coverage includes
constructor/type failures, reentrant native events, WebCrypto vectors, Blink
core APIs, HTTP/blob uploads, generated downloads, iframe focus/detachment,
fullscreen, startup/visibility and WebGL pixel correctness. Static WebGL windows
were resized through `720x460`, `640x420` and `560x400`; captures had the exact
backing dimensions and retained the expected resource/alpha/worker pixels.
Each process mode also completed 20 window/BrowserView lifecycle, reload,
hide/show and capture cycles with no reported renderer loss. A real desktop
lifecycle screenshot was inspected.

Five console programs were separately compiled, linked against the production
libraries and executed: Mojo ownership/validation/upload sequencing, closed
Blob/BytesProvider endpoints, Skia mesh resource lifetime, Snappy/RE2 boundaries,
and native audio resampling/channel scaling. Evidence, before-fix failures,
native exit codes, binary hashes and screenshots are retained under
`out/verification/crash-review-0.1.20/`.

This review did not rebuild or run macOS/AppKit, exercise an
`ENABLE_BASE_TRACING=1` configuration, or rerun the original packaged OMP Desktop.
The current-source Windows GPU checks used D3D12, not the software compositor.
These finite checks are not proof of indefinite crash freedom or leak freedom.

### macOS runtime 0.1.21

This macOS arm64 release focuses on crash safety, not broader API coverage.
Windows stays on runtime `0.1.18`; the npm installer stays on `0.1.12`:

- Native theme changes invalidate color caches and notify observers on their
  owning sequence instead of executing the inherited low-address write.
- macOS power-monitor initialization and teardown tolerate unavailable IOKit
  notification resources without relaxing the renderer sandbox.
- Fresh stack EVP cipher contexts are initialized before use; cleanup/reset must
  not inspect uninitialized pointers. Empty HKDF input remains a zero-byte key.
- Upload getters retain receiver/reply sequence ownership. Code-cache requests
  complete as real misses instead of accumulating an unbounded process cache.
- Changed entrypoints for absent storage/media backends fail explicitly. Windows
  parent/modal/kiosk/aspect-ratio operations and WebContents adoption are not
  implemented by this change; unsupported requests must not allocate windows or
  pretend to succeed. Windows native-SDK snapshot/print/PDF and recycle-bin
  backends remain unavailable rather than being implemented here.
- Disabled LCP prediction and its protobuf providers remain excluded. Legitimate
  diagnostic assertions are not blanket-disabled.
- Windows-only App creation-event dispatch stays out of the macOS build. The
  shared WebContents factory keeps its checked construction/lifetime guards
  without adding a second App implementation or new macOS event coverage.

Targeted macOS checks cover AppKit appearance notifications, typed Mojo lifetime
and upload sequencing, malformed IPC rejection, actual WebCrypto vectors and
invalid inputs, native event cancellation, and HTTP/blob/streamed upload paths.
The unmodified OMP Desktop 0.5.2 main/frontend and real daemon also ran for
183 seconds with isolated state: nine resizes, four hide/show cycles, two reloads,
and two real new-window/close cycles. No renderer loss or renderer-console errors
were observed; initial/final native screenshots were inspected and the owned
daemon shut down cleanly. This finite workload is not a leak-freedom proof.
These checks do not prove indefinite uptime. The Windows crash review above is
separate from this published macOS acceptance.
Full HTTP Permissions-Policy propagation is not established by this acceptance:
the current native path reports the tested features allowed despite a denying
response header. This change does not claim to complete that browser capability
or the unexposed optional media/storage APIs.
The existing HTTP/WebSocket curl configuration also disables TLS peer/host
verification. That independent security risk remains unchanged, not hidden by
this crash-hardening change.

### macOS runtime 0.1.20

This release publishes Apple Silicon macOS assets only. Windows remains on
runtime `0.1.18`; the npm installer remains `0.1.12`.

- Replaces the recurring 10 ms AppKit/libuv poll with native I/O readiness and
  exact timer deadlines while preserving main-thread callbacks and orderly quit.
- Suspends hidden/minimized windows, detached BrowserViews and their webview
  guests; restores rendering and interaction when they become visible again.
- Preserves hidden first-paint/`ready-to-show`, explicit `prepareToShow()` and
  `backgroundThrottling: false` behavior. Native occlusion uses AppKit's reported
  state, not a geometry-based guess.
- Enables Blink threaded CSS animation without capping visible animation or
  disabling GPU rendering.

The Windows DPAPI and preload source fixes are not delivered by this macOS-only
release; Windows runtime publication remains pending. Targeted Windows native
verification and its limits are recorded below.
See [macOS idle scheduling verification](RENDERING_PERFORMANCE.md#macos-idle-scheduling-and-visibility)
for measured workload results, native regressions and verification limits.

### Windows runtime 0.1.18

This release publishes Windows x64 assets only. The macOS channel remains on
runtime `0.1.16`; the npm installer remains `0.1.12`.

- Adds opt-in `--single-process` mode for trusted UI, with multiwindow IPC,
  storage, capture, teardown, and reopen support. Default mode keeps renderers
  in separate sandboxed processes.
- Root frames are submitted only for actual geometry/surface changes. BeginFrames
  carrying only presentation feedback or acknowledgements no longer trigger
  another full-window redraw.
- Minimized/hidden windows and detached BrowserViews stop Blink/Viz rendering;
  showing, restoring, and reattaching resumes it. Initial `show: false` windows
  retain their existing first-paint/`ready-to-show` behavior.
- Windows x64 renderer jobs use one quarter of physical RAM, bounded to 1–4 GiB,
  instead of a fixed 1 GiB cap. Each renderer Blink/Worker isolate configures V8
  to use at most half the enforced job budget, reserving native-memory headroom.
  Concurrent workers still share the aggregate process limit. Host Node and
  explicit single-process mode retain their previous heap policy.
- `render-process-gone` preserves the real exit code and distinguishes OOM,
  crashes, normal/abnormal exits, and IPC failures. Windows OOM status
  `0xe0000008` is no longer reduced to an unexplained `process-exit`.

The rebuilt Windows binary passed static/animated visibility transitions in GPU
and software modes, native composition/restore/resize, and disposable renderer
OOM verification. See [idle and memory verification](RENDERING_PERFORMANCE.md#windows-idle-rendering-and-memory-budget-correction).

### Windows runtime 0.1.17

This release publishes Windows x64 assets only. The macOS channel remains on
runtime `0.1.16`; the npm installer remains `0.1.12`.

- Restores real WebGL 1/2 contexts, Blink bindings, GPU drawing, CPU image
  interop, context loss/restoration, and Worker OffscreenCanvas publication.
  The macOS backend uses ANGLE/Metal; Windows ANGLE/D3D11 now also passes
  the rendering smoke in both output-compositor modes.
  See [WebGL support and limits](GRAPHICS.md#webgl-1-and-2).
- Restores native WebGPU through renderer-local Dawn wire/native channels,
  WGSL pipelines, buffer mapping, GPUCanvasContext, OffscreenCanvas and Workers.
  macOS Metal rendering and the original OMP Desktop WebGPU/WASM homepage
  animation are verified. Windows Dawn/D3D12 now compiles and passes the
  rendering/compute smoke. See [WebGPU support and limits](GRAPHICS.md#webgpu).
- Fixes Windows WebGPU bootstrap failures, missing native GPU dependencies,
  obsolete placeholder/linker collisions and the GPU texture-format binding
  crash. CPU VideoFrame construction and RGB/YUV copies are available in
  Window and DedicatedWorker contexts; hardware video decoding is not added.
- Fixes Windows WebGL implicit MSAA extension activation and serializes ANGLE
  D3D11 calls/state changes across Blink and Worker threads to prevent
  readback crashes and hangs from the shared immediate context.
- Keeps macOS source builds compatible with the shared Windows horizontal-wheel
  routing by completing the POSIX Win32 message definitions.

## Earlier runtime releases

macOS runtime `0.1.10` adds native Skia/Metal composition and presentation while
retaining CPU tile rasterization, display-aware 60/120 Hz VSync, native image
and editable-field context menus, and generated `data:`/`blob:` downloads.
It also fixes mixed GPU/CPU guest composition, resize completion, image alpha,
macOS UI-thread identification, and shutdown lifetime. See
[rendering verification and remaining limits](RENDERING_PERFORMANCE.md#macos-metal-gpu-compositing).

macOS runtime `0.1.11` increases mouse-wheel scrolling to 120 logical pixels per
vertical notch, preserves multi-notch input, and accepts ImageIO-supported native
clipboard image formats such as JPEG and multi-representation TIFF. Trackpad
pixel scrolling remains unchanged.

macOS runtime `0.1.12` also accepts native image bytes whose encoding differs
from their pasteboard type, including QQ's PNG data advertised as TIFF. ImageIO
identifies and validates the actual encoding; existing byte, dimension, and
pixel limits remain enforced. Valid PNG bytes are retained without re-encoding.

macOS runtime `0.1.13` sets ordinary mouse-wheel scrolling to 80 logical pixels
per vertical notch. Multi-notch distances remain proportional, and precise
trackpad scrolling is unchanged.

The `0.1.9` macOS runtime fixes trackpad scrolling by preserving AppKit's
precise logical-pixel deltas instead of multiplying them into wheel ticks.
Traditional mouse wheels continue to use 120-unit tick deltas. The executable
is stripped (`strip -x`), ad-hoc signed, and not notarized.
Version `0.1.5` includes the macOS selection, clipboard image paste, IME
positioning/composition colors, sidebar resizing, and image-preview IPC fixes.
Version `0.1.6` adds the macOS native `node:sqlite` binding, including column
metadata, backup, session/changesets, and Worker support. It also keeps Win32
keyboard-layout APIs Windows-only, links the shared keycode conversion on macOS,
and preserves native AppKit keyboard metadata.
Windows runtime `0.1.14` adds GPU acceleration and VSync, and fixes deadlocks and
white flashes during repeated native edge resizing. DirectComposition retains
the previous surface until the replacement is ready, initializes both flip
buffers, and avoids blocking the UI thread on GPU completion. The GDI fallback
stretches the retained frame to cover the resized viewport.
Windows runtime `0.1.15` fixes frame-latency waitable handle growth during
continuous DirectComposition presentation. Active and pending resize swap chains
own their handles until retirement, without blocking first-frame GPU completion.
Earlier Windows `0.1.8` fixes remain included: filesystem identity, temporary
directories, navigation, transparent companion windows, pointer-hover repaints,
and the ASAR-enabled `child_process` promisification contract.
