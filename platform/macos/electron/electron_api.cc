// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "platform/macos/electron/electron_api.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>

#include "runtime/engine/common/thread_call.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-external.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-script.h"
#include "v8/include/v8-template.h"

namespace mini_electron::electron {
namespace {

v8::Local<v8::String> V8String(v8::Isolate* isolate, const char* value) {
  return v8::String::NewFromUtf8(isolate, value).ToLocalChecked();
}

v8::Local<v8::String> V8String(v8::Isolate* isolate,
                               const std::string& value) {
  return v8::String::NewFromUtf8(isolate, value.data(), v8::NewStringType::kNormal,
                                 static_cast<int>(value.size()))
      .ToLocalChecked();
}

std::string Utf8(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::String::Utf8Value text(isolate, value);
  return *text ? std::string(*text, text.length()) : std::string();
}

void SetMethod(v8::Local<v8::Context> context,
               v8::Local<v8::Object> object,
               const char* name,
               v8::FunctionCallback callback,
               v8::Local<v8::Value> data = {}) {
  v8::Isolate* isolate = context->GetIsolate();
  if (data.IsEmpty())
    data = v8::Undefined(isolate);
  object
      ->Set(context, V8String(isolate, name),
            v8::Function::New(context, callback, data).ToLocalChecked())
      .Check();
}

v8::Local<v8::Promise> ResolvedPromise(v8::Local<v8::Context> context) {
  v8::Local<v8::Promise::Resolver> resolver =
      v8::Promise::Resolver::New(context).ToLocalChecked();
  resolver->Resolve(context, v8::Undefined(context->GetIsolate())).Check();
  return resolver->GetPromise();
}

class Runtime {
 public:
  Runtime(v8::Isolate* isolate, std::filesystem::path script_path)
      : isolate_(isolate),
        script_directory_(std::filesystem::absolute(script_path).parent_path()) {}

  AppPlan TakePlan() {
    AppPlan plan;
    plan.quit_requested = quit_requested_;
    plan.windows.reserve(windows_.size());
    for (const auto& window : windows_)
      plan.windows.push_back(*window);
    return plan;
  }

  v8::Local<v8::Object> CreateElectronModule(v8::Local<v8::Context> context) {
    v8::EscapableHandleScope scope(isolate_);
    v8::Local<v8::Object> module = v8::Object::New(isolate_);
    v8::Local<v8::Object> app = v8::Object::New(isolate_);
    v8::Local<v8::External> runtime = v8::External::New(isolate_, this);

    SetMethod(context, app, "whenReady", AppWhenReady, runtime);
    SetMethod(context, app, "isReady", AppIsReady, runtime);
    SetMethod(context, app, "on", AppOn, runtime);
    SetMethod(context, app, "once", AppOn, runtime);
    SetMethod(context, app, "quit", AppQuit, runtime);
    SetMethod(context, app, "exit", AppQuit, runtime);
    SetMethod(context, app, "getName", AppGetName, runtime);
    module->Set(context, V8String(isolate_, "app"), app).Check();

    v8::Local<v8::FunctionTemplate> window_template =
        v8::FunctionTemplate::New(isolate_, NewBrowserWindow, runtime);
    window_template->SetClassName(V8String(isolate_, "BrowserWindow"));
    window_template->InstanceTemplate()->SetInternalFieldCount(1);
    window_template->PrototypeTemplate()->Set(
        isolate_, "loadFile", v8::FunctionTemplate::New(isolate_, LoadFile));
    window_template->PrototypeTemplate()->Set(
        isolate_, "loadURL", v8::FunctionTemplate::New(isolate_, LoadURL));
    window_template->PrototypeTemplate()->Set(
        isolate_, "show", v8::FunctionTemplate::New(isolate_, Show));
    window_template->PrototypeTemplate()->Set(
        isolate_, "hide", v8::FunctionTemplate::New(isolate_, Hide));
    window_template->PrototypeTemplate()->Set(
        isolate_, "isVisible", v8::FunctionTemplate::New(isolate_, IsVisible));
    window_template->PrototypeTemplate()->Set(
        isolate_, "setTitle", v8::FunctionTemplate::New(isolate_, SetTitle));
    window_template->PrototypeTemplate()->Set(
        isolate_, "getTitle", v8::FunctionTemplate::New(isolate_, GetTitle));
    window_template->PrototypeTemplate()->Set(
        isolate_, "getBounds", v8::FunctionTemplate::New(isolate_, GetBounds));
    window_template->PrototypeTemplate()->Set(
        isolate_, "on", v8::FunctionTemplate::New(isolate_, WindowOn));
    window_template->PrototypeTemplate()->Set(
        isolate_, "once", v8::FunctionTemplate::New(isolate_, WindowOn));

    v8::Local<v8::Function> constructor =
        window_template->GetFunction(context).ToLocalChecked();
    SetMethod(context, constructor, "getAllWindows", GetAllWindows, runtime);
    module->Set(context, V8String(isolate_, "BrowserWindow"), constructor).Check();
    return scope.Escape(module);
  }

  void InstallRequire(v8::Local<v8::Context> context,
                      v8::Local<v8::Object> electron_module) {
    electron_module_.Reset(isolate_, electron_module);
    SetMethod(context, context->Global(), "require", Require,
              v8::External::New(isolate_, this));
  }

 private:
  static Runtime* FromData(const v8::FunctionCallbackInfo<v8::Value>& info) {
    return static_cast<Runtime*>(
        info.Data().As<v8::External>()->Value());
  }

  static BrowserWindowOptions* Window(
      const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (info.This()->InternalFieldCount() != 1)
      return nullptr;
    return static_cast<BrowserWindowOptions*>(
        info.This()->GetAlignedPointerFromInternalField(0));
  }

  static void Require(const v8::FunctionCallbackInfo<v8::Value>& info) {
    Runtime* runtime = FromData(info);
    if (info.Length() != 1 || Utf8(info.GetIsolate(), info[0]) != "electron") {
      info.GetIsolate()->ThrowException(v8::Exception::Error(
          V8String(info.GetIsolate(), "only require('electron') is supported")));
      return;
    }
    info.GetReturnValue().Set(
        runtime->electron_module_.Get(info.GetIsolate()));
  }

  static void AppWhenReady(const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue().Set(
        ResolvedPromise(info.GetIsolate()->GetCurrentContext()));
  }

  static void AppIsReady(const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue().Set(true);
  }

  static void AppOn(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (info.Length() >= 2 && Utf8(info.GetIsolate(), info[0]) == "ready" &&
        info[1]->IsFunction()) {
      v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
      info[1].As<v8::Function>()->Call(context, info.This(), 0, nullptr);
    }
    info.GetReturnValue().Set(info.This());
  }

  static void AppQuit(const v8::FunctionCallbackInfo<v8::Value>& info) {
    FromData(info)->quit_requested_ = true;
  }

  static void AppGetName(const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue().Set(V8String(info.GetIsolate(), "mini-electron"));
  }

  static void NewBrowserWindow(
      const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (!info.IsConstructCall()) {
      info.GetIsolate()->ThrowException(v8::Exception::TypeError(
          V8String(info.GetIsolate(), "BrowserWindow must be constructed")));
      return;
    }
    Runtime* runtime = FromData(info);
    auto window = std::make_unique<BrowserWindowOptions>();
    v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
    if (info.Length() > 0 && info[0]->IsObject()) {
      v8::Local<v8::Object> options = info[0].As<v8::Object>();
      auto read_int = [&](const char* name, int* output) {
        v8::Local<v8::Value> value;
        if (options->Get(context, V8String(info.GetIsolate(), name)).ToLocal(&value) &&
            value->IsInt32())
          *output = value.As<v8::Int32>()->Value();
      };
      auto read_bool = [&](const char* name, bool* output) {
        v8::Local<v8::Value> value;
        if (options->Get(context, V8String(info.GetIsolate(), name)).ToLocal(&value) &&
            value->IsBoolean())
          *output = value->BooleanValue(info.GetIsolate());
      };
      read_int("width", &window->width);
      read_int("height", &window->height);
      read_bool("resizable", &window->resizable);
      read_bool("show", &window->visible);
      v8::Local<v8::Value> title;
      if (options->Get(context, V8String(info.GetIsolate(), "title")).ToLocal(&title) &&
          title->IsString())
        window->title = Utf8(info.GetIsolate(), title);
    }
    window->width = std::max(1, window->width);
    window->height = std::max(1, window->height);
    BrowserWindowOptions* native_window = window.get();
    runtime->windows_.push_back(std::move(window));
    info.This()->SetAlignedPointerInInternalField(0, native_window);
    info.This()
        ->Set(context, V8String(info.GetIsolate(), "__scriptDirectory"),
              V8String(info.GetIsolate(), runtime->script_directory_.string()))
        .Check();

    v8::Local<v8::Object> web_contents = v8::Object::New(info.GetIsolate());
    SetMethod(context, web_contents, "getURL", WebContentsGetURL,
              v8::External::New(info.GetIsolate(), native_window));
    info.This()
        ->Set(context, V8String(info.GetIsolate(), "webContents"), web_contents)
        .Check();
    runtime->window_objects_.emplace_back(info.GetIsolate(), info.This());
    info.GetReturnValue().Set(info.This());
  }

  static void LoadFile(const v8::FunctionCallbackInfo<v8::Value>& info) {
    BrowserWindowOptions* window = Window(info);
    if (!window || info.Length() < 1)
      return;
    v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
    std::filesystem::path path(Utf8(info.GetIsolate(), info[0]));
    if (path.is_relative()) {
      v8::Local<v8::Value> directory;
      if (info.This()->Get(info.GetIsolate()->GetCurrentContext(),
                           V8String(info.GetIsolate(), "__scriptDirectory"))
              .ToLocal(&directory) && directory->IsString())
        path = std::filesystem::path(Utf8(info.GetIsolate(), directory)) / path;
    }
    window->url = std::filesystem::absolute(path).lexically_normal().string();
    info.GetReturnValue().Set(
        ResolvedPromise(info.GetIsolate()->GetCurrentContext()));
  }

  static void LoadURL(const v8::FunctionCallbackInfo<v8::Value>& info) {
    BrowserWindowOptions* window = Window(info);
    if (!window || info.Length() < 1)
      return;
    window->url = Utf8(info.GetIsolate(), info[0]);
    info.GetReturnValue().Set(
        ResolvedPromise(info.GetIsolate()->GetCurrentContext()));
  }

  static void Show(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (BrowserWindowOptions* window = Window(info))
      window->visible = true;
  }

  static void Hide(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (BrowserWindowOptions* window = Window(info))
      window->visible = false;
  }

  static void IsVisible(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (BrowserWindowOptions* window = Window(info))
      info.GetReturnValue().Set(window->visible);
  }

  static void SetTitle(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (BrowserWindowOptions* window = Window(info); window && info.Length() > 0)
      window->title = Utf8(info.GetIsolate(), info[0]);
  }

  static void GetTitle(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (BrowserWindowOptions* window = Window(info))
      info.GetReturnValue().Set(V8String(info.GetIsolate(), window->title));
  }

  static void GetBounds(const v8::FunctionCallbackInfo<v8::Value>& info) {
    BrowserWindowOptions* window = Window(info);
    if (!window)
      return;
    v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
    v8::Local<v8::Object> bounds = v8::Object::New(info.GetIsolate());
    bounds->Set(context, V8String(info.GetIsolate(), "x"), v8::Integer::New(info.GetIsolate(), 0)).Check();
    bounds->Set(context, V8String(info.GetIsolate(), "y"), v8::Integer::New(info.GetIsolate(), 0)).Check();
    bounds->Set(context, V8String(info.GetIsolate(), "width"), v8::Integer::New(info.GetIsolate(), window->width)).Check();
    bounds->Set(context, V8String(info.GetIsolate(), "height"), v8::Integer::New(info.GetIsolate(), window->height)).Check();
    info.GetReturnValue().Set(bounds);
  }

  static void WindowOn(const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue().Set(info.This());
  }

  static void WebContentsGetURL(
      const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* window = static_cast<BrowserWindowOptions*>(
        info.Data().As<v8::External>()->Value());
    info.GetReturnValue().Set(V8String(info.GetIsolate(), window->url));
  }

  static void GetAllWindows(const v8::FunctionCallbackInfo<v8::Value>& info) {
    Runtime* runtime = FromData(info);
    v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
    v8::Local<v8::Array> windows = v8::Array::New(
        info.GetIsolate(), static_cast<int>(runtime->window_objects_.size()));
    for (size_t i = 0; i < runtime->window_objects_.size(); ++i) {
      windows
          ->Set(context, static_cast<uint32_t>(i),
                runtime->window_objects_[i].Get(info.GetIsolate()))
          .Check();
    }
    info.GetReturnValue().Set(windows);
  }

  v8::Isolate* isolate_;
  std::filesystem::path script_directory_;
  std::vector<std::unique_ptr<BrowserWindowOptions>> windows_;
  std::vector<v8::Global<v8::Object>> window_objects_;
  bool quit_requested_ = false;
  v8::Global<v8::Object> electron_module_;
};

std::string ExceptionText(v8::Isolate* isolate,
                          v8::TryCatch* try_catch,
                          const std::string& script_path) {
  std::string text = Utf8(isolate, try_catch->Exception());
  v8::Local<v8::Message> message = try_catch->Message();
  if (!message.IsEmpty()) {
    int line = message->GetLineNumber(isolate->GetCurrentContext()).FromMaybe(0);
    return script_path + ":" + std::to_string(line) + ": " + text;
  }
  return script_path + ": " + text;
}

}  // namespace

std::optional<AppPlan> EvaluateMainScript(const std::string& script_path,
                                          std::string* error) {
  std::ifstream input(script_path);
  if (!input) {
    *error = "cannot open Electron main script: " + script_path;
    return std::nullopt;
  }
  std::ostringstream contents;
  contents << input.rdbuf();

  std::optional<AppPlan> result;
  content::ThreadCall::callBlinkThreadSync(FROM_HERE, [&] {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    if (!isolate) {
      *error = "Blink thread has no active V8 isolate";
      return;
    }
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);
    v8::TryCatch try_catch(isolate);
    Runtime runtime(isolate, script_path);
    v8::Local<v8::Object> module = runtime.CreateElectronModule(context);
    runtime.InstallRequire(context, module);

    v8::Local<v8::String> directory =
        V8String(isolate,
                 std::filesystem::absolute(script_path).parent_path().string());
    context->Global()
        ->Set(context, V8String(isolate, "__dirname"), directory)
        .Check();

    v8::Local<v8::String> source = V8String(isolate, contents.str());
    v8::Local<v8::String> resource_name = V8String(isolate, script_path);
    v8::ScriptOrigin origin(resource_name);
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> ignored;
    if (!v8::Script::Compile(context, source, &origin).ToLocal(&script) ||
        !script->Run(context).ToLocal(&ignored)) {
      *error = ExceptionText(isolate, &try_catch, script_path);
      return;
    }
    isolate->PerformMicrotaskCheckpoint();
    if (try_catch.HasCaught())
      *error = ExceptionText(isolate, &try_catch, script_path);
    else
      result = runtime.TakePlan();
  });
  return result;
}

}  // namespace mini_electron::electron
