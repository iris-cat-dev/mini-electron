#!/usr/bin/env python3
"""Run the identical offline UI workload in an installed, isolated Google Chrome."""
from __future__ import annotations

import argparse
import base64
from collections import deque
import ctypes
from ctypes import wintypes
import datetime as dt
import importlib.metadata
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Any

import run as common


class BasicLimits(ctypes.Structure):
    _fields_ = [("process_time", ctypes.c_longlong), ("job_time", ctypes.c_longlong),
                ("flags", wintypes.DWORD), ("min_working_set", ctypes.c_size_t),
                ("max_working_set", ctypes.c_size_t), ("active_limit", wintypes.DWORD),
                ("affinity", ctypes.c_size_t), ("priority", wintypes.DWORD),
                ("scheduling", wintypes.DWORD)]


class IoCounters(ctypes.Structure):
    _fields_ = [(name, ctypes.c_ulonglong) for name in
                ("read_operations", "write_operations", "other_operations",
                 "read_bytes", "write_bytes", "other_bytes")]


class ExtendedLimits(ctypes.Structure):
    _fields_ = [("basic", BasicLimits), ("io", IoCounters),
                ("process_memory_limit", ctypes.c_size_t),
                ("job_memory_limit", ctypes.c_size_t),
                ("peak_process_memory", ctypes.c_size_t),
                ("peak_job_memory", ctypes.c_size_t)]


class JobAccounting(ctypes.Structure):
    _fields_ = [(name, ctypes.c_longlong) for name in
                ("user_time", "kernel_time", "period_user_time", "period_kernel_time")] + [
                (name, wintypes.DWORD) for name in
                ("page_faults", "total_processes", "active_processes", "terminated_processes")]


class StartupInfo(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("reserved", wintypes.LPWSTR),
                ("desktop", wintypes.LPWSTR), ("title", wintypes.LPWSTR)] + [
                (name, wintypes.DWORD) for name in
                ("x", "y", "width", "height", "chars_x", "chars_y", "fill", "flags")] + [
                ("show", wintypes.WORD), ("reserved_bytes", wintypes.WORD),
                ("reserved_pointer", ctypes.c_void_p), ("stdin", wintypes.HANDLE),
                ("stdout", wintypes.HANDLE), ("stderr", wintypes.HANDLE)]


class ProcessInfo(ctypes.Structure):
    _fields_ = [("process", wintypes.HANDLE), ("thread", wintypes.HANDLE),
                ("pid", wintypes.DWORD), ("tid", wintypes.DWORD)]


class MemoryCounters(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("page_faults", wintypes.DWORD)] + [
                (name, ctypes.c_size_t) for name in
                ("peak_working_set", "working_set", "peak_paged_pool", "paged_pool",
                 "peak_nonpaged_pool", "nonpaged_pool", "pagefile", "peak_pagefile", "private_bytes")]


class OwnedChrome:
    """Assign the suspended browser to a kill-on-close job before it spawns children."""

    def __init__(self, command: list[str], log_path: Path, cwd: Path):
        import msvcrt

        self.k = ctypes.WinDLL("kernel32", use_last_error=True)
        signatures = {
            "CreateJobObjectW": ([ctypes.c_void_p, wintypes.LPCWSTR], wintypes.HANDLE),
            "SetInformationJobObject": ([wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD], wintypes.BOOL),
            "QueryInformationJobObject": ([wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p], wintypes.BOOL),
            "AssignProcessToJobObject": ([wintypes.HANDLE, wintypes.HANDLE], wintypes.BOOL),
            "CreateProcessW": ([wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p, ctypes.c_void_p,
                                wintypes.BOOL, wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR,
                                ctypes.POINTER(StartupInfo), ctypes.POINTER(ProcessInfo)], wintypes.BOOL),
            "ResumeThread": ([wintypes.HANDLE], wintypes.DWORD),
            "CloseHandle": ([wintypes.HANDLE], wintypes.BOOL),
            "WaitForSingleObject": ([wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
            "GetExitCodeProcess": ([wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL),
            "TerminateProcess": ([wintypes.HANDLE, wintypes.UINT], wintypes.BOOL),
            "TerminateJobObject": ([wintypes.HANDLE, wintypes.UINT], wintypes.BOOL),
            "OpenProcess": ([wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
            "IsProcessInJob": ([wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)], wintypes.BOOL),
            "K32GetProcessMemoryInfo": ([wintypes.HANDLE, ctypes.POINTER(MemoryCounters), wintypes.DWORD], wintypes.BOOL),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(self.k, name)
            function.argtypes, function.restype = arguments, result
        self.job = self.k.CreateJobObjectW(None, None)
        self.process = None
        self.pid = None
        if not self.job:
            raise ctypes.WinError(ctypes.get_last_error())
        limits = ExtendedLimits()
        limits.basic.flags = 0x00002000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        try:
            if not self.k.SetInformationJobObject(self.job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
                raise ctypes.WinError(ctypes.get_last_error())
            info = ProcessInfo()
            with log_path.open("wb") as log, open(os.devnull, "rb") as input_file:
                startup = StartupInfo()
                startup.cb = ctypes.sizeof(startup)
                startup.flags = 0x00000100  # STARTF_USESTDHANDLES
                startup.stdin = msvcrt.get_osfhandle(input_file.fileno())
                startup.stdout = startup.stderr = msvcrt.get_osfhandle(log.fileno())
                os.set_handle_inheritable(startup.stdin, True)
                os.set_handle_inheritable(startup.stdout, True)
                line = ctypes.create_unicode_buffer(subprocess.list2cmdline(command))
                if not self.k.CreateProcessW(command[0], line, None, None, True, 0x00000004,
                                             None, str(cwd), ctypes.byref(startup), ctypes.byref(info)):
                    raise ctypes.WinError(ctypes.get_last_error())
            self.process, self.pid = info.process, int(info.pid)
            try:
                if not self.k.AssignProcessToJobObject(self.job, self.process):
                    self.k.TerminateProcess(self.process, 1)
                    raise ctypes.WinError(ctypes.get_last_error())
                if self.k.ResumeThread(info.thread) == 0xFFFFFFFF:
                    raise ctypes.WinError(ctypes.get_last_error())
            finally:
                self.k.CloseHandle(info.thread)
        except BaseException:
            self.close()
            raise

    def accounting(self) -> dict[str, Any]:
        accounting = JobAccounting()
        limits = ExtendedLimits()
        for number, value in [(1, accounting), (9, limits)]:
            if not self.k.QueryInformationJobObject(self.job, number, ctypes.byref(value), ctypes.sizeof(value), None):
                raise ctypes.WinError(ctypes.get_last_error())
        return {"source": "QueryInformationJobObject; includes exited job processes",
                "kernel_100ns": accounting.kernel_time, "user_100ns": accounting.user_time,
                "total_cpu_seconds": (accounting.kernel_time + accounting.user_time) / 10_000_000,
                "total_processes": accounting.total_processes, "active_processes": accounting.active_processes,
                "peak_job_private_commit_bytes": limits.peak_job_memory}

    def memory(self) -> dict[str, Any]:
        capacity = 32
        while True:
            buffer = ctypes.create_string_buffer(8 + capacity * ctypes.sizeof(ctypes.c_size_t))
            if self.k.QueryInformationJobObject(self.job, 3, buffer, ctypes.sizeof(buffer), None):
                break
            if ctypes.get_last_error() != 234 or capacity >= 4096:
                raise ctypes.WinError(ctypes.get_last_error())
            capacity *= 2
        count = ctypes.cast(buffer, ctypes.POINTER(wintypes.DWORD))[1]
        pids = (ctypes.c_size_t * count).from_buffer(buffer, 8)
        records, unavailable = [], []
        for pid in pids:
            handle = self.k.OpenProcess(0x0400 | 0x0010, False, pid)
            if not handle:
                unavailable.append({"pid": int(pid), "win32_error": ctypes.get_last_error()})
                continue
            try:
                belongs = wintypes.BOOL()
                if not self.k.IsProcessInJob(handle, self.job, ctypes.byref(belongs)) or not belongs.value:
                    unavailable.append({"pid": int(pid), "error": "not in owned job at snapshot"})
                    continue
                counters = MemoryCounters()
                counters.cb = ctypes.sizeof(counters)
                if not self.k.K32GetProcessMemoryInfo(handle, ctypes.byref(counters), ctypes.sizeof(counters)):
                    unavailable.append({"pid": int(pid), "win32_error": ctypes.get_last_error()})
                    continue
                records.append({"pid": int(pid), "current_private_bytes": counters.private_bytes,
                                "current_working_set_bytes": counters.working_set})
            finally:
                self.k.CloseHandle(handle)
        return {"source": "GetProcessMemoryInfo over actual owned-job PID list",
                "processes": records, "unavailable": unavailable,
                "complete_snapshot": not unavailable,
                "current_private_bytes_sum": sum(p["current_private_bytes"] for p in records),
                "current_working_set_bytes_sum": sum(p["current_working_set_bytes"] for p in records),
                "working_set_note": "Summed process working sets double-count shared pages; not unique physical RSS."}

    def wait(self, seconds: float) -> int:
        result = self.k.WaitForSingleObject(self.process, int(seconds * 1000))
        if result == 258:
            raise TimeoutError("owned Chrome did not exit within its shutdown deadline")
        if result != 0:
            raise ctypes.WinError(ctypes.get_last_error())
        code = wintypes.DWORD()
        if not self.k.GetExitCodeProcess(self.process, ctypes.byref(code)):
            raise ctypes.WinError(ctypes.get_last_error())
        return int(code.value)

    def exited(self) -> bool:
        return self.k.WaitForSingleObject(self.process, 0) == 0

    def close(self) -> dict[str, Any]:
        cleanup = {"remaining_owned_processes_terminated": 0}
        try:
            if self.job:
                remaining = self.accounting()["active_processes"]
                cleanup["remaining_owned_processes_terminated"] = remaining
                if remaining:
                    if not self.k.TerminateJobObject(self.job, 1):
                        raise ctypes.WinError(ctypes.get_last_error())
                    deadline = time.perf_counter() + 10
                    while self.accounting()["active_processes"]:
                        if time.perf_counter() >= deadline:
                            raise TimeoutError("owned Chrome job did not finish cleanup")
                        time.sleep(0.01)
        finally:
            if self.job:
                self.k.CloseHandle(self.job)
                self.job = None
            if self.process:
                self.k.CloseHandle(self.process)
                self.process = None
        return cleanup


class Cdp:
    def __init__(self, websocket_module: Any, url: str):
        self.socket = websocket_module.create_connection(url, timeout=30, suppress_origin=True)
        self.next_id = 0
        self.events: deque[dict[str, Any]] = deque(maxlen=512)
        self.exceptions: list[dict[str, Any]] = []

    def receive(self, deadline: float) -> dict[str, Any]:
        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            raise TimeoutError("Chrome DevTools command deadline exceeded")
        self.socket.settimeout(remaining)
        frame = self.socket.recv()
        if not frame:
            raise RuntimeError("Chrome DevTools connection closed unexpectedly")
        message = json.loads(frame)
        if "method" in message:
            self.events.append(message)
            if message["method"] == "Runtime.exceptionThrown" and len(self.exceptions) < 32:
                self.exceptions.append(message)
        return message

    def call(self, method: str, params: dict[str, Any] | None = None,
             session: str | None = None, timeout: float = 30) -> dict[str, Any]:
        self.next_id += 1
        identifier = self.next_id
        request: dict[str, Any] = {"id": identifier, "method": method, "params": params or {}}
        if session:
            request["sessionId"] = session
        self.socket.send(json.dumps(request))
        deadline = time.perf_counter() + timeout
        while True:
            response = self.receive(deadline)
            if response.get("id") != identifier:
                continue
            if "error" in response:
                raise RuntimeError(f"{method}: {response['error']}")
            return response.get("result", {})

    def wait_event(self, method: str, session: str, predicate: Any, timeout: float) -> dict[str, Any]:
        deadline = time.perf_counter() + timeout
        while True:
            for event in self.events:
                if event.get("method") == method and event.get("sessionId") == session and predicate(event.get("params", {})):
                    return event["params"]
            self.receive(deadline)

    def evaluate(self, expression: str, session: str, timeout: float) -> Any:
        result = self.call("Runtime.evaluate", {"expression": expression, "returnByValue": True,
                           "awaitPromise": True}, session, timeout)
        if "exceptionDetails" in result:
            raise RuntimeError(json.dumps(result["exceptionDetails"], ensure_ascii=False))
        return result.get("result", {}).get("value")

    def notify_close(self) -> None:
        self.next_id += 1
        self.socket.send(json.dumps({"id": self.next_id, "method": "Browser.close"}))

    def close(self) -> None:
        self.socket.close()


def phase_promise(condition: str, result: str, trigger: str, timeout_ms: int) -> str:
    # Observe only the status element, not the large table's mutations.
    return f"""(() => new Promise((resolve, reject) => {{
      const element = document.getElementById('bench-status');
      if (!element) {{ reject(new Error('missing benchmark status element')); return; }}
      let timer;
      const observer = new MutationObserver(check);
      function finish(error, value) {{ observer.disconnect(); clearTimeout(timer);
        if (error) reject(error); else resolve(value); }}
      function check() {{ try {{
        if (typeof uiBench !== 'object') return;
        if (uiBench.status === 'error') {{ finish(new Error(JSON.stringify(uiBench.result()))); return; }}
        if ({condition}) finish(null, {result});
      }} catch (error) {{ finish(error); }} }}
      observer.observe(element, {{attributes:true, childList:true, characterData:true, subtree:true}});
      timer = setTimeout(() => finish(new Error('benchmark phase timeout')), {timeout_ms});
      try {{ {trigger}; check(); }} catch (error) {{ finish(error); }}
    }}))()"""


def metrics(cdp: Cdp, session: str) -> dict[str, float]:
    return {item["name"]: item["value"]
            for item in cdp.call("Performance.getMetrics", session=session)["metrics"]}


def validate_result(result: dict[str, Any], framework: str, version: str,
                    count: int, warmups: int, iterations: int) -> None:
    if result["framework"]["name"] != framework or result["framework"]["version"] != version:
        raise RuntimeError("actual framework identity differs from pinned manifest")
    for key, value in {"rows": count, "warmups": warmups, "iterations": iterations}.items():
        if result["config"][key] != value:
            raise RuntimeError(f"application configuration differs at {key}")
    if result["assertion_failures"] or result["capture_verification_assertions"] != 1:
        raise RuntimeError("application or populated capture assertions failed")
    if result["correctness_assertions"] != (warmups + iterations) * 7 + 1:
        raise RuntimeError("not every requested DOM transition was checked")
    expected = {"mount", "update_selected", "keyed_reorder", "prepend", "filter", "restore", "unmount"}
    if set(result["operation_js_commit_ms"]) != expected:
        raise RuntimeError("incomplete operation matrix")
    for name, samples in result["operation_js_commit_ms"].items():
        if len(samples) != iterations or any(isinstance(v, bool) or not isinstance(v, (int, float))
                or not math.isfinite(v) or v < 0 for v in samples):
            raise RuntimeError(f"invalid actual timing samples for {name}")


def run_cell(root: Path, output: Path, args: argparse.Namespace, framework: str,
             count: int, version: str, websocket_module: Any) -> dict[str, Any]:
    stem = f"{framework}-{count}"
    profile = Path(tempfile.mkdtemp(prefix="mini-ui-chrome-"))
    command = [str(args.chrome), f"--user-data-dir={profile}", "--remote-debugging-port=0",
               "--remote-debugging-address=127.0.0.1", "--no-first-run", "--no-default-browser-check",
               "--disable-background-networking", "--disable-component-update", "--disable-sync",
               "--disable-extensions", "--disable-default-apps", "--disable-background-timer-throttling",
               "--disable-renderer-backgrounding", "--disable-backgrounding-occluded-windows",
               "--force-device-scale-factor=1", f"--window-size={args.width},{args.height}"]
    if args.disable_gpu:
        command.append("--disable-gpu")
    command.append("about:blank")
    record: dict[str, Any] = {"schema_version": 1, "status": "error", "engine": "installed Google Chrome",
                              "framework": framework, "rows": count, "command": command,
                              "profile_directory": str(profile), "profile_is_private": True}
    browser_process = None
    cdp = None
    start = time.perf_counter()
    stage = "browser_launch"
    try:
        browser_process = OwnedChrome(command, output / f"{stem}-chrome.log", root)
        active_port = profile / "DevToolsActivePort"
        deadline = start + 60
        while True:
            if browser_process.exited():
                raise RuntimeError("owned Chrome exited before DevTools became available")
            if time.perf_counter() > deadline:
                raise TimeoutError("owned Chrome did not publish a readable complete DevTools endpoint")
            try:
                lines = active_port.read_text(encoding="utf-8").splitlines()
            except (FileNotFoundError, PermissionError):
                # Chrome creates this file before completing its exclusive write.
                lines = []
            if (len(lines) == 2 and lines[0].isdigit()
                    and lines[1].startswith("/devtools/browser/")):
                break
            time.sleep(0.02)
        cdp = Cdp(websocket_module, f"ws://127.0.0.1:{lines[0]}{lines[1]}")
        record["browser_version"] = cdp.call("Browser.getVersion")
        if not record["browser_version"]["product"].startswith("Chrome/"):
            raise RuntimeError("the control process is not the requested Google Chrome")
        record["gpu_info"] = cdp.call("SystemInfo.getInfo")
        targets = cdp.call("Target.getTargets")["targetInfos"]
        pages = [target for target in targets if target["type"] == "page"]
        if len(pages) != 1:
            raise RuntimeError("fresh owned Chrome did not expose exactly one initial page")
        session = cdp.call("Target.attachToTarget", {"targetId": pages[0]["targetId"], "flatten": True})["sessionId"]
        for method in ["Page.enable", "Runtime.enable", "Performance.enable"]:
            cdp.call(method, session=session)
        cdp.call("Page.setLifecycleEventsEnabled", {"enabled": True}, session)
        cdp.call("Emulation.setDeviceMetricsOverride", {"width": args.width, "height": args.height,
                 "deviceScaleFactor": args.device_scale, "mobile": False}, session)
        cdp.call("Page.bringToFront", session=session)
        stage = "navigation_ready"
        navigate_start = time.perf_counter()
        page = (root / "benchmarks/ui/assets" / f"{framework}.html").as_uri()
        navigation = cdp.call("Page.navigate", {"url": page}, session)
        if navigation.get("errorText"):
            raise RuntimeError(navigation["errorText"])
        cdp.wait_event("Page.lifecycleEvent", session,
                       lambda event: event.get("loaderId") == navigation.get("loaderId")
                       and event.get("name") == "load", args.timeout_ms / 1000)
        cdp.evaluate(phase_promise("uiBench.status === 'ready'", "true", "", args.timeout_ms),
                     session, args.timeout_ms / 1000 + 5)
        ready_time = time.perf_counter()
        record["cold_startup_wall_ms"] = {
            "browser_launch_to_protocol_ready": (ready_time - start) * 1000,
            "document_navigation_to_protocol_ready": (ready_time - navigate_start) * 1000}
        record["page_environment"] = cdp.evaluate("""(() => {
          let previous=performance.now(), minimum=Infinity;
          for(let i=0;i<10000;++i){const now=performance.now();if(now>previous)minimum=Math.min(minimum,now-previous);previous=now;}
          return {user_agent:navigator.userAgent,inner_width:innerWidth,inner_height:innerHeight,
            device_pixel_ratio:devicePixelRatio,cross_origin_isolated:crossOriginIsolated,
            secure_context:isSecureContext,url:location.href,
            minimum_observed_positive_performance_now_delta_ms:Number.isFinite(minimum)?minimum:null};
        })()""", session, 30)
        environment = record["page_environment"]
        if (environment["inner_width"], environment["inner_height"], environment["device_pixel_ratio"]) != (
                args.width, args.height, args.device_scale):
            raise RuntimeError("actual Chrome viewport or device scale differs from the requested control")
        record["metrics_before_workload"] = metrics(cdp, session)
        record["job_before_workload"] = browser_process.accounting()
        if args.profile:
            cdp.call("Profiler.enable", session=session)
            cdp.call("Profiler.start", session=session)
        stage = "workload"
        configuration = json.dumps({"rows": count, "warmups": args.warmups, "iterations": args.iterations})
        workload_start = time.perf_counter()
        cdp.evaluate(phase_promise("uiBench.done", "uiBench.result()", f"uiBench.start({configuration})",
                                  args.timeout_ms), session, args.timeout_ms / 1000 + 5)
        record["workload_wall_ms"] = (time.perf_counter() - workload_start) * 1000
        if args.profile:
            profile_result = cdp.call("Profiler.stop", session=session)
            common.write_json(output / f"{stem}.cpuprofile", profile_result["profile"])
            record["cpu_profile"] = str(output / f"{stem}.cpuprofile")
        record["metrics_after_workload"] = metrics(cdp, session)
        record["job_after_workload"] = browser_process.accounting()
        stage = "populated_capture"
        application = cdp.evaluate(phase_promise("uiBench.captureReady", "uiBench.result()",
                                   "uiBench.prepareCapture()", args.timeout_ms),
                                   session, args.timeout_ms / 1000 + 5)
        validate_result(application, framework, version, count, args.warmups, args.iterations)
        record["application_result"] = application
        screenshot = cdp.call("Page.captureScreenshot", {"format": "png", "fromSurface": True,
                              "captureBeyondViewport": False}, session, args.timeout_ms / 1000)
        capture = output / "captures" / f"{stem}.png"
        capture.parent.mkdir(parents=True, exist_ok=True)
        capture.write_bytes(base64.b64decode(screenshot["data"], validate=True))
        record["capture_png"] = str(capture)
        record["metrics_after_capture"] = metrics(cdp, session)
        record["job_after_capture"] = browser_process.accounting()
        record["process_memory_after_capture"] = browser_process.memory()
        record["reported_processes"] = cdp.call("SystemInfo.getProcessInfo")["processInfo"]
        record["uncaught_page_exceptions"] = cdp.exceptions
        if cdp.exceptions:
            raise RuntimeError("Chrome reported an uncaught page exception")
        stage = "browser_shutdown"
        shutdown_start = time.perf_counter()
        cdp.notify_close()
        record["normal_browser_exit_code"] = browser_process.wait(30)
        record["browser_shutdown_wall_ms"] = (time.perf_counter() - shutdown_start) * 1000
        record["job_after_browser_exit"] = browser_process.accounting()
        if record["normal_browser_exit_code"] != 0:
            raise RuntimeError("owned Chrome did not exit normally with code zero")
        record["status"] = "complete"
    except Exception as error:
        record["diagnostic"] = {"stage": stage, "type": type(error).__name__, "message": str(error)}
        if cdp:
            record["uncaught_page_exceptions"] = cdp.exceptions
    finally:
        if cdp:
            try:
                cdp.close()
            except Exception:
                pass  # The browser may already have closed its DevTools socket.
        if browser_process:
            try:
                record["owned_job_cleanup"] = browser_process.close()
            except Exception as error:
                record["status"] = "error"
                record["cleanup_error"] = str(error)
                record.setdefault("diagnostic", {
                    "stage": "owned_job_cleanup", "type": type(error).__name__, "message": str(error)})
        try:
            shutil.rmtree(profile)
            record["profile_removed"] = True
        except OSError as error:
            record["profile_removed"] = False
            record["profile_cleanup_error"] = str(error)
        record["full_control_wall_ms"] = (time.perf_counter() - start) * 1000
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--chrome", type=Path, default=Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe"))
    parser.add_argument("--out-dir", type=Path, default=Path("out/ui-chrome-results"))
    parser.add_argument("--frameworks", default="vue,react")
    parser.add_argument("--counts", default="100,1000,5000,10000")
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--iterations", type=int, default=7)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--device-scale", type=float, default=1.0)
    parser.add_argument("--timeout-ms", type=int, default=900000)
    parser.add_argument("--disable-gpu", action="store_true", help="additional Chrome software-rendering control")
    parser.add_argument("--profile", action="store_true", help="separate diagnostic run; do not pool profiled timings with benchmarks")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = (root / args.out_dir).resolve()
    try:
        if sys.platform != "win32" or ctypes.sizeof(ctypes.c_void_p) != 8:
            raise RuntimeError("this control uses real Windows x64 job and process accounting")
        import websocket
        frameworks = common.parse_csv_names(args.frameworks, {"vue", "react"}, "--frameworks")
        counts = common.parse_counts(args.counts)
        if not args.chrome.is_file():
            raise RuntimeError(f"installed Google Chrome is missing: {args.chrome}")
        if not (0 <= args.warmups <= 1000 and 1 <= args.iterations <= 1000
                and 1 <= args.width <= 32768 and 1 <= args.height <= 32768
                and math.isfinite(args.device_scale) and 0.1 <= args.device_scale <= 8
                and 100 <= args.timeout_ms <= 3600000):
            raise ValueError("invalid benchmark dimensions, iterations, scale or timeout")
        if (output / "run-metadata.json").exists():
            raise RuntimeError("output already contains a recorded run; choose a fresh --out-dir")
        manifest_path = root / "benchmarks/ui/frameworks.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        distributions = {entry["package"]: entry for entry in manifest["distributions"]}
        for entry in distributions.values():
            asset = root / "benchmarks/ui" / entry["file"]
            license_path = root / "benchmarks/ui" / entry["license_file"]
            if asset.stat().st_size != entry["bytes"] or common.sha256_file(asset) != entry["sha256"]:
                raise RuntimeError(f"pinned framework asset changed: {asset}")
            if common.sha256_file(license_path) != entry["license_sha256"]:
                raise RuntimeError(f"pinned framework license changed: {license_path}")
        metadata = {"schema_version": 1, "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                    "engine": "installed Google Chrome", "chrome_executable": str(args.chrome.resolve()),
                    "chrome_executable_sha256": common.sha256_file(args.chrome),
                    "control_source_sha256": common.sha256_file(Path(__file__).resolve()),
                    "runner": common.host_environment(),
                    "cdp_transport": {"package": "websocket-client", "version": importlib.metadata.version("websocket-client")},
                    "matrix": {"frameworks": frameworks, "row_counts": counts, "warmups": args.warmups,
                               "measured_iterations": args.iterations, "width": args.width, "height": args.height,
                               "device_scale": args.device_scale, "timeout_ms": args.timeout_ms},
                    "headed": True, "disable_gpu_requested": args.disable_gpu, "profiled_diagnostic": args.profile,
                    "frameworks": manifest,
                    "assets_sha256": {p.relative_to(root / "benchmarks/ui").as_posix(): common.sha256_file(p)
                                      for p in sorted((root / "benchmarks/ui/assets").rglob("*")) if p.is_file()},
                    "definitions": {
                        "operation_js_commit_ms": "Identical pinned assets/common.js interval around the framework DOM commit; excludes its later DOM assertion and paint.",
                        "workload_wall_ms": "Chrome wall interval includes genuine vsync-driven requestAnimationFrame waits; do not compare this to mini-ui CPU-offscreen throughput as display FPS.",
                        "cold_startup": "Fresh browser process and private profile for each cell; whole-browser startup includes CDP/viewport setup. Windows filesystem caches are not flushed.",
                        "cpu": "Real Windows job user+kernel accounting includes the Chrome browser and every owned descendant, including exited processes; Python CDP driver is outside the job.",
                        "memory": "Private bytes are summed over active owned-job processes after populated capture. Working-set sums double-count shared pages. Peak job memory is private commit, not peak physical RSS.",
                        "profile": "Profiler is disabled in comparison runs. --profile creates separately labeled diagnostic data.",
                        "normal_exit": "Success requires actual Google Chrome, checked DOM transitions, populated capture and browser process exit code zero. Only this suspended-at-launch owned process tree is assigned to the cleanup job."}}
        common.write_json(output / "run-metadata.json", metadata)
    except (ImportError, OSError, ValueError, KeyError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        print("Install developer-only transport: python -m pip install -r benchmarks/ui/requirements-chrome.txt", file=sys.stderr)
        return 2
    summaries: list[dict[str, Any]] = []
    failures = []
    for framework in frameworks:
        for count in counts:
            record = run_cell(root, output, args, framework, count, distributions[framework]["version"], websocket)
            raw_path = output / "raw" / f"{framework}-{count}.json"
            common.write_json(raw_path, record)
            print(framework, count, record["status"], flush=True)
            if record["status"] != "complete":
                failures.append({"framework": framework, "rows": count, "raw_file": str(raw_path),
                                 "diagnostic": record["diagnostic"]})
                continue
            for operation, samples in record["application_result"]["operation_js_commit_ms"].items():
                common.add_summary(summaries, framework, count, f"operation_js_commit.{operation}", "ms", samples, str(raw_path))
            for name, value in record["cold_startup_wall_ms"].items():
                common.add_summary(summaries, framework, count, f"cold_startup.{name}", "ms", [value], str(raw_path))
            for name, value in [("workload.wall", record["workload_wall_ms"]),
                                ("browser_shutdown.wall", record["browser_shutdown_wall_ms"])]:
                common.add_summary(summaries, framework, count, name, "ms", [value], str(raw_path))
            common.add_summary(summaries, framework, count, "chrome_job.cpu_startup_through_capture", "seconds",
                               [record["job_after_capture"]["total_cpu_seconds"]], str(raw_path))
            common.add_summary(summaries, framework, count, "chrome_job.cpu_workload", "seconds",
                               [record["job_after_workload"]["total_cpu_seconds"] - record["job_before_workload"]["total_cpu_seconds"]], str(raw_path))
            if record["process_memory_after_capture"]["complete_snapshot"]:
                common.add_summary(summaries, framework, count, "chrome_job.current_private_bytes", "bytes",
                                   [record["process_memory_after_capture"]["current_private_bytes_sum"]], str(raw_path))
    aggregate = {"schema_version": 1, "status": "error" if failures else "complete",
                 "metadata_file": str(output / "run-metadata.json"), "summaries": summaries, "failures": failures}
    common.write_json(output / "aggregate.json", aggregate)
    common.write_csv(output / "aggregate.csv", summaries)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
