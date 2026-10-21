// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef RUNTIME_ELECTRON_COMMON_RENDERER_PLATFORM_H_
#define RUNTIME_ELECTRON_COMMON_RENDERER_PLATFORM_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/electron/common/renderer_protocol.h"

namespace atom {

enum class RendererProcessState {
    kRunning,
    kExited,
};

struct RendererProcessStatus {
    RendererProcessState state = RendererProcessState::kRunning;
    uint32_t exit_code = 0;
    bool crashed = false;
    bool sandbox_memory_limit_exceeded = false;
};

struct RendererPlatformProcess {
    renderer_protocol::PipeHandle read_pipe = renderer_protocol::kInvalidPipe;
    renderer_protocol::PipeHandle write_pipe = renderer_protocol::kInvalidPipe;
    void* frame_memory = nullptr;
    size_t frame_mapping_size = 0;
    int process_id = 0;
    bool termination_observed = false;
    uint32_t exit_code = 0;
    bool crashed = false;
    bool sandbox_memory_limit_exceeded = false;
#if BUILDFLAG(IS_WIN)
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE frame_mapping = nullptr;
    HANDLE control_pipe = nullptr;
#else
    int frame_fd = -1; // Writable producer descriptor; never transferred.
    int frame_read_fd = -1; // Read-only descriptor used for guest transfers.
    int control_socket = -1;
#endif
};

// Read in the browser process, before the renderer enters its sandbox.
std::vector<std::string> GetPreferredSystemLanguages();

bool LaunchRendererSandbox(uint64_t auth_high, uint64_t auth_low,
    RendererPlatformProcess* process, std::string* error);
// Allocates the existing frame surface without creating a process or IPC pipes.
bool CreateInProcessRendererResources(RendererPlatformProcess* process,
    std::string* error);
bool IsRendererProcessAlive(const RendererPlatformProcess& process);
RendererProcessStatus ObserveRendererProcess(RendererPlatformProcess* process,
    int timeout_milliseconds);
bool WaitForRendererProcess(RendererPlatformProcess* process,
    int timeout_milliseconds);
void TerminateRendererProcess(RendererPlatformProcess* process);
void CloseRendererPlatformProcess(RendererPlatformProcess* process);
bool ShareRendererFrameBuffer(const RendererPlatformProcess& source,
    RendererPlatformProcess* target, int guest_contents_id, std::string* error);

// Called in renderer mode before Blink/V8 initialization. Failure is fatal and
// there is deliberately no unsandboxed continuation.
bool EnterRendererSandbox(std::string* error);

} // namespace atom

#endif // RUNTIME_ELECTRON_COMMON_RENDERER_PLATFORM_H_
