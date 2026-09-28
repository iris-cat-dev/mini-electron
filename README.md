# mini-electron

A lightweight Electron-compatible runtime for macOS, built on MiniBlink's
Chromium 132, Blink, and V8 engine sources.

> **Status:** Apple Silicon macOS. The implemented Electron subset covers app
> lifecycle, `BrowserWindow`, `loadFile()`, `loadURL()`, and
> `webContents.getURL()`; it is not a drop-in replacement for the full Electron
> API.

## Repository layout

- `platform/macos/app`: native AppKit host and Blink bitmap presentation
- `platform/macos/api`: macOS implementation of the public `mb` API
- `platform/macos/compat`: compatibility symbols required by inherited
  MiniBlink sources; these are part of the macOS binary, not Windows targets
- `platform/macos/build/sources`: checked-in GN source manifests
- `platform/macos/resources`: demo frontend assets
- `platform/macos/smoke`: focused V8 and run-loop smoke executable
- `base`, `content`, `third_party/blink`, `v8`: engine sources

The macOS build no longer reads Visual Studio projects. Its source manifests
are explicit GN lists, so legacy `.sln`, `.vcxproj`, Windows resource, and XP
compatibility files are not part of the repository layout.

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
./platform/macos/build.py --run
```

If GN is outside `PATH`, set `GN=/absolute/path/to/gn`. The output binary is
`out/mac-arm64/miniblink_mac_smoke`; a successful run prints:

```json
{"engine":"miniblink132","platform":"macOS","arch":"arm64","runLoop":"CFRunLoop","result":42}
```

### Native GUI demo

Build and open the test frontend in a native macOS window:

```sh
./platform/macos/build.py --gui
```

To load a remote page:

```sh
./platform/macos/build.py --gui --frontend https://omp.mintcat.work/
```

The command accepts a local HTML path or an HTTP(S) URL and opens it in an
AppKit window. MiniBlink's `mb` API creates `MbWebView`, Blink creates the
`Page`/`LocalFrame` and parses the network response, the repository V8 executes
page JavaScript, and Blink's software compositor copies its pixel buffer into
the `NSView`. After the frontend is ready, the demo exercises resize, mouse,
keyboard, focus, and IME paths and writes a verification snapshot to
`/tmp/miniblink132-gui-demo.png`.

### Electron API demo

Build and run an Electron-style main-process script:

```sh
./platform/macos/build.py --electron
```

The demo at `platform/macos/resources/electron-demo/main.js` uses
`require('electron')`, `app.whenReady()`, `BrowserWindow`, `loadFile()`, and
`webContents.getURL()`. The macOS compatibility layer also implements app
readiness/quit, window title/visibility/bounds methods, `loadURL()`, and
`BrowserWindow.getAllWindows()`. It creates native AppKit windows backed by the
same MiniBlink renderer as the GUI demo and writes its verification snapshot to
`/tmp/miniblink132-electron-demo.png`.

Run another compatible main script with:

```sh
./platform/macos/build.py --electron --electron-main /path/to/main.js
```

Add `--interactive` to keep the native window open until it is closed:

```sh
./platform/macos/build.py --electron --interactive \
  --electron-main /path/to/main.js
```

### OMP Desktop package

Build a self-contained OMP Desktop application from a sibling checkout:

```sh
./platform/macos/build.py --omp-desktop --omp-source ../omp-desktop
```

The package is written to `out/mac-arm64/OMP Desktop.app`. Packaging bundles
the CLI, daemon supervisor, and daemon worker while retaining required native
Node modules, strips local symbols from the MiniBlink executable, and applies
an ad-hoc code signature after all resources are installed.

The Win32-shaped public header now uses portable opaque handles on macOS.
Native Chromium `NS_RUNLOOP`, CoreText font selection, CPU bitmap callbacks,
POSIX paths, and the macOS Application Support storage root are selected in
their respective engine paths.
