# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Ensure malformed Torque reaches its diagnostic handler, not a raw trap.

Usage: python3 v8/test/torque/crash-diagnostic-regression.py out/mac-arm64/torque
Torque's CLI deliberately aborts after printing diagnostics; this test preserves
that contract and checks diagnostic delivery, not successful compilation.
"""

import pathlib
import subprocess
import sys
import tempfile


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve(strict=True)
    cases = {
        "syntax": ("@@@", "Torque Error"),
        "integer": (
            "const kOverflow: int32 = 184467440737095516160;",
            "integer literal value out of range",
        ),
        "double": (
            "const kOverflow: float64 = 1.0e9999;",
            "double literal out-of-range",
        ),
    }
    with tempfile.TemporaryDirectory(prefix="torque-diagnostic-") as directory:
        for name, (source, expected) in cases.items():
            path = pathlib.Path(directory, name + ".tq")
            path.write_text(source, encoding="utf-8")
            result = subprocess.run(
                [str(compiler), str(path)], capture_output=True, text=True,
                timeout=30, check=False,
            )
            if result.returncode == 0 or expected not in result.stderr:
                raise AssertionError(
                    f"{name}: exit={result.returncode}, "
                    f"expected diagnostic={expected!r}, stderr={result.stderr!r}"
                )
    print("Torque diagnostic regression passed")


if __name__ == "__main__":
    main()
