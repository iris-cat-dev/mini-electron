# mini-electron

A lightweight Electron-compatible runtime for Windows and macOS, built on
MiniBlink's Chromium 132, Blink, and V8 engine sources.

> **Supported targets:** Windows x64 and Apple Silicon macOS. API coverage differs
> between native hosts; neither is a drop-in replacement for full Electron.
> Intel macOS, Windows ARM64 and Linux are not supported targets.

## Install and run

Requires Node.js 18.17 or newer:

```sh
npm install --save-dev @iriscat/mini-electron@0.1.12
npx mini-electron path/to/main.js
```

Application main/preload scripts use `require('electron')`. Outside the runtime,
`require('@iriscat/mini-electron')` returns the installed executable path.
Use the explicit `mini-electron` command to avoid conflicts with Electron's CLI.

The npm installer and native runtime are versioned independently:

| Component | Version | Target |
| --- | --- | --- |
| npm installer | `0.1.12` | Both supported platforms |
| Windows runtime | `0.1.28` | Windows x64 |
| macOS runtime | `0.1.30` | macOS arm64 |
| Electron API compatibility version | `41.2.0` | Partial implementation, not full API parity |

Existing valid installations are reused; application startup does not check for
updates. Update the local runtime explicitly with `npx mini-electron --update-runtime`.
For reproducible builds, pin the runtime separately from the npm lockfile; see
[installation and runtime updates](https://github.com/iris-cat-dev/mini-electron/blob/main/docs/INSTALLATION.md).

## Build from source

From the repository root, after installing the platform toolchain:

```sh
python build.py --electron --interactive
```

`build.py` is the shared Windows/macOS build entry point. Without `--interactive`,
`--electron` runs the shared example as a short native-window smoke check.
See the [Windows guide](https://github.com/iris-cat-dev/mini-electron/blob/main/docs/WINDOWS.md)
or [macOS guide](https://github.com/iris-cat-dev/mini-electron/blob/main/docs/MACOS.md)
for prerequisites, build outputs and packaging.

## Repository structure

| Path | Responsibility |
| --- | --- |
| `src/runtime/` | Engine hosting, Electron/Node integration, network and storage |
| `src/platform/` | Windows, macOS and POSIX native implementations |
| `src/npm/` | npm installer, CLI, type declarations and package helpers |
| `build/`, `build.py` | Build backends, source manifests and shared build entry point |
| `scripts/` | Release assembly and build-tool regressions |
| `examples/`, `test/`, `benchmarks/` | Examples, regression/smoke checks and performance workloads |
| `docs/` | Detailed guides, release notes and analysis |
| `base/`, `third_party/`, `v8/`, etc. | Shared upstream engine sources; original include paths are preserved |
| `out/`, `.build-tools/` | Ignored build outputs and downloaded build dependencies |

## Documentation

- [Documentation index](https://github.com/iris-cat-dev/mini-electron/blob/main/docs/README.md)
- [Repository layout and conventions](https://github.com/iris-cat-dev/mini-electron/blob/main/docs/REPOSITORY_LAYOUT.md)
- [Development and runtime behavior](https://github.com/iris-cat-dev/mini-electron/blob/main/docs/DEVELOPMENT.md)
- [WebGL and WebGPU](https://github.com/iris-cat-dev/mini-electron/blob/main/docs/GRAPHICS.md)
- [Release notes](https://github.com/iris-cat-dev/mini-electron/blob/main/docs/RELEASES.md) · [Release process](https://github.com/iris-cat-dev/mini-electron/blob/main/docs/RELEASING.md)
- [Verification and known boundaries](https://github.com/iris-cat-dev/mini-electron/blob/main/docs/VERIFICATION.md)
- [Rendering performance](https://github.com/iris-cat-dev/mini-electron/blob/main/docs/RENDERING_PERFORMANCE.md) · [Package size analysis](https://github.com/iris-cat-dev/mini-electron/blob/main/docs/OMP_DESKTOP_SIZE_ANALYSIS.md)

## License

Apache-2.0; see [LICENSE](LICENSE). Vendored components retain their upstream
licenses, and runtime distributions include third-party notices.
