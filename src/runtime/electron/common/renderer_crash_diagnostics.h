// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef RUNTIME_ELECTRON_COMMON_RENDERER_CRASH_DIAGNOSTICS_H_
#define RUNTIME_ELECTRON_COMMON_RENDERER_CRASH_DIAGNOSTICS_H_


#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#include <memory>
#include <optional>
#include <string>
#include "base/values.h"
#endif

namespace v8 { class Isolate; }

namespace atom {

#if BUILDFLAG(IS_WIN)
class RendererCrashMonitor;
struct RendererCrashHandles {
    HANDLE mapping = nullptr;
    HANDLE requested = nullptr;
    HANDLE completed = nullptr;
};

std::shared_ptr<RendererCrashMonitor> CreateRendererCrashMonitor(std::string* error);
RendererCrashHandles GetRendererCrashHandles(
    const std::shared_ptr<RendererCrashMonitor>& monitor);
bool StartRendererCrashMonitor(const std::shared_ptr<RendererCrashMonitor>& monitor,
    HANDLE process, HANDLE job, const std::string& directory, std::string* error);
void StopRendererCrashMonitor(const std::shared_ptr<RendererCrashMonitor>& monitor);
// Waits only for a published fatal exception, with bounded capture/exit grace.
// A dump or exception code is not a substitute for the OS process exit.
bool WaitForRendererCrashExit(const std::shared_ptr<RendererCrashMonitor>& monitor);
std::optional<bool> RendererMemoryLimitExceeded(
    const std::shared_ptr<RendererCrashMonitor>& monitor);
void AppendRendererCrashDiagnostics(const std::shared_ptr<RendererCrashMonitor>& monitor,
    base::Value::Dict* details);
bool InitializeRendererCrashDiagnostics(RendererCrashHandles handles);

// Call once per second on the owning main-isolate Blink thread, after pressure
// handling. Samples V8 and services one automatic pre-OOM snapshot attempt.
// Snapshot requests use the existing host channel; never call from fatal/OOM
// callbacks. The host reads MINI_ELECTRON_PRE_OOM_SNAPSHOT_PERCENT before launch:
// 1..65 lowers the default 65% threshold; 0 disables; invalid values are reported.
void RecordRendererV8Memory(v8::Isolate* isolate) noexcept;
// Fatal callback: copies markers only; even an uninitialized V8 heap is safe.
void RecordRendererV8OOM(const char* location, bool is_heap_oom,
    const char* detail) noexcept;
#endif

} // namespace atom

#endif // RUNTIME_ELECTRON_COMMON_RENDERER_CRASH_DIAGNOSTICS_H_
