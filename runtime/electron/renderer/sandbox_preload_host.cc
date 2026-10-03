// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "runtime/electron/renderer/sandbox_preload_host.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "runtime/electron/common/gin_helper/converter.h"
#include "runtime/electron/common/world_ids.h"
#include "runtime/electron/renderer/authorized_file.h"
#include "runtime/electron/renderer/api/context_bridge.h"
#include "runtime/electron/renderer/api/object_cache.h"

namespace atom {
namespace {

v8::Local<v8::String> V8String(v8::Isolate* isolate, const std::string& value)
{
    return v8::String::NewFromUtf8(isolate, value.data(), v8::NewStringType::kNormal,
        static_cast<int>(value.size())).ToLocalChecked();
}

bool ReadChannel(const v8::FunctionCallbackInfo<v8::Value>& info, std::string* channel)
{
    if (!info.Length() || !info[0]->IsString())
        return false;
    v8::String::Utf8Value utf8(info.GetIsolate(), info[0]);
    if (!*utf8 || utf8.length() <= 0 || utf8.length() > 1024)
        return false;
    channel->assign(*utf8, utf8.length());
    return channel->find('\0') == std::string::npos;
}

base::Value::List ConvertArguments(const v8::FunctionCallbackInfo<v8::Value>& info,
    int first)
{
    base::Value::List result;
    for (int i = first; i < info.Length(); ++i) {
        base::Value value;
        if (!gin_helper::Converter<base::Value>::FromV8(info.GetIsolate(), info[i], &value))
            value = base::Value();
        result.Append(std::move(value));
    }
    return result;
}

void ThrowTypeError(v8::Isolate* isolate, const char* message)
{
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8(isolate, message).ToLocalChecked()));
}


} // namespace

SandboxPreloadHost::SandboxPreloadHost(int contents_id, bool context_isolation,
    bool webview_tag, bool preload_all_frames,
    std::vector<RendererPreload> preloads, base::Value::Dict process_metadata,
    SendCallback send, RequestCallback request)
    : contents_id_(contents_id)
    , context_isolation_(context_isolation)
    , webview_tag_(webview_tag)
    , preload_all_frames_(preload_all_frames)
    , preloads_(std::move(preloads))
    , process_metadata_(std::move(process_metadata))
    , send_(std::move(send))
    , request_(std::move(request))
{
}

SandboxPreloadHost::~SandboxPreloadHost() = default;

SandboxPreloadHost* SandboxPreloadHost::Self(
    const v8::FunctionCallbackInfo<v8::Value>& info)
{
    if (info.Data().IsEmpty() || !info.Data()->IsExternal())
        return nullptr;
    return static_cast<SandboxPreloadHost*>(
        info.Data().As<v8::External>()->Value());
}

void SandboxPreloadHost::DidCreateScriptContext(mini_electron_web_view view,
    mini_electron_web_frame_handle frame, v8::Local<v8::Context> context,
    int world_id)
{
    view_ = view;
    if (!main_frame_)
        main_frame_ = frame;
    v8::Isolate* isolate = context->GetIsolate();
    isolate_ = isolate;
    if (world_id == WorldIDs::MAIN_WORLD_ID) {
        main_context_.Reset(isolate, context);
        if (webview_tag_)
            InstallGuestBridge(context);
        if (context_isolation_ && !isolated_world_requested_) {
            isolated_world_requested_ = true;
            mini_electron_run_js(view_, frame, "void 0", FALSE, nullptr, nullptr,
                reinterpret_cast<void*>(WorldIDs::ISOLATED_WORLD_ID));
        } else if (!context_isolation_) {
            isolated_context_.Reset(isolate, context);
            Install(context);
            RunPreloads(context);
        }
        return;
    }
    if (world_id != WorldIDs::ISOLATED_WORLD_ID || !context_isolation_)
        return;
    isolated_context_.Reset(isolate, context);
    Install(context);
    RunPreloads(context);
}

v8::Local<v8::Object> SandboxPreloadHost::ElectronObject(
    v8::Local<v8::Context> context)
{
    v8::Isolate* isolate = context->GetIsolate();
    if (!electron_.IsEmpty())
        return electron_.Get(isolate);
    v8::Local<v8::External> self = v8::External::New(isolate, this);
    auto function = [&](v8::FunctionCallback callback) {
        return v8::Function::New(context, callback, self).ToLocalChecked();
    };
    v8::Local<v8::Object> bridge = v8::Object::New(isolate);
    bridge->Set(context, V8String(isolate, "exposeInMainWorld"),
        function(&SandboxPreloadHost::ExposeInMainWorld)).Check();
    v8::Local<v8::Object> ipc = v8::Object::New(isolate);
    ipc->Set(context, V8String(isolate, "send"), function(&SandboxPreloadHost::IpcSend)).Check();
    ipc->Set(context, V8String(isolate, "invoke"), function(&SandboxPreloadHost::IpcInvoke)).Check();
    ipc->Set(context, V8String(isolate, "on"), function(&SandboxPreloadHost::IpcOn)).Check();
    ipc->Set(context, V8String(isolate, "once"), function(&SandboxPreloadHost::IpcOnce)).Check();
    ipc->Set(context, V8String(isolate, "removeListener"),
        function(&SandboxPreloadHost::IpcRemoveListener)).Check();
    v8::Local<v8::Object> web_utils = v8::Object::New(isolate);
    web_utils->Set(context, V8String(isolate, "getPathForFile"),
        function(&SandboxPreloadHost::WebUtilsGetPathForFile)).Check();
    v8::Local<v8::Object> electron = v8::Object::New(isolate);
    electron->Set(context, V8String(isolate, "contextBridge"), bridge).Check();
    electron->Set(context, V8String(isolate, "ipcRenderer"), ipc).Check();
    electron->Set(context, V8String(isolate, "webUtils"), web_utils).Check();
    electron_.Reset(isolate, electron);
    return electron;
}

void SandboxPreloadHost::Install(v8::Local<v8::Context> context)
{
    v8::Isolate* isolate = context->GetIsolate();
    v8::Context::Scope scope(context);
    v8::Local<v8::External> self = v8::External::New(isolate, this);
    v8::Local<v8::Function> require = v8::Function::New(context,
        &SandboxPreloadHost::Require, self).ToLocalChecked();
    context->Global()->Set(context, V8String(isolate, "require"), require).Check();

    base::Value::Dict metadata = process_metadata_.Clone();
    metadata.Set("contextIsolated", context_isolation_);
    metadata.Set("sandboxed", true);
    metadata.Set("type", "renderer");
    v8::Local<v8::Value> process = gin_helper::Converter<base::Value::Dict>::ToV8(isolate, metadata);
    if (process->IsObject()) {
        process.As<v8::Object>()->SetIntegrityLevel(context, v8::IntegrityLevel::kFrozen).Check();
        context->Global()->Set(context, V8String(isolate, "process"), process).Check();
    }
}

void SandboxPreloadHost::RunPreloads(v8::Local<v8::Context> context)
{
    v8::Isolate* isolate = context->GetIsolate();
    v8::Context::Scope scope(context);
    for (const RendererPreload& preload : preloads_) {
        v8::TryCatch try_catch(isolate);
        std::string wrapped =
            "(function(require, process, electron, exports) { 'use strict';\n"
            + preload.source + "\n})";
        v8::Local<v8::Script> script;
        v8::Local<v8::Value> value;
        v8::ScriptOrigin origin(V8String(isolate, preload.filename));
        if (!v8::Script::Compile(context, V8String(isolate, wrapped), &origin)
                 .ToLocal(&script)
            || !script->Run(context).ToLocal(&value)
            || !value->IsFunction()) {
            ReportPreloadError(context, preload.filename, &try_catch);
            continue;
        }
        v8::Local<v8::Value> require;
        v8::Local<v8::Value> process;
        if (!context->Global()
                 ->Get(context, V8String(isolate, "require"))
                 .ToLocal(&require)
            || !context->Global()
                    ->Get(context, V8String(isolate, "process"))
                    .ToLocal(&process)) {
            ReportPreloadError(context, preload.filename, &try_catch);
            continue;
        }
        v8::Local<v8::Value> arguments[] = {
            require,
            process,
            ElectronObject(context),
            v8::Object::New(isolate),
        };
        v8::Local<v8::Value> ignored;
        if (!value.As<v8::Function>()
                 ->Call(context, v8::Undefined(isolate),
                     std::size(arguments), arguments)
                 .ToLocal(&ignored)) {
            ReportPreloadError(context, preload.filename, &try_catch);
        }
    }
}

void SandboxPreloadHost::ReportPreloadError(v8::Local<v8::Context> context,
    const std::string& filename, v8::TryCatch* try_catch)
{
    v8::Isolate* isolate = context->GetIsolate();
    base::Value::Dict payload;
    payload.Set("file", filename);
    std::string message_text = "Preload script failed";
    if (!try_catch->Exception().IsEmpty()) {
        v8::String::Utf8Value exception(isolate, try_catch->Exception());
        if (*exception)
            message_text.assign(*exception, exception.length());
    }
    payload.Set("error", message_text);
    v8::Local<v8::Message> message = try_catch->Message();
    if (!message.IsEmpty())
        payload.Set("line", message->GetLineNumber(context).FromMaybe(0));
    v8::Local<v8::Value> stack;
    if (try_catch->StackTrace(context).ToLocal(&stack) && stack->IsString()) {
        v8::String::Utf8Value stack_text(isolate, stack);
        if (*stack_text)
            payload.Set("stack",
                std::string(*stack_text, stack_text.length()));
    }
    if (!payload.FindString("stack"))
        payload.Set("stack", message_text);
    send_("preload-error", std::move(payload));
}

void SandboxPreloadHost::WebUtilsGetPathForFile(
    const v8::FunctionCallbackInfo<v8::Value>& info)
{
    if (info.Length() != 1 || !info[0]->IsObject()) {
        ThrowTypeError(info.GetIsolate(), "webUtils.getPathForFile expects a File");
        return;
    }
    std::string path;
    if (!GetAuthorizedPathForFile(info.GetIsolate()->GetCurrentContext(),
            info[0], &path)) {
        info.GetReturnValue().Set(v8::String::Empty(info.GetIsolate()));
        return;
    }
    info.GetReturnValue().Set(V8String(info.GetIsolate(), path));
}

void SandboxPreloadHost::InstallGuestBridge(v8::Local<v8::Context> context)
{
    v8::Isolate* isolate = context->GetIsolate();
    v8::Context::Scope scope(context);
    v8::Local<v8::External> self = v8::External::New(isolate, this);
    auto function = [&](v8::FunctionCallback callback) {
        return v8::Function::New(context, callback, self).ToLocalChecked();
    };
    v8::Local<v8::Object> native = v8::Object::New(isolate);
    native->Set(context, V8String(isolate, "create"), function(&SandboxPreloadHost::GuestCreate)).Check();
    native->Set(context, V8String(isolate, "update"), function(&SandboxPreloadHost::GuestUpdate)).Check();
    native->Set(context, V8String(isolate, "destroy"), function(&SandboxPreloadHost::GuestDestroy)).Check();
    native->Set(context, V8String(isolate, "command"), function(&SandboxPreloadHost::GuestCommand)).Check();
    native->SetIntegrityLevel(context, v8::IntegrityLevel::kFrozen).Check();
    static constexpr char kGuestScript[] = R"JS(
(native => {
  if (customElements.get('webview')) return;
  let nextId = 1;
  const attributes = element => {
    const value = {};
    for (const attr of element.attributes) value[attr.name] = attr.value;
    return value;
  };
  const bounds = element => {
    const rect = element.getBoundingClientRect();
    return {x: Math.round(rect.x), y: Math.round(rect.y),
            width: Math.max(0, Math.round(rect.width)),
            height: Math.max(0, Math.round(rect.height))};
  };
  class MiniElectronWebView extends HTMLElement {
    constructor() {
      super();
      this.__instanceId = nextId++;
      this.__connectedGeneration = 0;
      this.__ready = Promise.resolve();
      this.__observer = new ResizeObserver(() => {
        const generation = this.__connectedGeneration;
        this.__ready.then(() => {
          if (this.isConnected && generation === this.__connectedGeneration)
            native.update(this.__instanceId, bounds(this));
        });
      });
    }
    connectedCallback() {
      const generation = ++this.__connectedGeneration;
      this.__ready = native.create(this.__instanceId, attributes(this));
      this.__ready.then(() => {
        if (this.isConnected && generation === this.__connectedGeneration)
          native.update(this.__instanceId, bounds(this));
        else
          native.destroy(this.__instanceId);
      });
      this.__observer.observe(this);
    }
    disconnectedCallback() {
      const generation = ++this.__connectedGeneration;
      this.__observer.disconnect();
      this.__ready.then(() => {
        if (!this.isConnected && generation === this.__connectedGeneration)
          native.destroy(this.__instanceId);
      });
    }
    loadURL(url, options = {}) {
      return this.__ready.then(() =>
        native.command(this.__instanceId, 'navigate', {url: String(url), options}));
    }
    send(channel, ...args) {
      return this.__ready.then(() =>
        native.command(this.__instanceId, 'ipc-message', {channel, args}));
    }
    executeJavaScript(code, userGesture = false) {
      return this.__ready.then(() =>
        native.command(this.__instanceId, 'executeJavaScript',
                       {code: String(code), userGesture: !!userGesture}));
    }
    focus() {
      super.focus();
      void this.__ready.then(() =>
        native.command(this.__instanceId, 'setFocus', {focused: true}));
    }
  }
  customElements.define('webview', MiniElectronWebView);
})(arguments[0]);
)JS";
    // Run inside a wrapper so the native capability is never installed on the
    // page global. `arguments` belongs to this immediately invoked function.
    std::string wrapper = "(function(){ " + std::string(kGuestScript) + " })";
    v8::Local<v8::Script> wrapped;
    if (!v8::Script::Compile(context, V8String(isolate, wrapper)).ToLocal(&wrapped))
        return;
    v8::Local<v8::Value> value;
    if (!wrapped->Run(context).ToLocal(&value) || !value->IsFunction())
        return;
    v8::Local<v8::Value> argv[] = { native };
    (void)value.As<v8::Function>()->Call(
        context, v8::Undefined(isolate), 1, argv);
}

void SandboxPreloadHost::GuestCreate(
    const v8::FunctionCallbackInfo<v8::Value>& info)
{
    SandboxPreloadHost* self = Self(info);
    if (!self || info.Length() < 2 || !info[0]->IsInt32())
        return;
    v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
    v8::Local<v8::Promise::Resolver> resolver =
        v8::Promise::Resolver::New(context).ToLocalChecked();
    base::Value::Dict attributes;
    gin_helper::Converter<base::Value::Dict>::FromV8(
        info.GetIsolate(), info[1], &attributes);
    base::Value::Dict params;
    params.Set("elementInstanceId", info[0].As<v8::Int32>()->Value());
    params.Set("attributes", std::move(attributes));
    uint64_t id = self->request_("guest-create", std::move(params));
    if (!id) {
        resolver
            ->Reject(context, v8::Exception::Error(
                V8String(info.GetIsolate(), "guest view channel is closed")))
            .Check();
    } else {
        self->invokes_.emplace(id, v8::Global<v8::Promise::Resolver>(
            info.GetIsolate(), resolver));
    }
    info.GetReturnValue().Set(resolver->GetPromise());
}

void SandboxPreloadHost::GuestUpdate(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    SandboxPreloadHost* self = Self(info);
    if (!self || info.Length() < 2 || !info[0]->IsInt32())
        return;
    base::Value::Dict rect;
    if (!gin_helper::Converter<base::Value::Dict>::FromV8(info.GetIsolate(), info[1], &rect))
        return;
    rect.Set("elementInstanceId", info[0].As<v8::Int32>()->Value());
    self->send_("guest-bounds", std::move(rect));
}

void SandboxPreloadHost::GuestDestroy(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    SandboxPreloadHost* self = Self(info);
    if (!self || !info.Length() || !info[0]->IsInt32())
        return;
    base::Value::Dict params;
    params.Set("elementInstanceId", info[0].As<v8::Int32>()->Value());
    self->send_("guest-destroy", std::move(params));
}

void SandboxPreloadHost::GuestCommand(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    SandboxPreloadHost* self = Self(info);
    if (!self || info.Length() < 3 || !info[0]->IsInt32() || !info[1]->IsString())
        return;
    v8::String::Utf8Value method(info.GetIsolate(), info[1]);
    if (!*method || method.length() <= 0 || method.length() > 128)
        return;
    base::Value::Dict command;
    gin_helper::Converter<base::Value::Dict>::FromV8(info.GetIsolate(), info[2], &command);
    base::Value::Dict params;
    params.Set("elementInstanceId", info[0].As<v8::Int32>()->Value());
    params.Set("method", std::string(*method, method.length()));
    params.Set("params", std::move(command));
    self->send_("guest-command", std::move(params));
}

void SandboxPreloadHost::Require(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    SandboxPreloadHost* self = Self(info);
    if (!self || info.Length() != 1 || !info[0]->IsString()) {
        ThrowTypeError(info.GetIsolate(), "restricted require expects one module name");
        return;
    }
    v8::String::Utf8Value name(info.GetIsolate(), info[0]);
    if (!*name || std::string_view(*name, name.length()) != "electron") {
        ThrowTypeError(info.GetIsolate(), "sandboxed preload can only require('electron')");
        return;
    }
    info.GetReturnValue().Set(self->ElectronObject(info.GetIsolate()->GetCurrentContext()));
}

void SandboxPreloadHost::ExposeInMainWorld(
    const v8::FunctionCallbackInfo<v8::Value>& info)
{
    SandboxPreloadHost* self = Self(info);
    if (!self || info.Length() != 2 || !info[0]->IsString()
        || self->main_context_.IsEmpty() || self->isolated_context_.IsEmpty()) {
        ThrowTypeError(info.GetIsolate(), "invalid contextBridge exposure");
        return;
    }
    v8::String::Utf8Value key(info.GetIsolate(), info[0]);
    if (!*key || key.length() <= 0 || key.length() > 1024) {
        ThrowTypeError(info.GetIsolate(), "invalid contextBridge key");
        return;
    }
    v8::Local<v8::Context> source = self->isolated_context_.Get(info.GetIsolate());
    v8::Local<v8::Context> destination = self->main_context_.Get(info.GetIsolate());
    api::context_bridge::ObjectCache cache;
    v8::MaybeLocal<v8::Value> proxied = api::PassValueToOtherContext(source,
        destination, info[1], &cache, false, 0);
    v8::Local<v8::Value> value;
    if (!proxied.ToLocal(&value))
        return;
    v8::Context::Scope scope(destination);
    destination->Global()->Set(destination,
        V8String(info.GetIsolate(), std::string(*key, key.length())), value).Check();
}

void SandboxPreloadHost::IpcSend(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    SandboxPreloadHost* self = Self(info);
    std::string channel;
    if (!self || !ReadChannel(info, &channel)) {
        ThrowTypeError(info.GetIsolate(), "ipcRenderer.send requires a valid channel");
        return;
    }
    base::Value::Dict params;
    params.Set("channel", std::move(channel));
    params.Set("args", ConvertArguments(info, 1));
    params.Set("frameId", static_cast<double>(reinterpret_cast<uintptr_t>(self->main_frame_)));
    self->send_("ipc-message", std::move(params));
}

void SandboxPreloadHost::IpcInvoke(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    SandboxPreloadHost* self = Self(info);
    std::string channel;
    if (!self || !ReadChannel(info, &channel)) {
        ThrowTypeError(info.GetIsolate(), "ipcRenderer.invoke requires a valid channel");
        return;
    }
    v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
    v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(context).ToLocalChecked();
    base::Value::Dict params;
    params.Set("channel", std::move(channel));
    params.Set("args", ConvertArguments(info, 1));
    params.Set("frameId", static_cast<double>(reinterpret_cast<uintptr_t>(self->main_frame_)));
    uint64_t id = self->request_("ipc-invoke", std::move(params));
    if (!id) {
        resolver->Reject(context, v8::Exception::Error(
            V8String(info.GetIsolate(), "renderer IPC is closed"))).Check();
    } else {
        self->invokes_.emplace(id, v8::Global<v8::Promise::Resolver>(
            info.GetIsolate(), resolver));
    }
    info.GetReturnValue().Set(resolver->GetPromise());
}

void SandboxPreloadHost::AddListener(
    const v8::FunctionCallbackInfo<v8::Value>& info, bool once)
{
    std::string channel;
    if (!ReadChannel(info, &channel) || info.Length() < 2 || !info[1]->IsFunction()) {
        ThrowTypeError(info.GetIsolate(), "ipcRenderer listener requires channel and function");
        return;
    }
    Listener listener;
    listener.function.Reset(info.GetIsolate(), info[1].As<v8::Function>());
    listener.once = once;
    listeners_[channel].push_back(std::move(listener));
    info.GetReturnValue().Set(info.This());
}

void SandboxPreloadHost::IpcOn(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    if (SandboxPreloadHost* self = Self(info))
        self->AddListener(info, false);
}

void SandboxPreloadHost::IpcOnce(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    if (SandboxPreloadHost* self = Self(info))
        self->AddListener(info, true);
}

void SandboxPreloadHost::IpcRemoveListener(
    const v8::FunctionCallbackInfo<v8::Value>& info)
{
    SandboxPreloadHost* self = Self(info);
    std::string channel;
    if (!self || !ReadChannel(info, &channel) || info.Length() < 2
        || !info[1]->IsFunction())
        return;
    auto found = self->listeners_.find(channel);
    if (found == self->listeners_.end())
        return;
    v8::Local<v8::Function> function = info[1].As<v8::Function>();
    std::erase_if(found->second, [&](Listener& listener) {
        return listener.function.Get(info.GetIsolate())->StrictEquals(function);
    });
    if (found->second.empty())
        self->listeners_.erase(found);
    info.GetReturnValue().Set(info.This());
}

void SandboxPreloadHost::DeliverIpc(const std::string& channel,
    base::Value::List arguments)
{
    if (isolated_context_.IsEmpty())
        return;
    v8::Isolate* isolate = isolate_;
    v8::HandleScope handles(isolate);
    v8::Local<v8::Context> context = isolated_context_.Get(isolate);
    v8::Context::Scope scope(context);
    auto found = listeners_.find(channel);
    if (found == listeners_.end())
        return;
    base::Value::Dict event_value;
    event_value.Set("senderId", contents_id_);
    event_value.Set("frameId", static_cast<double>(reinterpret_cast<uintptr_t>(main_frame_)));
    std::vector<v8::Local<v8::Value>> argv;
    argv.reserve(arguments.size() + 1);
    argv.push_back(gin_helper::Converter<base::Value::Dict>::ToV8(isolate, event_value));
    for (auto& argument : arguments)
        argv.push_back(gin_helper::Converter<base::Value>::ToV8(isolate, std::move(argument)));
    for (size_t i = 0; i < found->second.size();) {
        Listener& listener = found->second[i];
        listener.function.Get(isolate)->Call(context, v8::Undefined(isolate),
            argv.size(), argv.data()).ToLocalChecked();
        if (listener.once)
            found->second.erase(found->second.begin() + i);
        else
            ++i;
    }
    if (found->second.empty())
        listeners_.erase(found);
}

void SandboxPreloadHost::ResolveInvoke(uint64_t request_id,
    base::Value::Dict result, std::string error)
{
    auto found = invokes_.find(request_id);
    if (found == invokes_.end() || isolated_context_.IsEmpty())
        return;
    v8::Isolate* isolate = isolate_;
    v8::HandleScope handles(isolate);
    v8::Local<v8::Context> context = isolated_context_.Get(isolate);
    v8::Context::Scope scope(context);
    v8::Local<v8::Promise::Resolver> resolver = found->second.Get(isolate);
    if (!error.empty()) {
        resolver->Reject(context, v8::Exception::Error(V8String(isolate, error))).Check();
    } else if (base::Value* value = result.Find("value")) {
        resolver->Resolve(context,
            gin_helper::Converter<base::Value>::ToV8(isolate, value->Clone())).Check();
    } else {
        resolver->Resolve(context,
            gin_helper::Converter<base::Value::Dict>::ToV8(isolate, result)).Check();
    }
    invokes_.erase(found);
}

} // namespace atom
