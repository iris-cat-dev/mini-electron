#!/usr/bin/env python3
# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

import argparse
import os
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parent


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build mini-electron from this checkout on Windows x64 or macOS arm64."
    )
    parser.add_argument("--out", help="output directory, relative to the repository root")
    parser.add_argument(
        "--jobs", "-j", type=int, default=min(os.cpu_count() or 1, 16),
        help="maximum parallel build jobs (default: up to 16)",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--run", action="store_true", help="build and run the platform smoke check")
    mode.add_argument("--electron", action="store_true", help="build and run the shared Electron demo")
    mode.add_argument("--gui", action="store_true", help="build and run the native browser demo (macOS)")
    mode.add_argument("--omp-desktop", action="store_true", help="package OMP Desktop")
    parser.add_argument(
        "--electron-main", default="examples/electron/main.js",
        help="Electron-compatible main script (default: shared example)",
    )
    parser.add_argument(
        "--interactive", action="store_true",
        help="keep the Electron demo or packaged OMP Desktop open until its last window closes",
    )
    parser.add_argument("--screenshot", help="verification snapshot path (macOS)")
    parser.add_argument(
        "--frontend", default="examples/browser/index.html",
        help="browser demo HTML path or HTTP(S) URL (macOS)",
    )
    parser.add_argument("--omp-source", default="../omp-desktop", help="OMP Desktop source checkout")
    parser.add_argument("--app-output", help="output directory for packaged OMP Desktop")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")

    sys.path.insert(0, str(ROOT / "build"))
    machine = platform.machine().lower()
    if sys.platform == "win32" and machine in ("amd64", "x86_64"):
        from mini_electron import windows as backend
        args.out = args.out or "out/windows-x64"
    elif sys.platform == "darwin" and machine == "arm64":
        from mini_electron import macos as backend
        args.out = args.out or "out/mac-arm64"
    else:
        parser.error("supported hosts are Windows x64 and Apple Silicon macOS")

    try:
        return backend.build(ROOT, args)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"build failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
