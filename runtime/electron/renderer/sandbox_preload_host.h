// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef RUNTIME_ELECTRON_RENDERER_SANDBOX_PRELOAD_HOST_H_
#define RUNTIME_ELECTRON_RENDERER_SANDBOX_PRELOAD_HOST_H_

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "base/values.h"
#include "runtime/electron/common/renderer_client.h"
#include "runtime/engine/public/engine_api.h"
#include "v8.h"

namespace atom {

class SandboxPreloadHost {
public:
    using SendCallback = std::function<void(std::string, base::Value::Dict)>;
    using RequestCallback = std::function<uint64_t(std::string, base::Value::Dict)>;

    SandboxPreloadHost(int contents_id, bool context_isolation, bool webview_tag,
        bool preload_all_frames, std::vector<RendererPreload> preloads,
        base::Value::Dict process_metadata, SendCallback send, RequestCallback request);
    ~SandboxPreloadHost();

    void DidCreateScriptContext(mini_electron_web_view view,
        mini_electron_web_frame_handle frame, v8::Local<v8::Context> context,
        int world_id);
    void DeliverIpc(const std::string& channel, base::Value::List arguments);
    void ResolveInvoke(uint64_t request_id, base::Value::Dict result,
        std::string error);

private:
    struct Listener {
        v8::Global<v8::Function> function;
        bool once = false;
    };

    static void Require(const v8::FunctionCallbackInfo<v8::Value>& info);
    static void ExposeInMainWorld(const v8::FunctionCallbackInfo<v8::Value>& info);
    static void IpcSend(const v8::FunctionCallbackInfo<v8::Value>& info);
    static void IpcInvoke(const v8::FunctionCallbackInfo<v8::Value>& info);
    static void IpcOn(const v8::FunctionCallbackInfo<v8::Value>& info);
    static void IpcOnce(const v8::FunctionCallbackInfo<v8::Value>& info);
    static void IpcRemoveListener(const v8::FunctionCallbackInfo<v8::Value>& info);
    static void WebUtilsGetPathForFile(
        const v8::FunctionCallbackInfo<v8::Value>& info);
    static void GuestCreate(const v8::FunctionCallbackInfo<v8::Value>& info);
    static void GuestUpdate(const v8::FunctionCallbackInfo<v8::Value>& info);
    static void GuestDestroy(const v8::FunctionCallbackInfo<v8::Value>& info);
    static void GuestCommand(const v8::FunctionCallbackInfo<v8::Value>& info);
    static SandboxPreloadHost* Self(const v8::FunctionCallbackInfo<v8::Value>& info);

    void Install(v8::Local<v8::Context> context);
    void RunPreloads(v8::Local<v8::Context> context);
    void ReportPreloadError(v8::Local<v8::Context> context,
        const std::string& filename, v8::TryCatch* try_catch);
    void InstallGuestBridge(v8::Local<v8::Context> context);
    v8::Local<v8::Object> ElectronObject(v8::Local<v8::Context> context);
    void AddListener(const v8::FunctionCallbackInfo<v8::Value>& info, bool once);

    int contents_id_;
    bool context_isolation_;
    bool webview_tag_;
    bool preload_all_frames_;
    mini_electron_web_view view_ = NULL_WEBVIEW;
    v8::Isolate* isolate_ = nullptr;
    mini_electron_web_frame_handle main_frame_ = nullptr;
    std::vector<RendererPreload> preloads_;
    base::Value::Dict process_metadata_;
    SendCallback send_;
    RequestCallback request_;
    bool isolated_world_requested_ = false;
    v8::Global<v8::Context> main_context_;
    v8::Global<v8::Context> isolated_context_;
    v8::Global<v8::Object> electron_;
    std::map<std::string, std::vector<Listener>> listeners_;
    std::map<uint64_t, v8::Global<v8::Promise::Resolver>> invokes_;
};

} // namespace atom

#endif // RUNTIME_ELECTRON_RENDERER_SANDBOX_PRELOAD_HOST_H_
