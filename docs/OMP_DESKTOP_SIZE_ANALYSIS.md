# OMP Desktop size analysis

This report records Windows x64 release reductions and the original Apple
Silicon package analysis. Windows measurements are separated below; all
remaining component attribution and compiler estimates describe macOS.
The macOS baseline was taken from commit `87cf65a9f`. The equivalent packaging
command through the unified entry point is:

```sh
python build.py --omp-desktop --omp-source ../omp-desktop
```

The macOS packaged host is stripped with `strip -x` and then ad-hoc signed.
Sizes in MiB use 1 MiB = 1,048,576 bytes.

## Windows x64: implemented release reductions

The Windows `Release|x64` target disables Blink/V8/Node Inspector and DevTools,
and excludes the experimental Permission Element implementation and binding.
The measurements below describe the historical ASAR package; current OMP Desktop releases stage
the application under `resources/app`. That measured build removed `.d.ts`, `.d.mts`, and `.d.cts`
files before ASAR creation and removed `node-pty` PDBs. Ordinary TypeScript sources, runtime
native modules, remote-backend archives, and licenses remained.

Measured logical file bytes, including the final Windows caption fixes:

| Artifact | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Host executable | 83,176,960 bytes (79.324 MiB) | 80,138,240 bytes (76.426 MiB) | 3,038,720 bytes (2.898 MiB, 3.65%) |
| `resources/app.asar.unpacked` | 120,236,510 bytes (114.666 MiB) | 76,769,987 bytes (73.214 MiB) | 43,466,523 bytes (41.453 MiB) |
| `resources/app.asar` | 2,610,815 bytes (2.490 MiB) | 1,820,695 bytes (1.736 MiB) | 790,120 bytes (0.754 MiB) |

The freshly assembled complete Windows package is 431,346,933 bytes
(411.364 MiB), excluding generated runtime state. Its bundled `bin/omp.exe`
alone is 243,411,968 bytes (232.136 MiB), and reports `omp/18.4.4`.
Both older comparison directories had subsequently lost that bundled file,
so their current directory totals are not complete-package baselines.
No build PDB or static library is included in the release package.

Verification exercised the built host and packaged production modules:

- Main, preload, and Node workers report Inspector disabled. Both
  `node:inspector` modules fail with `ERR_INSPECTOR_NOT_AVAILABLE`; the
  `--inspect` flag exits with code 9.
- DevTools/inspection methods are absent. `<permission>` creates an
  `HTMLUnknownElement`, with no `HTMLPermissionElement` constructor.
- Page and worker exception locations, stacks, and rejected promises remain
  available. Canvas pixels, fetch, WebAssembly, local/session storage, and
  cross-frame storage events pass the runtime probe.
- Node retains Chinese `Intl`, crypto, WebAssembly, worker execution, native
  keyring discovery, and a real ConPTY command with graceful worker cleanup.
- ASAR inventory contains no TypeScript declaration files or `node-pty` PDBs.
  The Chinese OMP frontend loads and the isolated daemon is reachable.
- At 144 DPI, restored caption glyph bounds and bright-pixel counts match the
  installed Electron reference: 15-by-2 minimize, 15-by-15 maximize and close.
  The cached opaque Skia background covers the two-pixel restored top inset;
  no white pixels remain on the former caption edge. Native DWM rendering
  stays enabled. Minimize, maximize, restore, and close succeed, and the
  maximized client matches the visible 3840-by-2100 work area.

Detailed runtime evidence is recorded in
`out/verification/windows-size-cut-results.json`; the complete package is
`out/windows-x64/OMP Desktop Optimized`. Existing test-home and desktop profile
state are preserved.

## macOS: frontend startup I/O (2026-10-17)

The OMP Desktop 0.5.1 entry script is 17,993,691 bytes. Its local file response
used the Node stream default of 64 KiB per read. The custom protocol waits for
the complete response body before delivering the script, so repeated small
reads delayed the first usable frontend. The runtime now caps read chunks at
1 MiB or the file size, whichever is smaller. Streaming, byte-based
backpressure, cancellation, MIME types, and HEAD semantics are preserved;
there is no persistent cache or whole-file read added.

The same real packaged frontend and isolated profile were exercised with the
macOS arm64 runtime before and after the change. A temporary protocol wrapper
recorded body consumption and page paint events; the final measurement used
the production implementation without an I/O override.

| Stage | Before | After |
| --- | ---: | ---: |
| Entry-script response body consumption | 4660 ms | 365.4 ms |
| Navigation to first contentful paint | 5364 ms | 1073.2 ms |

These are individual warm-filesystem samples, not cold-disk guarantees or a
comparison with the installed conventional Electron build. Main-process
module loading and daemon readiness are separate stages. A real local
workspace and conversation input opened after the optimized startup.

The full packaged application was also launched through macOS Launch Services
(`open -n -W -a`) with the same isolated profile and Iris enabled. Removing the
desktop daemon manager's import of the complete server entrypoint and deferring
pet rendering moved work off the main-window critical path.

| Full launch stage | Before | After |
| --- | ---: | ---: |
| Main module loaded | 897 ms | 474 ms |
| Main window created | 2457 ms | 1068 ms |
| First contentful paint | 3676 ms | 2325 ms |

In the final package run, managed-daemon startup returned at 4780 ms, the frontend
signaled settled startup at 4963 ms, and the pet window was created at 5632 ms,
after main content was visible. The isolated managed daemon reported `running`.
An earlier optimized run reached first contentful paint at 2632 ms. These are
single warm-filesystem observations with temporary timing instrumentation, not
a cold-launch latency guarantee.

## macOS: single runtime executable (2026-10-16)

The macOS runtime no longer stages four full executable copies inside
`Electron Helper*.app`. The GUI, renderer subprocesses, and Node workers reuse
the host executable. Renderer process isolation and the Seatbelt profile are
unchanged. Squirrel, Mantle, and ReactiveObjC remain required dynamic libraries.

Measurements from the rebuilt runtime and the OMP Desktop `0.5.1` arm64 bundle,
summing regular-file bytes without following symlinks:

| Artifact | Before | After |
| --- | ---: | ---: |
| Complete `OMP Desktop.app` | 696,993,733 bytes (664.71 MiB) | 348,976,942 bytes (332.81 MiB) |
| Runtime ZIP | 161,615,734 bytes (154.13 MiB) | 33,240,012 bytes (31.70 MiB) |
| Helper application bundles | 4 | 0 |

The remaining `Contents/Frameworks` is 1,528,708 bytes (1.46 MiB). This reduction
does not remove the bundled OMP executable or application resources.

All roles share the app's Info.plist, so it defaults to `LSUIElement=true`.
Only GUI BrowserWindow creation promotes its process to a regular Dock
application. Without this default, renderer and Node subprocesses also acquire
Dock icons even though they no longer use separate helper executables.

Verification exercised the real packaged GUI, separate renderer, frontend
connection to the desktop-managed daemon (`online`, directory `ready`), native
mouse navigation to Schedules, CLI version/status, and a cold CLI daemon start.
During 45 startup samples only the GUI had activation policy `Regular`;
the renderer, supervisor, and daemon had `Accessory`. The packaged smoke
regression also checks background process trees for unwanted Dock visibility.
`codesign --verify --deep --strict` accepted the final ad-hoc-signed app.
Developer ID signing and notarization were not performed.

The macOS renderer also preserves Skia's native RGBA order instead of applying
the Windows BGRA swap unconditionally. A real rendered six-swatch page and
`capturePage().toPNG()` matched red, green, blue, brand cyan `#00DBE4`, brand
magenta `#F84FCC`, and white exactly. The packaged application's native window
was visually checked after the fix. The current runtime disables DevTools, so
the CDP screenshot branch was not exercised.

The system libcurl exported WebSocket symbols but rejected `ws`/`wss`.
The runtime statically links pinned libcurl 8.23.0 with WebSocket support,
reusing the existing TLS library. The current HTTP/WebSocket curl configuration
disables TLS peer/host verification; earlier certificate-rejection claims are
not current verification. The recorded native renderer regression covers
subprotocol negotiation, fragmented text, binary data, ping/pong, and clean close.
No extra networking framework bundle is introduced.

Native AppKit handling now consumes Blink's draggable-region updates, subtracts
`no-drag` controls, and handles caption mouse-down and double-click. macOS keeps
native drag regions after zooming instead of switching to the manual fallback.
Real mouse events moved the packaged window by (+80, +40) points without resizing,
zoomed it from 1200 × 800 to 1647 × 1084, and restored its exact previous bounds.
The titlebar sidebar toggle remained clickable without moving the window.

IPC invocation and send events now retain their native source-frame identity.
The main-frame wrapper is stable for a document and becomes detached after
replacement. The native regression verifies main-frame authorization, subframe
rejection, identity reuse, and navigation detachment. In the actual packaged
frontend, 23 activity publications over 159 seconds succeeded with no failures,
including a language change; no pet-activity error toast remained. The optional
pet overlay separately lacks `setIgnoreMouseEvents` and is not covered by that
activity-sync result.

The `system-ui` CJK regression required only metadata access to the `/System`
directory itself. Independent CoreText probes isolated that ancestor lookup;
existing access under `/System/Library` was insufficient. Before/after rendered
font comparisons and the actual Chinese settings UI were visually checked.
The sandbox otherwise remains unchanged and no fonts are added to the bundle.

Native AppKit event injection into the real packaged renderer moved a scrollbar
from 0 to 1472 pixels, stopped after release, and scrolled down to 1522 then back
to 1472. Wheel scrolling also moved the actual Settings sidebar to 38.5 pixels.
The Settings sidebar deliberately hides its native scrollbar, so its empty edge
is not a drag target. The workstation was locked during these checks; global
physical-pointer injection could not be exercised, so an external temporary
native addon delivered real NSEvent objects through `MiniElectronRendererView`.

The runtime now supplies OS preferred languages to the sandboxed navigator
instead of the legacy `cn-ZH,cn` constant. macOS language smoke scenarios with
per-process English and Chinese preferences both passed without changing global
system settings. App-level manual overrides retain their existing precedence.
A fresh packaged-app profile automatically displayed Chinese for the real
`zh-Hans-US, en-US` preference list. Manual English survived reload despite
the Chinese system preference; selecting System restored the Chinese UI.

Hover and scroll flashing had the same native presentation race: the host
cleared the full view before a shared-frame read, then returned with only the
clear color if the renderer was writing. The presenter now retains a validated
image while a separate reusable buffer receives the next frame. A failed read
never modifies the presented pixels, and ordinary redraws reuse the image.
The permanent native regression fails on the previous runtime and passes after
the fix; it forces writer contention through the real NSView draw path and
then verifies that a later complete frame still changes the displayed color.

macOS `webContents.prepareToShow(): Promise<void>` is the startup presentation
fence used after application content-ready IPC. It requests a successful
post-request compositor presentation with forced viewport damage, receives the
stable shared-frame generation, and resolves only after the hidden native view
has drawn that generation or a newer complete frame into its backing store.
This is not the initial `ready-to-show` lifetime paint or a `capturePage` read,
and does not certify WindowServer scanout. Destroying or losing the renderer
rejects outstanding preparation. Native hosts without this backing-store
acknowledgement reject the operation rather than pretending to present.
`show`, `showInactive`, and `focus` also draw the current complete shared frame
before ordering a macOS window onscreen. The existing native presentation
regression now starts hidden and prepares static content before first reveal.

The final packaged OMP startup profile kept the application window hidden through
the renderer's first contentful paint at 3,339 ms. The native presentation fence
started the reveal at 3,450 ms, returned with the window visible at 3,458 ms, and
`capturePage()` returned a non-empty branded startup frame at 3,514 ms. The separate
AppKit regression supplies the native-view draw proof; this renderer capture is not
claimed as independent WindowServer scanout evidence. Moving Mermaid, WebSocket
response validation, and terminal emulation behind their use sites reduced the
startup script body from about 18 MB to 11,307,818 bytes. Its 3.4 MB, 2.1 MB, and
880.6 KB chunks, respectively, were absent from the initial request set; the
validator loaded after first reveal when the daemon connection first needed it.

Hidden-titlebar/full-size-content and frameless windows also need an interior
native resize border: the existing Resizable style alone left edge events on
the renderer/caption input path. The previous runtime failed a real NSWindow
left-edge drag with an unchanged 520-point width instead of the expected 544.
The host now handles a five-point edge/corner border before caption/web input,
uses screen-space deltas, anchors the opposite edge, and clamps window min/max
sizes. Full-screen and non-resizable windows retain their non-resize behavior.
The native regression covers all eight directions, shrinking and expanding,
constraints, release, resize cursors, no-drag controls and viewport updates.
It also enters/exits real AppKit full screen for OMP's hidden-titlebar style.

The native-edge revision passed eight resize directions and
cursor checks, growing from 1200 × 800 to 1240 × 830 for corner drags. Six
hover targets and Settings wheel scrolling retained identical presented pixels
during forced frame contention. The Chinese System-language UI remained
readable, the managed host was online, and 17 pet-activity publications had no
errors. These checks delivered genuine NSEvents to the actual NSWindow/view;
an external process-targeted pointer probe did not change the bounds, so this
does not claim an independent physical-pointer/manual acceptance check.

Those geometry and final-viewport checks did not cover continuous resize frame
publication; the user subsequently reported white screens and flickering.

The resize compositor was submitting a root frame without its page embedded,
while Blink waited for the acknowledgment before preparing replacement content.
An allocation handoff could also let queued damage publish a new white-cleared
bitmap before painting. The corrected path retains the active child surface
through a referenced fallback range until replacement activation. It coalesces
size/scale changes and publishes only actual software swaps, with acknowledgment
after UI consumption and retained allocation ownership through teardown.

The new continuous-resize regression observes committed CALayer pixels through
rapid timer-paced native events, not direct `drawRect:` calls. It covers retained
presentation with a paused renderer, later color/frame updates, bounded final
viewport recovery and a visible replacement-layout marker. The actual OMP
baseline additionally captured four samples where all nine detection points
became white during one 80-step resize sequence.

The white-frame revision passed the same 80-step native resize sequence with
zero white detection points in every sample (previous bundle: four all-white
samples). After release, the native layer and matching renderer viewport were
observed within 75 ms. The committed-layer regression also passed rapid resize,
writer pause/resume, subsequent complete colors, and visible replacement layout;
the eight-direction/full-screen, contention, IPC, WebSocket, language and 11 npm
tests passed. Chinese System-language rendering remained readable; the isolated
host was online and pet activity publication had no errors. Verification used
genuine AppKit event injection and committed-layer sampling, not physical display
scanout or a manual pointer acceptance check. Windows/Linux runtime resize and
shutdown were not exercised on this macOS workstation.

The following geometry revision addresses the subsequent squeezed-content
report. Its predecessor nonuniformly stretched retained native images and child
fallback surfaces. A deterministic paused-renderer portrait test changed an
80 × 80 DIP square into 46 × 104 DIP. The corrected presenter clips at original
pixel density, and Viz uses natural fallback geometry with alpha-correct child
background gutters. A readiness-triggered viewport retry closes the static-page
lost-final-resize race; canonical allocator merging prevents stale metadata from
rewinding the parent ID while retaining the new dimensions.

The geometry regression passed paused retention, static final convergence,
rapid static transitions, animated layouts and subsequent real frame updates.
It checks viewport and marker bounds across 700 × 1050, 1500 × 500, 620 × 1000
and 1200 × 800. In the actual packaged UI, temporary visual markers retained an
80 × 80 fixed square and 100 × 100 responsive square at all four sizes; responsive
positions matched DOM layout. The temporary marker overlay was removed before
visually checking the unmodified Chinese homepage. Eight-direction/full-screen,
contention, IPC, WebSocket, language and 11 npm regressions also passed. This
revision retains the same single-executable/process-isolated architecture;
Windows/Linux runtime verification and physical-pointer scanout remain outside
the exercised checks.

### Duplicate-window and storage correction

Apple crash reports established two renderer exits under the same GUI process.
The native storage stub synchronously acknowledged writes before Blink queued
its pending mutation; burst writes reproduced a renderer exit in the actual OMP
page. Storage notifications now retain their payload and run on the next Blink
task. Storage and synchronous navigation reply callbacks also notify under their
completion lock, preventing the waiter from destroying its stack condition
variable before the reply thread's final use.

Renderer liveness is no longer reported as `WebContents.isDestroyed()`. The
owning native window remains eligible for selection until actual destruction.
OMP coalesces implicit reopen/tray/pet/relaunch creation, while explicit New
Window/project creation remains independent. Only the successful desktop lock
owner performs daemon/update shutdown; failed-lock secondary launches exit
without stopping the primary's managed daemon.

Before correction, terminating only an isolated probe's renderer and issuing
two activate events produced three real native windows. The final signed bundle
completed 1,000 alternating storage writes in 8.776 seconds without a renderer
exit. Ten native reopen callbacks reused the same window and renderer. The real
AppKit New Window menu action added one window; after both windows were destroyed,
ten reopen callbacks created just one replacement. CGWindowList confirmed one
visible native main window. Three plain same-profile launches exited successfully
without adding windows or stopping the managed daemon; actual primary quit then
removed the owned daemon record and exited successfully.

Desktop launch/background/navigation/quit checks passed 31 tests. Native storage
burst/Unicode/delete/clear/reload persistence and renderer-loss lifetime smokes
passed, as did IPC, WebSocket, navigator languages, cached presentation and 11 npm
tests. Native edge input on the actual final UI changed width from 1200 to 1248
while preserving height 800. The broader resize smoke stopped waiting for AppKit
to finish entering full screen while `loginwindow` was frontmost; this revision
does not claim a newly verified full-screen transition or physical-pointer input.
The GUI/profile used for these acceptance checks was temporary; no existing user
GUI or its renderer was deliberately terminated.

The following sections retain the earlier engine-size baseline and optimization
history; their historical totals are not measurements of this bundle.

## macOS executive summary

The baseline `OMP Desktop` executable was 90,623,888 bytes (86.43 MiB).
Sharing the full ICU data package between Blink and Node/V8 reduced it to
88,736,048 bytes (84.63 MiB). Removing unsupported desktop surfaces reduced
the current signed host to 85,415,856 bytes (81.46 MiB): 5,208,032 bytes
(4.97 MiB, 5.75%) below the original baseline. Baseline live-byte attribution
was:

- Blink/MiniBlink: 38.29 MiB
- V8: 21.71 MiB
- ICU: 13.78 MiB before deduplication, approximately 11.98 MiB after
- Node and its protocol dependencies: 9.81 MiB
- OpenSSL: 2.24 MiB

The linker already removes dead code and the package already strips local
symbols. More aggressive `strip` saves only about 6 KiB and removes N-API
exports required by `node-pty` and `@napi-rs/keyring`, causing those modules to
crash while loading.

The current 81.46 MiB host reached the upper edge of the original conservative
target without changing JavaScript, WebAssembly, `Intl`, media, or rendering
semantics. Further large reductions require compiler/V8 changes or explicit
compatibility and performance tradeoffs.

The complete `.app` now occupies 377,684 KiB (368.83 MiB). This includes both
the engine reductions and removal of the obsolete Sherpa backend. Its largest
single file remains the 198.80 MiB `Resources/bin/omp` Bun executable.

## Implemented optimization: shared ICU data

The macOS source manifest no longer compiles
`runtime/engine/resources/icudtl.cc`. During Blink startup,
`RenderThreadImpl::initializeICUData()` calls `u_init()`, which discovers the
full `//third_party/icu:icudata` package already linked for V8 and Node. It no
longer replaces ICU's common data pointer with MiniBlink's legacy 1.80 MiB
array. Non-macOS builds retain the legacy fallback.

| Measurement | Baseline | Shared ICU data | Reduction |
| --- | ---: | ---: | ---: |
| Signed host executable | 90,623,888 bytes | 88,736,048 bytes | 1,887,840 bytes (1.80 MiB, 2.08%) |
| Complete `.app`, allocated | 417,136 KiB | 415,296 KiB | 1,840 KiB (1.80 MiB) |

Verification used the packaged application:

- Blink and Node both resolved `Intl.Locale('zh-CN').maximize()` to
  `zh-Hans-CN`.
- `Intl.DateTimeFormat` produced `2026年3月17日星期二` for the fixed
  Asia/Shanghai test value.
- `Intl.Collator('zh-CN')` sorted 北京、广州、上海 in that order.
- A Blink visual smoke rendered Chinese and exercised marked-text composition
  plus committed text, producing `中文PASTE_OK` in an input.
- The packaged OMP frontend and its Chinese UI loaded successfully.
- `codesign --verify --deep --strict` accepted the resulting `.app`.

## Implemented optimization: unsupported desktop surfaces

The macOS target now compiles with `MINIBLINK_DISABLE_DEVTOOLS` and
`MINIBLINK_DISABLE_PERMISSION_ELEMENT`.

- Frame and worker DevTools transports, inspector agents, and inspector-only
  probe dispatch are unreachable. The lightweight debugger hooks still forward
  page and worker exceptions and rejected promises through Blink's normal
  console reporting paths. Non-DevTools probes used by ad tracking, animation
  timing, LCP observation, and performance monitoring remain enabled.
- `HTMLPermissionElement` and its generated V8 binding are absent.
  `document.createElement("permission")` produces an `HTMLUnknownElement`, so
  pages do not retain a partially implemented experimental API.
- The HarfBuzz core aggregate replaces `harfbuzz-subset.cc`. Complex-script
  shaping remains; `hb_subset_*`, subset planning, CFF subsetting, and repacking
  are not linked. The macOS host exposes no print-to-PDF or font-subsetting
  entry point.

The measured linker-map changes are:

| Measurement | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Total live linker output | 88,303,772 bytes | 84,990,929 bytes | 3,312,843 bytes (3.16 MiB) |
| Signed host executable | 88,736,048 bytes | 85,415,856 bytes | 3,320,192 bytes (3.17 MiB, 3.74%) |

The live reduction decomposes into 2,740,739 bytes (2.61 MiB) from the
Inspector/DevTools/probe dependency closure, 563,598 bytes (0.54 MiB) from
Permission Element, and 8,506 bytes from HarfBuzz. Normal issue reporting and
the settings/device-emulation helper remain; no DevTools transport can
instantiate their inspector agents.

The original 0.65 MiB HarfBuzz candidate was the complete shaping-plus-subset
aggregate, not the subset implementation alone. Apple `ld -dead_strip` had
already removed nearly all unreachable subset atoms, so replacing the aggregate
still removes the API and source dependency but saves only 8.3 KiB of live
output.

Verification covered the changed paths:

- A rendered page reported
  `permission=unknown worker=42 shaping=ok`; `<permission>` was an
  `HTMLUnknownElement`, a dedicated worker completed, and expected page/worker
  error reporting did not crash.
- The same page rendered Chinese, Arabic, Devanagari, and Korean text and
  produced `中文PASTE_OK` through marked-text composition and paste.
- The final linker map contained no `hb_subset_*`, `WebDevToolsAgentImpl`, or
  V8 inspector-creation symbol and no Permission Element implementation or
  binding object. The stripped host exported none of those APIs.
- The packaged OMP Chinese UI loaded; packaged Node retained full Chinese
  `Intl`; `node-pty` spawned `/bin/zsh` and returned `TERMINAL_OK`.
- `codesign --verify --deep --strict` accepted the packaged application.

## Implemented optimization: removed Sherpa backend

The current OMP backend no longer uses `sherpa-onnx-node`. The package optimizer
therefore no longer externalizes or copies that package, its Darwin arm64
binary, or the local `silero_vad.onnx` asset.

Together with the host reductions above, this changed complete application
allocation from 415,296 KiB (405.56 MiB) to 377,684 KiB (368.83 MiB), a
37,612 KiB (36.73 MiB) reduction. Backend allocation is now 69,220 KiB
(67.60 MiB), down from approximately 101.0 MiB.

## Measurement method

The analysis used:

- `stat` and `du` for logical and allocated sizes;
- `size -m` for Mach-O segments and sections;
- an Apple linker map generated from the GN/Ninja link inputs;
- per-object live-symbol aggregation from the linker map;
- `strip -x`, default `strip`, and ad-hoc signing on temporary copies;
- representative `-O2`, `-O3`, and `-Os` recompiles;
- direct loading of the packaged N-API modules after stripping.

The linked target already uses:

```text
-O2                           normal Blink and Node targets
-O3                           V8 speed-optimized targets
-g0
-fvisibility=hidden
-fno-exceptions
-fno-rtti
-Wl,-dead_strip
symbol_level=0
```

The final packaging step adds:

```text
strip -x
codesign --force --deep --sign -
```

`-dead_strip` removed 26.67 MiB of unused atoms before the final binary was
written. This is why archive file sizes cannot be added directly to estimate
the executable.

## Main executable baseline

### Strip results

| Variant | Bytes | MiB | Reduction from unstripped |
| --- | ---: | ---: | ---: |
| Unstripped link output | 131,205,368 | 125.13 | - |
| `strip -S` | 131,205,352 | 125.13 | 16 bytes |
| `strip -x` | 91,153,672 | 86.93 | 38.20 MiB / 30.53% |
| `strip -x` plus final signature | 90,623,888 | 86.43 | 38.70 MiB / 30.93% |
| Default `strip` plus signature | 90,617,664 | 86.42 | 38.71 MiB / 30.93% |

Default `strip` is not usable. It saves only 6,224 additional bytes but removes
such exports as `_napi_create_function` and `_napi_get_cb_info`. Built-in Node
modules still work, but both `node-pty` and `@napi-rs/keyring` terminate with
`SIGSEGV` while loading. `strip -x` is therefore the correct production mode.

The executable contains 156 intentionally exported symbols for Node native
addons. Its `__LINKEDIT` segment is only 0.70 MiB, so symbol-table work cannot
produce another meaningful reduction.

### Mach-O sections

| Section | MiB | Share of file |
| --- | ---: | ---: |
| `__TEXT,__text` | 60.09 | 69.5% |
| `__TEXT,__const` | 17.95 | 20.8% |
| `__TEXT,__cstring` | 2.62 | 3.0% |
| `__DATA_CONST,__const` | 2.62 | 3.0% |
| `__DATA,__data` | 2.27 | 2.6% |
| Other sections and link metadata | 0.88 | 1.1% |

The main opportunity is reducing compiled code and embedded data, not stripping
more names.

### Logical component attribution

The following values sum live symbols and data atoms from the baseline linker
map, before ICU deduplication. Small alignment gaps and link metadata are not
assigned to a component.

| Component | MiB | Share of live bytes |
| --- | ---: | ---: |
| Blink/MiniBlink rendering stack | 38.29 | 44.5% |
| V8 engine and snapshot | 21.71 | 25.2% |
| ICU data and implementation | 13.78 | 16.0% |
| Node runtime and protocol dependencies | 9.81 | 11.4% |
| OpenSSL | 2.24 | 2.6% |
| Native Electron/OMP host | 0.08 | 0.1% |
| Linker/runtime metadata and system stubs | 0.09 | 0.1% |

The embedded Node implementation is directly responsible for about 9.81 MiB,
not the full executable. Blink and Node share one V8 implementation in the same
Mach-O image.

## Component details

### Blink and MiniBlink

Largest linked groups:

| Group | MiB |
| --- | ---: |
| Blink core | 22.74 |
| Generated Blink bindings and CSS data | 5.13 |
| Content integration | 4.51 |
| Chromium support code | 2.43 |
| Utility code | 1.66 |
| Skia | 1.41 |
| HarfBuzz | 0.65 |
| Base | 0.60 |
| libxml and libxslt | 0.51 |

Candidate feature groups, measured from the original linker map, are:

| Candidate | Baseline attribution | Current status |
| --- | ---: | --- |
| Inspector/devtools/probes | 1.20 MiB lower bound | Removed; complete dependency closure saved 2.61 MiB |
| SVG | 1.22 MiB | Required by the OMP UI; keep |
| Permission Element | 0.54 MiB measured | Removed |
| Canvas/WebGL/GPU | 0.59 MiB | Used by rich rendering; keep unless proven unused |
| Media/audio/video | 0.55 MiB | Needed for OMP voice/media capability |
| Workers/service workers | 0.46 MiB | Workers retained; service-worker subset may be removable |
| HarfBuzz aggregate | 0.65 MiB | Core shaping retained; subset API removed, saving 8.3 KiB |

`html_permission_element.o` contributed 560,557 live bytes and its generated V8
binding contributed 3,041 bytes. Both are absent from the macOS link; generated
tag registration now creates an `HTMLUnknownElement`.

The OMP frontend loads `/poster_bg-DAFj3nST.wasm`; WebAssembly support is not an
optional browser feature for this package.

### V8

| V8 group or feature | Measured live size |
| --- | ---: |
| Base runtime without compiler | 11.60 MiB |
| Turbofan/compiler directory | 7.01 MiB |
| Embedded snapshot | 2.26 MiB |
| Maglev-named objects | 2.31 MiB |
| WebAssembly/Liftoff-named objects | 2.17 MiB |
| Intl/Temporal-named objects | 0.62 MiB |
| Profiler/debug-named objects | 0.60 MiB |

Feature rows overlap the directory rows and are lower bounds, not additive
subtotals.

Disabling Maglev with `v8_enable_maglev=false` should remove approximately
2-3 MiB while preserving JavaScript semantics, Sparkplug, and Turbofan. The
tradeoff is slower warm-up and lower performance for medium-hot JavaScript.

Disabling WebAssembly is not acceptable because the packaged frontend uses a
WASM module. V8 lite mode is also unsuitable: it disables WebAssembly and JIT
compiler tiers and would materially reduce daemon and frontend performance.

Moving the 2.26 MiB V8 snapshot to a resource file only moves bytes out of the
Mach-O; it does not reduce the application package.

### ICU

The binary now carries:

| ICU contribution | MiB |
| --- | ---: |
| Full current ICU data (`icudtl_dat.o`) | 9.98 |
| ICU implementation code | approximately 2.0 |
| Legacy MiniBlink data in the macOS target | 0 |

The full data package is shared by Blink and Node/V8. Removing the legacy
MiniBlink array from the macOS link saved exactly 1,887,840 bytes in the final
signed host without adding an external resource.

The OMP frontend directly uses `Intl.Locale`, so disabling V8 internationalization
is not compatible. Filtering full ICU data to a fixed locale set could save an
estimated 6-8 MiB, but it would make unsupported locales fail and should only be
done with an explicit product locale policy.

Externalizing the 9.98 MiB ICU blob would reduce the executable but leave the
`.app` roughly unchanged.

### Node

Node's linked 9.81 MiB includes approximately 5.10 MiB of generated, embedded
core JavaScript. Largest entries include:

| Builtin | Size |
| --- | ---: |
| Undici/fetch | 650.7 KiB |
| Acorn | 465.6 KiB |
| HTTP/2 core | 107.4 KiB |
| Web Streams readable stream | 94.2 KiB |
| `fs` | 84.1 KiB |
| `util.inspect` | 82.1 KiB |
| `net` | 68.1 KiB |
| CommonJS loader | 67.5 KiB |

Clearly optional builtins are small:

| Optional group | Size |
| --- | ---: |
| Test runner | 206.8 KiB |
| REPL/debugger | 118.4 KiB |
| QUIC JavaScript | 105.8 KiB |
| V8 profiling tools | 134.4 KiB |

A custom builtin allowlist would save only about 0.55 MiB before more aggressive
and risky dependency analysis. Node core modules are loaded dynamically, so
maintaining this fork is unlikely to justify the saving.

Other Node dependencies are already small after dead stripping. Examples:

- Ada URL parser: 432.6 KiB
- Zstd implementation: approximately 350 KiB
- simdutf: 118.3 KiB
- nghttp2: 0.17 MiB
- libuv: 0.09 MiB
- c-ares: 0.08 MiB

Zstd and HTTP/2 are required by the working OMP backend and must remain.

### OpenSSL

OpenSSL contributes 2.24 MiB. It is needed by Node TLS/crypto, the relay
connection, and HTTPS. Removing legacy algorithms could save a few hundred KiB
but has a high compatibility cost. Replacing it with macOS Security/CommonCrypto
would be a substantial Node port rather than a packaging optimization.

## Compiler and linker opportunities

### Prefer size for normal targets

Setting the GN argument below changes normal non-V8 release targets from `-O2`
to `-Os`:

```gn
optimize_for_size = true
```

Representative object-section measurements:

| Object | Current `-O2` | `-Os` | Reduction |
| --- | ---: | ---: | ---: |
| `html_permission_element.o` | 587 KiB | 241 KiB | 59% |
| `longhands.o` | 592 KiB | 470 KiB | 21% |
| `document.o` | 297 KiB | 258 KiB | 13% |
| `RendererBlinkPlatformImpl.o` | 514 KiB | 512 KiB | less than 1% |
| Node `crypto_context.o` | 271 KiB | 270 KiB | less than 1% |

These are object-level samples, not a full optimized build. Based on the live
code distribution, the expected executable saving is 3-5 MiB. That estimate
must be replaced with a full build measurement before accepting the change.

### Reduce V8 from `-O3`

V8 speed-optimized targets explicitly use `-O3`, even when ordinary targets use
`-O2`. Representative recompiles produced:

| Object | `-O3` | `-O2` | `-Os` |
| --- | ---: | ---: | ---: |
| V8 `elements.o` | 885 KiB | 626 KiB | 519 KiB |
| Maglev graph builder | 675 KiB | 663 KiB | 554 KiB |
| Turbofan graph builder | 334 KiB | 303 KiB | 270 KiB |

Changing V8 to `-O2` is the conservative option and is expected to save 1-3 MiB.
Using `-Os` should save more, but has a larger JavaScript throughput risk.
Performance testing should cover OMP startup, daemon request latency, terminal
creation, and a sustained agent session.

### Link-time optimization

The current build uses Apple `ld`, which rejected `-icf`; identical code folding
is unavailable in this configuration. LLVM `lld` plus ThinLTO/ICF may save an
estimated 2-6 MiB, but requires a compatible Chromium LLVM toolchain and a full
rebuild. This is a later optimization because the toolchain and build-time cost
are larger than the first-pass source/configuration changes.

### Function-start metadata

Linking with `-no_function_starts` reduced the final signed binary from
90,623,888 to 90,233,152 bytes, a saving of 390,736 bytes (0.37 MiB). This
weakens crash symbolication and stack tooling, so it is not recommended unless
every few hundred KiB matters.

## Recommended executable plan

### Phase 1: low-risk compiler changes

ICU deduplication and unsupported-surface removal are complete; the measured
signed host is 81.46 MiB. Remaining compiler work:

1. Enable `optimize_for_size=true` for ordinary targets.
2. Change V8 speed targets from `-O3` to `-O2`.
3. Re-run OMP startup, daemon request latency, compressed fetch, terminal, and
   sustained agent-session scenarios before accepting either change.

The previous target range must be remeasured from the new 81.46 MiB baseline.

### Phase 2: bounded feature removal

Completed:

1. Remove Blink Permission Element.
2. Remove inspector/DevTools transports and inspector-only probes.
3. Remove HarfBuzz font-subsetting support while retaining shaping.

Remaining candidates:

1. Disable Maglev after startup and sustained-session performance testing.
2. Remove the service-worker subset only if the desktop origin does not
   register it.

### Phase 3: explicit product tradeoffs

1. Filter ICU to an approved locale set.
2. Adopt LLVM `lld` and ThinLTO/ICF.
3. Maintain a reduced Node builtin set.

No updated target is assigned to this phase until the compiler and bounded
feature candidates are measured from the current baseline.

## Complete application package

The application currently occupies 377,684 KiB (368.83 MiB). Measured
top-level allocation is:

| Path | Allocated size |
| --- | ---: |
| `Contents/MacOS` | 83,416 KiB (81.46 MiB) |
| `Contents/Resources/bin` | 203,580 KiB (198.81 MiB) |
| `Contents/Resources/backend` | 69,220 KiB (67.60 MiB) |
| `Contents/Resources/app-dist` | 21,316 KiB (20.82 MiB) |

### Bundled OMP executable

`Resources/bin/omp` is 208,460,816 bytes (198.80 MiB). It is a signed arm64 Bun
standalone executable. Its `__BUN` section alone is 145,092,913 bytes (138.37
MiB). Stripping the host cannot affect this file.

The upstream desktop project supports resolving `omp` from `PATH` when no
binary is bundled. A non-self-contained package could therefore save about
198.8 MiB, but this changes installation and reliability requirements. A more
ambitious option is running a Node-compatible OMP source distribution on the
already embedded Node runtime; feasibility depends on the OMP CLI's Bun-specific
APIs and is not established by this analysis.

### Backend resources

Largest remaining backend packages by allocated size:

| Runtime package | Allocated size |
| --- | ---: |
| `node-pty` | 25.74 MiB |
| `esbuild` | 9.61 MiB |
| `@esbuild/darwin-arm64` | 9.48 MiB |
| bundled `@omp-desktop/cli` | 9.15 MiB |
| bundled `@omp-desktop/server` | 8.53 MiB |
| `@vscode/ripgrep-darwin-arm64` | 4.33 MiB |

`node-pty/prebuilds` contains 23.22 MiB of platform binaries. The Windows x64
and arm64 variants alone account for 22.89 MiB, while the required Darwin arm64
prebuild is only 0.13 MiB. Removing non-Darwin-arm64 prebuilds plus package
build sources should save roughly 25 MiB with low runtime risk.

`esbuild` and its platform binary are deliberately external runtime packages in
the current backend bundle. They must not be removed without tracing and
exercising the runtime compile paths that use them.

## Distribution size

The baseline signed 86.43 MiB host executable compressed to 31.73 MiB in a ZIP
test.
For download-size goals, a compressed DMG or ZIP provides a much larger return
than additional symbol manipulation. Compression does not change installed
logical size or memory mapping.

## Priority order

For installed `.app` size, the highest-return remaining sequence is:

1. prune non-target `node-pty` files: approximately 25 MiB;
2. decide whether the 198.80 MiB Bun OMP executable must remain bundled;
3. evaluate `optimize_for_size`, V8 `-O2`, and Maglev with performance tests;
4. evaluate the service-worker subset against the packaged frontend.

ICU deduplication, the three Blink/HarfBuzz surface removals, and the obsolete
Sherpa backend removal are complete. Further stripping is both unsafe and
immaterial.
