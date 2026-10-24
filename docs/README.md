# Documentation

[Project overview](../README.md)

Commands in these guides run from the repository root unless stated otherwise.
Platform support and verification records are scoped to the versions and workloads
named in each document; source-build fixes are not automatically present in an
older downloaded runtime.

## Getting started

| Document | Contents |
| --- | --- |
| [Installation](INSTALLATION.md) | npm setup, runtime updates and pinning, offline installation, mirrors |
| [Development](DEVELOPMENT.md) | Shared Electron example, Notification, safeStorage, preload scope, single-process mode |
| [Windows](WINDOWS.md) | Windows x64 toolchain, build commands, native API coverage, OMP Desktop packaging |
| [macOS](MACOS.md) | Apple Silicon toolchain, native hosts, API coverage, runtime and app packaging |
| [Graphics](GRAPHICS.md) | WebGL/ANGLE, WebGPU/Dawn, prerequisites, smoke commands and limitations |

## Repository and releases

| Document | Contents |
| --- | --- |
| [Repository layout](REPOSITORY_LAYOUT.md) | Directory responsibilities, source/build boundaries, naming and migration conventions |
| [Release process](RELEASING.md) | Runtime-only releases, immutable assets, platform channels, npm installer publication |
| [Release notes](RELEASES.md) | Platform-specific release history and acceptance limits |

## Verification and analysis

| Document | Contents |
| --- | --- |
| [Verification and boundaries](VERIFICATION.md) | Historical native runtime, packaging and cleanup verification |
| [Rendering performance](RENDERING_PERFORMANCE.md) | GPU composition, VSync, idle scheduling, memory budgets and measurements |
| [OMP Desktop size analysis](OMP_DESKTOP_SIZE_ANALYSIS.md) | Package size, component attribution and optimization evidence |

## Documentation maintenance

- Keep the root README focused on the project, supported targets, quick start and links.
- Put build/API details in the relevant platform or feature guide; record releases
  and measured evidence separately rather than extending the project overview.
- Preserve version, platform and verification limits when updating historical records.
- Keep repository-relative shell commands rooted at the checkout, and resolve Markdown
  links relative to the containing document.
