# Verification and known boundaries

[Documentation index](README.md) · [Project overview](../README.md)

> Run commands from the repository root unless a section explicitly changes directory.

Each record applies only to its stated source layout, runtime, platform and
workload. Older records migrated from the root README are not evidence of a new
verification run. Release-specific evidence is in [release notes](RELEASES.md).

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
