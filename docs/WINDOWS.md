# Windows build and runtime

[Documentation index](README.md) · [Project overview](../README.md)

> Run commands from the repository root unless a section explicitly changes directory.

Shared examples and runtime behavior: [development guide](DEVELOPMENT.md). Runtime installation: [installation guide](INSTALLATION.md).

## Windows (x64)

Requirements:

- Visual Studio 2022 with the Desktop development with C++ workload (v143) and
  the matching C++ ATL x64 headers and libraries
- Windows SDK `10.0.26100.0`
- LLVM 22 with `clang-cl`, `lld-link`, and `llvm-lib`; install in the standard
  LLVM location or set `LLVM_INSTALL_DIR`
- Python 3.12 or newer, plus GN and Ninja on `PATH` (or
  `MINI_ELECTRON_GN` / `MINI_ELECTRON_NINJA` executable paths) for ANGLE/D3D11
- CMake 3.24 or newer for Dawn/D3D12

The backend selects the newest installed v143 toolchain with matching ATL and
passes that exact `VCToolsVersion` to MSBuild. A stale Visual Studio default must
not select a different toolchain with missing `atldef.h` or ATL libraries.

NASM 2.16.03, pinned VSNASM customizations, and nghttp2 1.61.0 sources are
downloaded into `.build-tools` with SHA-256 verification. The Windows Node
target compiles nghttp2 for HTTP/2 support. No sibling checkout or prebuilt
demo EXE is used.
The restored Windows inputs originate from MiniBlink revision
`191f82c33fc0ff9ddaeedab90e6406a54a8e76c6`; they compile the current shared engine
sources, including the macOS changes behind their platform guards.

The application Release build must use the same V8 check configuration as the
V8 and Blink Release libraries. Defining `V8_ENABLE_CHECKS` only for the executable
instantiates cppgc tracing calls that those libraries do not provide.

The Windows `Release|x64` build disables Blink/V8/Node Inspector and DevTools.
`process.features.inspector` is `false`; the `node:inspector` modules and
`webContents` DevTools/inspection methods are unavailable. The F12 shortcut and
developer-tools menu entries are removed. Ordinary JavaScript error stacks,
console messages, page/worker error reporting, and storage events remain active;
source locations use native V8 stack frames without an Inspector session.
The experimental `<permission>` implementation and binding are excluded:
the tag creates an `HTMLUnknownElement`, with no `HTMLPermissionElement`
constructor. Ordinary permissions-policy handling is retained.

```sh
python build.py
python build.py --run
```

The first command builds and packages
`out/windows-x64/mini-electron.exe` plus `resources/app`. The second
also opens a native window and verifies graceful exit after `WM_CLOSE`.
Run the package with its directory as the working directory, or use
`python build.py --electron --interactive`.

Embedded Electron JavaScript and default-app resources are generated from
`src/runtime/electron/lib` during the build, with explicit resource byte lengths.
Generated headers live under `generated/runtime/electron/common` in the build
output, rather than being checked into the source tree. Windows object paths
distinguish same-named source files; long paths use a stable SHA-256 filename to
stay below FileTracker's path limit. The backend prefers native x64 MSBuild.
Windows does not currently provide the macOS-only browser demo or screenshot
option. OMP Desktop packaging is supported on both desktop targets.

Use the freshly built executable for source verification, not a stale
`runtime-dist/electron.exe`. `python build.py --package-dist` produces the
declared Windows runtime distribution, ZIP, and matching archived EXE/PDB pair;
publishing those assets is a separate [release step](RELEASING.md).

Windows defaults to Skia Ganesh/D3D12 GPU compositing when hardware setup
succeeds. Shared textures and fences feed retained D3D11 textures and
DirectComposition swapchains in the browser process; ordinary presentation
does not read the page back to CPU memory. Intermediate render passes are
GPU-backed; DOM/layout and cc tile rasterization remain CPU work. This does
not enable Chromium's GPU service, WebGL, or WebGPU.

Native-composition windows select `WS_EX_NOREDIRECTIONBITMAP` at creation,
including transparent companion windows. Premultiplied alpha, `skipTaskbar`,
nonactivating display, and mouse passthrough do not require another HWND.
Initial renderer GPU failure can use CPU layers through DirectComposition.
`--disable-gpu`, `--disable-gpu-compositing`, or
`app.disableHardwareAcceleration()` before creating windows selects the
legacy CPU/GDI presentation path. `MINI_ELECTRON_DISABLE_GPU=1` selects CPU
renderer composition while leaving eligible native presentation enabled.

Each DirectComposition swap chain owns one frame-latency waitable handle for
its lifetime. Presents borrow that handle instead of acquiring another one
per frame. Active and pending chains retain separate ownership; cancellation,
replacement, and destruction stop any native wait before releasing the handle.
First-frame GPU completion remains asynchronous during resizing.
Queued D3D publications may outlive their view. The live-view registry lookup
already releases its lock on a missing owner; frame completion must unlock only
a successful lookup, matching the Metal ownership rule.

Windows frame scheduling follows the selected monitor's DXGI VBlank clock,
with validated DWM timing for the primary monitor or a refresh-calibrated
timer when hardware timing is unavailable. Non-blocking successful DXGI
waits are rejected rather than generating a busy loop. BrowserViews and
webviews inherit the host monitor; moving between monitors updates the clock.

Windows hidden-title windows honor `titleBarStyle`, `titleBarOverlay`, and
`setTitleBarOverlay()`. Minimize/maximize/restore/close controls are rasterized
into a cached premultiplied-alpha Skia surface and composited over the painted
HTML titlebar. Short/long RGB and RGBA hex colors retain their alpha, so
`#00000000` exposes the real header rather than becoming an opaque black patch.
Repainting clears cached caption pixels. GPU presentation uploads the caption
only when it changes; CPU fallback blits use a complete cropped DIB so partial
invalidation cannot select the wrong underlying rows. Opaque borderless
windows retain their native DWM shadow without a rounded window region;
maximized client bounds match the visible work area. CSS
app-region dragging and no-drag elements remain active. `autoHideMenuBar`,
explicit menu visibility, Alt activation, menu replacement, and null clearing
are supported; ordinary framed windows retain their native title bar and menu.
Native close transitions to the destroyed state before `closed` listeners run;
`isDestroyed()` reports that state rather than retaining a live-window result.

The Windows host declares Per-Monitor V2 DPI awareness and initializes its native
screen before creating the first window. Window bounds/positions and viewport sizes remain in DIP;
Viz renders physical pixels at the window's device scale; native textures and
CPU fallback bitmaps are presented 1:1. Native mouse **widget** coordinates stay in
device pixels until Blink applies that scale. Native **screen** coordinates are
snapshotted in the browser process and converted to desktop DIP before crossing
the renderer boundary; pointer, mouse, and wheel events retain that snapshot even
if application code moves the window while input is queued. Page zoom is
independent of display scaling; factor/level conversion preserves
`setZoomFactor(1)` as a true reset.

The Windows coordinate fix for [issue #7](https://github.com/iris-cat-dev/mini-electron/issues/7)
also publishes distinct native client/widget and outer-window screen rectangles
on creation, move-only updates, resize, display/DPI changes, and maximize/restore.
Renderer resize must not reset their origins to zero. BrowserViews and embedded
guests retain the host outer window rectangle and their own widget origin.
`setPosition()` converts desktop DIP to native screen pixels, and `getPosition()`
converts back using the window's display, consistently with `getBounds()`.
OMP Desktop's restore-drag logic requires these desktop coordinates; substituting
client coordinates creates a position feedback loop rather than a valid drag.

Native Windows wheel input preserves partial and multiple 120-unit notches.
Vertical lines and horizontal characters follow `SPI_GETWHEELSCROLLLINES` and
`SPI_GETWHEELSCROLLCHARS`, rather than imposing the macOS 80-pixel wheel policy.
`WM_MOUSEHWHEEL` is routed through BrowserWindow and BrowserView, with positive
native horizontal deltas scrolling right. A page-scroll setting uses page
granularity; regressions read these settings without changing them. The engine
wheel entry point now accepts independent X/Y tick deltas, so C/C++ embedders
must rebuild against the updated `engine_api.h`.

The renderer receives the full Windows preferred UI-language list. Language
configuration made before asynchronous Blink view creation is retained and
applied with the initial renderer preferences, before the first document attaches.
It must neither dereference the uninitialized view nor discard the configured
languages in favor of platform defaults.

Windows renderer editing uses an authenticated clipboard broker; no direct OS
clipboard privileges are granted to the sandbox. Text and HTML commit together,
including explicitly empty text. PNG and bounded native DIB/DIBV5 images become
real DOM PNG files; malformed images are rejected, and an invalid registered PNG
does not hide a valid DIB. DIB conversion completes on the browser thread without
pumping a nested message loop while a renderer is waiting synchronously.
The public main-process `clipboard.readImage()` uses this same PNG/DIB/DIBV5
pipeline, including malformed-PNG fallback and bounds of 32 MiB encoded PNG,
8192 pixels per dimension, and 40 × 1024² decoded pixels. Native PNG metadata
and pixels are validated with one codec before returning or brokering the bytes.
macOS-only ImageIO JPEG/TIFF pasteboard behavior is not claimed for Windows.
Keyboard descriptors retain scan code, layout, modifiers and keyup state;
disabled and `registerAccelerator: false` shortcuts reach the DOM instead of
invoking a menu action. Native click counts honor the Windows double-click time
and distance on both press and release.

Dark inactive selections use a readable foreground without changing light-scheme,
active, forced-color, or author `::selection` behavior. `webContents.copyImageAt()`
copies decoded pixels; PNG/DIB images pasted alongside HTML/URL data remain readable
DOM PNG files, while HTML/URL-only data does not create an image attachment.
Windows IMM32 composition updates and commits reach the focused Blink editor once.
Candidate anchors and a hidden native system caret use its asynchronous physical-pixel
caret snapshot, including BrowserView offsets, without applying DPI/page zoom twice.
Geometry follows page/editor scrolling and window movement; cancel, blur, non-editable
focus, and BrowserView detach clear stale native state.
The native IME context is associated only while Blink reports an editable text
client; moving to non-editable content clears the association so refocusing an
editor cannot resume a stale Pinyin preedit.
Unstyled Windows preedit text uses a translucent neutral-gray background, preserving
the editor foreground in light and dark schemes. Explicit IME span colors and
selection/forced-color handling are unchanged.

The Windows Node runtime compiles and explicitly registers `node:sqlite` against
the bundled SQLite library, with column metadata and session/changeset support.
The same binding is available in standalone Node mode and Workers. Extension
loading remains disabled unless explicitly allowed when the database is created.
The native regression exercises OAuth-style account filtering, WAL, read-only
`DatabaseSync.isTransaction` across savepoints, errors and automatic rollback,
numbered/named parameter binding, writer-lock isolation, read-only access,
metadata, backup, changesets, Workers, and extension restrictions.

Windows file identity uses the same volume serial for `stat`/`lstat` and `fstat`.
Number and bigint stats agree across synchronous, callback, and promises APIs;
replacing a file remains distinguishable from the original open handle.
This preserves application identity checks, including guarded desktop-pet asset loading.

Windows libuv and c-ares use the system `SystemFunction036` (`RtlGenRandom`) source.
The removed stack-address-seeded generator repeated temporary-directory suffixes,
so `mkdtemp` could loop on an existing directory and block OMP Desktop agent history
loading before its OMP process started. Repeated and concurrent temporary-directory
creation is covered across sync, callback, and promises APIs.

The ASAR compatibility layer is initialized even for unpacked `resources/app` applications.
Its `child_process.exec` and `execFile` wrappers preserve Node's custom promisification:
success returns `{ stdout, stderr }`, failures retain both streams, and the Promise's
`.child` is the actual child process. ASAR executable rewriting remains active.
Changes to these wrappers must update both `src/runtime/electron/lib/common/asar.js`
and the embedded `src/runtime/electron/common/asar/asar_js.cc` initializer and its length.

Run the filesystem and child-process contracts in both runtime modes (Git Bash):

```sh
./out/windows-x64/mini-electron.exe --app=test/node-file-identity-smoke.js
ELECTRON_RUN_AS_NODE=1 ./out/windows-x64/mini-electron.exe test/node-file-identity-smoke.js
./out/windows-x64/mini-electron.exe --app=test/node-temp-directory-smoke.js
ELECTRON_RUN_AS_NODE=1 ./out/windows-x64/mini-electron.exe test/node-temp-directory-smoke.js
./out/windows-x64/mini-electron.exe --app=test/node-child-process-smoke.js
ELECTRON_RUN_AS_NODE=1 ./out/windows-x64/mini-electron.exe test/node-child-process-smoke.js
```

Run the real Windows input regressions sequentially on an unlocked interactive desktop:

```sh
python build.py --jobs 16
./out/windows-x64/mini-electron.exe --app=test/windows-gpu-compositor-smoke.js
./out/windows-x64/mini-electron.exe --app=test/windows-swapchain-handle-smoke.js
MINI_GPU_SMOKE_EXPECT_SOFTWARE=1 ./out/windows-x64/mini-electron.exe --disable-gpu --app=test/windows-gpu-compositor-smoke.js
MINI_GPU_SMOKE_EXPECT_SOFTWARE=1 MINI_GPU_SMOKE_DISABLE_WITH_API=1 ./out/windows-x64/mini-electron.exe --app=test/windows-gpu-compositor-smoke.js
MINI_ELECTRON_DISABLE_GPU=1 MINI_GPU_SMOKE_EXPECT_SOFTWARE=1 MINI_GPU_SMOKE_EXPECT_NATIVE_CPU=1 ./out/windows-x64/mini-electron.exe --app=test/windows-gpu-compositor-smoke.js
./out/windows-x64/mini-electron.exe --app=test/windows-gpu-guests-smoke.js
./out/windows-x64/mini-electron.exe --app=test/execute-javascript-smoke.js
./out/windows-x64/mini-electron.exe --app=test/generated-image-download-smoke.js
./out/windows-x64/mini-electron.exe --app=test/windows-clipboard-image-smoke.js
./out/windows-x64/mini-electron.exe --app=test/windows-wheel-smoke.js
./out/windows-x64/mini-electron.exe --app=test/windows-context-menu-smoke.js
./out/windows-x64/mini-electron.exe --app=test/windows-native-input-smoke.js
./out/windows-x64/mini-electron.exe --app=test/windows-editor-smoke.js
./out/windows-x64/mini-electron.exe --app=test/windows-caption-overlay-smoke.js
./out/windows-x64/mini-electron.exe --app=test/windows-hover-presentation-smoke.js
./out/windows-x64/mini-electron.exe --app=test/windows-restore-presentation-smoke.js
./out/windows-x64/mini-electron.exe --app=test/windows-companion-smoke.js
./out/windows-x64/mini-electron.exe --app=test/windows-screen-coordinates-smoke.js
./out/windows-x64/mini-electron.exe --app=test/windows-pointer-screen-coordinates-smoke.js
./out/windows-x64/mini-electron.exe --app=test/same-document-navigation-smoke.js
./out/windows-x64/mini-electron.exe --app=test/windows-pointer-interaction-smoke.js
./out/windows-x64/mini-electron.exe --app=test/windows-image-preview-smoke.js
./out/windows-x64/mini-electron.exe --app=test/windows-composition-colors-smoke.js
ELECTRON_RUN_AS_NODE=1 ./out/windows-x64/mini-electron.exe test/node-sqlite-smoke.js
```

The Windows input tests compile a shared WinForms helper with the system .NET Framework 4 C# compiler
and send real keyboard, pointer, and OLE input. Clipboard-mutating tests snapshot
and restore the original clipboard, refusing formats that cannot be preserved losslessly.
The caption, hover-presentation, restore-presentation, companion, screen-coordinate,
navigation, pointer, image-preview, and composition-color regressions never read or change clipboard content.
The pointer-screen-coordinate regression uses a separate PID/HWND-guarded native
message helper. It compares trusted DOM coordinates with native `ClientToScreen`
and window geometry, including negative/positive positions and non-100% DPI.
It does not send global physical input or change display/input settings.
Set `MINI_POINTER_SCREEN_COORDINATES_RESULT` to retain its structured evidence.

Runtime `0.1.32` uses typed Blink cursor notifications. While buttons
are held, the captured BrowserView or main WebContents owns the cursor; otherwise
the topmost BrowserView/active webview under the pointer wins, falling back to the
window's own WebContents. Cursor selection does not require keyboard focus; native
non-client chrome/caption controls and frameless resize borders take precedence.
Occluded, hidden, disabled and mouse-ignored windows cannot steal the cursor.
Stock Win32 cursors cover standard types; checked-in Chromium `.cur` resources
cover grab/grabbing, zoom, copy/alias, cell, vertical text, row/column resize and
panning. `cursor:none` uses a null handle. Owned custom handles retain the image
and hotspot, and are resampled only when the image or monitor DPI changes.
Chorded input preserves all held buttons rather than releasing capture early.
Stationary CSS updates retain the captured DOM target outside the view bounds.
Offscreen views never take native focus/capture with a null HWND. Native guest
input converts host physical pixels to logical embedding coordinates before
hit-testing/forwarding; the guest's existing 1x backing-store policy is unchanged.
Held-button masks also honor native Win32 button messages.
Queued renderer updates re-hit-test native chrome and cannot reset another
window's cursor when their originating window is not hovered, or is hidden,
disabled or mouse-ignored.
Run `test/windows-cursor-smoke.js` with the rebuilt runtime on an unlocked,
unobscured desktop. Its separate C# helper observes real cursor bitmaps,
hotspots and native input; it restores its pointer position and held buttons.
It covers standard/resource/custom cursors, stationary updates, native
caption/resize precedence, BrowserView and chorded guest capture, inactive
hover, stale-window updates and navigation reset. Mixed-DPI transitions run
only when a second differently scaled display is available.

Local issue-7 verification on Windows at 150% scaling passed 55 native-window/DOM
checks in default GPU and single-process software modes, including move-only,
negative/positive positions, resize, maximize/restore, framed client offsets,
BrowserView input, pointer capture/release, and position API DIP roundtrips.
A separately authorized physical-input fixture reused OMP Desktop 0.5.2's actual
drag hook, WindowChromeProvider, preload, and window-manager handlers without a
daemon or application workaround. Four held movements produced 100 actual
position updates with the installed 0.1.22 runtime; 49 adjacent updates moved the
window while the cursor was unchanged. The local fix produced nine updates and
zero stationary-cursor feedback pairs. Native button/capture state stayed held
through all ten stationary samples; one press/capture/release/capture-loss sequence
was observed. Normal native dragging also preserved the physical pointer delta,
and input cleanup restored the cursor and released the button.
This is a local unreleased-runtime check, not a full installed-application run.
Only one 150%-scale display was available: cross-display mixed-DPI transitions
and physical negative-origin monitors remain unverified.

The editor and composition-color regressions
require Microsoft Pinyin already selected. The editor regression samples active/inactive
selection pixels, decodes pasted image Files, and checks native/DOM caret geometry,
composition, cancellation, selection replacement, scrolling, zoom, and BrowserView focus.
It never changes the system input source, theme, or high-contrast settings. Set
`MINI_NATIVE_INPUT_ARTIFACTS` to retain native screenshots and a JSON editor report.
The caption regression samples transparent, partial-alpha, and opaque native pixels,
checks hover clearing and both restore-square outlines while maximized under opaque,
transparent, and hovered overlays, and clicks minimize/maximize/restore/close controls.
Closing must emit each lifecycle event once and expose the destroyed state to its listeners.
Opaque hidden-titlebar windows compose webpage content, BrowserViews, and native
caption controls in a reused backing DIB before presenting the dirty region.
Each Windows WebContents retains an immutable, seqlock-validated source frame and
reuses separate BGRA crop storage. Failed reads leave the stable source untouched,
so a busy renderer cannot cause native caption controls to blend over their own
previous presentation. Renderer damage remains clipped through BrowserWindow and
BrowserView invalidation instead of turning a row hover into a whole-window repaint.
The caption regression continuously samples all three glyphs during real pointer
sweeps in restored/maximized windows with opaque and transparent overlays.
The hover-presentation regression measures physical-pointer-to-display latency and
samples once-composited caption backgrounds under continuous renderer publication.
Hidden renderer context-menu initialization never activates a foreground window.
Minimization preserves the real content and renderer viewport sizes; restoration
synchronously presents retained webpage and BrowserView pixels without waiting for
a fresh renderer frame. Its native desktop sampler covers normal and maximized
restores of both native-frame and opaque hidden-titlebar windows.
`showInactive()` shows without minimizing or activating the window. Showing hidden
static content publishes its retained frame immediately, including per-pixel-alpha
companion artwork; no opaque background is added to transparent windows.

Window construction, topmost/frame changes, and passive geometry setters explicitly
use `SWP_NOACTIVATE`; geometry-only changes also preserve Z-order. Nonfocusable
pointer handling does not call native `SetFocus`. The companion regression first
observes the application's ordinary main HWND as foreground, then checks the exact
foreground HWND after construction, loading, showing, and each passive setter,
without companion input or a corrective refocus. It also checks native visibility,
minimized state, topmost/NOACTIVATE/tool-window styles, and taskbar exclusion.
These source fixes for issue #4 passed native Windows foreground/style acceptance
with the rebuilt source executable. The published Windows `0.1.8` artifact is
unchanged and does not gain these fixes without a new native release.

Windows `screen` display bounds, work areas, matching/nearest-point queries, and
cursor positions use the same DIP coordinate space as BrowserWindow bounds.
Native monitor scale factors come from ScreenWin, avoiding a second DPI scaling
step when positioning a default bottom-right companion on a scaled display.
The screen-coordinate regression checks that default placement against physical
monitor bounds and samples real transparent/artwork pixels.
Companion `skipTaskbar` is applied during construction and can be changed at runtime
without overwriting layered or topmost styles. Nonfocusable Windows windows imply
taskbar exclusion; restoring focusability respects the explicit `skipTaskbar`
preference. Runtime changes also update an existing Shell taskbar entry.
Programmatic initial `loadURL()` does not emit cancellable `will-navigate` or
`will-frame-navigate` events, while renderer-initiated navigation remains cancellable.
The pointer regression checks full-edge hover and captured pane drags on both axes
without moving the native window. The image regression persists and decodes a
491,978-byte PNG through nested sliced binary IPC, verifies exact bytes and bounds,
and checks real mouse input and timers afterward. The composition-color regression
samples native marked-text pixels in both schemes and exercises cancellation/commit.
Inspector/CDP and renderer permission bypasses are not used.

CSS drag rectangles reach the native window through the renderer callback and
authenticated IPC. Win32 `WM_NCHITTEST` returns `HTCAPTION` only for eligible drag
regions; no-drag holes win even when rectangles overlap. Native caption handling
replaces the obsolete synthetic `SC_MOVE` and double-click tracking paths.

### Renderer memory diagnostics and reclamation

Windows x64 Blink/Worker isolates use V8's machine-derived
`ResourceConstraints::ConfigureDefaults(physical, virtual)` policy, not half of
the renderer Job limit. Let `H` be that policy's maximum old-generation plus
young-generation bytes. The sandboxed renderer Job budget is
`clamp(max(physical RAM / 2, 2 * H, H + 2 GiB), 2 GiB, 8 GiB)`.
On the verified 32-GiB workstation this gives an approximately 4-GiB V8 heap
and an 8-GiB aggregate process limit, leaving room for Blink/Oilpan, Skia and
other native allocations. Workers share that process limit; this is not
aggregate heap admission control. The 32-bit Job remains 1 GiB. Explicit
single-process mode does not become sandboxed or crash-isolated.

An independent, preallocated crash channel copies the renderer's exception
record, actual failing thread and register context. The browser writes a
minidump and diagnostic JSON under `app.getPath('crashDumps')`; applications
can select that directory with `app.setPath('crashDumps', directory)` before
creating their renderer. Dump writing does not depend on the normal IPC queue
or allocation in the failing renderer.

`render-process-gone` retains `reason` and the actual unsigned `exitCode`, and
adds the captured diagnostics:

- `crashCaptureStatus`, `crashDumpPath`, `crashDiagnosticsPath`, and explicit
  `crashDumpError` / `crashDiagnosticsError` Win32 errors when output fails.
- `exceptionCode`, `exceptionThreadId`, `rendererPid`, and
  `failedAllocationBytes` when the exception supplied an allocation size;
  otherwise that size is `null`, not a guessed zero.
- `memory.processMemoryLimit`, current private bytes, process/Job commit peaks,
  physical availability, and system commit limit/availability.
- `v8.location`, `detail`, `isHeapOOM` and `failureThreadId` when V8 reported
  a fatal allocation failure. Periodic heap/external-byte samples are explicitly
  labeled `sampleKind: "last-main-isolate"` with their age, or `"unavailable"`.
  A worker failure does not turn the last main-isolate sample into worker data.
- `image.pdbGuid`, `pdbAge` and `pdbName`, plus PE image identity, for selecting
  matching symbols. Windows builds archive the verified EXE/PDB pair beneath
  `out/windows-x64/symbols/<pdbName>/<GUID><hex-age>/` with hashes in `symbols.json`.
  The minidump includes module symbol identity and an embedded diagnostic stream.
- `preOomHeapSnapshot`: once-only main-isolate capture status, Win32 error/reason,
  trigger/capture heap samples, serialized bytes, and a path only after the host
  atomically commits a complete snapshot. A local JSON sidecar records attempted
  capture progress and terminal status without waiting for a crash.

`oomSource` distinguishes a matching V8 heap-limit report from a V8 allocation
failure and an unknown source. It does not derive the cause from the mere
presence of PartitionAlloc/V8 frames. `memoryLimitEvidence: "job-notification"`
means a real Job process-memory refusal was observed; absent evidence stays
`"unknown"` and `sandboxMemoryLimitExceeded` stays `null`, never `false`.
The refusal flag is historical for that renderer; its age is reported, and it
does not reclassify a subsequent unrelated native exception as OOM.

The Blink-thread monitor samples renderer private commit, the enforced Job
limit, and main-isolate V8 used/limit bytes every second, and also observes
Windows low-memory notifications. Process and V8 utilization each enter
moderate/critical at 70%/85%, with independent 60%/75% exit hysteresis. The
stronger signal wins; system low-memory always requests critical pressure.
Moderate re-entry and sustained critical notifications remain rate-limited to
30/60 seconds. Pressure reaches Blink, V8 isolates, discardable memory and Skia
caches. The discardable allocator preserves locked data, frees unlocked
buffers on purge, and reports discarded relock failure for reconstruction.
Tight non-yielding JavaScript can still delay sampling; pressure cannot free
reachable application objects and is not an OOM guarantee.

Automatic pre-OOM snapshots trigger at 65% of the main isolate's V8 limit
(about 2.6 GiB for a 4-GiB heap). The browser creates a trusted temporary file
under `app.getPath('crashDumps')` and duplicates a write-only handle into the
sandbox; the renderer cannot choose an output path. V8 profiling/serialization
runs synchronously on its owning Blink thread, never in the fatal OOM callback.
The host finalizes `renderer-pre-oom-*.heapsnapshot` and its JSON status sidecar.
No snapshot is attempted merely because a renderer starts or closes.

Capture requires fresh memory-budget samples and free physical memory, system
commit and Job headroom of at least the greater trigger/capture heap usage plus
256 MiB. A delayed request is skipped if the heap has already exceeded 85%.
Progress and output chunks check these budgets and a cooperative 30-second
deadline. Serialization is capped at 2 GiB; one host reserves at most four
automatic snapshot/temporary files in a directory and requires 2 GiB free disk.
It never deletes existing snapshots to make space. Failed/aborted attempts do
not publish a completed snapshot, and the same renderer does not retry.

These guards do not bound V8's internal snapshot-graph allocations or guarantee
cancellation of stalled kernel I/O. Capture can pause the UI and increase memory.
Non-yielding code may reach OOM without any pre-OOM snapshot. Main-isolate
snapshots do not diagnose a separate Worker's heap.

`MINI_ELECTRON_PRE_OOM_SNAPSHOT_PERCENT` is read only by the trusted browser
before launch: unset means 65, values 1–65 lower the trigger, and 0 disables
automatic capture. Invalid values report `unavailable` / `invalid-configuration`.
Heap snapshots can contain source, paths, conversation text and credentials.
They stay local and are not uploaded automatically; handle them as sensitive
data. Move/delete old automatic snapshots explicitly when the storage budget
is full.

Exercise the native crash/output and production pressure paths in disposable
runtime instances, not a live Desktop session:

```sh
out/windows-x64/stage/mini-electron.exe --app=test/windows-renderer-memory-smoke.js
out/windows-x64/stage/mini-electron.exe --app=test/windows-renderer-memory-smoke.js --unwritable-dump-directory
out/windows-x64/stage/mini-electron.exe --app=test/windows-renderer-pressure-reclamation-smoke.js
out/windows-x64/stage/mini-electron.exe --app=test/windows-renderer-v8-headroom-smoke.js
out/windows-x64/stage/mini-electron.exe --app=test/windows-pre-oom-snapshot-smoke.js --then-oom
out/windows-x64/stage/mini-electron.exe --app=test/windows-pre-oom-snapshot-smoke.js --unwritable-dump-directory
out/windows-x64/stage/mini-electron.exe --app=test/windows-pre-oom-snapshot-smoke.js --storage-budget
out/windows-x64/stage/mini-electron.exe --app=test/windows-pre-oom-snapshot-smoke.js --near-heap-limit
out/windows-x64/stage/mini-electron.exe --app=test/windows-capture-disposal-smoke.js
MINI_CAPTURE_DISPOSAL_MODE=continue out/windows-x64/stage/mini-electron.exe --app=test/windows-capture-disposal-smoke.js
```

The pressure smoke deliberately approaches the actual Job threshold, validates
retained buffer contents and reclaimed memory, and checks real post-purge
software-compositor pixels. These Windows-specific diagnostics and monitor
checks do not imply macOS verification.

The pressure smoke uses a visible software surface. It does not promise that
initial hidden software captures resume a suspended compositor. The separate
capture-disposal regression destroys a hidden PNG-backed window with multiple
captures pending, checking both immediate host exit and rejected promises
followed by a working replacement renderer. Run it in both process modes.
Cancellation retains callbacks in owner-sequence state until delivery or
shutdown, rather than leaving V8 promises owned by late queue-destructor work.
Minidumps may contain application data; retain them locally or share only with
the intended diagnostic recipient.

### OMP Desktop package

Use the original OMP Desktop main/preload scripts and Expo frontend from
[iris-cat-dev/omp-desktop](https://github.com/iris-cat-dev/omp-desktop).
Prepare its build products with Node/npm; the Electron download is not needed:

```sh
git clone https://github.com/iris-cat-dev/omp-desktop ../omp-desktop
cd ../omp-desktop
ELECTRON_SKIP_BINARY_DOWNLOAD=1 npm install --no-audit --no-fund
node scripts/download-omp-binaries.mjs windows-x64
npm run build:remote-backend
npm run build:main --workspace=@omp-desktop/desktop
cd packages/app
npx cross-env PASEO_WEB_PLATFORM=electron expo export --platform web
cd ../../../mini-electron
python build.py --omp-desktop --omp-source ../omp-desktop --jobs 16
```

The original `packages/desktop/electron-builder.yml` and after-pack hooks build
`out/windows-x64/OMP Desktop/win-unpacked/OMP Desktop.exe`,
`OMP-Desktop-Setup-0.4.0-x64.exe`, its `.blockmap`, the Windows ZIP, and `latest.yml`.
Launch the executable from `win-unpacked`, or add `--interactive` to the build
command. `--app-output PATH` selects another release directory.

To omit the bundled OMP executable on Windows, reuse OMP's no-OMP builder
configuration (Git Bash):

```sh
OMP_DESKTOP_BUNDLE_OMP=0 python build.py --omp-desktop --omp-source ../omp-desktop --jobs 16
```

This writes a separate `out/windows-x64/OMP Desktop No OMP` release with
`OMP-Desktop-No-OMP-Setup-VERSION-x64.exe`, its blockmap, ZIP, and `latest.yml`.
The package retains the desktop runtime, daemon, terminal bindings, and ripgrep,
but neither requires the downloaded OMP binary nor includes `resources/bin/omp.exe`.
OMP agent sessions require an independently installed OMP executable; the existing
provider accepts `OMP_COMMAND` or discovers `omp` through PATH.
Both variants retain the original application identity and update feed; separate
output directories do not make them side-by-side installations or isolated update channels.

Packaging requires the original compiled desktop, frontend, CLI
(`packages/cli/dist/output/**/*.js`), and remote-backend build products.
The custom `electronDist` supplies this runtime; electron-builder stages the production
application under `resources/app` and retains native modules, bundled executables, NSIS
configuration, GitHub publisher metadata, and `resources/app-update.yml`. It does not download
or ship stock Electron or a separate Node executable. The obsolete editor-target icon resource
and unused esbuild requirement are removed.


The same executable runs the GUI, CLI, daemon supervisor, and daemon worker.
Setting `ELECTRON_RUN_AS_NODE=1` selects the statically linked Node runtime;
no stock Electron or separate Node executable is shipped. Both modes initialize
the shared V8 snapshot and ICU data. The OMP bundle stores JavaScript under `resources/app`;
native package readers also support ASAR for other consumers. `resources/bin/omp-desktop.cmd`
is the bundled CLI launcher.

For an isolated instance, set `OMP_DESKTOP_HOME` to a separate directory and
set `daemon.listen` in that directory's `config.json`. Use
`PASEO_ELECTRON_USER_DATA_DIR` to isolate desktop preferences and Chromium-style
session directories as well. The CLI status probe uses the persisted listen
target; setting only `PASEO_LISTEN` does not isolate it from an existing daemon.
