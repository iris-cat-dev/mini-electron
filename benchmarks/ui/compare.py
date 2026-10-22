#!/usr/bin/env python3
"""Compare completed mini-ui and genuine Chrome runs without mixing timing scopes."""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import math
from pathlib import Path
import statistics
import sys
from typing import Any

import run as common


OPERATIONS = ("mount", "update_selected", "keyed_reorder", "prepend", "filter", "restore", "unmount")


def read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def timing_summary(values: list[float]) -> dict[str, Any]:
    if not values or any(isinstance(value, bool) or not isinstance(value, (int, float))
                         or not math.isfinite(value) or value < 0 for value in values):
        raise RuntimeError("invalid measured timing series")
    return {"sample_count": len(values), "median_ms": statistics.median(values),
            "p95_nearest_rank_ms": common.percentile_nearest_rank(values, 0.95)}


def ratio(value: float, reference: float) -> float | None:
    return value / reference if reference > 0 else None


def checked_metadata(directory: Path) -> dict[str, Any]:
    aggregate = read_json(directory / "aggregate.json")
    if aggregate["status"] != "complete" or aggregate["failures"]:
        raise RuntimeError(f"cannot compare an incomplete or failed matrix: {directory}")
    return read_json(directory / "run-metadata.json")


def check_control(mini: dict[str, Any], chrome: dict[str, Any]) -> None:
    if chrome.get("engine") != "installed Google Chrome" or chrome.get("profiled_diagnostic"):
        raise RuntimeError("control must be an unprofiled genuine Google Chrome run")
    for key in ("frameworks", "row_counts", "warmups", "measured_iterations", "width", "height", "device_scale"):
        if mini["matrix"][key] != chrome["matrix"][key]:
            raise RuntimeError(f"matrix definition differs at {key}")
    if mini["frameworks"] != chrome["frameworks"] or mini["assets_sha256"] != chrome["assets_sha256"]:
        raise RuntimeError("framework distributions, licenses or actual workload asset hashes differ")
    for key in ("operating_system", "machine", "processor_name", "logical_processors",
                "installed_physical_memory_bytes", "active_power_scheme"):
        if mini["runner"][key] != chrome["runner"][key]:
            raise RuntimeError(f"recorded machine or power scheme differs at {key}")


def checked_application(document: dict[str, Any], framework: str, count: int,
                        matrix: dict[str, Any], chrome: bool) -> dict[str, Any]:
    if document["status"] != "complete":
        raise RuntimeError("a failed raw cell cannot enter comparison")
    code = (document["normal_browser_exit_code"] if chrome
            else document["process_execution"]["exit_code"])
    if code != 0:
        raise RuntimeError("raw cell has no normal process-exit proof")
    if chrome and not document["browser_version"]["product"].startswith("Chrome/"):
        raise RuntimeError("raw control cell does not identify actual Chrome")
    result = document["application_result"]
    if result["framework"]["name"] != framework:
        raise RuntimeError("raw cell framework mismatch")
    expected = {"rows": count, "warmups": matrix["warmups"], "iterations": matrix["measured_iterations"]}
    for key, value in expected.items():
        if result["config"][key] != value:
            raise RuntimeError(f"raw cell configuration mismatch at {key}")
    if result["assertion_failures"] or result["capture_verification_assertions"] != 1:
        raise RuntimeError("raw cell did not complete its checked populated capture")
    if result["correctness_assertions"] != (matrix["warmups"] + matrix["measured_iterations"]) * 7 + 1:
        raise RuntimeError("raw cell skipped requested DOM transitions")
    if set(result["operation_js_commit_ms"]) != set(OPERATIONS):
        raise RuntimeError("raw cell is missing measured operations")
    for operation in OPERATIONS:
        samples = result["operation_js_commit_ms"][operation]
        if len(samples) != matrix["measured_iterations"]:
            raise RuntimeError("raw cell has a different timing sample count")
        timing_summary(samples)
    return result


def compare_control(mini_dir: Path, chrome_dir: Path, mini_metadata: dict[str, Any],
                    label: str) -> dict[str, Any]:
    chrome_metadata = checked_metadata(chrome_dir)
    check_control(mini_metadata, chrome_metadata)
    if bool(chrome_metadata["disable_gpu_requested"]) != (label == "chrome_software"):
        raise RuntimeError("GPU control label does not match actual launch request")
    matrix = mini_metadata["matrix"]
    cases = []
    for framework in matrix["frameworks"]:
        for count in matrix["row_counts"]:
            relative = Path("raw") / f"{framework}-{count}.json"
            mini_document = read_json(mini_dir / relative)
            chrome_document = read_json(chrome_dir / relative)
            mini_result = checked_application(mini_document, framework, count, matrix, False)
            chrome_result = checked_application(chrome_document, framework, count, matrix, True)
            if mini_result["framework"] != chrome_result["framework"]:
                raise RuntimeError("actual page-reported framework versions differ")
            operations = []
            for operation in OPERATIONS:
                mini = timing_summary(mini_result["operation_js_commit_ms"][operation])
                chrome = timing_summary(chrome_result["operation_js_commit_ms"][operation])
                operations.append({"operation": operation, "mini_ui": mini, label: chrome,
                                   "mini_ui_over_chrome_median": ratio(mini["median_ms"], chrome["median_ms"])})
            iteration_count = matrix["measured_iterations"]
            mini_totals = [sum(mini_result["operation_js_commit_ms"][operation][index]
                               for operation in OPERATIONS) for index in range(iteration_count)]
            chrome_totals = [sum(chrome_result["operation_js_commit_ms"][operation][index]
                                 for operation in OPERATIONS) for index in range(iteration_count)]
            mini_total = timing_summary(mini_totals)
            chrome_total = timing_summary(chrome_totals)
            memory = chrome_document["process_memory_after_capture"]
            chrome_private = memory["current_private_bytes_sum"] if memory["complete_snapshot"] else None
            mini_private = mini_document["process_memory"]["current_private_bytes"]
            cases.append({"framework": framework, "rows": count,
                          "mini_ui_raw_file": str(mini_dir / relative),
                          "chrome_raw_file": str(chrome_dir / relative),
                          "operations": operations,
                          "iteration_js_commit_sum": {"definition": "Per measured iteration, sum the seven aligned operation samples, then compute median/p95 over iterations; excludes shared validation and paint.",
                              "mini_ui": mini_total, label: chrome_total,
                              "mini_ui_over_chrome_median": ratio(mini_total["median_ms"], chrome_total["median_ms"])},
                          "private_memory_after_populated_capture": {
                              "mini_ui_single_process_bytes": mini_private,
                              "chrome_whole_owned_job_bytes": chrome_private,
                              "mini_ui_over_chrome": ratio(mini_private, chrome_private) if chrome_private is not None else None,
                              "chrome_snapshot_complete": memory["complete_snapshot"]},
                          "cpu_startup_through_capture": {
                              "mini_ui_single_process_seconds": mini_document["process_cpu"]["total_seconds_delta"],
                              "chrome_owned_job_seconds": chrome_document["job_after_capture"]["total_cpu_seconds"],
                              "scope_note": "Mini-ui includes its C++ controller; Chrome job includes all browser descendants but not its Python controller. Neither includes teardown."},
                          "separately_scoped_wall_ms": {
                              "mini_ui_workload": mini_document["workload_wall_ms"],
                              "chrome_workload_with_vsync_waits": chrome_document["workload_wall_ms"],
                              "mini_ui_view_teardown": mini_document["view_teardown_wall_ms"],
                              "chrome_whole_browser_shutdown": chrome_document["browser_shutdown_wall_ms"]},
                          "chrome_workload_performance_duration_deltas_seconds": {
                              key: chrome_document["metrics_after_workload"][key] - chrome_document["metrics_before_workload"][key]
                              for key in ("ScriptDuration", "LayoutDuration", "RecalcStyleDuration", "TaskDuration")
                              if key in chrome_document["metrics_after_workload"] and key in chrome_document["metrics_before_workload"]},
                          "chrome_observed_timer_resolution_ms": chrome_document["page_environment"]["minimum_observed_positive_performance_now_delta_ms"],
                          "chrome_gpu_features": chrome_document["gpu_info"]["gpu"]["featureStatus"]})
    return {"label": label, "chrome_metadata_file": str(chrome_dir / "run-metadata.json"),
            "chrome_executable_sha256": chrome_metadata["chrome_executable_sha256"], "cases": cases}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mini-out", type=Path, default=Path("out/ui-benchmark-results"))
    parser.add_argument("--chrome-out", type=Path, default=Path("out/ui-chrome-results"))
    parser.add_argument("--chrome-software-out", type=Path)
    parser.add_argument("--out-dir", type=Path, default=Path("out/ui-comparison"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    mini_dir = (root / args.mini_out).resolve()
    output = (root / args.out_dir).resolve()
    try:
        mini_metadata = checked_metadata(mini_dir)
        controls = [compare_control(mini_dir, (root / args.chrome_out).resolve(), mini_metadata, "chrome_default")]
        if args.chrome_software_out:
            controls.append(compare_control(mini_dir, (root / args.chrome_software_out).resolve(), mini_metadata, "chrome_software"))
        if len({control["chrome_executable_sha256"] for control in controls}) != 1:
            raise RuntimeError("default and software controls used different Chrome binaries")
        comparison = {"schema_version": 1, "status": "complete",
                      "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                      "mini_ui_metadata_file": str(mini_dir / "run-metadata.json"),
                      "mini_ui_library_sha256": mini_metadata["mini_ui_library_sha256"],
                      "matrix": mini_metadata["matrix"], "controls": controls,
                      "limitations": [
                          "Same machine, viewport, pinned assets and operations do not imply the same Blink revision, JS engine, GC policy or rendering architecture.",
                          "The primary comparison is the exact same JS/DOM commit interval; it includes framework asynchronous commit scheduling, not just interpreter instruction time.",
                          "A ratio above one means mini-ui's median is slower/larger. A zero Chrome median has a null ratio, not infinity; Chrome timer quantization limits tiny intervals.",
                          "Seven samples in one process per cell are not seven independent cold processes. Nearest-rank p95 at N=7 equals the maximum; no confidence interval or production SLA is claimed.",
                          "Whole-workload wall contains Chrome vsync waits and different host scheduling. CPU offscreen throughput is not display FPS.",
                          "Private-memory snapshots include actual allocator/GC retention at that point; a large snapshot alone does not prove a leak.",
                          "Startup and teardown scopes differ and are retained separately, not presented as engine-only speedup ratios."]}
        common.write_json(output / "comparison.json", comparison)
        output.mkdir(parents=True, exist_ok=True)
        with (output / "operations.csv").open("w", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream, lineterminator="\n")
            writer.writerow(["control", "framework", "rows", "operation", "samples",
                             "mini_ui_median_ms", "chrome_median_ms", "mini_ui_p95_ms", "chrome_p95_ms",
                             "mini_ui_over_chrome_median"])
            for control in controls:
                for case in control["cases"]:
                    for operation in case["operations"]:
                        mini, chrome = operation["mini_ui"], operation[control["label"]]
                        writer.writerow([control["label"], case["framework"], case["rows"], operation["operation"],
                                         mini["sample_count"], mini["median_ms"], chrome["median_ms"],
                                         mini["p95_nearest_rank_ms"], chrome["p95_nearest_rank_ms"],
                                         operation["mini_ui_over_chrome_median"]])
        print(output / "comparison.json")
        return 0
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
