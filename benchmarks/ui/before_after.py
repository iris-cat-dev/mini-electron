"""Compare complete, unchanged-workload native matrices; preserve regressions."""
from __future__ import annotations

import argparse
import csv
import datetime as dt
from pathlib import Path
import sys

import compare as comparison
import run as common


def changed(before: float, after: float) -> dict:
    return {"before": before, "after": after,
            "before_over_after": comparison.ratio(before, after),
            "reduction_percent": (100 * (before - after) / before
                                  if before > 0 else None)}


def compare_runs(before_dir: Path, after_dir: Path) -> dict:
    before = comparison.checked_metadata(before_dir)
    after = comparison.checked_metadata(after_dir)
    if before.get("profiled_diagnostic") or after.get("profiled_diagnostic"):
        raise RuntimeError("profiled diagnostics cannot enter native comparison")
    for field in ("frameworks", "assets_sha256", "definitions"):
        if before[field] != after[field]:
            raise RuntimeError(f"native workload differs at {field}")
    for field in ("frameworks", "row_counts", "warmups", "measured_iterations",
                  "width", "height", "device_scale"):
        if before["matrix"][field] != after["matrix"][field]:
            raise RuntimeError(f"native matrix differs at {field}")
    for field in ("operating_system", "machine", "processor_name", "logical_processors",
                  "installed_physical_memory_bytes", "active_power_scheme"):
        if before["runner"][field] != after["runner"][field]:
            raise RuntimeError(f"recorded machine/power differs at {field}")
    matrix = before["matrix"]
    cases = []
    for framework in matrix["frameworks"]:
        for rows in matrix["row_counts"]:
            relative = Path("raw") / f"{framework}-{rows}.json"
            old = comparison.read_json(before_dir / relative)
            new = comparison.read_json(after_dir / relative)
            old_app = comparison.checked_application(old, framework, rows, matrix, False)
            new_app = comparison.checked_application(new, framework, rows, matrix, False)
            if old_app["framework"] != new_app["framework"]:
                raise RuntimeError("actual page-reported framework version differs")
            operations = []
            for operation in comparison.OPERATIONS:
                old_summary = comparison.timing_summary(old_app["operation_js_commit_ms"][operation])
                new_summary = comparison.timing_summary(new_app["operation_js_commit_ms"][operation])
                operations.append({"operation": operation, "before": old_summary,
                                   "after": new_summary,
                                   "median_change": changed(old_summary["median_ms"], new_summary["median_ms"])})
            sums = []
            for application in (old_app, new_app):
                sums.append(comparison.timing_summary([
                    sum(application["operation_js_commit_ms"][operation][index]
                        for operation in comparison.OPERATIONS)
                    for index in range(matrix["measured_iterations"])]))
            cases.append({
                "framework": framework, "rows": rows,
                "before_raw": str(before_dir / relative),
                "after_raw": str(after_dir / relative), "operations": operations,
                "aligned_iteration_commit": {"before": sums[0], "after": sums[1],
                    "median_change": changed(sums[0]["median_ms"], sums[1]["median_ms"])},
                "private_bytes": changed(old["process_memory"]["current_private_bytes"], new["process_memory"]["current_private_bytes"]),
                "process_cpu_seconds": changed(old["process_cpu"]["total_seconds_delta"], new["process_cpu"]["total_seconds_delta"]),
                "workload_wall_ms": changed(old["workload_wall_ms"], new["workload_wall_ms"]),
                "full_process_wall_ms": changed(old["process_execution"]["wall_ms"], new["process_execution"]["wall_ms"]),
                "view_teardown_wall_ms": changed(old["view_teardown_wall_ms"], new["view_teardown_wall_ms"]),
                "before_cpu_offscreen_frames": old["cpu_offscreen_frames"],
                "after_cpu_offscreen_frames": new["cpu_offscreen_frames"]})
    return {"schema_version": 1, "status": "complete",
            "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "before_metadata": str(before_dir / "run-metadata.json"),
            "after_metadata": str(after_dir / "run-metadata.json"),
            "before_library_sha256": before["mini_ui_library_sha256"],
            "after_library_sha256": after["mini_ui_library_sha256"],
            "matrix": matrix, "cases": cases,
            "definitions": [
                "Commit totals align all seven operations within each iteration before median/p95.",
                "before_over_after>1 means faster/smaller after; reduction_percent<0 is regression.",
                "Allocation-budgeted production GC is included in workload/Tick/CPU/process scopes; do not hide its pauses behind commit-only improvements.",
                "N=7 nearest-rank p95 is the maximum observation, not a stable tail estimate.",
                "Private commit, working set, live JS/native heap and archive size are distinct."]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True)
    parser.add_argument("--after", required=True)
    parser.add_argument("--out-dir", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = (root / args.out_dir).resolve()
    result = compare_runs((root / args.before).resolve(), (root / args.after).resolve())
    common.write_json(output / "before-after.json", result)
    output.mkdir(parents=True, exist_ok=True)
    with (output / "changes.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["framework", "rows", "metric", "before", "after",
                         "before_over_after", "reduction_percent"])
        for case in result["cases"]:
            metrics = {"aligned_iteration_commit_ms": case["aligned_iteration_commit"]["median_change"]}
            metrics.update((name, case[name]) for name in (
                "private_bytes", "process_cpu_seconds", "workload_wall_ms",
                "full_process_wall_ms", "view_teardown_wall_ms"))
            metrics.update((operation["operation"] + "_commit_ms", operation["median_change"])
                           for operation in case["operations"])
            for name, change in metrics.items():
                writer.writerow([case["framework"], case["rows"], name,
                                 change["before"], change["after"],
                                 change["before_over_after"], change["reduction_percent"]])
    print(output / "before-after.json")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (KeyError, ValueError, OSError, RuntimeError) as failure:
        print(f"error: {failure}", file=sys.stderr)
        raise SystemExit(1)
