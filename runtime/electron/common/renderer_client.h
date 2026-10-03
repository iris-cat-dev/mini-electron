// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef RUNTIME_ELECTRON_COMMON_RENDERER_CLIENT_H_
#define RUNTIME_ELECTRON_COMMON_RENDERER_CLIENT_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/values.h"

namespace atom {

struct RendererPreload {
    std::string filename;
    std::string source;
};
struct RendererPrivilegedScheme {
    std::string scheme;
    bool standard = false;
    bool secure = false;
    bool support_fetch_api = false;
    bool cors_enabled = false;
    bool allow_service_workers = false;
};


struct RendererLaunchOptions {
    int contents_id = 0;
    std::vector<RendererPreload> preloads;
    std::vector<RendererPrivilegedScheme> privileged_schemes;
    bool context_isolation = true;
    bool webview_tag = false;
    bool preload_all_frames = false;
    bool web_security = true;
    bool allow_running_insecure_content = false;
    bool transparent = false;
    base::Value::Dict preload_process_metadata;
};

struct RendererFrameSnapshot {
    int width = 0;
    int height = 0;
    int stride = 0;
    uint64_t generation = 0;
    std::vector<uint8_t> rgba;
};

class RendererClient {
public:
    using EventCallback = std::function<void(base::Value::Dict)>;
    using ReplyCallback = std::function<void(base::Value::Dict, std::string)>;

    virtual ~RendererClient() = default;

    // Launches a renderer sandbox and returns only after its authenticated IPC
    // channel is ready. Events and replies are always delivered on the creating
    // sequence. A failure to create the sandbox or channel returns nullptr.
    static std::unique_ptr<RendererClient> Create(RendererLaunchOptions options,
        EventCallback event_callback);

    // Sends a cancellable request. A non-zero id is returned only if queued.
    // Closing the renderer completes every outstanding callback with an error.
    virtual uint64_t Request(std::string method, base::Value::Dict params,
        ReplyCallback callback) = 0;
    virtual bool Send(std::string method, base::Value::Dict params) = 0;

    // Completes a renderer-originated request (ipc-invoke, before-input-event,
    // resource/storage broker, or window-open). Unknown/completed ids fail.
    virtual bool Respond(uint64_t request_id, base::Value::Dict result,
        std::string error) = 0;

    virtual int GetProcessId() const = 0;
    virtual bool IsAlive() const = 0;
    virtual bool CopyFrame(RendererFrameSnapshot* snapshot) const = 0;
    // Makes this renderer's read-only frame mapping available to |target| for
    // webview compositing. The OS handle is transferred out-of-band and is
    // registered under guest_contents_id; raw handle values never enter JS.
    virtual bool ShareFrameBufferWith(RendererClient& target,
        int guest_contents_id) const = 0;
    virtual void Close() = 0;
};

} // namespace atom

#endif // RUNTIME_ELECTRON_COMMON_RENDERER_CLIENT_H_
