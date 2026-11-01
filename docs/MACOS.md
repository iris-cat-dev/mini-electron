# macOS build and runtime

[Documentation index](README.md) · [Project overview](../README.md)

> Run commands from the repository root unless a section explicitly changes directory.

Shared examples and runtime behavior: [development guide](DEVELOPMENT.md). WebGPU builds additionally require Python 3.12+ and CMake 3.24+; see [graphics prerequisites](GRAPHICS.md#webgpu).

## macOS (Apple Silicon)

The macOS targets host the Chromium 132/Blink/V8 engine in native AppKit and
headless smoke executables. They use the Xcode arm64 toolchain, generate the V8
snapshot with `mksnapshot`, and execute work from native run loops.

Requirements:

- Apple Silicon Mac with Xcode
  and a working `xcrun --find metal`; if Xcode ships Metal separately, install
  it with `xcodebuild -downloadComponent MetalToolchain`
- Ninja (`brew install ninja`)
- GN, or `cipd` from
  [depot_tools](https://chromium.googlesource.com/chromium/tools/depot_tools/)
  on `PATH`

Build and run:

```sh
python build.py --run
```

If GN is outside `PATH`, set `GN=/absolute/path/to/gn`. The output binary is
`out/mac-arm64/mini-electron-smoke`. The smoke executable reports platform,
Unicode, locale, and V8 evaluation checks as JSON.

### Native dependency regression checks

After generating the macOS build graph, exercise native Snappy and the standalone
RE2 regression dependency independently of the desktop application:

```sh
ninja -C out/mac-arm64 mini_electron_third_party_dependencies_smoke mini_electron_skia_mesh_smoke
out/mac-arm64/mini_electron_third_party_dependencies_smoke
out/mac-arm64/mini_electron_skia_mesh_smoke
```

These checks cover compressed-data round trips and truncated-input rejection,
regular expression matching, and mesh recording resource lifetime, replay and
bounds culling. They do not establish renderer stability or Windows runtime
behavior. The disabled LCP/protobuf backend is not linked into this check or the
macOS production runtime.


### Native GUI demo

Build and open the test frontend in a native macOS window:

```sh
python build.py --gui
```

To load a remote page:

```sh
python build.py --gui --frontend https://omp.mintcat.work/
```

The command accepts a local HTML path or an HTTP(S) URL and opens it in an
AppKit window. The `mini_electron_*` native API creates the web view; Blink creates the
`Page`/`LocalFrame` and parses the network response, the repository V8 executes
page JavaScript, and Blink's software compositor copies its pixel buffer into
the `NSView`. After the frontend is ready, the demo exercises resize, mouse,
keyboard, focus, and IME paths and writes a verification snapshot to
`/tmp/mini-electron-browser.png`.

### Electron host

`python build.py --electron` runs the shared example in native AppKit windows
backed by the same renderer as the GUI demo. Like the Windows backend, it waits
for a visible window, requests quit (AppKit terminate → `app.quit()`), and
requires exit status 0. `--screenshot PATH` additionally captures that window
with `screencapture`, which needs Screen Recording permission for the terminal.
The macOS adapter implements readiness/quit, window title/visibility/bounds
methods, `loadFile()`, `loadURL()`, `BrowserWindow.getAllWindows()`, and
`webContents.getURL()`. The main process uses the same Node options as Windows
(`--unhandled-rejections=warn`, no CommonJS module-type detection).

The renderer initializes ICU's system time-zone cache before Seatbelt lockdown.
`Date` local fields and default `Intl.DateTimeFormat` output use the macOS time
zone rather than falling back to UTC when `/etc/localtime` becomes inaccessible.
Persisted UTC timestamps are unchanged. This initializes the zone at renderer
startup; live system time-zone changes are not monitored.

Verify local completion labels, local date boundaries, and daylight-saving offsets:

```sh
out/mac-arm64/mini-electron test/macos-local-time-smoke.js
TZ=America/New_York out/mac-arm64/mini-electron test/macos-local-time-smoke.js
TZ=UTC out/mac-arm64/mini-electron test/macos-local-time-smoke.js
```

The macOS host supports transparent, nonactivating companion windows with native
mouse passthrough and movement forwarding. `focusable`, `alwaysOnTop`, `hasShadow`,
`showInactive()`, and `setVisibleOnAllWorkspaces()` retain their native behavior.
The first pointer movement reaches Blink without requiring keyboard focus.

On macOS, `BrowserWindow({ backgroundColor: '#RRGGBB' })` supplies the native
backing surface and Blink's initial page background before the first frame.
It remains visible while the first document response is pending. Transparent
companion windows retain a clear backing surface; ordinary windows without an
explicit background keep their white default. `setBackgroundColor()` updates
the displayed background even before the first navigation, using genuine Blink
painting without changing loaded-document rendering deferrals.

Application menus register native Command/Control accelerators and standard Edit
roles. Editing targets the focused Blink frame; sandboxed clipboard operations use
the authenticated browser broker, preserving text, HTML, and copied-image PNG
data in one pasteboard transaction without granting the renderer direct
pasteboard access. `webContents.copyImageAt()` copies actual decoded image pixels;
native bitmap clipboard images supported by ImageIO (including PNG, TIFF, and
JPEG) expose readable PNG `File` items on paste into textarea and contenteditable
inputs even alongside HTML/URL representations. Multi-representation images use
their first image, and an invalid preferred representation does not hide another
valid image format. Native image labels are hints: the actual encoding is decoded
even when an application advertises PNG bytes as TIFF. HTML/URL-only clipboard
content is not fetched or synthesized into an image attachment.
Page zoom uses the existing WebContents zoom level, not CSS scaling. Disabled
and unregistered accelerators do not invoke menu actions.

Blink's native context-menu action emits `webContents`' `context-menu` event with
image/link targets, selection, editable state, and actual edit capabilities.
Renderer menus that call `preventDefault()` suppress that native event.
Popup edit roles target the window passed to `Menu.popup({ window })`, even when
another window is active.
Image copying also recognizes loaded CSS image backgrounds used by React Native
Web, without changing Blink's general hit-test behavior.

`webContents.downloadURL()` supports HTTP(S), base64/percent-encoded `data:` URLs,
and authorized `blob:` URLs belonging to a current frame in that WebContents.
Generated-image downloads retain the original bytes and MIME-derived filename,
use the normal native save dialog and DownloadItem lifecycle, and stream blobs
in bounded chunks. Revoked, foreign-renderer, or unsupported sources interrupt
without writing a destination; cancellation preserves an existing destination.

macOS regression probes, after building the runtime:

```sh
out/mac-arm64/mini-electron test/macos-context-menu-smoke.js
out/mac-arm64/mini-electron test/generated-image-download-smoke.js
out/mac-arm64/mini-electron test/macos-clipboard-editing-smoke.js
```


Blink's macOS selection and focus colors resolve through AppKit's native system
colors for the document's light/dark appearance, rather than falling back to
black when sandbox support is absent.

Desktop views advertise a fine pointer with hover capability through both
primary and available-pointer media queries. Mouse/trackpad users therefore get
full-height sidebar resize targets and hover feedback instead of touch-only
grips. Native pointer capture retains drag input across the content pane and
releases it on mouse-up.

Native input methods receive the focused Blink editor's UTF-16 selection,
composition range, and caret rectangle asynchronously. Candidate anchors follow
the caret through page/editor scrolling, Retina scale, and native window movement;
blur and non-editable focus clear stale anchors. Marked-text updates and Unicode
commits use Blink's composition controller instead of remaining local to AppKit.
On macOS, marked text without explicit IME styling uses a translucent neutral-gray
background instead of yellow, preserving the editor's foreground color in light
and dark themes. Native selection colors and candidate-panel styling are unchanged.

Sandbox IPC preserves nested `ArrayBuffer` and typed-array properties as binary
structured-clone data in both directions. Image attachment writes retain the
typed-array view's offset and length instead of enumerating each byte into a
numeric-key dictionary, which stalled previews and exceeded the object-entry
limit for larger files.

Native AppKit file drags and Windows Explorer OLE `CF_HDROP` file/folder drags
are exposed as brokered DOM `File` objects only after the browser authorizes the
current document. Regular files are limited to 50 MiB each and 256 MiB per drag;
symbolic links, Windows reparse-point path components, special files, stale
documents, and forged `File` objects cannot disclose host paths. Directories
expose entry metadata without granting renderer traversal. Clipboard images in
ImageIO-supported formats on macOS and PNG/native DIB input on Windows are decoded
and bounded before Blink receives them; malformed images are omitted. Native drag hit testing converts AppKit
logical view coordinates or Win32 physical client coordinates to Blink viewport
pixels using the current display scale; screen coordinates remain logical.
Bounded drag targets ensure DPI conversion errors cannot pass through
document-wide acceptance.

Custom protocol `Response` headers preserve MIME type, charset, and binary data;
HTML with `Content-Type: text/html; charset=utf-8` is parsed as a document.
Local `net.fetch(file:)` responses remain streamed with backpressure and
cancellation. Read chunks are capped at 1 MiB and the file size to amortize
native main-loop work when loading large frontend assets; HEAD responses do
not open a body stream.

Shutdown invalidates AppKit menu actions before disposing V8 and retains sandbox
preload hosts until Blink has destroyed their frame contexts, so live menus,
pending preload IPC, and companion windows can exit without late handle access.

Targeted native regressions: `test/macos-companion-smoke.js`,
`test/macos-shortcuts-smoke.js`, `test/macos-clipboard-editing-smoke.js`,
`test/macos-selection-colors-smoke.js`, `test/macos-ime-position-smoke.js`,
`test/macos-pointer-interaction-smoke.js`, `test/macos-scroll-smoke.js`,
`test/macos-precise-wheel-smoke.js`,
`test/macos-image-preview-smoke.js`,
`test/macos-file-drag-smoke.js`, `test/macos-protocol-response-smoke.js`,
`test/macos-initial-background-smoke.js`, `test/macos-frame-presentation-smoke.js`,
`test/macos-window-buttons-smoke.js`, and `test/macos-exit-lifecycle-smoke.js`.
Input helpers require Xcode. The initial-background regression captures actual
composed display pixels and requires Screen Recording permission and unobstructed
test windows. Optional physical passthrough verification uses
`MINI_COMPANION_PHYSICAL=1` and macOS Accessibility permission.

Run the selection, image-clipboard, IME, pointer, scroll, and image-preview regressions
against a rebuilt runtime:

```sh
out/mac-arm64/mini-electron --app=test/macos-selection-colors-smoke.js
out/mac-arm64/mini-electron --app=test/macos-clipboard-editing-smoke.js
out/mac-arm64/mini-electron --app=test/macos-ime-position-smoke.js
out/mac-arm64/mini-electron --app=test/macos-pointer-interaction-smoke.js
out/mac-arm64/mini-electron --app=test/macos-scroll-smoke.js
out/mac-arm64/mini-electron --app=test/macos-precise-wheel-smoke.js
out/mac-arm64/mini-electron --app=test/macos-image-preview-smoke.js
```

The selection regression captures actual native renderer-view pixels in both
color schemes. The IME regression clicks real native editor targets, checks the
`NSTextInputClient` screen rectangles and composition lifecycle, and does not
change the system keyboard input source; the system candidate panel itself
remains a manual visual check.
The pointer regression sends actual AppKit movement/drag events, verifies hover
feedback along the whole divider and width changes across captured drags, and
retains native PNG evidence without requiring Screen Recording permission.
The scroll regression sends native AppKit pixel/line wheel events to an owned
renderer view without activating its window. It checks proportional trackpad
motion, high-frequency and fractional input, both axes, nested scroll hit testing,
and responsive ordinary mouse-wheel steps in both directions, including several
notches in one event, retaining compositor PNG evidence through `capturePage()`.
The separate `test/macos-precise-wheel-smoke.js` checks native precise-wheel
`deltaX/Y` and legacy `wheelDeltaX/Y` consistency in both directions on both
axes, including one-logical-pixel input on Retina displays, without requiring
application activation. This catches xterm/VS Code scrollback failures caused
by zero wheel ticks even when ordinary DOM scroll containers still move.
The clipboard regression invokes actual AppKit editing actions on its owned
renderer without activating the application. It checks decoded image dimensions
and pixels from PNG, TIFF, JPEG with HTML/URL co-representations, multi-image TIFF,
PNG bytes advertised as TIFF alongside a file URL, and invalid-PNG/valid-TIFF
input in both textarea and contenteditable targets, alongside rich-text
copy/cut/paste and malformed-image rejection.
The image-preview regression sends a large PNG in a nested sliced `Uint8Array`
through the isolated preload, writes and reads its exact bytes on disk, decodes
the returned Blob URL, and verifies that renderer mouse input and timers still
work afterward.

Hidden-titlebar traffic lights retain AppKit's native button size and gap when
`trafficLightPosition` or `setWindowButtonPosition()` moves them. Their title-bar
container follows the requested top margin, keeping all three native hit targets
clickable above document drag regions. Fractional runtime positions are preserved.

CSS `-webkit-app-region: drag` regions use native AppKit window dragging.
Overlapping `no-drag` regions take precedence so titlebar controls stay clickable.
Double-clicking a drag region follows `AppleActionOnDoubleClick`: zoom/restore
by default, minimize for `Minimize`, and no action for `None`. Applications
should retain native drag regions when a macOS window is zoomed.

Frameless and hidden-titlebar/full-size-content windows reserve a five-point
native resize border. Edge and corner drags take precedence over caption/web
input, keep the opposite edge anchored, and honor window minimum/maximum sizes.
Non-resizable and full-screen windows do not use this border. macOS 15+ uses
AppKit's directional frame cursors; older systems use axis resize cursors.

Runtime `0.1.30` forwards Blink cursor changes over the binary
renderer channel instead of leaving the content cursor at the default arrow.
Input/contenteditable text, links, explicit CSS pointer/crosshair/cell/context-menu,
move/grab/grabbing, resize, copy/alias/forbidden, zoom, vertical-text and hidden
cursors use the existing Cocoa cursor mapping. Native chrome and resize borders
remain authoritative. Wait/progress share AppKit's busy-but-clickable cursor.

Cursor ownership follows the hovered or mouse-captured native renderer/active
webview guest, not keyboard focus; invisible, occluded and mouse-ignored surfaces cannot overwrite
another window's cursor. Standard native cursors are cached. Custom cursors carry
premultiplied sRGB RGBA, image scale and hotspot, with Retina image/representation
sizes converted to points. Hidden cursors use a transparent image, not unbalanced
global `hide`/`unhide` calls. Navigation and renderer exit clear stale state.
Chorded mouse events retain every held button until its own release, keeping
pointer capture and grab/grabbing transitions valid across guest bounds.

The cursor regression requires an unlocked macOS console and Xcode. It sends
AppKit events to owned views, checks actual NSCursor shapes and raster pixels,
and exercises stationary style changes, BrowserView/guest ownership, capture,
native resize precedence and navigation cleanup. Pointer position is restored:

```sh
env -u ELECTRON_RUN_AS_NODE out/mac-arm64/mini-electron \
  --app=test/macos-cursor-smoke.js
```

The incremental macOS source build and existing native pointer-screen-coordinate
regression passed. Real AppKit-view injection also passed retained native cursor
mapping, stationary updates, custom 16×16-point RGBA/hotspot `(5,7)`, BrowserView,
navigation reset and guest capture with DOM `buttons` transitions `1 → 3 → 2 → 0`
and a negative guest `clientX` while the right button remained held.
The final physical-pointer regression remains pending while the console is locked;
retained-native-cursor inspection is not a substitute for the unlocked screen.

Mouse dragging preserves the event's held-button state. AppKit precise wheel
events retain fractional pixel deltas and both axes through IPC and Blink, with
Retina scaling applied once. Precise input also populates Blink's wheel ticks
from physical pixel deltas divided by 120, so legacy DOM `wheelDeltaX/Y` remain
consistent with logical pixels; leaving ticks at zero disables JavaScript
scrollers such as xterm. Trackpad pixel motion is not multiplied into
mouse-wheel notches; ordinary mouse wheels scroll 80 logical pixels per vertical
notch and retain multi-notch distances instead of clamping each event to one
step. The renderer recognizes left, middle, and right buttons
rather than interpreting native string names as left clicks.
Native window and BrowserView drawing retain the last complete image while the
renderer writes a newer shared frame. Pending and presented pixel buffers are
separate and reused; a failed seqlock read never exposes partial pixels or
replaces the window with its clear color. Images and color spaces are cached
between redraws instead of rebuilding them for every exposed region.

Viewport updates coalesce the latest size and scale as one transaction; unchanged
scale no longer bypasses the resize gate. The root compositor retains the active
child surface until its replacement activates, rather than submitting an empty
resize warmup frame. Software pixels are published only for actual swaps, and
the draw acknowledgment waits for completed UI publication. Retained allocation
ownership keeps the previous native bitmap valid through resize and teardown,
without adding per-frame pixel copies.
Retained native images keep their original pixel density and clip at the new
window bounds; they are not independently stretched in each axis. Viz fallback
surfaces likewise use natural geometry, with canonical child-background gutters.
Readiness and publication both wake the pending latest viewport, including on
static pages. Child metadata merges into the canonical parent surface allocator
instead of rolling a newer resize ID back to an older frame.

The sandbox allows metadata access to the `/System` directory itself, in addition
to reads under `/System/Library`. CoreText needs that ancestor lookup to resolve
virtual `system-ui` CJK fonts; denying it produces missing-glyph boxes despite
explicit CJK font families working. No font bundle or broad filesystem access
is needed.

The browser process reads the OS preferred-language list before sandbox startup
and supplies it to `navigator.language` and `navigator.languages`. macOS also
implements `app.getLocale()` and `app.getPreferredSystemLanguages()`. Restart the
application after changing system language preferences to refresh existing UI.

IPC `senderFrame` and `mainFrame` share stable, native-validated frame identities.
Subframes remain distinct and replaced documents become detached; consumers
can enforce main-frame authorization without disabling their checks.

Blink classifies same-document history and fragment commits explicitly. In-page
navigation updates the URL and emits `did-navigate-in-page` without replacing frame
identity, rerunning preload, revoking file authority, or creating a loading cycle.
Back/forward traversal keeps Blink's history offset aligned with stored entries.
Full navigation and reload, including the same URL, replace the document. Brokered
documents require an observed authorized response; intrinsic documents retain their
existing checks. Custom-protocol requests preserve their path and query. Resource
authorization ignores fragments while retaining exact path, query, and method checks.
The navigation regression covers state/IPC continuity, same-URL custom-protocol and
HTTP reloads with query/fragment URLs, and detachment of previous-document frames.

`webContents.executeJavaScript()` now returns the renderer value directly rather
than a `{ value: ... }` wrapper. Native Blink execution waits for Promise/thenable
settlement, propagates synchronous exceptions and asynchronous rejections, and
uses the existing structured-clone IPC format for objects, arrays, undefined,
BigInt, cycles, and binary data. Pending execution is rejected when its document
or window is destroyed. No eval wrapper, preload bridge, or DevTools is required;
strict-CSP image decoding and asynchronous canvas PNG conversion use this same path.
Existing application/test callers must consume the direct result, not `.value`.

Run issue #3's real sandboxed-renderer regression after rebuilding:

```sh
out/mac-arm64/mini-electron test/execute-javascript-smoke.js
./out/windows-x64/runtime-dist/electron.exe test/execute-javascript-smoke.js
```

The macOS native run covers the seven reported result/error cases, structured
values, concurrent settlement, source-document/window destruction, and independent
pixel comparison of an asynchronously decoded canvas PNG. The unmodified OMP Desktop
`0.5.2` generated installer also converted the bundled Iris PNG and atomically
installed its 3840-by-960 spritesheet in an isolated profile. This verifies the
shared execution path on macOS, not the Windows binary; the published Windows
`0.1.8` artifact does not contain this source fix.

Local-storage mutation acknowledgements are posted after the native setter
returns, matching Mojo ordering and allowing Blink to register its pending
mutation first. Synchronous broker/navigation replies notify while holding their
completion lock, so a waiter cannot destroy the stack condition variable before
the reply thread's final use.
`WebContents.isDestroyed()` tracks actual object destruction, not renderer
liveness. Renderer loss still emits `render-process-gone`; the owning native
window remains registered until it is closed.
Windows runtime `0.1.18` also supplies the actual unsigned native
`exitCode` with renderer exits. Reasons include `oom`, `crashed`, `clean-exit`,
and `abnormal-exit`; a transport failure while the child remains alive reports
`ipc-failure` with its `error` detail instead of claiming the process exited.

macOS builds pin [libcurl 8.23.0](https://curl.se/download.html) with WebSocket
support and link it statically against the runtime's existing TLS library.
The current HTTP/WebSocket curl configuration disables TLS peer/host verification;
SecTrust support alone is not a certificate-validation guarantee. The system libcurl is
not used: it exports WebSocket symbols but rejects `ws`/`wss` as unsupported
protocols on the tested macOS installation. No additional framework bundle is
distributed for networking.

After building, exercise the real sandboxed renderer's WebSocket handshake,
subprotocol, fragmented text, binary data, ping/pong, and clean close:

```sh
out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron \
  --app=test/websocket-smoke.js
```

Exercise native frame authorization and per-process system language overrides
without changing global macOS preferences:

```sh
out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron \
  --app=test/ipc-frame-smoke.js
out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron \
  --app=test/macos-language-smoke.js -AppleLanguages '(en-US)' \
  --expected-language=en-US
out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron \
  --app=test/macos-language-smoke.js -AppleLanguages '(zh-Hans-CN,en-US)' \
  --expected-language=zh-Hans-CN
```

Native storage and lifetime regressions exercise sustained same-key writes,
Unicode, delete/clear, persistence across completed reload, and loss of only the
smoke's own renderer child followed by actual window close:

```sh
out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron \
  --app=test/macos-storage-mutation-smoke.js
out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron \
  --app=test/macos-renderer-loss-lifetime-smoke.js
```

The macOS renderer responsiveness regression targets only its own renderer with
native application events and observes WindowServer's hang flag for 20 seconds.
It also checks page interaction, sandbox isolation and renderer exit on window
close. A live JavaScript/IPC loop alone does not establish native responsiveness.
This smoke requires a graphical macOS login session and Xcode:

```sh
out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron \
  --app=test/macos-renderer-responsiveness-smoke.js
```

The macOS idle regressions cover event-driven AppKit/libuv scheduling, native
visibility propagation to Blink/Viz and embedded contents, and CSS compositor
animation continuing while the page's JavaScript thread is busy:

```sh
ELECTRON_RUN_AS_NODE=1 out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron \
  test/macos-idle-pump-smoke.js
out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron \
  --app=test/macos-idle-visibility-smoke.js
out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron \
  --app=test/macos-threaded-animation-smoke.js
```

Hidden contents stop rAF and unnecessary Viz work by default. Signal initial
readiness with load/DOM events before requesting `prepareToShow()`; waiting for
a hidden-page animation frame before requesting preparation can deadlock.
Initial first-paint and explicit preparation leases preserve hidden startup.
Constructor and setter `backgroundThrottling: false` opt out deliberately.
Native occlusion is trusted only after AppKit has reported an exposure; an
uninitialized exposure bit must not suspend an onscreen first frame.
See [macOS idle scheduling and visibility](RENDERING_PERFORMANCE.md#macos-idle-scheduling-and-visibility)
for measured CPU, verification coverage and unavailable native OS checks.

Native mouse events preserve the originating Shift, Control, Command and Option
flags through the host, renderer IPC and Blink dispatch, including pointer moves,
press/release, clicks and context menus. The queued event's modifier snapshot is
used instead of reading global keyboard state later on the compositor thread.
This allows file-tree range and additive selection without application workarounds.
The regression sends AppKit events with each modifier and their combinations:

```sh
out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron \
  --app=test/macos-mouse-modifiers-smoke.js
```

The presentation regression requires Xcode and compiles a temporary native
helper against the production frame header. It pauses only its own renderer,
holds an in-progress publication, and exercises the actual NSView drawing path;
it also checks that a later complete frame changes the displayed pixels:

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron \
  --app=test/macos-frame-presentation-smoke.js
```

The window-button regression compiles a native helper, verifies equal AppKit
spacing and hit targets at constructor and runtime positions, then sends real
mouse events through the window to close, minimize, and zoom:

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron \
  --app=test/macos-window-buttons-smoke.js
```

The native resize regression also requires Xcode. It sends real AppKit events
through the owned NSWindow, checking all eight directions, size limits, release,
cursor changes, no-drag controls and renderer viewport synchronization. It
exercises a real full-screen transition for OMP's hidden-titlebar window style:

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron \
  --app=test/macos-window-resize-smoke.js
```

The continuous-presentation regression samples the actual committed CALayer,
not a fresh `drawRect:` into a test bitmap. It checks retained colors while the
owned renderer is paused, rapid grow/shrink drags with animation, later complete
frames, final viewport latency, and the replacement layout's visible marker:

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron \
  --app=test/macos-resize-presentation-smoke.js
```

The geometry regression checks actual marker bounds, not just matching colors.
An 80-DIP square must remain square while its renderer is paused and the window
becomes tall; static and animated portrait/landscape layouts must then converge
to the requested viewport and responsive marker positions:

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  out/mac-arm64/runtime-dist/Electron.app/Contents/MacOS/Electron \
  --app=test/macos-resize-geometry-smoke.js
```

### Console, creation, and desktop coordinates

Runtime `0.1.29` forwards DevTools-free `console-message` events with the actual
owning-isolate URL and line. `app` emits `web-contents-created` before page output;
listeners that destroy contents or throw abort prospective BrowserWindow/BrowserView
construction, release native owners, and preserve the original exception.

Native mouse and ordinary/precise wheel input carry desktop DIP coordinates from
the original AppKit/CGEvent. Window movement updates widget/window rectangles
without requiring a resize, including BrowserViews and webview guests. The
regression covers negative-display positions and movement during pointer capture:

```sh
out/mac-arm64/runtime-dist-0.1.29/Electron.app/Contents/MacOS/Electron \
  test/macos-pointer-screen-coordinates-smoke.js
```

### Renderer memory pressure and local pre-OOM diagnostics

Default independent renderers sample native macOS memory pressure and V8 heap
usage on the renderer owner thread. Kernel warning/critical signals remain
distinct; V8 uses independent 70%/85% tiers, hysteresis, and 30/60-second reclaim
cooldowns. Reclamation reaches base/Blink, discardable memory, Skia, and every
registered isolate. macOS does not acquire a Windows-style Job/process hard cap.

Automatic main-isolate heap snapshots default to 65% utilization. Set
`MINI_ELECTRON_PRE_OOM_SNAPSHOT_PERCENT=0` before creating a renderer to disable;
`1..65` lowers the trigger, and invalid settings report `unavailable` with
`invalid-configuration`. Each renderer makes at most one attempt, including refusals.
Capture refuses near 85% heap usage, critical/unknown pressure, or insufficient
sampled free-page headroom for the estimated graph plus 256 MiB. Free/speculative
pages are normalized without double counting; V8 logical heap usage is not
assumed to be bounded by process physical footprint. Serialization has a 2 GiB
byte cap and 30-second deadline; the destination needs 2 GiB free and permits
at most four completed or in-flight snapshots.

The browser exclusively creates/writes 0600 local files under `app.getPath('crashDumps')`
(default `userData/Crashpad`). The renderer receives only bounded datagram transport,
never a writable snapshot file descriptor. Browser-private state owns thresholds,
deadline, byte accounting, and the frozen result. Closing a window cancels and
joins the monitor before renderer shutdown; cancellation and final rename share
one lock. Darwin peer-queue `ENOBUFS` uses bounded nonspinning backpressure.

Local JSON sidecars and `render-process-gone` expose `preOomHeapSnapshot`; exit-time
`rendererReportedV8Oom` is separate from the frozen snapshot outcome. Graphs are
labelled `heapGraphSource: 'renderer-reported'`, not attested renderer contents.
Signal exits are not fabricated as V8 OOM or minidumps. Heap graphs contain
sensitive application data: they stay local and are never uploaded automatically.
Automatic capture is unsupported in `--single-process` mode. npm `0.1.12` is
unchanged; these macOS diagnostic additions do not imply newly published typings.
See [release-specific verification](VERIFICATION.md#macos-runtime-0129-2026-10-30).

### macOS Metal compositing

The macOS runtime defaults to native Skia Ganesh/Metal composition when device,
context and remote-layer initialization succeed. Blink layout and cc tile
rasterization remain CPU work. Viz draws quads, render passes and filters to GPU
surfaces, then presents through `CAMetalLayer` and a remote `CALayerHost`; normal
frames publish metadata rather than a CPU RGBA framebuffer.

Force the existing CPU backend for diagnosis or matched comparisons:

```sh
MINI_ELECTRON_DISABLE_GPU=1 out/mac-arm64/mini-electron --app=your-app.js
```

`capturePage()` explicitly reads GPU pixels back and preserves premultiplied
transparency. Its existing crop coordinates are physical framebuffer pixels.
The Metal compositor alone does not enable GPU tile rasterization, accelerated
Canvas2D, WebGPU, hardware video decoding, or Chromium's full GPU service.
[WebGL has its own ANGLE implementation](GRAPHICS.md#webgl-1-and-2), with CPU readback into
the resource protocol; `MINI_ELECTRON_DISABLE_GPU=1` changes output composition,
not WebGL availability. Metal capability is opened in the renderer before
sandbox lockdown, not in an isolated GPU process; see
[rendering architecture and verification](RENDERING_PERFORMANCE.md#macos-metal-gpu-compositing).

Native pixel regressions capture only owned WindowServer windows. They require
an unlocked graphical session and functioning OS capture authorization; granting
Screen Recording can require restarting the launcher. Producer GPU readback is
not accepted as proof of final native-window presentation.

### Rendering performance comparison (local Chrome)

Compare the built macOS runtime with the Chrome already installed on this
machine. This does not download or update either browser. Requires Node.js 22+
and a graphical macOS session:

```sh
npm run benchmark:render
# Override executable paths or increase repetitions:
npm run benchmark:render -- --trials 5 \
  --runtime out/mac-arm64/mini-electron \
  --chrome "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
```

The runner serves the identical local page to both engines, opens only one
engine at a time, alternates the first engine across trials, and uses fresh
owned profiles. Defaults: three trials per engine, a 1000×700 actual content
viewport, two-second animated measurements after 500 ms warmups, and 20
operation samples after three discarded warmup operations. Chrome keeps its
default rendering/GPU configuration. The runner does not override mini-electron's
backend: current macOS builds try Metal composition with CPU tile rasterization,
with CPU fallback or explicit `MINI_ELECTRON_DISABLE_GPU=1`. Viewport/DPR mismatches,
hidden pages, renderer failures, or failed workload invariants reject the run
instead of producing comparison ratios.
Keep the test window unobscured, do not resize or interact with it, and leave
the machine otherwise idle.

The six scenarios are an empty rAF baseline, creation/layout of 2,000 text
rows, updates/layout of 1,000 grid items, Canvas2D repaint of 1,000 primitives
per callback, transforms of 150 cards, and programmatic scrolling of 3,000 rows.
The scroll case does not measure native input latency.

Artifacts go to `out/rendering-benchmark/<timestamp>/`: `report.md` contains
side-by-side medians/ranges and mini/Chrome ratios; `results.json` and
`trial-*.json` retain raw samples, workload checks, engine/environment metadata,
page source hashes, display/power snapshots, and process logs. Aggregates are
medians of per-trial metrics, not pooled quantiles. Costs use lower-is-better;
rAF callback frequency uses higher-is-better. There is no composite score.

Work time measures synchronous JavaScript and explicitly forced layout or
canvas submission, not completed GPU work. Double-rAF opportunity time is a
next-render-opportunity proxy, not presentation latency. rAF frequency is
**not actual presented FPS**; intervals above 25 ms use a fixed 60 Hz reference,
not a measured dropped-frame count. Display refresh rate and engine scheduling
caps matter. Startup, network, native input, and actual display presentation
are outside this benchmark; it is a synthetic rendering comparison, not an
overall application-speed score.

For an interactive page without launching either engine:

```sh
npm run benchmark:render -- --serve
```

Open the printed URL, keep the page at the top, and click **Run benchmark**.
This manual mode uses the current window's viewport; reload to run again.
Use automated mode for matched-viewport comparisons.

The diagnosis, optimization priorities, and macOS display-timing results are
recorded in [Rendering performance](RENDERING_PERFORMANCE.md). The P0 native
regression exercises actual 60/120 Hz screens, equal-size/DPR window movement,
BrowserView reparenting, display-timing fallback/recovery, and static CPU/wakeups:

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  out/mac-arm64/mini-electron --app=test/macos-vsync-smoke.js
```

This requires two attached displays with different refresh rates and equal DPR
for full screen-switch coverage; unavailable required coverage is reported as
incomplete, not passed. It moves only test-owned windows and does not change
display settings. JSON/PNG evidence goes to `out/rendering-vsync/`; set
`MINI_VSYNC_ARTIFACTS` to choose a directory. CPU counters use the native Mach
timebase, independently calibrated against `getrusage`. They and wakeup counts
are power proxies, not electrical-power measurements. `MINI_VSYNC_BASELINE=1`
collects an older binary's observations without invoking the new display command.

### Runtime distribution

Version `0.1.9` publishes this macOS runtime. The release declares
`darwin`/`arm64` in `package.json` and `darwin-arm64` in `artifacts.json`:

```sh
python build.py --package-dist
```

This writes `out/mac-arm64/runtime-dist` and
`out/mac-arm64/mini-electron-v<runtimeVersion>-darwin-arm64.zip`. The runtime is an
ad-hoc-signed `Electron.app` with one stripped host executable, the
Squirrel/Mantle/ReactiveObjC update frameworks, runtime JavaScript, and license
notices. No Electron Helper bundles are generated. Renderers start the same
executable with `--type=renderer` in separate sandboxed processes; Node workers
use `ELECTRON_RUN_AS_NODE=1`. Sharing the executable does not merge processes.
The bundle defaults to `LSUIElement=true`, so background roles never register
as ordinary Dock applications. Creating a GUI BrowserWindow promotes only
that process to `NSApplicationActivationPolicyRegular`.
The browser main process runs one continuous AppKit/Chromium event loop. A
blocking libuv-backend watcher and an exact native deadline timer wake that loop;
libuv callbacks stay on the main thread. There is no recurring 10 ms GUI poll or
repeated `NSApplication.run`/`stop` cycle that cancels native button gestures.
The window-button smoke queues its events with a 100 ms hold to cover this path.
AppKit launch is completed explicitly before Electron readiness, retaining Dock,
activation and accessibility registration.
Sandboxed renderer processes instead own a native AppKit UI message pump on
their main thread; Blink's rendering thread retains its NSRunLoop. Initialize
libobjc's block-method trampolines before seatbelt lockdown so AppKit's lazy
native-event handling does not abort inside the sandbox. Renderer startup never
initializes Node or trusted browser bindings, and shutdown joins Blink before
releasing its main-thread task executor.
Shared frames use premultiplied RGBA. The renderer converts from Skia's native
pixel format only when needed: macOS RGBA is copied directly, while Windows
BGRA swaps red and blue. Screenshot conversion follows the same native format.

The runtime ZIP preserves framework symlinks and measures 31.70 MiB, down from
154.13 MiB with four helper copies. OMP Desktop carries an `app-builder-lib`
patch that lets electron-builder package a distribution without a generic
Helper bundle. Other electron-builder consumers need the same optional-helper
handling rather than staging duplicate executables.

### OMP Desktop package

Build a self-contained OMP Desktop application from a sibling checkout:

```sh
python build.py --omp-desktop --omp-source ../omp-desktop
```

The package is written to `out/mac-arm64/OMP Desktop.app`. One executable
serves both roles: it starts the AppKit/Blink GUI normally and starts the
statically linked Node runtime for CLI and daemon child processes when
`ELECTRON_RUN_AS_NODE=1`. Node uses this repository's V8 build, so the bundle
does not contain a second Node distribution or `Contents/Resources/node`.
Blink and Node/V8 also share the same statically linked full ICU data package;
the macOS target does not compile MiniBlink's legacy embedded ICU blob.

The macOS Node runtime compiles and explicitly registers the native `node:sqlite`
binding against the bundled SQLite library, including column metadata,
session/changeset support, and positional bindings for numbered `?NNN` slots.
Numbered slots retain their SQLite indices when mixed with named parameters.
The numbered-binding correction is currently source-only: published runtime
`0.1.30` still rejects the mixed numbered/named regression with
`column index out of range`. Rebuild from the committed source for this fix.
OMP Desktop uses `DatabaseSync` to read its OAuth
accounts before querying quota; JavaScript module availability or a SQLite
version string alone does not establish that the native binding is present.
Run the isolated regression smoke with the current source build or a rebuilt packaged application:

```sh
ELECTRON_RUN_AS_NODE=1 out/mac-arm64/mini-electron \
  test/node-sqlite-smoke.js
ELECTRON_RUN_AS_NODE=1 "out/mac-arm64/OMP Desktop.app/Contents/MacOS/OMP Desktop" \
  test/node-sqlite-smoke.js
```

The smoke verifies OAuth-style account filtering, WAL, native read-only
`DatabaseSync.isTransaction` state, nested savepoints, failed-transaction rollback
recovery, writer-lock isolation, numbered bindings, read-only access, column
metadata, backup, changesets, and Worker loading using temporary databases; it does not read or
change user credentials. The transaction getter reports SQLite engine state and
throws `ERR_INVALID_STATE` on a closed connection. This native binding addition
is macOS-only; the Windows source lists are unchanged.

The macOS host also omits unsupported browser-only surfaces: Blink's
DevTools/Inspector transport, the experimental `<permission>` element, and
HarfBuzz font-subsetting APIs. JavaScript console/error delivery and HarfBuzz
text shaping remain available. The host has no print-to-PDF or font-subsetting
entry point.
The packaged backend no longer carries `sherpa-onnx-node`, its Darwin binary,
or the local Silero VAD model; the current OMP backend does not use that local
speech provider.
Packaging still downloads the backend's declared Node release into
`.build-tools` to run `npm ci`; that executable is build-time tooling only.
nghttp2 is likewise downloaded, checksum-verified, built as a static library,
and linked into the host for the daemon's HTTP/2 support. Packaging bundles
and optimizes the CLI, daemon supervisor, and daemon worker, retains required
native Node modules, strips local symbols, and applies an ad-hoc signature.

Detailed size measurements and optimization priorities are documented in
[`docs/OMP_DESKTOP_SIZE_ANALYSIS.md`](OMP_DESKTOP_SIZE_ANALYSIS.md).

Default security boundary: the Electron host runs Blink in separate renderer
processes; macOS applies a Seatbelt profile before the renderer protocol starts.
The opt-in [single-process UI mode](DEVELOPMENT.md#trusted-single-process-ui-mode) removes this
OS process/sandbox boundary entirely.
The shared executable still builds V8 with its sandbox disabled because
embedded Node's external `ArrayBuffer` APIs are incompatible with that sandbox.
This is not equivalent to Chromium/Electron's complete sandbox architecture;
load only packaged or trusted web content.

The Win32-shaped public header now uses portable opaque handles on macOS.
Native Chromium `NS_RUNLOOP`, CoreText font selection, CPU bitmap callbacks,
POSIX paths, and the macOS Application Support storage root are selected in
their respective engine paths.
