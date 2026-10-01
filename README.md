# mini-electron

A lightweight Electron-compatible runtime for Windows and macOS, built on
MiniBlink's Chromium 132, Blink, and V8 engine sources.

> **Targets:** Windows x64 and Apple Silicon macOS. Both use this checkout,
> the root build command, and the same Electron example. API coverage differs
> between the native hosts; neither is a drop-in replacement for full Electron.

## Repository layout

- `build.py`: the single build, package, and run entry point; selects the host OS
- `build/mini_electron`: internal Windows and macOS build backends
- `build/mini_electron/windows/manifest.json`: ordered Windows build dependencies
- `build/windows/projects`: Windows source lists, with shared settings in `build/windows`
- `build/macos`: GN targets and desktop source manifests
- `runtime/engine`: public native API, browser/view hosting, rendering, and bindings
- `runtime/electron`: Electron/Node integration and embedded JavaScript
- `runtime/network`: loaders, cookies, and WebSocket integration
- `runtime/storage`: persistent profile and storage integration
- `ipc`: native Message/Channel IPC, serialization, and Blink scheduler interfaces
- `platform/windows`: executable entry point, application resources, and compatibility code
- `platform/macos`: AppKit hosts and the native API adapter
- `platform/posix/win32`: Win32-shaped compatibility interfaces for POSIX hosts
- `third_party/quickjs`: vendored QuickJS sources
- `examples/electron`: shared main script, HTML, and app manifest
- `examples/browser`: native browser demo frontend
- `docs`: project analysis documents
- `base`, `third_party`, `v8`, etc.: one shared upstream engine source tree
- `.build-tools`: downloaded build tools and dependency sources (ignored)
- `out`: build products, generated resources, and packaged applications (ignored)

The engine's upstream include paths are preserved rather than duplicating or
relocating Chromium/Blink/V8. Platform directories contain native implementation
details, not independently maintained projects. Windows uses LLVM/MSBuild;
macOS uses Xcode/GN/Ninja. GN alone is the macOS backend, not the shared entry point.

### Naming and compatibility cutover

Project-owned native source filenames use `snake_case`; directories describe
responsibility rather than historical product names. Product names and executable
names use `mini-electron`. Upstream source names, copyright notices, and provenance
are retained.

The public native header is `runtime/engine/public/engine_api.h`. Native functions
and types use `mini_electron_*`, and project API macros use `MINI_ELECTRON_*`.
This is a deliberate SDK/ABI break: native consumers must update and rebuild.
There are no `mb*` SDK aliases or legacy DLL-loader compatibility layer.
The injected query function is `mini_electron_query`, not `mbQuery`.
Electron JavaScript names, including `require('electron')` and `BrowserWindow`,
remain unchanged.

The cleanup removes obsolete request/WebSocket callback APIs, unsupported native
stubs, the duplicate legacy network/storage backend, and unused samples.
No CEF backend was present; the stale CEF reference was removed.
Android/iOS-only ports, mobile build entry points, and Mojo Java sources are
removed. Desktop dependencies with mobile-related names remain, including Blink
control themes, portable image codecs, web-platform types, protocol declarations,
and macOS IOSurface. Shared platform flags and upstream licensing are retained.

The Android-only cleanup also removes URL Java/JNI bindings and Robolectric
helpers, Viz Android overlay/input/output/performance-hint backends, V8 device
drivers and deployment tools, and Skia JetSki/SkQP APK/AOSP/NDK helpers.
It removes 110 files and their obsolete source-list, JNI, test and IDE references;
the Windows/macOS compilation source manifests remain unchanged. Portable Skia
codecs, desktop SkQP, Blink mobile themes/web-platform types, Vulkan declarations,
shared platform flags and upstream licenses remain.

Native Windows context menus no longer append the Chinese or English
framework-test disclaimer. Standard context-sensitive actions remain available;
when no standard action applies, no empty popup is shown.

Windows session data now lives under `profiles`. An existing `minieleses`
directory is renamed only when the destination does not exist. The macOS default
Application Support directory similarly changes from `miniblink132` to
`mini-electron`. Migration does not merge or overwrite an existing destination;
rename failure keeps using the old directory, without copy-and-delete fallback.
macOS uses [exclusive rename](https://www.manpagez.com/man/2/renamex_np/osx-10.12.3.php).
Explicit storage paths are unchanged. These legacy directory strings remain
solely for data migration. The OMP bundle identifier is now
`sh.omp.desktop.mini-electron`; OS settings keyed to the old bundle identity are
not migrated.

Native IPC has one canonical implementation and include path under `ipc/`.
Its shared `IPC::Listener` includes channel callbacks and the associated-interface
callbacks used by Chromium consumers; Blink's urgent-message observer remains
separate. Message/traits macro headers retain their multi-pass include semantics.
Native consumers must rebuild against `ipc/...`; no forwarding directory remains.
This does not replace the Electron JavaScript `ipcMain`/`ipcRenderer` APIs.

## Shared Electron example

Python 3.10 or newer is required. From the repository root, on either supported OS:

```sh
python build.py --electron --interactive
```

`examples/electron/main.js` exercises readiness, window bounds/registry/title,
`loadFile()`, and `webContents.getURL()`. Without `--interactive`, `--electron`
runs a short native-window smoke check and exits. Use `--jobs N` to control build
parallelism and `--out PATH` to change the output directory.

To run another compatible application:

```sh
python build.py --electron --interactive --electron-main /path/to/main.js
```

On Windows the script's containing directory is packaged as `resources/app`,
with `package.json.main` set to the chosen filename. On macOS the host reads the
script directly; its lightweight main-script evaluator only provides
`require('electron')`, not general Node module loading.

## Windows (x64)

Requirements:

- Visual Studio 2022 with the Desktop development with C++ workload (v143)
- Windows SDK `10.0.26100.0`
- LLVM 22 with `clang-cl`, `lld-link`, and `llvm-lib`; install in the standard
  LLVM location or set `LLVM_INSTALL_DIR`

NASM 2.16.03, pinned VSNASM customizations, and nghttp2 1.61.0 sources are
downloaded into `.build-tools` with SHA-256 verification. The Windows Node
target compiles nghttp2 for HTTP/2 support. No sibling checkout or prebuilt
demo EXE is used.
The restored Windows inputs originate from MiniBlink revision
`191f82c33fc0ff9ddaeedab90e6406a54a8e76c6`; they compile the current shared engine
sources, including the macOS changes behind their platform guards.

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
`runtime/electron/lib` during the build, with explicit resource byte lengths.
Generated headers live under `generated/runtime/electron/common` in the build
output, rather than being checked into the source tree. Windows object paths
distinguish same-named source files; long paths use a stable SHA-256 filename to
stay below FileTracker's path limit. The backend prefers native x64 MSBuild.
Windows does not currently provide the macOS-only browser demo or screenshot
option. OMP Desktop packaging is supported on both desktop targets.

Windows hidden-title windows honor `titleBarStyle`, `titleBarOverlay`, and
`setTitleBarOverlay()`. Minimize/maximize/restore/close controls are rasterized
into a cached, opaque Skia surface and alpha-composited onto the Win32 window,
using its DPI and the requested overlay height/colors. The background covers
the restored top inset as well, so page scrollbars cannot expose a white caption
edge. Opaque borderless windows retain their native DWM shadow without a rounded
window region; maximized client bounds match the visible work area. CSS
app-region dragging and no-drag elements remain active. `autoHideMenuBar`,
explicit menu visibility, Alt activation, menu replacement, and null clearing
are supported; ordinary framed windows retain their native title bar and menu.

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

The output is `out/windows-x64/OMP Desktop/OMP Desktop.exe`.
Launch it from that directory, or add `--interactive` to the build command.
`--app-output PATH` selects another package directory. Packaging requires npm
and retains the backend lockfile's production dependencies, native Node modules,
bundled OMP/ripgrep executables, and remote-backend release archives.
It creates a real `resources/app.asar`, with main/preload scripts and production
modules under `app.asar.unpacked`, and the frontend under `resources/app-dist`.
Packaging removes source maps and TypeScript declaration files (`.d.ts`,
`.d.mts`, `.d.cts`) from the staged application before creating ASAR, and removes
`node-pty` PDBs. JavaScript, ordinary `.ts` sources, native `.node`/DLL/EXE files,
licenses, and remote-backend release archives are retained.


The same executable runs the GUI, CLI, daemon supervisor, and daemon worker.
Setting `ELECTRON_RUN_AS_NODE=1` selects the statically linked Node runtime;
no stock Electron or separate Node executable is shipped. Both modes initialize
the shared V8 snapshot and ICU data. Node's native package readers and legacy
ESM resolution understand ASAR paths. `resources/bin/omp-desktop.cmd` is the
bundled CLI launcher.

For an isolated instance, set `OMP_DESKTOP_HOME` to a separate directory and
set `daemon.listen` in that directory's `config.json`. The CLI status probe uses
the persisted listen target; setting only `PASEO_LISTEN` does not isolate it
from an already running daemon on the default port.


## macOS (Apple Silicon)

The macOS targets host the Chromium 132/Blink/V8 engine in native AppKit and
headless smoke executables. They use the Xcode arm64 toolchain, generate the V8
snapshot with `mksnapshot`, and execute work from native run loops.

Requirements:

- Apple Silicon Mac with Xcode
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

The shared `python build.py --electron` command creates native AppKit windows
backed by the same renderer as the GUI demo and writes its verification
snapshot to `/tmp/mini-electron-demo.png`. Use `--screenshot PATH` to
choose another location. The macOS adapter implements readiness/quit, window
title/visibility/bounds methods, `loadFile()`, `loadURL()`,
`BrowserWindow.getAllWindows()`, and `webContents.getURL()`.

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
[`docs/OMP_DESKTOP_SIZE_ANALYSIS.md`](docs/OMP_DESKTOP_SIZE_ANALYSIS.md).

Current security boundary: the lightweight host renders Blink in-process and
builds V8 with its sandbox disabled because embedded Node's external
`ArrayBuffer` APIs are incompatible with that sandbox. It is not equivalent
to Chromium/Electron's renderer-process sandbox; load only packaged or trusted
web content.

The Win32-shaped public header now uses portable opaque handles on macOS.
Native Chromium `NS_RUNLOOP`, CoreText font selection, CPU bitmap callbacks,
POSIX paths, and the macOS Application Support storage root are selected in
their respective engine paths.

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
The original application's update check currently receives HTTP 404 for the
release's missing `latest.yml`; no automatic-update download was verified.

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
