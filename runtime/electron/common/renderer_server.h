// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef RUNTIME_ELECTRON_COMMON_RENDERER_SERVER_H_
#define RUNTIME_ELECTRON_COMMON_RENDERER_SERVER_H_

#include <functional>
#include <memory>

#include "base/values.h"
#include "runtime/electron/common/renderer_protocol.h"

namespace atom {

struct RendererPlatformProcess;

// Process-wide mode, fixed when the first WebContents is created.
bool IsSingleProcessMode();

// Shares the engine and UI sequence with the host, not its JavaScript context.
// Events and replies retain the normal client's asynchronous delivery.
class InProcessRenderer {
public:
    using MessageCallback = std::function<void(renderer_protocol::WireMessage)>;
    virtual ~InProcessRenderer() = default;
    virtual bool Start(base::Value::Dict options, MessageCallback callback) = 0;
    virtual bool Send(renderer_protocol::WireMessage message) = 0;
    // Borrows the source mapping and takes its own reference before returning.
    virtual bool RegisterFrameBuffer(int guest_contents_id,
        const RendererPlatformProcess& source) = 0;
#if BUILDFLAG(IS_WIN)
    virtual bool RegisterCapture(uint64_t request_id, HANDLE mapping,
        size_t mapping_size) = 0;
    virtual void CancelCapture(uint64_t request_id) = 0;
#endif
    virtual void Close() = 0;
};

std::unique_ptr<InProcessRenderer> CreateInProcessRenderer(
    RendererPlatformProcess* process, std::string* error);

bool IsRendererProcess(int argc, const char* const* argv);
bool IsRendererProcess(int argc, const wchar_t* const* argv);
int RunRendererProcess(int argc, char* argv[]);
int RunRendererProcess(int argc, wchar_t* argv[]);

} // namespace atom

#endif // RUNTIME_ELECTRON_COMMON_RENDERER_SERVER_H_
