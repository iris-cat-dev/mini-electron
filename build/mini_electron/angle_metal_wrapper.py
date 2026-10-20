#!/usr/bin/env python3
# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

"""Resolve Xcode's separately installed Metal toolchain through xcrun."""

from pathlib import Path
import subprocess
import sys


def main(arguments: list[str]) -> int:
    tool = Path(arguments[0]).name
    executable = subprocess.check_output(
        ["xcrun", "--sdk", "macosx", "--find", tool], text=True
    ).strip()
    return subprocess.run([executable, *arguments[1:]], check=False).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
