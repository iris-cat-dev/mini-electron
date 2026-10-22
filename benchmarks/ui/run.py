#!/usr/bin/env python3
# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.
"""Build the public consumer harness on request and run the offline UI matrix."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time
from typing import Any


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")


def parse_csv_names(value: str, allowed: set[str], option: str) -> list[str]:
    values = [item.strip() for item in value.split(",") if item.strip()]
    if not values or any(item not in allowed for item in values) or len(set(values)) != len(values):
        raise ValueError(f"{option} must be a unique comma-separated subset of {','.join(sorted(allowed))}")
    return values


def parse_counts(value: str) -> list[int]:
    try:
        counts = [int(item.strip()) for item in value.split(",") if item.strip()]
    except ValueError as error:
        raise ValueError("--counts must contain comma-separated integers") from error
    if not counts or len(set(counts)) != len(counts) or any(count < 1 or count > 100000 for count in counts):
        raise ValueError("--counts must contain unique values from 1 through 100000")
    return counts


def configured_host_job(ui_out: Path) -> tuple[Path, dict[str, Any]]:
    jobs = ui_out / "jobs"
    if not jobs.is_dir():
        raise RuntimeError(f"configured jobs directory is missing: {jobs}")
    matches: list[tuple[Path, dict[str, Any]]] = []
    for path in jobs.glob("*.json"):
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if document.get("source", "").replace("\\", "/") == "examples/ui/main.cc":
            matches.append((path, document))
    if len(matches) != 1:
        raise RuntimeError(f"expected exactly one configured examples/ui/main.cc compile job, found {len(matches)}")
    return matches[0]


def split_windows_command_line(command_line: str) -> list[str]:
    import ctypes

    argument_count = ctypes.c_int()
    parser = ctypes.windll.shell32.CommandLineToArgvW
    parser.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    parser.restype = ctypes.POINTER(ctypes.c_wchar_p)
    arguments = parser(command_line, ctypes.byref(argument_count))
    if not arguments:
        raise RuntimeError("CommandLineToArgvW could not parse the configured link response")
    local_free = ctypes.windll.kernel32.LocalFree
    local_free.argtypes = [ctypes.c_void_p]
    local_free.restype = ctypes.c_void_p
    try:
        return [arguments[index] for index in range(argument_count.value)]
    finally:
        local_free(ctypes.cast(arguments, ctypes.c_void_p))


def build_host(root: Path, ui_out: Path, build_dir: Path, *,
               source: Path | None = None,
               defines: tuple[str, ...] = (),
               link_options: tuple[str, ...] = ()) -> tuple[Path, dict[str, Any]]:
    source_closure = ui_out / "source-closure.json"
    library = ui_out / "mini-ui.lib"
    link_job_path = ui_out / "jobs/link.json"
    if not source_closure.is_file() or not library.is_file() or not link_job_path.is_file():
        raise RuntimeError("configured source-closure.json, jobs/link.json, and built mini-ui.lib are required")

    compile_job_path, compile_job = configured_host_job(ui_out)
    link_job = json.loads(link_job_path.read_text(encoding="utf-8"))
    compile_command = list(compile_job.get("command", []))
    link_command = list(link_job.get("command", []))
    if not compile_command or not link_command or len(link_command) != 2 or not link_command[1].startswith("@"):
        raise RuntimeError("configured compile/link job command schema is invalid")
    compile_command.extend("/D" + define for define in defines)

    build_dir.mkdir(parents=True, exist_ok=True)
    source = (source or root / "benchmarks/ui/host.cc").resolve()
    object_path = (build_dir / "ui-benchmark.obj").resolve()
    executable = (build_dir / "ui-benchmark.exe").resolve()
    map_path = (build_dir / "ui-benchmark.map").resolve()
    old_source = str((root / compile_job["source"]).resolve())
    old_object = str(Path(compile_job["output"]).resolve())

    replaced_source = False
    replaced_output = False
    for index, argument in enumerate(compile_command):
        if os.path.normcase(os.path.abspath(argument)) == os.path.normcase(old_source):
            compile_command[index] = str(source)
            replaced_source = True
        if argument.startswith("/Fo"):
            if os.path.normcase(os.path.abspath(argument[3:])) != os.path.normcase(old_object):
                raise RuntimeError("configured host /Fo output disagrees with compile job output")
            compile_command[index] = "/Fo" + str(object_path)
            replaced_output = True
    if not replaced_source or not replaced_output:
        raise RuntimeError("could not substitute source and object in configured host compile command")

    configured_rsp = Path(link_command[1][1:])
    response_arguments = split_windows_command_line(configured_rsp.read_text(encoding="utf-8").strip())
    old_executable = str(Path(link_job["output"]).resolve())
    old_map = str(Path(old_executable).with_suffix(".map"))
    replaced_link_output = False
    replaced_link_map = False
    replaced_link_object = False
    for index, argument in enumerate(response_arguments):
        if argument.lower().startswith("/out:"):
            if os.path.normcase(os.path.abspath(argument[5:])) != os.path.normcase(old_executable):
                raise RuntimeError("configured link /out disagrees with link job output")
            response_arguments[index] = "/out:" + str(executable)
            replaced_link_output = True
        elif argument.lower().startswith("/map:"):
            if os.path.normcase(os.path.abspath(argument[5:])) != os.path.normcase(old_map):
                raise RuntimeError("configured link /map disagrees with link job output")
            response_arguments[index] = "/map:" + str(map_path)
            replaced_link_map = True
        elif os.path.normcase(os.path.abspath(argument)) == os.path.normcase(old_object):
            response_arguments[index] = str(object_path)
            replaced_link_object = True
    if not replaced_link_output or not replaced_link_map or not replaced_link_object:
        raise RuntimeError("could not substitute output, map, and host object in configured link response")
    response_arguments.extend(link_options)
    benchmark_rsp = build_dir / "ui-benchmark-link.rsp"
    benchmark_rsp.write_text(subprocess.list2cmdline(response_arguments) + "\n",
                             encoding="utf-8", newline="\n")
    benchmark_link_command = [link_command[0], "@" + str(benchmark_rsp.resolve())]

    sys.path.insert(0, str(root / "build"))
    try:
        from mini_ui.windows import visual_studio_environment
    finally:
        sys.path.pop(0)
    environment = visual_studio_environment()
    subprocess.run(compile_command, cwd=root, env=environment, check=True)
    subprocess.run(benchmark_link_command, cwd=root, env=environment, check=True)

    compiler_version = subprocess.check_output(
        [compile_command[0], "--version"], cwd=root, env=environment, text=True,
        errors="replace").strip()
    linker_version = subprocess.check_output(
        [benchmark_link_command[0], "--version"], cwd=root, env=environment,
        text=True, errors="replace").strip()
    manifest = {
        "schema_version": 1,
        "build_kind": ("public mini_ui consumer harness only"
                       if source == (root / "benchmarks/ui/host.cc").resolve()
                       else "native runtime invariant consumer only"),
        "production_library_rebuilt": False,
        "configured_ui_output": str(ui_out),
        "configured_compile_job": str(compile_job_path),
        "configured_link_job": str(link_job_path),
        "configured_link_response": str(configured_rsp),
        "source_closure_sha256": sha256_file(source_closure),
        "mini_ui_library": str(library),
        "mini_ui_library_sha256": sha256_file(library),
        "host_source_sha256": sha256_file(source),
        "compile_command": compile_command,
        "link_command": benchmark_link_command,
        "compiler_version": compiler_version,
        "linker_version": linker_version,
        "executable": str(executable),
        "executable_sha256": sha256_file(executable),
    }
    write_json(build_dir / "benchmark-build.json", manifest)
    return executable, manifest


def percentile_nearest_rank(values: list[float], percentile: float) -> float:
    if not values:
        raise ValueError("cannot summarize an empty sample")
    ordered = sorted(values)
    return ordered[max(0, math.ceil(percentile * len(ordered)) - 1)]


def add_summary(rows: list[dict[str, Any]], framework: str, count: int,
                metric: str, unit: str, values: list[float], raw_file: str) -> None:
    if not values:
        raise RuntimeError(f"{raw_file}: metric {metric} has no samples")
    if any(not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(float(value)) for value in values):
        raise RuntimeError(f"{raw_file}: metric {metric} contains a missing or non-finite value")
    samples = [float(value) for value in values]
    rows.append({
        "framework": framework,
        "rows": count,
        "metric": metric,
        "unit": unit,
        "sample_count": len(samples),
        "median": statistics.median(samples),
        "p95_nearest_rank": percentile_nearest_rank(samples, 0.95),
        "raw_file": raw_file,
    })


def summarize_run(rows: list[dict[str, Any]], framework: str, count: int,
                  document: dict[str, Any], raw_file: str) -> None:
    result = document["application_result"]
    for operation, samples in result["operation_js_commit_ms"].items():
        add_summary(rows, framework, count, f"operation_js_commit.{operation}", "ms", samples, raw_file)
    frames = document["cpu_offscreen_frames"]
    add_summary(rows, framework, count, "cpu_offscreen_frame.generated_tick_wall", "ms",
                [entry["wall_ms"] for entry in frames["generation_change_ticks"]], raw_file)
    add_summary(rows, framework, count, "view_tick.all_call_wall", "ms",
                frames["all_tick_wall_ms"], raw_file)
    for name, value in document["cold_startup_wall_ms"].items():
        add_summary(rows, framework, count, f"cold_startup.{name}", "ms", [value], raw_file)
    add_summary(rows, framework, count, "workload.wall", "ms", [document["workload_wall_ms"]], raw_file)
    if "view_teardown_wall_ms" in document:
        add_summary(rows, framework, count, "view_teardown.wall", "ms",
                    [document["view_teardown_wall_ms"]], raw_file)
    add_summary(rows, framework, count, "process_cpu.total", "seconds",
                [document["process_cpu"]["total_seconds_delta"]], raw_file)
    add_summary(rows, framework, count, "process_memory.current_working_set", "bytes",
                [document["process_memory"]["current_working_set_bytes"]], raw_file)
    add_summary(rows, framework, count, "process_memory.peak_working_set", "bytes",
                [document["process_memory"]["peak_working_set_bytes"]], raw_file)
    add_summary(rows, framework, count, "process_memory.current_private", "bytes",
                [document["process_memory"]["current_private_bytes"]], raw_file)
    add_summary(rows, framework, count, "cpu_offscreen_frame.throughput_per_cpu_tick_second",
                "frames/cpu_tick_second", [frames["throughput_frames_per_cpu_tick_second"]], raw_file)


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as output:
        fields = ["framework", "rows", "metric", "unit", "sample_count", "median", "p95_nearest_rank", "raw_file"]
        writer = csv.DictWriter(output, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run offline Vue/React mini-ui CPU offscreen benchmarks")
    parser.add_argument("--build", action="store_true",
                        help="compile/link only benchmarks/ui/host.cc using the existing configured UI jobs")
    parser.add_argument("--ui-out", default="out/ui-windows-x64",
                        help="existing configured production UI output (default: %(default)s)")
    parser.add_argument("--exe", help="prebuilt benchmark host; defaults to OUT_DIR/build/ui-benchmark.exe")
    parser.add_argument("--out-dir", default="out/ui-benchmark-results",
                        help="raw captures and aggregates (default: %(default)s)")
    parser.add_argument("--frameworks", default="vue,react")
    parser.add_argument("--counts", default="100,1000,5000,10000")
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--iterations", type=int, default=7)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--device-scale", type=float, default=1.0)
    parser.add_argument("--timeout-ms", type=int, default=120000)
    return parser.parse_args()


def host_environment() -> dict[str, Any]:
    result = {
        "python_version": sys.version,
        "operating_system": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "logical_processors": os.cpu_count(),
        "perf_counter_resolution_seconds": time.get_clock_info("perf_counter").resolution,
    }
    if sys.platform == "win32":
        import ctypes
        import winreg

        with winreg.OpenKey(
                winreg.HKEY_LOCAL_MACHINE,
                r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as processor:
            result["processor_name"] = winreg.QueryValueEx(
                processor, "ProcessorNameString")[0].strip()
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        installed_kib = ctypes.c_ulonglong()
        if not kernel32.GetPhysicallyInstalledSystemMemory(ctypes.byref(installed_kib)):
            raise ctypes.WinError(ctypes.get_last_error())
        result["installed_physical_memory_bytes"] = installed_kib.value * 1024
        frequency = ctypes.c_longlong()
        if not kernel32.QueryPerformanceFrequency(ctypes.byref(frequency)):
            raise ctypes.WinError(ctypes.get_last_error())
        result["query_performance_frequency_hz"] = frequency.value
        power = subprocess.run(
            ["powercfg.exe", "/getactivescheme"], capture_output=True,
            text=True, encoding="oem", errors="strict", timeout=10)
        result["active_power_scheme"] = power.stdout.strip()
        result["active_power_scheme_query_exit"] = power.returncode
    return result


def main() -> int:
    args = parse_arguments()
    script_dir = Path(__file__).resolve().parent
    root = script_dir.parent.parent
    try:
        frameworks = parse_csv_names(args.frameworks, {"vue", "react"}, "--frameworks")
        counts = parse_counts(args.counts)
        if args.warmups < 0 or args.warmups > 1000:
            raise ValueError("--warmups must be from 0 through 1000")
        if args.iterations < 1 or args.iterations > 1000:
            raise ValueError("--iterations must be from 1 through 1000")
        if args.width < 1 or args.width > 32768 or args.height < 1 or args.height > 32768:
            raise ValueError("--width and --height must be from 1 through 32768")
        if not math.isfinite(args.device_scale) or args.device_scale < 0.1 or args.device_scale > 8:
            raise ValueError("--device-scale must be from 0.1 through 8")
        if args.timeout_ms < 100 or args.timeout_ms > 3600000:
            raise ValueError("--timeout-ms must be from 100 through 3600000")
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    output_dir = (root / args.out_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    ui_out = (root / args.ui_out).resolve()
    build_manifest: dict[str, Any] | None = None
    try:
        if args.build:
            executable, build_manifest = build_host(root, ui_out, output_dir / "build")
        else:
            executable = ((root / args.exe).resolve() if args.exe
                          else (output_dir / "build/ui-benchmark.exe"))
            if not executable.is_file():
                raise RuntimeError(f"benchmark host is missing: {executable}; pass --build or --exe")
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    frameworks_manifest = script_dir / "frameworks.json"
    source_closure = ui_out / "source-closure.json"
    library = ui_out / "mini-ui.lib"
    missing_inputs = [path for path in (frameworks_manifest, source_closure, library) if not path.is_file()]
    if missing_inputs:
        print("error: required reproducibility input(s) missing: "
              + ", ".join(str(path) for path in missing_inputs), file=sys.stderr)
        return 1
    try:
        if not args.build:
            manifest_path = executable.parent / "benchmark-build.json"
            build_manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            expected_hashes = {
                "executable_sha256": sha256_file(executable),
                "source_closure_sha256": sha256_file(source_closure),
                "mini_ui_library_sha256": sha256_file(library),
                "host_source_sha256": sha256_file(script_dir / "host.cc"),
            }
            for name, digest in expected_hashes.items():
                if build_manifest.get(name) != digest:
                    raise RuntimeError(
                        f"prebuilt benchmark provenance disagrees at {name}; pass --build")
        framework_data = json.loads(frameworks_manifest.read_text(encoding="utf-8"))
        distributions = {item["package"]: item for item in framework_data["distributions"]}
        for item in distributions.values():
            asset = script_dir / item["file"]
            if asset.stat().st_size != item["bytes"] or sha256_file(asset) != item["sha256"]:
                raise RuntimeError(f"pinned framework asset does not match manifest: {asset}")
            license_path = script_dir / item["license_file"]
            if sha256_file(license_path) != item["license_sha256"]:
                raise RuntimeError(f"pinned framework license does not match manifest: {license_path}")
        expected_versions = {name: distributions[name]["version"] for name in frameworks}
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        print(f"error: invalid pinned framework inputs: {error}", file=sys.stderr)
        return 1
    process_timeout_ms = 3 * args.timeout_ms + 30000
    metadata = {
        "schema_version": 1,
        "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "runner": host_environment(),
        "definitions": {
            "operation_js_commit_ms": "Browser performance.now elapsed from state mutation through the framework's synchronous commit (React flushSync) or queued DOM commit (Vue nextTick), excluding the subsequent shared DOM assertion and paint.",
            "view_teardown.wall": "Native steady-clock wall duration of View destruction after populated capture; separate from JS operation timings, workload wall and the runner's full process wall.",
            "cpu_offscreen_frame.generated_tick_wall": "Native steady-clock wall duration of workload View::Tick calls whose frame generation advanced; excludes the later populated-capture verification and is software offscreen output, not onscreen/vsync FPS.",
            "cpu_offscreen_frame.throughput_per_cpu_tick_second": "Workload frame generation advance divided by summed wall time inside generation-producing workload View::Tick calls; excludes the later populated-capture verification and is software CPU offscreen throughput, not onscreen/vsync FPS.",
            "p95_nearest_rank": "Sort ascending and select element ceil(0.95*N), using one-based rank.",
            "median": "Arithmetic midpoint of the one or two central sorted observations.",
        },
        "matrix": {
            "frameworks": frameworks,
            "row_counts": counts,
            "warmups": args.warmups,
            "measured_iterations": args.iterations,
            "width": args.width,
            "height": args.height,
            "device_scale": args.device_scale,
            "timeout_ms": args.timeout_ms,
            "process_timeout_ms": process_timeout_ms,
        },
        "executable": str(executable),
        "executable_sha256": sha256_file(executable),
        "frameworks_manifest": str(frameworks_manifest),
        "frameworks_manifest_sha256": sha256_file(frameworks_manifest),
        "frameworks": framework_data,
        "assets_sha256": {
            path.relative_to(script_dir).as_posix(): sha256_file(path)
            for path in sorted((script_dir / "assets").rglob("*")) if path.is_file()
        },
        "configured_ui_output": str(ui_out),
        "source_closure_sha256": sha256_file(source_closure),
        "mini_ui_library_sha256": sha256_file(library),
        "build": build_manifest,
    }
    write_json(output_dir / "run-metadata.json", metadata)
    # These are runner-owned cell artifacts, not the reusable consumer build.
    # Remove every previous matrix cell, including cells absent from this run.
    for directory, suffix in (("raw", ".json"), ("captures", ".bmp")):
        for previous in (output_dir / directory).glob(f"*{suffix}"):
            framework, separator, count = previous.stem.rpartition("-")
            if separator and framework in {"vue", "react"} and count.isdecimal():
                previous.unlink()
    for name in ("aggregate.json", "aggregate.csv"):
        (output_dir / name).unlink(missing_ok=True)

    summary_rows: list[dict[str, Any]] = []
    failures: list[dict[str, Any]] = []
    for framework in frameworks:
        for count in counts:
            stem = f"{framework}-{count}"
            raw_path = output_dir / "raw" / f"{stem}.json"
            capture_path = output_dir / "captures" / f"{stem}.bmp"
            raw_path.parent.mkdir(parents=True, exist_ok=True)
            capture_path.parent.mkdir(parents=True, exist_ok=True)
            command = [
                str(executable),
                "--framework", framework,
                "--rows", str(count),
                "--warmups", str(args.warmups),
                "--iterations", str(args.iterations),
                "--resource-root", str(script_dir / "assets"),
                "--output", str(raw_path),
                "--capture", str(capture_path),
                "--width", str(args.width),
                "--height", str(args.height),
                "--device-scale", str(args.device_scale),
                "--timeout-ms", str(args.timeout_ms),
            ]
            process_started = time.perf_counter()
            try:
                completed = subprocess.run(
                    command, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    timeout=process_timeout_ms / 1000.0, text=True, errors="replace")
            except subprocess.TimeoutExpired as error:
                diagnostic = {
                    "schema_version": 1,
                    "status": "error",
                    "diagnostic": {
                        "stage": "runner_timeout",
                        "message": f"host exceeded {process_timeout_ms} ms process timeout",
                    },
                    "command": command,
                }
                write_json(raw_path, diagnostic)
                failures.append({"framework": framework, "rows": count, "exit_code": None,
                                 "raw_file": str(raw_path), "stderr": str(error.stderr or "")})
                continue
            process_wall_ms = (time.perf_counter() - process_started) * 1000

            try:
                document = json.loads(raw_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as error:
                write_json(raw_path, {
                    "schema_version": 1,
                    "status": "error",
                    "diagnostic": {
                        "stage": "host_process_without_valid_result",
                        "message": str(error),
                        "exit_code": completed.returncode,
                        "stdout": completed.stdout,
                        "stderr": completed.stderr,
                    },
                    "command": command,
                })
                failures.append({"framework": framework, "rows": count,
                                 "exit_code": completed.returncode, "raw_file": str(raw_path),
                                 "stderr": completed.stderr, "error": f"invalid/missing raw JSON: {error}"})
                continue
            document["process_execution"] = {
                "command": command,
                "exit_code": completed.returncode,
                "wall_ms": process_wall_ms,
                "timeout_ms": process_timeout_ms,
                "stdout": completed.stdout,
                "stderr": completed.stderr,
            }
            if completed.returncode != 0:
                document["status"] = "error"
                if not document.get("diagnostic"):
                    document["diagnostic"] = {
                        "stage": "native_process_exit",
                        "message": "native process failed after writing its application result",
                        "exit_code": completed.returncode,
                    }
            write_json(raw_path, document)
            if completed.returncode != 0 or document.get("status") != "complete":
                failures.append({"framework": framework, "rows": count,
                                 "exit_code": completed.returncode, "raw_file": str(raw_path),
                                 "stderr": completed.stderr, "diagnostic": document.get("diagnostic")})
                continue
            try:
                result = document["application_result"]
                config = result["config"]
                if result["framework"]["name"] != framework or config["rows"] != count:
                    raise RuntimeError("raw result framework/count does not match requested matrix cell")
                if result["framework"]["version"] != expected_versions[framework]:
                    raise RuntimeError("raw result framework version does not match pinned manifest")
                if config["warmups"] != args.warmups or config["iterations"] != args.iterations:
                    raise RuntimeError("raw result warmup/iteration definition does not match request")
                operation_samples = result["operation_js_commit_ms"]
                expected_operations = {
                    "mount", "update_selected", "keyed_reorder", "prepend",
                    "filter", "restore", "unmount",
                }
                if set(operation_samples) != expected_operations:
                    raise RuntimeError("raw result operation set is invalid")
                if any(not isinstance(samples, list) or len(samples) != args.iterations
                       for samples in operation_samples.values()):
                    raise RuntimeError("raw result operation sample count is invalid")
                expected_assertions = (args.warmups + args.iterations) * 7 + 1
                if result["assertion_failures"] or result["correctness_assertions"] != expected_assertions:
                    raise RuntimeError("raw result correctness assertion count is invalid")
                if result.get("capture_verification_assertions") != 1:
                    raise RuntimeError("populated capture verification assertion is missing")
                if not capture_path.is_file() or capture_path.stat().st_size <= 54:
                    raise RuntimeError("BGRA BMP capture is missing or empty")
                cell_summaries: list[dict[str, Any]] = []
                summarize_run(cell_summaries, framework, count, document, str(raw_path))
            except (KeyError, TypeError, ValueError, OverflowError, RuntimeError) as error:
                document["status"] = "error"
                document["diagnostic"] = {
                    "stage": "runner_validation",
                    "message": str(error),
                }
                write_json(raw_path, document)
                failures.append({"framework": framework, "rows": count,
                                 "exit_code": completed.returncode, "raw_file": str(raw_path),
                                 "stderr": completed.stderr, "error": str(error)})
            else:
                summary_rows.extend(cell_summaries)

    aggregate = {
        "schema_version": 1,
        "status": "complete" if not failures else "error",
        "metadata_file": str(output_dir / "run-metadata.json"),
        "definitions": metadata["definitions"],
        "summaries": summary_rows,
        "failures": failures,
    }
    write_json(output_dir / "aggregate.json", aggregate)
    write_csv(output_dir / "aggregate.csv", summary_rows)
    if failures:
        print(f"benchmark matrix failed in {len(failures)} cell(s); see {output_dir / 'aggregate.json'}", file=sys.stderr)
        return 1
    print(output_dir / "aggregate.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
