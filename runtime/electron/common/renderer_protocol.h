// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef RUNTIME_ELECTRON_COMMON_RENDERER_PROTOCOL_H_
#define RUNTIME_ELECTRON_COMMON_RENDERER_PROTOCOL_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "base/values.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace atom::renderer_protocol {

inline constexpr uint32_t kWireMagic = 0x4d455250; // "MERP"
inline constexpr uint16_t kWireVersion = 3;
inline constexpr size_t kMaxMessageBytes = 64 * 1024 * 1024;
inline constexpr size_t kMaxContainerEntries = 64 * 1024;
inline constexpr size_t kMaxValueDepth = 64;
inline constexpr uint32_t kFrameMagic = 0x4d454652; // "MEFR"
inline constexpr size_t kMaxFrameBytes = 64 * 1024 * 1024;

#if BUILDFLAG(IS_WIN)
using PipeHandle = HANDLE;
inline const PipeHandle kInvalidPipe = INVALID_HANDLE_VALUE;
#else
using PipeHandle = int;
inline constexpr PipeHandle kInvalidPipe = -1;
#endif

enum class MessageKind : uint16_t {
    kHello = 1,
    kRequest = 2,
    kSend = 3,
    kResponse = 4,
    kEvent = 5,
    kCancel = 6,
    kShutdown = 7,
};


struct WireMessage {
    MessageKind kind = MessageKind::kSend;
    uint64_t request_id = 0;
    base::Value::Dict payload;
};
inline constexpr uint32_t kControlMagic = 0x4d454643; // "MEFC"
struct FrameControlMessage {
    uint32_t magic = kControlMagic;
    int32_t guest_contents_id = 0;
    uint64_t frame_handle = 0; // Windows target-process handle; zero on POSIX.
    uint64_t frame_mapping_size = 0;
    // Zero identifies the long-lived guest mapping above. A non-zero value
    // identifies a single capture request and requires guest_contents_id == 0.
    uint64_t capture_request_id = 0;
};
static_assert(offsetof(FrameControlMessage, guest_contents_id) == 4);
static_assert(offsetof(FrameControlMessage, frame_handle) == 8);
static_assert(offsetof(FrameControlMessage, frame_mapping_size) == 16);
static_assert(offsetof(FrameControlMessage, capture_request_id) == 24);
static_assert(sizeof(FrameControlMessage) == 32);


// generation is a seqlock: odd while the renderer writes, even while stable.
// CPU frames and explicit native captures store premultiplied RGBA after it.
struct alignas(64) FrameBufferHeader {
    uint32_t magic = kFrameMagic;
    uint32_t capacity = 0;
    std::atomic<uint64_t> generation { 0 };
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t data_size = 0;
    uint32_t output_format = 0; // 0: CPU RGBA, 1: Metal layer, 2: shared D3D12.
    uint32_t native_context_id = 0;
    float native_scale_factor = 1.f;
    uint32_t native_flags = 0;
    uint64_t presented_generation = 0;
    uint64_t native_texture_handle = 0; // Producer-process NT handles, never JS.
    uint64_t native_fence_handle = 0;
    uint64_t native_ready_value = 0;
    uint64_t native_frame_id = 0;
    uint32_t native_adapter_luid_low = 0;
    int32_t native_adapter_luid_high = 0;
    uint64_t native_source_id = 0;
};
static_assert(offsetof(FrameBufferHeader, generation) == 8);
static_assert(offsetof(FrameBufferHeader, data_size) == 28);
static_assert(offsetof(FrameBufferHeader, output_format) == 32);
static_assert(offsetof(FrameBufferHeader, native_context_id) == 36);
static_assert(offsetof(FrameBufferHeader, native_scale_factor) == 40);
static_assert(offsetof(FrameBufferHeader, native_flags) == 44);
static_assert(offsetof(FrameBufferHeader, presented_generation) == 48);
static_assert(offsetof(FrameBufferHeader, native_texture_handle) == 56);
static_assert(offsetof(FrameBufferHeader, native_frame_id) == 80);
static_assert(offsetof(FrameBufferHeader, native_source_id) == 96);
static_assert(sizeof(FrameBufferHeader) == 128);

bool ReadMessage(PipeHandle pipe, uint64_t auth_high, uint64_t auth_low,
    WireMessage* message, std::string* error);
bool WriteMessage(PipeHandle pipe, uint64_t auth_high, uint64_t auth_low,
    const WireMessage& message, std::string* error);
void ClosePipe(PipeHandle pipe);

// Renderer IPC only accepts this deliberately small command vocabulary. File
// paths and process-spawning operations are never accepted from an untrusted
// renderer; resource/storage/file access must use a broker-request event.
bool IsBrowserToRendererMethod(const std::string& method);
bool IsRendererBrokerKind(const std::string& kind);

} // namespace atom::renderer_protocol

#endif // RUNTIME_ELECTRON_COMMON_RENDERER_PROTOCOL_H_
