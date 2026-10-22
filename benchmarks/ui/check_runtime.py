"""Build and exercise the real native runtime invariants; never rebuild the library."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

import run as benchmark


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ui-out", default="out/ui-windows-x64")
    parser.add_argument("--out-dir", default="out/ui-runtime-checks")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = (root / args.out_dir).resolve()
    executable, build = benchmark.build_host(
        root, (root / args.ui_out).resolve(), output / "build",
        source=root / "benchmarks/ui/runtime_checks.cc",
        defines=("INSIDE_BLINK=1",),
        # Distinct real typed views must exercise the secondary arena even when
        # the production linker would fold identical view constructors.
        link_options=("/OPT:NOICF",))
    command = [str(executable)]
    start = time.perf_counter()
    try:
        process = subprocess.run(command, cwd=root, capture_output=True,
                                 timeout=180, check=False)
        exit_code = process.returncode
        timed_out = False
        stdout = process.stdout.decode("utf-8", errors="replace")
        stderr = process.stderr.decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired as failure:
        exit_code = None
        timed_out = True
        stdout = (failure.stdout or b"").decode("utf-8", errors="replace")
        stderr = (failure.stderr or b"").decode("utf-8", errors="replace")
    result = {"schema_version": 1, "command": command,
              "library_sha256": build["mini_ui_library_sha256"],
              "source_sha256": build["host_source_sha256"],
              "exit_code": exit_code, "timed_out": timed_out,
              "wall_seconds": time.perf_counter() - start,
              "stdout": stdout, "stderr": stderr}
    benchmark.write_json(output / "runtime-checks.json", result)
    sys.stdout.write(stdout)
    sys.stderr.write(stderr)
    print(output / "runtime-checks.json")
    return 1 if timed_out else int(exit_code != 0)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as failure:
        print(f"error: {failure}", file=sys.stderr)
        raise SystemExit(1)
