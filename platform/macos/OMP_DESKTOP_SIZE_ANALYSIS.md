# OMP Desktop size analysis

This report records the size baseline and optimization options for the Apple
Silicon OMP Desktop package. Measurements were taken from commit `87cf65a9f`
after packaging with:

```sh
./platform/macos/build.py --omp-desktop --omp-source ../omp-desktop
```

The packaged host is stripped with `strip -x` and then ad-hoc signed. Sizes in
MiB use 1 MiB = 1,048,576 bytes.

## Executive summary

`OMP Desktop` is 90,623,888 bytes (86.43 MiB). The remaining bytes are mostly
live machine code and immutable runtime data, not debug symbols:

- Blink/MiniBlink: 38.29 MiB
- V8: 21.71 MiB
- ICU: 13.78 MiB
- Node and its protocol dependencies: 9.81 MiB
- OpenSSL: 2.24 MiB

The linker already removes dead code and the package already strips local
symbols. More aggressive `strip` saves only about 6 KiB and removes N-API
exports required by `node-pty` and `@napi-rs/keyring`, causing those modules to
crash while loading.

A conservative optimization pass should target 78-81 MiB for the main
executable. Reaching approximately 72-76 MiB requires disabling optional V8
and Blink features. Going below 60 MiB would require material compatibility or
performance losses such as removing WebAssembly, full `Intl`, JIT tiers, or a
large portion of Blink.

The complete `.app` is approximately 407.36 MiB. Its largest single file is not
the host executable but the bundled 198.80 MiB `Resources/bin/omp` Bun
executable. Optimizing only the 86.43 MiB host cannot make the overall package
small by itself.

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

The following values sum live symbols and data atoms from the linker map.
Small alignment gaps and link metadata are not assigned to a component.

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

Candidate feature groups, measured by live object names, are:

| Candidate | Live-size lower bound | Constraint |
| --- | ---: | --- |
| Inspector/devtools/probes | 1.20 MiB | Remove only if no developer-tools path is exposed |
| SVG | 1.22 MiB | Required by the OMP UI; keep |
| Permission Element | 0.60 MiB | Experimental API; strong removal candidate |
| Canvas/WebGL/GPU | 0.59 MiB | Used by rich rendering; keep unless proven unused |
| Media/audio/video | 0.55 MiB | Needed for OMP voice/media capability |
| Workers/service workers | 0.46 MiB | Service-worker subset may be removable |
| HarfBuzz subsetting | 0.65 MiB | Evaluate if PDF, printing, and font subsetting are absent |

`html_permission_element.o` alone contributes 547 KiB of live output. The API
is behind Blink's `PermissionElement` runtime feature and is not needed by the
current OMP UI. Removing it requires updating both the handwritten source list
and generated bindings/tag registration.

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

The binary currently carries:

| ICU contribution | MiB |
| --- | ---: |
| Full current ICU data (`icudtl_dat.o`) | 9.98 |
| Legacy MiniBlink embedded data (`content/resources/icudtl.cpp`) | 1.80 |
| ICU implementation code | approximately 2.0 |

The best low-risk data optimization is to initialize Blink from the current
full ICU data and remove the legacy 1.80 MiB blob. This must be verified against
Chinese text, locale direction, date/number formatting, sorting, and input
methods.

The OMP frontend directly uses `Intl.Locale`, so disabling V8 internationalization
is not compatible. Filtering full ICU data to a fixed locale set could save an
estimated 6-8 MiB, but it would make unsupported locales fail and should only be
done with an explicit product locale policy.

Externalizing the 9.98 MiB ICU blob reduces the executable but leaves the `.app`
roughly unchanged.

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

### Phase 1: low-risk build and data changes

1. Enable `optimize_for_size=true` for ordinary targets.
2. Change V8 speed targets from `-O3` to `-O2`.
3. Remove the duplicate 1.80 MiB MiniBlink ICU payload by sharing current ICU
   data.
4. Re-run the packaged OMP UI, daemon, compressed fetch, and terminal scenarios.

Expected target: 78-81 MiB. This range is an estimate until a complete build is
measured.

### Phase 2: bounded feature removal

1. Disable Maglev.
2. Remove Blink Permission Element.
3. Remove inspector/devtools support if it has no supported user entry point.
4. Remove the service-worker subset if the desktop origin does not register it.
5. Evaluate HarfBuzz subsetting after confirming no print/PDF dependency.

Expected target: approximately 72-76 MiB.

### Phase 3: explicit product tradeoffs

1. Filter ICU to an approved locale set.
2. Adopt LLVM `lld` and ThinLTO/ICF.
3. Maintain a reduced Node builtin set.

Expected target: approximately 62-70 MiB. Each item changes compatibility,
build infrastructure, or maintenance cost.

## Complete application package

The application currently occupies approximately 417,136 KiB (407.36 MiB).
Measured top-level allocation is:

| Path | Allocated size |
| --- | ---: |
| `Contents/MacOS` | approximately 86.4 MiB |
| `Contents/Resources/bin` | approximately 198.8 MiB |
| `Contents/Resources/backend` | approximately 101.0 MiB |
| `Contents/Resources/app-dist` | approximately 20.9 MiB |

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

Largest backend packages by logical file size:

| Runtime package | Size |
| --- | ---: |
| `sherpa-onnx-darwin-arm64` | 32.51 MiB |
| `node-pty` | 25.63 MiB |
| `esbuild` | 9.59 MiB |
| `@esbuild/darwin-arm64` | 9.48 MiB |
| bundled `@omp-desktop/server` | 9.26 MiB |
| bundled `@omp-desktop/cli` | 9.19 MiB |
| `@vscode/ripgrep-darwin-arm64` | 4.32 MiB |

`node-pty/prebuilds` contains 23.22 MiB of platform binaries. The Windows x64
and arm64 variants alone account for 22.89 MiB, while the required Darwin arm64
prebuild is only 0.13 MiB. Removing non-Darwin-arm64 prebuilds plus package
build sources should save roughly 25 MiB with low runtime risk.

Sherpa/ONNX should be considered for optional or on-demand installation if
local speech is not a mandatory offline feature. That can save another 32.51
MiB from the base package.

`esbuild` and its platform binary are deliberately external runtime packages in
the current backend bundle. They must not be removed without tracing and
exercising the runtime compile paths that use them.

## Distribution size

The signed 86.43 MiB host executable compressed to 31.73 MiB in a ZIP test.
For download-size goals, a compressed DMG or ZIP provides a much larger return
than additional symbol manipulation. Compression does not change installed
logical size or memory mapping.

## Priority order

For installed `.app` size, the highest-return sequence is:

1. prune non-target `node-pty` files: approximately 25 MiB;
2. decide whether Sherpa/ONNX is base or optional: 32.51 MiB;
3. decide whether the 198.80 MiB Bun OMP executable must remain bundled;
4. apply `-Os`, V8 `-O2`, and ICU deduplication to the 86.43 MiB host;
5. only then consider Maglev/Blink feature removal or a new LTO toolchain.

For the host executable alone, start with build optimization and ICU
deduplication. Further stripping is both unsafe and immaterial.
