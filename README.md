# mini-ui

`mini-ui` is a native Windows x64 C++ library that combines QuickJS with a
purpose-built, CPU-rendered subset of Blink. It is intended for trusted local UI
resources embedded by a native application. This repository is not an Electron
runtime, a Node.js package, or a cross-platform release.

## Scope

The public API is [`include/mini_ui/ui.h`](include/mini_ui/ui.h). A `mini_ui::View`
loads local HTML, evaluates JavaScript, accepts resize/pointer/keyboard input,
advances work with a host-provided monotonic `Tick`, and exposes the latest
premultiplied BGRA software frame through `frame()`.

The retained web platform includes the DOM, CSS layout and painting, Canvas 2D
(including offscreen 2D/image-bitmap contexts), Promises and animation timing,
and local classic scripts and ES modules. `fetch` uses Blink's genuine Fetch
implementation: file-backed requests use the pinned local-resource loader and
document-created Blob URLs use in-process Blob/data-pipe services. Neither is
an HTTP transport; no socket, TLS, network service, or remote URL path is included.

Deliberately omitted are Electron and Node APIs, OpenSSL/network transport,
media elements and codecs, WebAudio, service workers, persistent web storage
(including cookies, `localStorage`, and IndexedDB), WebGL, WebGPU, and GPU
composition. Applications must not assume general Chromium compatibility.

## Build

### Prerequisites

Builds are native-only and require:

- 64-bit Windows 10 or later and 64-bit Python 3;
- Visual Studio C++ x64 build tools and a Windows SDK (`vswhere.exe` is used
  outside an already configured x64 Native Tools prompt);
- LLVM 22 with `clang-cl`, `clang`, `lld-link`, `llvm-lib`, `llvm-readobj`,
  `llvm-nm`, and the x64 compiler runtime. Set `LLVM_INSTALL_DIR` if LLVM is not
  on `PATH` or installed under `C:\Program Files\LLVM`;
- Ninja 1.10 or later on `PATH`;
- `ml64.exe` and NASM 2.16.03 or compatible on `PATH` (the build also recognizes
  the repository-local cached NASM path);
- the complete source checkout and its bundled Chromium Python generation
  inputs. Binding generation imports `json5`, `mako`, and `jinja2`.

No dependency bootstrap is performed. From the repository root:

```powershell
py -3 build.py --ui --ui-target library
py -3 build.py --ui --ui-target host
```

The default output directory is `out/ui-windows-x64`. The library target writes
`mini-ui.lib`; the host target also builds `mini-ui-example.exe` from
[`examples/ui/main.cc`](examples/ui/main.cc). Use `--out out/ui-name` to select
another output below `out/ui-*`, `--jobs N` to set parallelism, or add `--run`
to the host command to launch it. `--ui-configure-only` creates the Ninja graph
without claiming to build the library.

The generated `source-closure.json`, job descriptions, response files, and map
file record the exact configured native graph. There are no npm/package build
steps.

Launch the interactive Win32 demo from the repository root:

```powershell
out/ui-windows-x64/mini-ui-example.exe
```

Its assets are in `examples/ui/resources`. The sidebar links scroll to sections
of this one page; they are not separate routes and do not change the selected
navigation highlight. `--smoke` runs the regression below and then closes.

Run the native host's end-to-end regression explicitly:

```powershell
out/ui-windows-x64/mini-ui-example.exe --smoke
```

It exercises actual script/module execution, Blob URL response bytes and headers,
Promise/rAF and clone/transfer behavior, iframe realms, native input, software
select popups, scaled BGRA output, resource-root denial, and destroy/recreate.
It also checks sidebar fragment navigation and Win32 mouse hover/leave while
Canvas animation continues, including the hover style and real Canvas pixels.
The demo's Canvas backing store follows its observed CSS content size and
`devicePixelRatio`; drawing remains in CSS pixels rather than stretching a fixed
bitmap. The smoke checks the marker's pixel aspect ratio at 1x/1.25x/2x scale,
wide viewports, and content-only layout changes.
The MINI profile retains Canvas drawing/frame finalization and its draw-type
enum, but excludes the Chrome-only per-task Canvas performance observer and
its reporting lifecycle. Other Canvas API counters and drawing are unchanged.

## Embedding

Link `mini-ui.lib`, compile as C++20 with the same Windows SDK/toolchain ABI, and
include `include/`. The complete Win32 host demonstrates input translation and
BGRA presentation; the essential lifecycle is:

```cpp
#include <chrono>
#include <memory>
#include <stdexcept>
#include "mini_ui/ui.h"

static double Now() {
  return std::chrono::duration<double>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

mini_ui::Error error;
mini_ui::ViewOptions options;
options.resource_root = LR"(C:\absolute\path\to\app-resources)";
options.width = 1280;
options.height = 720;
options.device_scale = 1.0f;
options.native_window = hwnd;  // borrowed; the host still owns this HWND

auto view = mini_ui::View::Create(options, &error);
if (!view || !view->LoadFile("index.html", &error)) {
  throw std::runtime_error(error.message);
}

// Call from the creating/native UI thread while pumping Win32 messages.
if (view && view->Tick(Now(), &error)) {
  mini_ui::FrameView frame = view->frame();
  // Copy or present frame.pixels as premultiplied BGRA using frame.stride.
}

// Before DestroyWindow(hwnd):
view.reset();
```

`FrameView` memory is owned by the view and remains valid only until the next
mutating view operation or destruction. `Tick` values must be finite,
non-negative, and monotonically increasing; use a real monotonic clock rather
than a synthetic frame counter.

View dimensions and absolute pointer coordinates are logical CSS/DIP units.
`device_scale` maps those units to the physical pixels returned by `frame()`.
For a native HWND, use its actual DPI divided by 96 and pass physical Win32
mouse coordinates divided by that scale to `Pointer()`. Call `Resize()` when
the client dimensions or DPI change; do not multiply the input coordinates a
second time.

A non-null `native_window` is borrowed, not adopted. The host owns its creation,
message pump, and destruction, and must destroy the view first. A page-initiated
close requests closure with `PostMessage(WM_CLOSE)` only; the library does not
synchronously destroy the HWND. Native pointer lock additionally relies on that
window's input messages and temporary capture/raw-input ownership as described
in the public header.

The native adapter synchronizes real OS focus and native tab traversal, owns
only its custom cursor handles, and positions the existing Windows IME
composition/candidate windows from the focused DOM caret. It does not replace
the host's IME association or change the user's input language. Committed text
is supplied through `KeyEvent::Type::text` as UTF-16, including surrogate pairs.

Inherited/keyword text starts with the canonical renderer font-size preferences
(16px proportional, 13px fixed), rather than bare `WebView`'s zero-initialized
settings. Windows system-font families/metrics and antialias preferences come
from real Win32 settings; user text gamma/contrast uses the native DirectWrite
parameters when configured. Font metrics are initialized at the 96-DPI CSS
baseline and view scaling is applied separately.

Blink/QuickJS process state is initialized once on the thread that creates the
first view and is retained between views until process shutdown. All views and
all their operations must remain on that single native UI thread. Hosts should
therefore treat initialization and teardown as process-lifetime facilities, not
as independently restartable per-thread runtimes.

## Local-resource boundary

`resource_root` is opened and pinned when the view is created. File-backed
`LoadFile`, subresources, module imports, and Fetch resolve only beneath that
root. Absolute escape, traversal, UNC/device/drive-relative paths,
network-relative URLs, and external network schemes are rejected. Replacing
the root pathname cannot redirect an existing pinned grant. In-memory Blob
URLs and CPU data-URL decoding do not confer file or network authority. This
boundary is for local application assets; it does not turn untrusted page code
into a networked browser sandbox.

## Benchmarks

[`benchmarks/ui/README.md`](benchmarks/ui/README.md) documents the reproducible
public-consumer Vue 3.5.13 and React 18.3.1 workloads, pinned asset provenance,
commands, raw captures/results, and aggregation definitions. Build the official
library before running that consumer; do not run its build step concurrently
with Ninja.

Reported frame throughput is generation-producing, CPU-offscreen `View::Tick`
throughput. It is **not** onscreen FPS, display refresh rate, or vsync FPS.
[The 2026-10-22 recorded comparison](benchmarks/ui/results/2026-10-22/README.md)
contains 15 actual native validation scenarios and 24 completed Vue/React
matrix cells, including genuine Google Chrome GPU/software controls. It records
the remaining JS/DOM commit and memory gaps, raw data, observed failures/fixes,
and measurement limitations rather than claiming Chrome-equivalent performance.

## License and upstream code

Project-authored code is licensed under Apache License 2.0; see [`LICENSE`](LICENSE).
The checkout contains substantial upstream Chromium/Blink, Skia, QuickJS, ICU,
and other third-party code under their respective licenses and copyright
notices. Relevant pointers include
[`third_party/blink/LICENSE_FOR_ABOUT_CREDITS`](third_party/blink/LICENSE_FOR_ABOUT_CREDITS),
[`third_party/skia/LICENSE`](third_party/skia/LICENSE), QuickJS source-file
license headers under `third_party/quickjs/`, and license/readme files within the
other `third_party/` trees. Redistribution must preserve the applicable upstream
notices.
