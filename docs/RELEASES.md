# Release notes

[Documentation index](README.md) · [Project overview](../README.md)

> Run commands from the repository root unless a section explicitly changes directory.

These are historical release and verification records, not a claim that every platform ships every change. npm installer and native runtime versions are independent. See [installation](INSTALLATION.md) and [publishing](RELEASING.md).

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
