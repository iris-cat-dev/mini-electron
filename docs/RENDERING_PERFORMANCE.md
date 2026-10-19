# Rendering performance diagnosis and optimization

## Scope and baseline

This records the local macOS rendering comparison and runtime optimizations.
The P0 sections document the completed native display-ID → DisplayLink/VSync
cutover. The later Metal section records subsequent native GPU composition.
DOM/layout algorithms, compiler optimization and the 10 ms AppKit/libuv wakeup
policy remain unchanged.

The 2026-10-19 baseline used an Apple M4 Pro, a 120 Hz built-in display,
1000×700 CSS-pixel viewports, DPR 2, headed windows, and the same local page.
The installed Chrome was 155.0.8059.84; mini-electron embeds Chromium 132.
Three alternating, warmed trials per engine completed successfully.

| Workload / metric | mini-electron | Chrome |
| --- | ---: | ---: |
| Create 2,000 text rows and force layout, work P50 | 39.5 ms | 12.2 ms |
| Update 1,000 grid items and force layout, work P50 | 13.1 ms | 5.4 ms |
| Empty rAF callback frequency | about 60 Hz | about 120 Hz |
| Canvas, transforms and programmatic scrolling, rAF frequency | about 60 Hz | about 120 Hz |

These are medians of per-trial metrics, not pooled sample quantiles. rAF
frequency is not measured display presentation FPS. Synchronous work timings
include JavaScript and explicitly forced style/layout, not subsequent raster,
GPU work or native presentation. The original local report is under
`out/rendering-benchmark/2026-10-19T00-31-40-301Z/`; generated artifacts are not
checked in. Reproduce with `npm run benchmark:render`.

Supplementary CPU phase sampling had two valid paired trials. A third pair
was incomplete because Chrome failed to finish/exit and is excluded in full.
For 2,000 text rows, detached construction was approximately 1.25/0.75 ms,
attachment 0.30/0.15 ms, and synchronous style/layout 37.70/11.35 ms
(mini-electron/Chrome). Removing text reduced total time to 5.15/2.20 ms.
This points to text/style/layout for profiling; it does not identify a specific
font, shaping function or GC policy as the cause. Current generated compilation
commands use -O2 for Blink/bindings and -O3 for V8; no JIT-disabled policy was
found. Compiler flags and source inspection are not a complete binary provenance
attestation.

## Original software-baseline diagnosis and priorities

| Priority | Work | Evidence / boundary |
| --- | --- | --- |
| P0 | Native display ID → DisplayLink/VSync, including screen changes | Custom Viz root settings leave the display ID invalid; the macOS source falls back to a 1/60-second timer. This matches the baseline, but the fix needs runtime verification. |
| P1 | Profile text, style/layout and GC, then optimize demonstrated hotspots | The additional synchronous work is predominantly in the forced style/layout phase. GPU work does not directly explain that timing. |
| P2 | Measure and optimize publication, IPC, pixel copies and native wakeup | Software framebuffer transport has full-frame copy costs; current ownership/retention protections must remain intact. |
| P3 | Evaluate targeted compiler optimization, engine upgrade or a real GPU path separately | None is a one-flag explanation/fix for the measured gap. GPU plumbing was disconnected in the original baseline; the later Metal section documents the subsequent native composition path, not the full Chromium GPU service. |

At that software-only baseline, Blink/Viz remained CPU-only even if Core
Animation used the GPU for the operating system's final window composition.
That is distinct from the subsequent native Metal canvas implementation below.

## P0 design and invariants

The browser owns native screen discovery. The sandboxed renderer must receive
only the window's display identity through the trusted browser→renderer path.
Use the existing Viz macOS display-ID update and DisplayLink source rather than
introducing a separate periodic 120 Hz animation timer.

- Apply the actual window screen at initial attachment and after screen changes,
  including screen changes with unchanged viewport size and backing scale.
- Cover BrowserWindow, BrowserView and the standalone native GUI frontend.
- Marshal updates onto the existing renderer/Blink and Viz owning sequences;
  pending initialization and teardown must not use raw dangling callbacks.
- Preserve the existing on-demand BeginFrame subscriptions. No permanent
  high-rate timer, forced redraw loop or new idle polling is permitted.
- Preserve timer fallback when no usable display/DisplayLink is available.
- Do not relax the renderer sandbox simply to make DisplayLink work. If direct
  access is unavailable, use browser-owned display timing through a bounded,
  demand-driven existing external BeginFrame interface instead.
- Preserve completed-frame retention, resize behavior, alpha/color/DPR and
  graceful renderer/window shutdown. Do not change the pixel transport here.

Relevant baseline source locations:

- `runtime/engine/viz/viz_host.cc`: custom root compositor construction.
- `components/viz/common/display/renderer_settings.h`: invalid display default.
- `components/viz/service/frame_sinks/external_begin_frame_source_mac.cc`:
  DisplayLink creation, display updates and timer fallback.
- `components/viz/common/frame_sinks/begin_frame_args.h`: 1/60-second fallback.
- `platform/macos/electron/mac_electron_bindings.mm`: native view attachment,
  screen/backing notifications and BrowserWindow/BrowserView viewport updates.
- `runtime/electron/common/renderer_server.cc`: browser→renderer command handling.

## Refresh rate and resource budget

Following a 120 Hz display improves potential smoothness, not the efficiency of
each frame. Active animation can perform more JavaScript, rasterization, copies
and composition per second than at 60 Hz. Per-frame time falls from 16.67 ms to
8.33 ms; fixing the clock does not make expensive layout faster.

P0 must not turn static pages into permanent 120 Hz workloads. Existing
on-demand subscriptions must stop when work ends. A page that continuously
requests rAF is still an active workload even if its pixels appear unchanged.
Report active CPU costs separately from genuine static CPU/wakeup measurements;
neither rAF throughput nor idle counters alone establish electrical efficiency.

## Acceptance and verification

1. On a 120 Hz display, a lightweight active page receives approximately 120 Hz
   rAF callbacks; on a 60 Hz display it receives approximately 60 Hz. Record the
   actual source/display identity and interval where accessible. Do not infer
   successful native presentation solely from rAF.
2. Moving an owned window between different displays updates timing even when
   its viewport size/DPR do not change. Returning to the original screen works;
   invalid/unavailable display timing falls back and later recovers.
3. A settled static page does not continually publish/copy frames or retain an
   active high-rate frame source. Compare native paint generations and process
   CPU/wakeups with the unchanged baseline under the same conditions. This is a
   proxy for idle cost, not a claim of measured electrical power consumption.
4. Existing native frame-retention and static/animated resize behavior remain
   correct, and application/renderer processes exit normally.
5. Record unavailable physical hardware or OS measurement capabilities explicitly.
   A simulated display-ID update or fixed 60 Hz timer does not count as a real
   60↔120 Hz screen-change measurement.

## Implementation and measured results

P0 is implemented. Native screen identity is sent independently of viewport
dimensions/DPR for BrowserWindow and BrowserView; the standalone GUI uses the
same engine setter. Blink caches the latest ID and Viz applies it at creation
or on subsequent screen changes. The existing CVDisplayLink source and its
on-demand subscriptions remain in use; no permanent high-rate timer, GPU
enablement, sandbox expansion or AppKit pump-frequency change was introduced.

The checked-in shared Mojo bindings lacked the macOS display-ID method.
The real method is restored as macOS-only ordinal 12; existing Windows
ordinals 0–11 remain unchanged. This runtime's lightweight Mojo Remote exposes
the receiver directly, rather than marshaling through a generated proxy.
Display-ID updates therefore explicitly run on the existing Viz service
sequence, with synchronous completion protecting the receiver's lifetime;
root receiver task runners are pinned to the scheduler's owning runner.
An initial incorrect host-thread dispatch was caught by invalid-ID recovery,
fixed without suppressing the scheduler CHECK, and retained as a repeated
active-recovery regression.

### P0 failing-before reproduction

The unchanged runtime was retained at `out/vsync-verification/before/mini-electron`
(SHA-256 `4a0dfd0dc0f6078e0d15a5bce729f518941299efb9c62c7b6e235b63d4ebffd0`).
The physical setup now includes two attached screens with the same DPR 2:
the built-in 1728×1117 logical display at 120 Hz (ID 1) and an HP Z27k G3
1920×1080 logical display at 60 Hz (ID 3). The test moves only its own
1000×700 windows; it does not modify display modes, layout or primary display.

`MINI_VSYNC_BASELINE=1` records the expected failures without invoking new IPC:

| Scenario | Display target | Unchanged runtime rAF |
| --- | ---: | ---: |
| BrowserWindow on built-in screen | 120 Hz | 59.94 Hz |
| Same-size/DPR move to HP | 60 Hz | 60.01 Hz |
| Move back to built-in | 120 Hz | 59.99 Hz |
| BrowserView attached on built-in | 120 Hz | 59.97 Hz |
| BrowserView reparented to HP | 60 Hz | 60.00 Hz |
| BrowserView reparented back | 120 Hz | 60.00 Hz |

CoreVideo reference creation succeeded and reported nominal 120/60 Hz on
the respective physical displays. Thus the old 60 Hz behavior was not caused
by the operating system lacking a usable 120 Hz reference on this machine.
All three baseline static phases had unchanged framebuffer generations.
The calibrated browser counters used about 0.76–0.92% of one CPU core and
228–233 interrupt wakeups per 2.5 seconds; renderer CPU time and wakeups were
near zero. Raw samples, actual screen-change notifications, counters and native
PNGs are retained in `out/vsync-verification/before-calibrated/result.json`.

CPU accounting uses `mach_timebase_info` (125/3 on this machine), not a direct
division of `proc_pid_rusage` ticks by one billion. Independent `getrusage`
seconds are recorded and checked before sampling. This avoids the known
[Apple Silicon CPU-counter unit pitfall](https://github.com/osquery/osquery/issues/7459).
Earlier `before/` and `after/` reports predate this calibration; their CPU
percentages must not be used for resource comparisons.

### P0 passing-after verification

`out/vsync-verification/after-final/result.json` reports `passed`, exit code 0,
with the same physical displays, 1000×700 owned windows, and unchanged DPR 2.
The observed callback frequencies are not native presentation/scanout FPS.

| Scenario | Display target | Fixed runtime rAF |
| --- | ---: | ---: |
| BrowserWindow on built-in screen | 120 Hz | 119.99 Hz |
| Explicit built-in placement | 120 Hz | 120.00 Hz |
| Same-size/DPR move to HP | 60 Hz | 60.00 Hz |
| Move back to built-in | 120 Hz | 121.31 Hz |
| Invalid display-ID timer fallback | 60 Hz default | 59.98 Hz |
| Valid display-ID recovery | 120 Hz | 121.36 Hz |
| BrowserView attached on built-in | 120 Hz | 119.98 Hz |
| BrowserView reparented to HP | 60 Hz | 60.00 Hz |
| BrowserView reparented back | 120 Hz | 120.01 Hz |

Three additional invalid→valid cycles replace the source with frames already
in flight: six transitions passed, about 59.98–59.99 Hz for fallback and
120.00–120.03 Hz for native recovery. Software framebuffer generations advanced
during active samples. All three subsequent static phases had no new generations.
Required browser/renderer CPU counters were observed and each static renderer
remained below the regression's busy-CPU/active-rate-wakeup limits.

The matched single-window static phase (before the extra recovery stress)
measured browser CPU at 0.86% before versus 0.80% after, with 231 interrupt
wakeups in each 2.5-second interval. Renderer CPU was approximately 0.001%
before versus 0.003% after, with 1 versus 2 wakeups. These small observations
show no sustained high-refresh idle loop; they are not statistical proof of
electrical-power improvement. Later multi-window phases had ordinary CPU
variation and a different preceding workload, so they are not treated as a
matched before/after efficiency comparison.

Active high-refresh work costs more: in the controlled moving-square sample,
browser-plus-renderer CPU increased from 21.50% at about 60 Hz before the fix
to 36.77% at about 120 Hz after it. On the 60 Hz external screen the totals were
22.40% before and 21.74% after. Here 100% means one fully occupied CPU core,
not the machine's total CPU capacity. The same native reference observer runs
in both active samples; it is stopped before static measurements.

Reproduce against a built runtime:

```sh
env -u SDKROOT DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  MINI_VSYNC_ARTIFACTS=out/vsync-verification/after-final \
  out/mac-arm64/mini-electron --app=test/macos-vsync-smoke.js
```

Physical unplug/replug, sleep/wake, changing a screen's OS refresh-mode setting,
actual scanout timestamps and electrical wattage were not measured. Invalid-ID
injection is reported separately from the real physical window movements.
Windows interfaces remain conditionally excluded/preserved, but no Windows
build or hardware test was run for this macOS-only change.

### Final build and adjacent regression checks

Both `mini_electron` and `mini_electron_browser` built with Xcode's macOS SDK.
The final binaries passed these existing native behavior checks:

- `test/macos-frame-presentation-smoke.js`: complete-frame retention under
  shared-memory writer contention, followed by later-frame presentation.
- `test/macos-resize-geometry-smoke.js`: real responsive layout, native geometry
  and later changing pixels across extreme aspect-ratio changes.
- `test/macos-resize-presentation-smoke.js`: committed native layers remain
  complete during continuous resize and present later genuine frames.
- `test/macos-renderer-loss-lifetime-smoke.js`: renderer loss does not prematurely
  destroy WebContents or BrowserWindow.
- `test/macos-exit-lifecycle-smoke.js`: application-menu and preload-backed
  windows exit cleanly.

The standalone GUI was also launched with `examples/browser/index.html`; its
1920×1280 screenshot at DPR 2 was inspected for actual rendered content:
`out/vsync-verification/after-final/gui.png`. The owned GUI exited cleanly.

A short, single-trial run of the original comparison against installed
Chrome 155 completed, using five synchronous-operation samples and
one-second animation samples:

```sh
npm run benchmark:render -- --trials 1 --duration-ms 1000 --samples 5 \
  --output out/vsync-verification/rendering-comparison
```

Both engines now reported about 120 Hz for the empty-rAF, Canvas2D and
transform workloads; scroll was about 119 versus 120 Hz. This is a smoke
comparison, not a replacement for repeated performance trials. The CPU-bound
layout gap remains: creating 2,000 text rows took 39.9 versus 12.5 ms p50,
and updating 1,000 grid items took 11.4 versus 4.1 ms p50. P0 removes the
incorrect 60 Hz scheduling ceiling; it does not solve layout cost or enable
GPU page compositing.

Full report: `out/vsync-verification/rendering-comparison/report.md`;
raw per-trial samples and provenance are retained next to it.

## macOS Metal GPU compositing

### Execution and transport

- macOS defaults to Skia Ganesh/Metal when initial device, context and remote-layer
  setup succeed. `MINI_ELECTRON_DISABLE_GPU=1` selects the existing CPU backend.
  Initial GPU setup failure also falls back to CPU.
- DOM/style/layout and cc tile rasterization remain CPU work. Viz's existing
  quad renderer draws into real GPU-backed `SkSurface`s, including intermediate
  render passes, transforms, blending, masks and filters. This is not uploading
  a fully CPU-composited page as a substitute for GPU composition.
- A persistent BGRA GPU root preserves damage. GPU blits copy the completed
  backing to a `CAMetalLayer` drawable. Geometry and presentation are committed
  together; native views host the remote `CAContext` through `CALayerHost`.
  Ordinary GPU frame events do not force a full AppKit bitmap redraw.
- The 64-byte shared frame header now carries output format, remote context,
  scale, error flags and a native completion generation. Wire version is 2:
  rebuild browser and renderer together. Normal GPU publication updates metadata
  only. Explicit captures can leave cached RGBA bytes in the mapping; their
  presence does not mean later normal frames perform readback.
- `capturePage()` explicitly reads GPU pixels back. Crop coordinates retain the
  existing physical-pixel convention. The native image wraps an owned,
  premultiplied bitmap; it must not premultiply transport RGBA a second time.
- Native GPU guests share remote layers. CPU-fallback guests use owned image
  layers in a GPU host. A CPU host with a GPU guest uses coalesced explicit guest
  readback. These compatibility paths are not claimed as fully GPU-only.
- GPU completion plus a Core Animation commit is not proof of physical scanout.
  The native completion generation and rAF frequency must not be called actual
  displayed FPS. Runtime GPU failure rejects preparation/capture; automatic
  device-loss recovery is not implemented.

The class/resource-protocol names `SoftwareRenderer` and `gpu_compositing=false`
remain because cc still supplies CPU-rasterized tile resources. The output
canvas, render-pass surfaces and presentation can nevertheless execute on Metal.
The full Chromium GPU service, GPU tile rasterization, accelerated Canvas2D,
WebGL/WebGPU and hardware video decoding are not enabled by this change.

### Sandbox boundary

The renderer opens its Metal device and command queue before Seatbelt lockdown.
The profile adds only the named compiler XPC lookup:

```scheme
(allow mach-lookup (xpc-service-name "com.apple.MTLCompilerService"))
```

A sandboxed cold, unique shader probe compiled a real pipeline, completed GPU
work and checked its output pixels. No wildcard driver/Mach permissions or
arbitrary file-write access were added. Metal disk-cache extension-denial
diagnostics remain visible; successful in-memory compilation does not justify
broadening the profile. This design gives the renderer GPU capability directly;
it is not the security boundary of a separately isolated Chromium GPU process.

### Exercised checks before native-capture authorization

On the local M4 Pro, both native executable targets compiled and linked.
Actual GPU output metadata reported a real remote context, output format 1,
DPR 2 and 1200×800 pixels for a 600×400 CSS-pixel page. Before the first explicit
capture, `dataSize` was zero.

Independent PNG decoding checked 20 colored samples per backend across full
captures, physical-pixel crops and subsequent changes: translated content,
vacated background, opacity, brightness, backdrop brightness and a corner
marker. GPU and CPU sampled colors agreed. The native publication generation
advanced after content changes and stayed fixed across nine static observations.
Evidence: `out/gpu-verification/gpu-readback-pixel-evidence.json` and the
`gpu-readback-{gpu,cpu}-*.png` captures.

An additional translucent capture reproduced double alpha multiplication:
half-transparent red decoded to premultiplied `[64,0,0,128]` rather than
`[128,0,0,128]`. The corrected owned-bitmap path was rebuilt and exercised;
its actual decoded result is `[128,0,0,128]`. The GPU regression retains this
consumer-visible case. PNG decoding, not an assumed native N32 byte order,
is used for permanent capture color assertions.

GPU renderer-loss lifetime and preload/application exit lifecycle smokes passed.
The standalone GUI launched with Metal, reported `GUI_READY`, and its 1920×1280
explicit GPU snapshot was inspected for rendered page content:
`out/gpu-verification/gui-metal.png`. That image is producer readback, not a
WindowServer screenshot.

### Restarted-launcher native verification

After the launcher restart, ScreenCaptureKit successfully captured this
process's actual WindowServer-composed windows. Authorization is no longer the
blocker; producer readback is still not substituted for native pixels.

Native testing exposed two presentation defects that producer PNGs could not
detect. AppKit's private drawing sublayer covered the remote Metal surface;
the surface now sits above that backing, with BrowserViews above the surface.
Retained remote backings also needed a lower-left-to-top-left position
adjustment while resized content was pending.

The real-window GPU root regression passed transformed content, opacity,
brightness/backdrop filters, subsequent changes, explicit full/cropped captures,
premultiplied transparency, BrowserView attach/reparent/reattach, and paused
producer shrink/enlarge without stretching. Embedded `<webview>` and matched
GPU/CPU resource verification continue below; no electrical-power or physical
scanout-rate claim follows from these pixel checks.

The embedded GPU/GPU native check also passed CSS-positioned attachment,
premultiplied guest blending, live updates, independent guest updates while the
host renderer was stopped, and moving the guest without stale old pixels.
It exposed missing Blink embedder-name authorization for `<webview>` and
incorrect DPR conversion of guest bounds; both fixes were rebuilt and exercised.

The next check reproduced a resize stall: a guest requested at 200×140 remained
published at 180×120. The CPU publication callback released the engine resize
gate, but Metal completion did not. Both backends now share the publication
acknowledgment without generating CPU pixels for GPU frames. The rebuilt
embedded regression passed subsequent source resize as well as moving and
detaching the guest.

CPU-host compatibility exposed additional real defects: merely waking the
thread loop did not redraw a static host after guest readback/removal, and CPU
publication copied guest alpha over the page instead of compositing it.
Host invalidation now requests the existing forced compositor commit; CPU
guest paint and mapping readiness also notify the host. Guest pixels always
use premultiplied source-over blending. The regression reproduced
`[120,40,16,128]` instead of opaque `[136,72,64,255]` before this correction.
All four GPU/GPU, GPU/CPU, CPU/GPU and CPU/CPU embedded combinations now pass
actual owned-window pixels, current full/cropped page captures, update, resize,
move, detach and frame-resource release.

GPU and forced-CPU extreme-aspect geometry, continuous paused resize, writer
contention/frame retention and later animated-pixel checks have now passed.
GPU renderer-loss lifetime and preload/application-menu exit checks also passed.
The owned-window helper
accounts for ScreenCaptureKit excluding AppKit's border; it samples only actual
visible pixels, outside native titlebar controls, without resampling or filling
missing pixels. Geometry still checks marker position/size/aspect and native
view/layer agreement with the DOM.

The standalone GUI's real 1920×1344 WindowServer screenshot was also inspected:
`out/gpu-verification/gui-native.png`. It shows the native titlebar and correctly
positioned red, green and yellow page markers over the expected blue background.
Unlike the earlier GUI producer PNGs, this is proof of the final owned window.

Resume from the repository root with the native GPU visual regression:

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
unset SDKROOT
out/mac-arm64/mini-electron --app=test/macos-gpu-compositing-smoke.js
out/mac-arm64/mini-electron --app=test/macos-gpu-guests-smoke.js
out/mac-arm64/mini-electron --app=test/macos-frame-presentation-smoke.js
out/mac-arm64/mini-electron --app=test/macos-resize-geometry-smoke.js
out/mac-arm64/mini-electron --app=test/macos-resize-presentation-smoke.js
MINI_VSYNC_ARTIFACTS=out/gpu-verification/vsync-gpu \
  out/mac-arm64/mini-electron --app=test/macos-vsync-smoke.js
MINI_ELECTRON_DISABLE_GPU=1 MINI_VSYNC_ARTIFACTS=out/gpu-verification/vsync-cpu \
  out/mac-arm64/mini-electron --app=test/macos-vsync-smoke.js
```

Run GPU and CPU resource measurements sequentially on the same rebuilt binary,
without concurrent builds or other workload activity. Preserve the existing
60/120 Hz physical-display, same-DPR switching, BrowserView reparenting and
calibrated static CPU/wakeup criteria. Stop diagnostic DisplayLinks before idle
measurement. Then rerun the local Chrome comparison with its ordinary GPU
configuration. Old software baseline reports are not evidence of new GPU speed.

### Matched GPU/CPU resource measurements

The final rebuilt binary was run sequentially with default Metal and with
`MINI_ELECTRON_DISABLE_GPU=1`, after the concurrent deployment/browser workload
had finished. The scene was a moving square in a 1000×700 DIP window at DPR 2.
CPU below sums the actual browser and renderer process counters; 100% means one
core, not the whole machine.

| Physical screen phase | CPU backend CPU | Metal backend CPU | CPU / Metal rAF Hz |
| --- | ---: | ---: | ---: |
| Built-in 120 Hz | 38.8% | 13.7% | 119.99 / 119.99 |
| HP 60 Hz, same DPR | 21.3% | 9.4% | 59.99 / 60.00 |
| Return to built-in 120 Hz | 41.0% | 14.4% | 119.99 / 120.00 |

The two 120 Hz observations reduce combined process CPU by approximately 65%
in this scene. The initial 120 Hz browser component falls from 27.8% to 2.2%;
renderer CPU is similar (11.0% CPU backend versus 11.5% Metal). This is evidence
for removing browser-side framebuffer transport/presentation cost, not for
accelerating DOM/layout or all raster work.

Both runs passed real 120→60→120 equal-DPR window movement, invalid-display
recovery, six active source replacements, BrowserView cross-screen reparenting,
owned-window screenshots and all three static observations. All owned frame
generations stayed unchanged while static. In the final static observation,
the browser used about 0.74%/0.76% of a core (Metal/CPU); each Metal renderer was
below 0.005% and each CPU renderer measured zero CPU in that short interval.
Renderer interrupt wakeups stayed below one per second individually. The
existing browser wakeup policy remains; there is no claim of zero whole-app
idle activity.

Evidence: `out/gpu-verification/vsync-{gpu,cpu}/result.json` and the PNGs in
those directories. These are short, single-scene observations, not universal
speedups, electrical-watt measurements or physical scanout FPS. Actual physical
display unplug/reconnect was not run; invalid-ID recovery is not a substitute.

### Final local Chrome comparison

The installed browser reported `Chrome/155.0.8059.84` through CDP.

The final comparison uses three complete alternating trials per engine,
1000×700 actual content viewports, DPR 2, 500 ms warmups, 2 s animation
measurements and 20 operation samples. Both native windows are now placed at
global (40,40); actual Electron/CDP window bounds are retained in every trial.
Matching DPR alone did not control the earlier 60/120 Hz screen-selection
variation. Every final mini-electron trial logged an active M4 Pro Metal
compositor.

| Workload / metric | mini-electron Metal | Installed Chrome | mini / Chrome |
| --- | ---: | ---: | ---: |
| Create 2,000 text rows + forced layout, work P50 | 38.5 ms | 11.9 ms | 3.23× |
| Update 1,000 grid items + forced layout, work P50 | 13.2 ms | 5.1 ms | 2.59× |
| Canvas2D submission, work P50 | 0.4 ms | 0.3 ms | 1.33× |
| Empty rAF, median callback frequency | about 120 Hz | about 120 Hz | about 1× |
| 150 transformed cards, median callback frequency | about 120 Hz | about 120 Hz | about 1× |
| Programmatic text scrolling, median callback frequency | about 120 Hz | about 120 Hz | about 1× |

This is not uniformly perfect cadence: mini-electron's per-trial empty-rAF
frequency ranged about 115–120 Hz and Canvas ranged about 113–120 Hz. Its
Canvas maximum per-trial percentage of intervals over 25 ms was 1.33%, versus
zero for Chrome in this run. Keep these ranges rather than reporting only
the approximately equal median. rAF remains a callback metric, not measured
physical presentation.

The earlier unpinned run and incomplete fixed-window attempts remain diagnostic
artifacts. Incomplete Chrome attempts (suspended rAF / page not ready) produce
no comparison ratios and are excluded in full; the final table uses only the
six complete trials in `out/gpu-verification/rendering-comparison-final/`.
The driver activates Chrome after native placement/resize and records page
state on a readiness failure; it does not disable Chrome GPU features, extend
the stalled-frame timeout, or replay missing results.

Metal substantially reduces the measured transport/presentation CPU cost, but
does not close the synchronous text/style/layout gap. The next optimization
target remains a trace of text/style/layout on the 2,000-row and grid workloads,
followed by changes to demonstrated hotspots. Do not infer GPU tile raster,
accelerated Canvas2D, WebGL/WebGPU or hardware video support from this result.

```sh
node benchmarks/rendering/run.js --trials 3 \
  --output out/gpu-verification/rendering-comparison-final
```

The final native check index is
`out/gpu-verification/native-verification.json`. Temporary source probes,
injected libraries and diagnostic executables were removed after verification;
JSON/PNG/log evidence and permanent regression fixtures remain. Verification
uses the current `out/mac-arm64` executables; no new release was published and
the OMP installation copy was not replaced by this verification task.

