# Repository layout

[Documentation index](README.md) · [Project overview](../README.md)

> Run commands from the repository root unless a section explicitly changes directory.

## Directory map

```text
mini-electron/
├── README.md              # Project overview and quick start
├── docs/                  # Guides, release notes and analysis
├── build.py               # Shared build/package/run entry point
├── build/
│   ├── mini_electron/     # Python backends and dependency bootstrap
│   ├── windows/           # MSBuild projects and shared properties
│   └── macos/             # GN targets and source manifests
├── src/                   # Project-owned product code
│   ├── runtime/
│   │   ├── engine/        # Native API, browser/view hosting and rendering
│   │   ├── electron/      # Electron/Node bindings and embedded JavaScript
│   │   ├── network/       # Loaders, cookies and WebSockets
│   │   └── storage/       # Profiles and persistent storage
│   ├── platform/          # Windows/macOS hosts and POSIX compatibility
│   └── npm/               # Installer, CLI, types and lib/ helpers
├── examples/              # Shared Electron and native browser demos
├── test/                  # npm tests and native/runtime smoke checks
├── benchmarks/rendering/  # Rendering workloads and comparison runner
├── scripts/               # Release assembly and build-tool regressions
├── gen/                   # Pre-generated engine inputs used by build manifests
├── base/, ipc/, v8/, ...  # Shared Chromium engine source directories
├── third_party/           # Vendored and bootstrapped dependencies
├── .build-tools/          # Ignored downloaded tools and dependency caches
└── out/                   # Ignored build outputs, releases and verification
```

The tree shows responsibility boundaries, not every upstream directory. Root
`package.json` selects the implementation under `src/npm/`; `artifacts.json`
describes native releases. Installed `dist/` and `path.txt` remain at the npm
package root, not under `src/`. Package names and CLI commands are unchanged.

## Source and build responsibilities


- `build.py`: the single build, package, and run entry point; selects the host OS
- `build/mini_electron`: internal Windows and macOS build backends
- `build/mini_electron/windows/manifest.json`: ordered Windows build dependencies
- `build/windows/projects`: Windows source lists, with shared settings in `build/windows`
- `build/macos`: GN targets and desktop source manifests
- `src/npm`: installer, executable resolver, CLI, TypeScript declarations and archive helpers
- `src/runtime/engine`: public native API, browser/view hosting, rendering, and bindings
- `src/runtime/electron`: Electron/Node integration and embedded JavaScript
- `src/runtime/network`: loaders, cookies, and WebSocket integration
- `src/runtime/storage`: persistent profile and storage integration
- `ipc`: native Message/Channel IPC, serialization, and Blink scheduler interfaces
- `src/platform/windows`: executable entry point, application resources, and compatibility code
- `src/platform/macos`: AppKit hosts and the native API adapter
- `src/platform/posix/win32`: Win32-shaped compatibility interfaces for POSIX hosts
- `third_party/quickjs`: vendored QuickJS sources
- `examples/electron`: shared main script, HTML, and app manifest
- `examples/browser`: native browser demo frontend
- `docs`: installation/build guides, release notes, verification and analysis
- `base`, `third_party`, `v8`, etc.: one shared upstream engine source tree
- `.build-tools`: downloaded build tools and dependency sources (ignored)
- `out`: build products, generated resources, and packaged applications (ignored)

The engine's upstream include paths are preserved rather than duplicating or
relocating Chromium/Blink/V8. Platform directories contain native implementation
details, not independently maintained projects. Windows uses LLVM/MSBuild;
macOS uses Xcode/GN/Ninja. GN alone is the macOS backend, not the shared entry point.

Project-owned source includes use `src/runtime/...` and `src/platform/...`.
Windows MSBuild and macOS GN source manifests use those same source locations;
there are no forwarding headers or old-root include aliases. Generated build
headers retain their existing `generated/runtime/...` paths under the output
directory. The npm release assembler preserves the `src/npm/` layout.


## Generated files and cleanup boundaries

- Python `__pycache__/`, `*.pyc` and `*.pyo` files are disposable and ignored.
  Cache-only directories left by removed modules are not source modules.
- Keep `gen/`: native source manifests consume these pre-generated inputs.
- Keep downloaded dependency trees in `.build-tools/`, `third_party/angle/` and
  `third_party/dawn/` unless intentionally rebuilding their caches.
- `out/` contains more than disposable objects: release archives, native runtime
  distributions and verification evidence also live there. Do not blanket-delete
  it as part of source cleanup.
- Keep shared upstream directory names and platform guards. An unselected
  Windows/macOS source or a desktop dependency with a mobile-related name is not
  evidence that the file is obsolete.

Removed remnants include the cache-only `runtime/ui/`, `build/mini_ui/`,
`benchmarks/ui/` and `tools/idl_parser/` directories. The active rendering code
remains in `src/runtime/engine/ui/`. The unused `runtime/electron/runtime.h` wrapper
and its Windows IDE entry were removed together; no compilation unit changed.


## Naming and compatibility cutover

Project-owned native source filenames use `snake_case`; directories describe
responsibility rather than historical product names. Product names and executable
names use `mini-electron`. Upstream source names, copyright notices, and provenance
are retained.

The public native header is `src/runtime/engine/public/engine_api.h`. Native functions
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

Windows session data now lives under `profiles`; macOS's default Application
Support directory changes from `miniblink132` to `mini-electron`. When the new
destination is absent, migration copies the legacy profile into an owned sibling
staging tree and publishes it with an exclusive atomic rename. The legacy source
is retained. An existing or concurrently-created destination wins; failed copying
or publication keeps using the legacy profile and removes only owned staging.
Explicit storage paths are unchanged. These legacy directory strings remain
solely for data migration. Cookie and Local Storage outputs use the same exclusive
publication rule; malformed or unsupported Local Storage data rejects the whole
import rather than committing a partial database. The OMP bundle identifier is now
`sh.omp.desktop.mini-electron`; OS settings keyed to the old bundle identity are
not migrated.

Native IPC has one canonical implementation and include path under `ipc/`.
Its shared `IPC::Listener` includes channel callbacks and the associated-interface
callbacks used by Chromium consumers; Blink's urgent-message observer remains
separate. Message/traits macro headers retain their multi-pass include semantics.
Native consumers must rebuild against `ipc/...`; no forwarding directory remains.
This does not replace the Electron JavaScript `ipcMain`/`ipcRenderer` APIs.
