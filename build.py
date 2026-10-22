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
        description="Build the local-only QuickJS/Blink UI library and native Windows host."
    )
    parser.add_argument("--out", help="output directory, relative to the repository root")
    parser.add_argument(
        "--jobs", "-j", type=int, default=min(os.cpu_count() or 1, 16),
        help="maximum parallel build jobs (default: up to 16)",
    )
    parser.add_argument(
        "--ui", action="store_true", required=True,
        help="select the local-only QuickJS/Blink UI build",
    )
    parser.add_argument(
        "--ui-target", choices=("host", "library"), default="host",
        help="independent UI target (default: host, which also links mini-ui.lib)",
    )
    parser.add_argument(
        "--ui-configure-only", action="store_true",
        help="generate and inventory the independent UI graph without compiling",
    )
    parser.add_argument("--run", action="store_true", help="build and run the native Windows host")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.ui and args.run and (args.ui_target != "host" or args.ui_configure_only):
        parser.error("--ui --run requires a built --ui-target host")

    sys.path.insert(0, str(ROOT / "build"))
    machine = platform.machine().lower()
    if sys.platform != "win32" or machine not in ("amd64", "x86_64"):
        parser.error("the native UI target requires Windows x64")
    from mini_ui import windows as backend
    args.out = args.out or "out/ui-windows-x64"

    try:
        return backend.build(ROOT, args)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"build failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
