# Offline Vue and React CPU-offscreen benchmark

This directory is a permanent public-API consumer benchmark for `mini_ui::View`. It does not modify or enter the production build graph. It uses only local resources at runtime: there is no network access, npm, Node.js, bundler, or framework substitute.

## Pinned framework assets

`frameworks.json` is the machine-readable provenance manifest. The checked-in browser production distributions are:

| Package | Version | Local file | SHA-256 |
| --- | --- | --- | --- |
| Vue | 3.5.13 | `assets/vendor/vue.global.prod.js` | `c459ba7cc8db65c982589fa5d64c7ff478877e8e5b0fd75683207cec6a4e89e8` |
| React | 18.3.1 | `assets/vendor/react.production.min.js` | `d949f1c3687aedadcedac85261865f29b17cd273997e7f6b2bfc53b2f9d4c4dd` |
| ReactDOM | 18.3.1 | `assets/vendor/react-dom.production.min.js` | `35f4f974f4b2bcd44da73963347f8952e341f83909e4498227d4e26b98f66f0d` |

The unmodified upstream MIT license texts are next to the distributions as `LICENSE.vue.txt`, `LICENSE.react.txt`, and `LICENSE.react-dom.txt`. Exact download URLs, byte counts, and distribution and license checksums are in `frameworks.json`.

## Workload

Both pages use `assets/common.js`, `assets/app.css`, and the same deterministic rows and transition assertions. The framework-specific applications use actual production Vue reactivity/computed state and actual production React hooks/memoization. Their nested toolbar, summary, table, row, and conditional-detail components exercise event handlers and keyed row identity.

Each warmup and measured iteration performs this fixed sequence:

1. mount the requested deterministic row count;
2. select every tenth row;
3. reverse/reorder the keyed rows (including swapping the edge keys);
4. prepend 100 deterministic keyed rows;
5. filter to IDs divisible by ten;
6. restore all rows;
7. unmount the application.

After every transition, shared code checks real DOM row counts, total and selected counts, keyed order hash, conditional detail count, representative labels/scores, and the visible controls. An assertion failure is returned through `uiBench.result()` and makes the host emit diagnostic JSON and exit nonzero.

After the last timed unmount has been genuinely executed and asserted, the host closes the workload wall/frame sample and calls a separate protocol phase that mounts one full-count dashboard with every tenth row selected. The page asserts that capture state and leaves it populated. This final verification is outside the operation samples, workload wall timer, and workload CPU-offscreen frame wall/throughput samples, so the saved BGRA image shows the complex table, summary, controls, and conditional details rather than a blank unmounted root. Its separate Tick call and generation-advance counts are retained in the raw result.

Defaults are 100, 1000, 5000, and 10000 rows, two warmups, and seven measured iterations. Each iteration creates and destroys the framework application/root; neither renderer retains a mounted root between iterations.

## Run after the official library build finishes

The configured output must already contain `source-closure.json`, `jobs/*.json`, `jobs/link.json`, `link.rsp`, and the completed `mini-ui.lib`. Do not run this while the official Ninja build is active.

From the repository root, build only the public consumer host and run the default matrix:

```powershell
py -3 benchmarks/ui/run.py --build --ui-out out/ui-windows-x64 --out-dir out/ui-benchmark-results --timeout-ms 900000
```

For a single-cell smoke before the complete matrix:

```powershell
py -3 benchmarks/ui/run.py --build --ui-out out/ui-windows-x64 --out-dir out/ui-benchmark-smoke --frameworks vue --counts 100 --warmups 0 --iterations 1
```

`--build` does not invoke Ninja and does not rebuild the production library. It locates the one configured `examples/ui/main.cc` compile job, substitutes only `benchmarks/ui/host.cc` and its object output, parses the configured link response, substitutes only the consumer object/map/executable, and reuses its compiler, flags, library, compiler runtime, system library paths, and linker. It obtains the Visual Studio/SDK environment from `mini_ui.windows.visual_studio_environment`, with the repository's `build` directory on the import path. The exact commands, tool versions, input hashes, and executable hash are recorded in `build/benchmark-build.json` under the selected result directory.

To use an already-built host without recompiling it:

```powershell
py -3 benchmarks/ui/run.py --exe out/ui-benchmark-results/build/ui-benchmark.exe --ui-out out/ui-windows-x64 --out-dir out/ui-benchmark-repeat
```

The prebuilt executable must have an adjacent `benchmark-build.json` whose executable, library, source-closure, and consumer-source hashes match the current inputs. Otherwise, use `--build`; an old executable is never attributed to a newly rebuilt static library.

Each invocation clears prior runner-owned matrix JSON/BMP files and aggregates from its output directory, including cells outside the new matrix. The reusable `build/` directory is preserved.

Useful overrides are `--frameworks vue,react`, `--counts`, `--warmups`, `--iterations`, `--width`, `--height`, `--device-scale`, and `--timeout-ms`.

Before launching a cell, the runner verifies every pinned distribution's byte count and SHA-256 and the license checksums, then checks the reported framework version against the manifest. The process limit covers all three native phase budgets (startup, workload, populated capture) plus 30 seconds.

The native host can also run one cell directly:

```powershell
out/ui-benchmark-results/build/ui-benchmark.exe --framework vue --rows 1000 --warmups 2 --iterations 7 --resource-root benchmarks/ui/assets --output out/vue-1000.json --capture out/vue-1000.bmp --width 1280 --height 720 --device-scale 1 --timeout-ms 120000
```

## Real Google Chrome control

`chrome.py` drives the **installed Google Chrome executable**, not mini-ui, a
Chromium mock, or a replacement renderer. It uses exactly the same local HTML,
pinned framework distributions, `common.js`, operations, DOM assertions, row
counts, warmups and measured iterations.

The optional transport is developer-only Python tooling, not a mini-ui runtime
dependency:

```powershell
py -3 -m venv .build-tools/ui-benchmark-venv
.\.build-tools\ui-benchmark-venv\Scripts\python.exe -m pip install -r benchmarks/ui/requirements-chrome.txt
.\.build-tools\ui-benchmark-venv\Scripts\python.exe benchmarks/ui/chrome.py --out-dir out/ui-chrome-results
```

Run it **after** the mini-ui matrix, not concurrently. `--chrome` selects a
different installed Chrome path. The default is
`C:\Program Files\Google\Chrome\Application\chrome.exe`.

Each cell launches a visible browser with a fresh private profile. The browser
is created suspended, assigned to an owned Windows kill-on-close job, and then
resumed, so descendants are accounted for and cleaned up without touching the
user's existing Chrome sessions. No `--no-sandbox` or `--disable-web-security`
flag is used. Page viewport and device scale are checked against the requested
values. Input hashes, actual `Browser.getVersion`, V8 version, GPU feature
status, exact launch arguments, page timer resolution, and normal exit status
are retained.

The default reference keeps Chrome's normal GPU path. For a separate software
reference, use a fresh directory:

```powershell
.\.build-tools\ui-benchmark-venv\Scripts\python.exe benchmarks/ui/chrome.py --disable-gpu --out-dir out/ui-chrome-software-results
```

Chrome CPU values use real Windows job accounting over the browser and its
descendants, including exited processes; the Python controller is outside that
job. Private bytes are summed over the active owned process tree after the
populated capture. Summed working sets double-count shared pages; peak job
private commit is **not** peak physical RSS.

Compare the shared **JS/DOM commit intervals** directly. Whole-workload wall
time also contains Chrome's vsync-driven animation-frame waits, whereas mini-ui
generates software offscreen frames on host `Tick` calls. Browser launch,
renderer navigation, native View creation, and View/browser teardown are
different scopes. Do not present a ratio of those fields as an engine-only
speedup or compare mini-ui offscreen throughput to Chrome's display FPS.

`--profile` records a Chrome CPU profile in a separately labeled diagnostic
run. Never pool its instrumented timings into the unprofiled comparison.
Previously recorded result directories are not overwritten.

Aggregate the checked control matrices without rerunning the browsers:

```powershell
py -3 benchmarks/ui/compare.py --mini-out out/ui-benchmark-results --chrome-out out/ui-chrome-results --chrome-software-out out/ui-chrome-software-results --out-dir out/ui-comparison
```

This rejects failed/incomplete cells, mismatched assets or matrix/machine
definitions, different Chrome binaries between modes, missing normal exits, and profiled runs. `comparison.json` retains
all seven operation medians/p95 values plus the median/p95 of each iteration's
aligned seven-operation sum. `operations.csv` is the flat operation table.
Ratios use those identical commit intervals, not differently paced frame loops.

## Outputs and measurement definitions

The runner retains:

- `raw/<framework>-<rows>.json`: the complete native result or diagnostic result for each matrix cell;
- `captures/<framework>-<rows>.bmp`: the actual final populated-dashboard premultiplied-BGRA frame copied from `View::frame()`;
- `run-metadata.json`: framework manifest, matrix, hashes, viewport, warmups/iterations, and configured production inputs;
- `aggregate.json` and `aggregate.csv`: sample counts, medians, and nearest-rank p95 values;
- `build/benchmark-build.json`: exact consumer compile/link provenance when `--build` is used.

Measurements are deliberately named by what they measure:

- **Cold startup wall time** uses `std::chrono::steady_clock` around `View::Create`, `LoadFile`, and polling until the real framework page installs the ready protocol.
- **Operation JS commit time** uses browser `performance.now()` from state mutation through React's `flushSync` commit or Vue's `nextTick` DOM commit. The subsequent shared DOM assertion and paint are excluded.
- **CPU offscreen frame Tick wall time** is the native steady-clock duration of each `View::Tick` call whose `FrameView::generation` advances.
- **CPU offscreen frame throughput** is generation advance divided by the summed wall time inside generation-producing `View::Tick` calls. It is software/offscreen CPU throughput, **not onscreen FPS and not vsync FPS**.
- **Process CPU** is the real kernel/user delta reported by Windows `GetProcessTimes` from immediately before `View::Create` through the final populated capture. It excludes View and process teardown.
- **View teardown wall time** is measured separately with the native steady clock around `view.reset()`, after the populated dashboard has been captured. It includes the actual DOM/realm release and collection performed by View destruction. The host writes a successful result only after this release finishes.
- **Memory** is current working set, lifetime peak working set, and current private bytes from Windows `GetProcessMemoryInfo`.
- **Frame metadata** is the actual width, height, stride, generation, pixel format, and capture path from `View::frame()`.

The host uses a real monotonic clock for every `Tick` argument. It polls only through `View::Evaluate`, embeds the returned application result JSON directly in the native wrapper, and does not use a C++ JSON dependency. A page error, failed DOM assertion, mini-ui error, missing process/frame metric, or timeout produces diagnostic JSON and a nonzero exit. The runner additionally enforces a process timeout, preserves its own timeout diagnostic, and returns nonzero if any matrix cell fails.

The runner's full process wall time also includes process-global teardown. A
completed application result is not sufficient: the native process must exit
normally with code zero. Keep a timeout or shutdown failure as a failed cell
rather than aggregating its application timings as a successful measurement.

Each matrix cell starts a fresh native process. Warmups and measured iterations
share that cell's runtime, but no mounted framework root survives an iteration.
The runner does not clear the Windows filesystem cache. “Cold startup” therefore
means cold process/runtime initialization, not a cold disk read. Run the matrix
without concurrent Ninja builds or other stress workloads; record the machine
and power scheme from `run-metadata.json` with the results.

The aggregate median is the middle observation (or arithmetic midpoint of the two middle observations). The p95 is nearest-rank: sort ascending and select one-based rank `ceil(0.95 * N)`.

With the default seven measured iterations, nearest-rank p95 is simply the
largest of those seven samples, not a statistically stable tail-latency estimate.
Chrome's recorded timer resolution can quantize tiny commit intervals to zero;
a zero denominator has no speedup ratio and must not be read as zero work.

## Native optimization regression consumer

After rebuilding the official library, run:

```powershell
py -3 benchmarks/ui/check_runtime.py --out-dir out/ui-runtime-checks
```

This builds and launches a real native consumer against that library. It checks
native property data/reentrancy, inheritance and cloning, access/interceptor
ordering, descriptor/query/getter precedence, intercepted and ordinary
delete/define failures, callback and microtask GC exclusion, stable persistent
handles across arena reclamation, native/JavaScript cross-heap lifetime and
native ephemerons, balanced RegExp constructor ownership, and real asynchronous
Blob reads during joint GC. Blob checks verify returned bytes and Promise
completion, not merely the absence of a crash. The consumer uses
`/OPT:NOICF` so identical typed-view constructors are not folded and both Cell
and secondary ApiView arenas are exercised. Framework performance hosts retain
the production linker settings.

`runtime-checks.json` records the library/source hashes, normal process exit,
stdout/stderr and observed arena/GC counters. It is correctness evidence, not
a performance sample.

The production `ScriptRuntime::DrainJobs` path is exercised with nested native
microtasks, exception cleanup and attempted arena trimming while a job is active.
Property checks include native readonly SameValue redefinition (`NaN` and signed
zero), setter rejection through Reflect/strict assignment, and definition-query
exception propagation. These are behavioral regressions, not performance samples.

Identity-key collection checks cover thousands of objects and symbols across
hash-table growth, equality, live iteration, deletion/reinsertion, weak-container
deletion, proxy identity without traps, and clearing. They protect semantics
when changing the VM's internal object-key hash, without timing thresholds.

Ordinary field-read checks cover shape changes and compaction, colliding slot
hints, getter/Proxy behavior, inherited method receivers, exceptions and
temporary-value lifetime. They validate the VM read path without timing gates.

For two completed native matrices with identical workloads:

```powershell
py -3 benchmarks/ui/before_after.py --before PATH_TO_BASELINE --after PATH_TO_NEW_MATRIX --out-dir PATH_TO_COMPARISON
```

Compare commit latency together with whole-workload wall time, process CPU,
Tick durations and memory. Automatic host-idle GC can move work outside the
JavaScript commit interval; a lower commit time alone does not prove a faster
application. Positive reduction percentages mean improvement; negative values
are regressions. Keep diagnostic/forced-GC runs separate from timed matrices.

## Recorded comparison

[2026-10-22: mini-ui and genuine Google Chrome](results/2026-10-22/README.md)
retains all 24 completed cells, final library/browser hashes, raw samples,
actual captures, native validation, separate heap/CPU diagnostics, historical
failures, and an evidence-backed optimization assessment. Pre-font native
measurements are explicitly isolated from the final comparison.


[2026-10-22 P1/P2: measured optimization and refreshed Chrome controls](results/2026-10-22-p1-p2/README.md)
records the final eight-cell native matrix, sixteen refreshed real-Chrome cells,
failed-before/passed-after native regressions, retained-root attribution, phase
diagnostics, rejected compiler/GC variants, and explicit regressions/limitations.

[2026-10-22 Map hash: sample-driven object-key hashing improvement](results/2026-10-22-map-hash/README.md)
records a new paired native matrix, real raster comparisons, isolated native
sampling, rejected micro-optimizations and the remaining React reorder cost.
Vue gains and React regressions are reported separately; sampling is not pooled
with benchmark timings.

[2026-10-22 Property slots: measured React reorder optimization](results/2026-10-22-property-slots/README.md)
records validated ordinary-field slot hints, explicit Windows inlining, complete
native matrices and reverse-order independent repeats. Small-case and Vue wall
time variability are retained rather than hidden in the React improvement.
