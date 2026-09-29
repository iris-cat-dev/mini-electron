# OMP Desktop size analysis

This report records the original size baseline and optimization options for the
Apple Silicon OMP Desktop package. Baseline measurements were taken from commit
`87cf65a9f` after packaging with:

```sh
./platform/macos/build.py --omp-desktop --omp-source ../omp-desktop
```

The packaged host is stripped with `strip -x` and then ad-hoc signed. Sizes in
MiB use 1 MiB = 1,048,576 bytes.

## Executive summary

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
`content/resources/icudtl.cpp`. During Blink startup,
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
