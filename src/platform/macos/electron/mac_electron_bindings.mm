// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <ServiceManagement/ServiceManagement.h>

#include "src/platform/macos/electron/mac_electron_bindings.h"
#include "src/platform/macos/electron/mac_platform_apis.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <unistd.h>

#include "base/apple/scoped_cftyperef.h"
#include "base/files/file_path.h"
#include "base/message_loop/message_pump_apple.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "src/runtime/electron/browser/api/session.h"
#include "src/runtime/electron/browser/api/web_contents.h"
#include "src/runtime/electron/browser/api/window_list.h"
#include "src/runtime/electron/browser/api/window_interface.h"
#include "src/runtime/electron/common/api/event_emitter.h"
#include "src/runtime/electron/common/gin_helper/dictionary.h"
#include "src/runtime/electron/common/gin_helper/object_template_builder.h"
#include "src/runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "src/runtime/electron/common/gin_helper/public/wrapper_info.h"
#include "src/runtime/electron/common/gin_helper/wrappable.h"
#include "src/runtime/electron/common/renderer_platform.h"
#include "src/runtime/engine/public/engine_api.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_buffer.h"
#include "third_party/libuv/include/uv.h"
#include "ui/events/keycodes/keyboard_code_conversion_mac.h"
#include "ui/events/keycodes/dom/dom_key.h"

// The layer-host class is supplied by QuartzCore; only its context identifier
// crosses the renderer boundary, never a Mach port or an Objective-C object.
@interface CALayerHost : CALayer
@property(nonatomic) uint32_t contextId;
@end

@class MiniElectronApplicationDelegate;

namespace mini_electron::electron {
namespace {
MiniElectronApplicationDelegate* g_application_delegate = nil;

class MacBrowserWindow;
void DestroyAllWindows();
bool CloseAllWindows();
void RequestMainLoopExit();

struct HostState {
  node::Environment* environment = nullptr;
  v8::Isolate* isolate = nullptr;
  v8::Global<v8::Context> context;
  v8::Global<v8::Object> app;
  MacBindingOptions options;
  std::string name = "OMP Desktop";
  std::string version = "0.0.0";
  std::string app_path;
  bool packaged = false;
  bool ready = false;
  int exit_code = 0;
  bool quitting = false;
  bool exiting = false;
  bool exit_requested = false;
  int next_window_id = 1;
  int single_instance_fd = -1;
  std::string single_instance_notification;
  std::unordered_map<std::string, std::string> paths;
  std::vector<MacBrowserWindow*> windows;
};

HostState& State() {
  static HostState state;
  return state;
}

// AppKit delegate and notification callbacks run from the event pump outside
// any V8 scope. Entering JavaScript from them needs a HandleScope, the main
// context, and a Node CallbackScope so microtasks and async hooks drain like
// any other native-to-JS callback.
class NativeCallbackScope {
 public:
  explicit NativeCallbackScope(HostState& state)
      : handle_scope_(state.isolate),
        context_scope_(state.context.Get(state.isolate)),
        callback_scope_(state.isolate,
                        state.context.Get(state.isolate)->Global(),
                        {0, 0}) {}
  NativeCallbackScope(const NativeCallbackScope&) = delete;
  NativeCallbackScope& operator=(const NativeCallbackScope&) = delete;

 private:
  v8::HandleScope handle_scope_;
  v8::Context::Scope context_scope_;
  node::CallbackScope callback_scope_;
};

bool CanEnterJavaScript(const HostState& state) {
  return state.isolate && state.environment && !state.context.IsEmpty();
}

bool CanEmitAppEvent(const HostState& state) {
  return CanEnterJavaScript(state) && !state.app.IsEmpty();
}

v8::Local<v8::String> V8String(v8::Isolate* isolate, const char* value) {
  return v8::String::NewFromUtf8(isolate, value).ToLocalChecked();
}

v8::Local<v8::String> V8String(v8::Isolate* isolate,
                               const std::string& value) {
  return v8::String::NewFromUtf8(isolate, value.data(),
                                 v8::NewStringType::kNormal,
                                 static_cast<int>(value.size()))
      .ToLocalChecked();
}

std::string Utf8(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::String::Utf8Value text(isolate, value);
  return *text ? std::string(*text, text.length()) : std::string();
}

NSString* NSStringFromUtf8(const std::string& value) {
  NSString* result = [NSString stringWithUTF8String:value.c_str()];
  return result ?: @"";
}

void SetMethod(v8::Local<v8::Context> context,
               v8::Local<v8::Object> target,
               const char* name,
               v8::FunctionCallback callback) {
  v8::Isolate* isolate = context->GetIsolate();
  target
      ->Set(context, V8String(isolate, name),
            v8::Function::New(context, callback).ToLocalChecked())
      .Check();
}

void SetValue(v8::Local<v8::Context> context,
              v8::Local<v8::Object> target,
              const char* name,
              v8::Local<v8::Value> value) {
  target->Set(context, V8String(context->GetIsolate(), name), value).Check();
}

v8::Local<v8::Object> AppObject(v8::Isolate* isolate) {
  return State().app.Get(isolate);
}

void Emit(v8::Local<v8::Object> object,
          const char* name,
          int argument_count = 0,
          v8::Local<v8::Value>* arguments = nullptr) {
  if (object.IsEmpty())
    return;
  v8::Isolate* isolate = object->GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Value> emit_value;
  if (!object->Get(context, V8String(isolate, "emit")).ToLocal(&emit_value) ||
      !emit_value->IsFunction()) {
    return;
  }
  std::vector<v8::Local<v8::Value>> values;
  values.reserve(static_cast<size_t>(argument_count) + 1);
  values.push_back(V8String(isolate, name));
  for (int index = 0; index < argument_count; ++index)
    values.push_back(arguments[index]);
  (void)emit_value.As<v8::Function>()->Call(
      context, object, static_cast<int>(values.size()), values.data());
}

void PreventDefault(const v8::FunctionCallbackInfo<v8::Value>& info) {
  *static_cast<bool*>(info.Data().As<v8::External>()->Value()) = true;
}

bool EmitCancelable(v8::Local<v8::Object> object, const char* name) {
  bool prevented = false;
  v8::Isolate* isolate = object->GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Object> event = v8::Object::New(isolate);
  event
      ->Set(context, V8String(isolate, "preventDefault"),
            v8::Function::New(context, PreventDefault,
                              v8::External::New(isolate, &prevented))
                .ToLocalChecked())
      .Check();
  v8::Local<v8::Value> argument = event;
  Emit(object, name, 1, &argument);
  return prevented;
}

void EmitCancelableWithValue(v8::Local<v8::Object> object, const char* name,
                             v8::Local<v8::Value> value) {
  bool prevented = false;
  v8::Isolate* isolate = object->GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Object> event = v8::Object::New(isolate);
  event->Set(context, V8String(isolate, "preventDefault"),
             v8::Function::New(context, PreventDefault,
                               v8::External::New(isolate, &prevented))
                 .ToLocalChecked())
      .Check();
  v8::Local<v8::Value> arguments[] = {event, value};
  Emit(object, name, 2, arguments);
}

std::string SearchPath(NSSearchPathDirectory directory) {
  NSArray<NSString*>* paths =
      NSSearchPathForDirectoriesInDomains(directory, NSUserDomainMask, YES);
  return [paths count] ? [paths[0] fileSystemRepresentation] : std::string();
}

CGFloat DesktopTop() {
  return NSMaxY(NSScreen.screens.firstObject.frame);
}

NSRect CocoaRectFromElectron(NSRect rect) {
  rect.origin.y = DesktopTop() - rect.origin.y - rect.size.height;
  return rect;
}

NSRect ElectronRectFromCocoa(NSRect rect) {
  rect.origin.y = DesktopTop() - NSMaxY(rect);
  return rect;
}

void RefreshNamedPaths() {
  HostState& state = State();
  const std::string application_support = SearchPath(NSApplicationSupportDirectory);
  const std::string caches = SearchPath(NSCachesDirectory);
  const std::string logs = SearchPath(NSLibraryDirectory) + "/Logs";
  state.paths["home"] = [NSHomeDirectory() fileSystemRepresentation];
  state.paths["temp"] = [NSTemporaryDirectory() fileSystemRepresentation];
  state.paths["appData"] = application_support;
  state.paths["cache"] = caches;
  state.paths["logs"] = logs + "/" + state.name;
  state.paths["userData"] = application_support + "/" + state.name;
  state.paths["documents"] = SearchPath(NSDocumentDirectory);
  state.paths["downloads"] = SearchPath(NSDownloadsDirectory);
  state.paths["desktop"] = SearchPath(NSDesktopDirectory);
  state.paths["pictures"] = SearchPath(NSPicturesDirectory);
  state.paths["music"] = SearchPath(NSMusicDirectory);
  state.paths["videos"] = SearchPath(NSMoviesDirectory);
  state.paths["exe"] = [[[NSBundle mainBundle] executablePath] fileSystemRepresentation];
}

void AppSetName(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() < 1)
    return;
  State().name = Utf8(info.GetIsolate(), info[0]);
  RefreshNamedPaths();
  if (!atom::SessionMgr::get()->setRootDir(
          base::FilePath::FromUTF8Unsafe(State().paths["userData"]))) {
    info.GetIsolate()->ThrowException(v8::Exception::Error(
        V8String(info.GetIsolate(), "Unable to update app userData directory")));
    return;
  }
  SetValue(info.GetIsolate()->GetCurrentContext(), info.This(), "name",
           V8String(info.GetIsolate(), State().name));
}

void AppGetName(const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(V8String(info.GetIsolate(), State().name));
}

void AppSetVersion(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length())
    State().version = Utf8(info.GetIsolate(), info[0]);
}

void AppGetVersion(const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(V8String(info.GetIsolate(), State().version));
}

void AppGetLocale(const v8::FunctionCallbackInfo<v8::Value>& info) {
  const auto languages = atom::GetPreferredSystemLanguages();
  if (languages.empty())
    info.GetReturnValue().Set(v8::String::Empty(info.GetIsolate()));
  else
    info.GetReturnValue().Set(V8String(info.GetIsolate(), languages.front()));
}

void AppGetPreferredSystemLanguages(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  const auto languages = atom::GetPreferredSystemLanguages();
  v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
  v8::Local<v8::Array> result = v8::Array::New(info.GetIsolate(), languages.size());
  for (size_t index = 0; index < languages.size(); ++index)
    result->Set(context, index, V8String(info.GetIsolate(), languages[index])).Check();
  info.GetReturnValue().Set(result);
}

void AppSetAppPath(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (!info.Length())
    return;
  State().app_path = Utf8(info.GetIsolate(), info[0]);
  atom::WebContents::setApplicationRoot(State().app_path);
}

void AppGetAppPath(const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(V8String(info.GetIsolate(), State().app_path));
}

void AppSetPackaged(const v8::FunctionCallbackInfo<v8::Value>& info) {
  State().packaged = info.Length() && info[0]->BooleanValue(info.GetIsolate());
  SetValue(info.GetIsolate()->GetCurrentContext(), info.This(), "isPackaged",
           v8::Boolean::New(info.GetIsolate(), State().packaged));
}

void AppSetReady(const v8::FunctionCallbackInfo<v8::Value>& info) {
  State().ready = true;
  // Finish launching before Electron's ready event, including when bootstrap
  // reaches readiness before the continuous AppKit event loop begins.
  [NSApp finishLaunching];
}

void AppIsReady(const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(State().ready);
}

void AppGetPath(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (!info.Length())
    return;
  const std::string name = Utf8(info.GetIsolate(), info[0]);
  auto found = State().paths.find(name);
  if (found == State().paths.end()) {
    info.GetIsolate()->ThrowException(v8::Exception::Error(
        V8String(info.GetIsolate(), "Unknown app path: " + name)));
    return;
  }
  info.GetReturnValue().Set(V8String(info.GetIsolate(), found->second));
}

void AppSetPath(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length() < 2)
    return;
  const std::string name = Utf8(info.GetIsolate(), info[0]);
  const std::string path = Utf8(info.GetIsolate(), info[1]);
  if (name == "userData" && !atom::SessionMgr::get()->setRootDir(
                                base::FilePath::FromUTF8Unsafe(path))) {
    info.GetIsolate()->ThrowException(v8::Exception::Error(
        V8String(info.GetIsolate(), "Unable to select the userData directory")));
    return;
  }
  State().paths[name] = path;
}

void AppFocus(const v8::FunctionCallbackInfo<v8::Value>&) {
  [NSApp activateIgnoringOtherApps:YES];
}

void AppHide(const v8::FunctionCallbackInfo<v8::Value>&) {
  [NSApp hide:nil];
}

void AppShow(const v8::FunctionCallbackInfo<v8::Value>&) {
  [NSApp unhide:nil];
  [NSApp activateIgnoringOtherApps:YES];
}

void AppRequestSingleInstanceLock(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  HostState& state = State();
  if (state.single_instance_fd >= 0) {
    info.GetReturnValue().Set(true);
    return;
  }
  NSString* lock_directory = NSStringFromUtf8(state.paths["userData"]);
  [[NSFileManager defaultManager] createDirectoryAtPath:lock_directory
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
  NSString* lock_path = [lock_directory
      stringByAppendingPathComponent:@"mini-electron.instance.lock"];
  int descriptor = open([lock_path fileSystemRepresentation], O_CREAT | O_RDWR, 0600);
  const bool acquired = descriptor >= 0 && flock(descriptor, LOCK_EX | LOCK_NB) == 0;
  if (acquired) {
    state.single_instance_fd = descriptor;
    state.single_instance_notification =
        "mini-electron.second-instance." + state.name;
    [[NSDistributedNotificationCenter defaultCenter]
        addObserver:g_application_delegate
           selector:@selector(secondInstance:)
               name:NSStringFromUtf8(state.single_instance_notification)
             object:nil];
  } else {
    if (descriptor >= 0)
      close(descriptor);
    NSDictionary* user_info = @{
      @"arguments" : NSProcessInfo.processInfo.arguments,
      @"workingDirectory" : NSFileManager.defaultManager.currentDirectoryPath
    };
    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:NSStringFromUtf8(
                                 "mini-electron.second-instance." + state.name)
                      object:nil
                    userInfo:user_info
          deliverImmediately:YES];
  }
  info.GetReturnValue().Set(acquired);
}

void ReleaseSingleInstanceLock() {
  HostState& state = State();
  if (!state.single_instance_notification.empty()) {
    [[NSDistributedNotificationCenter defaultCenter]
        removeObserver:g_application_delegate
                  name:NSStringFromUtf8(state.single_instance_notification)
                object:nil];
    state.single_instance_notification.clear();
  }
  if (state.single_instance_fd < 0)
    return;
  flock(state.single_instance_fd, LOCK_UN);
  close(state.single_instance_fd);
  state.single_instance_fd = -1;
}

void AppReleaseSingleInstanceLock(
    const v8::FunctionCallbackInfo<v8::Value>&) {
  ReleaseSingleInstanceLock();
}

void AppGetExitCode(const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(State().exit_code);
}

// Mirrors App::finishQuit on Windows: 'quit' is emitted once, the main loop
// stops, and RunMainProcess then emits process 'exit'.
void FinishQuit(v8::Isolate* isolate, int exit_code, bool force) {
  HostState& state = State();
  if (state.exiting)
    return;
  state.exiting = true;
  state.quitting = true;
  state.exit_code = exit_code;
  if (force)
    DestroyAllWindows();
  EmitCancelableWithValue(AppObject(isolate), "quit",
                          v8::Integer::New(isolate, exit_code));
  ReleaseSingleInstanceLock();
  RequestMainLoopExit();
}

void AppExit(const v8::FunctionCallbackInfo<v8::Value>& info) {
  int exit_code = 0;
  if (info.Length() && info[0]->IsInt32())
    exit_code = info[0].As<v8::Int32>()->Value();
  FinishQuit(info.GetIsolate(), exit_code, true);
}

bool RequestApplicationQuit(v8::Isolate* isolate) {
  HostState& state = State();
  if (state.quitting)
    return true;
  v8::Local<v8::Object> app = AppObject(isolate);
  if (EmitCancelable(app, "before-quit"))
    return false;
  state.quitting = true;
  if (!CloseAllWindows() || EmitCancelable(app, "will-quit")) {
    state.quitting = false;
    return false;
  }
  FinishQuit(isolate, 0, false);
  return true;
}

void AppQuit(const v8::FunctionCallbackInfo<v8::Value>& info) {
  RequestApplicationQuit(info.GetIsolate());
}

}  // namespace
}  // namespace mini_electron::electron

using mini_electron::electron::AppObject;
using mini_electron::electron::CanEmitAppEvent;
using mini_electron::electron::CanEnterJavaScript;
using mini_electron::electron::Emit;
using mini_electron::electron::EmitCancelableWithValue;
using mini_electron::electron::HostState;
using mini_electron::electron::NativeCallbackScope;
using mini_electron::electron::RequestApplicationQuit;
using mini_electron::electron::State;
using mini_electron::electron::V8String;

@interface MiniElectronApplicationDelegate : NSObject <NSApplicationDelegate>
- (void)secondInstance:(NSNotification*)notification;
@end

@implementation MiniElectronApplicationDelegate
- (void)secondInstance:(NSNotification*)notification {
  HostState& state = State();
  if (!CanEmitAppEvent(state))
    return;
  NativeCallbackScope scope(state);
  v8::Local<v8::Context> context = state.isolate->GetCurrentContext();
  NSArray<NSString*>* values = notification.userInfo[@"arguments"];
  v8::Local<v8::Array> arguments =
      v8::Array::New(state.isolate, values.count);
  for (NSUInteger index = 0; index < values.count; ++index) {
    arguments
        ->Set(context, index,
              V8String(state.isolate, values[index].UTF8String ?: ""))
        .Check();
  }
  EmitCancelableWithValue(
      AppObject(state.isolate), "second-instance", arguments);
}
- (void)applicationDidBecomeActive:(NSNotification*)notification {
  HostState& state = State();
  if (!CanEmitAppEvent(state))
    return;
  NativeCallbackScope scope(state);
  Emit(AppObject(state.isolate), "activate");
  (void)notification;
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)application
                    hasVisibleWindows:(BOOL)has_visible_windows {
  HostState& state = State();
  if (CanEmitAppEvent(state)) {
    NativeCallbackScope scope(state);
    v8::Local<v8::Value> argument =
        v8::Boolean::New(state.isolate, has_visible_windows);
    Emit(AppObject(state.isolate), "activate", 1, &argument);
  }
  (void)application;
  return YES;
}

- (void)application:(NSApplication*)application
            openURLs:(NSArray<NSURL*>*)urls {
  HostState& state = State();
  if (!CanEmitAppEvent(state))
    return;
  NativeCallbackScope scope(state);
  for (NSURL* url in urls) {
    const char* text = url.absoluteString.UTF8String;
    EmitCancelableWithValue(
        AppObject(state.isolate), "open-url",
        V8String(state.isolate, text ? text : ""));
  }
  (void)application;
}

- (void)application:(NSApplication*)application
           openFiles:(NSArray<NSString*>*)filenames {
  HostState& state = State();
  if (!CanEmitAppEvent(state))
    return;
  NativeCallbackScope scope(state);
  for (NSString* filename in filenames) {
    EmitCancelableWithValue(
        AppObject(state.isolate), "open-file",
        V8String(state.isolate, filename.UTF8String ?: ""));
  }
  [application replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}

- (NSApplicationTerminateReply)applicationShouldTerminate:
    (NSApplication*)application {
  HostState& state = State();
  if (!CanEmitAppEvent(state))
    return NSTerminateNow;
  NativeCallbackScope scope(state);
  RequestApplicationQuit(state.isolate);
  (void)application;
  return NSTerminateCancel;
}
@end

namespace mini_electron::electron {
namespace {

void AppGetLoginItemSettings(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  bool enabled = false;
  if (@available(macOS 13.0, *))
    enabled = SMAppService.mainAppService.status == SMAppServiceStatusEnabled;
  v8::Local<v8::Object> result = v8::Object::New(info.GetIsolate());
  SetValue(info.GetIsolate()->GetCurrentContext(), result, "openAtLogin",
           v8::Boolean::New(info.GetIsolate(), enabled));
  info.GetReturnValue().Set(result);
}

void AppSetLoginItemSettings(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (!info.Length() || !info[0]->IsObject())
    return;
  gin_helper::Dictionary options(info.GetIsolate(), info[0].As<v8::Object>());
  bool open_at_login = false;
  options.GetBydefaultVal("openAtLogin", false, &open_at_login);
  if (@available(macOS 13.0, *)) {
    NSError* error = nil;
    bool success = open_at_login
        ? [SMAppService.mainAppService registerAndReturnError:&error]
        : [SMAppService.mainAppService unregisterAndReturnError:&error];
    if (!success) {
      info.GetIsolate()->ThrowException(v8::Exception::Error(
          V8String(info.GetIsolate(), error.localizedDescription.UTF8String)));
    }
  }
}

void AppSetBadgeCount(const v8::FunctionCallbackInfo<v8::Value>& info) {
  int count = info.Length()
      ? info[0]->Int32Value(info.GetIsolate()->GetCurrentContext()).FromMaybe(0)
      : 0;
  NSApp.dockTile.badgeLabel =
      count > 0 ? [NSString stringWithFormat:@"%d", count] : @"";
  info.GetReturnValue().Set(true);
}

void AppGetAppMetrics(const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(v8::Array::New(info.GetIsolate()));
}

void CommandLineAppendSwitch(const v8::FunctionCallbackInfo<v8::Value>&) {}
void CommandLineAppendArgument(const v8::FunctionCallbackInfo<v8::Value>&) {}

v8::Local<v8::Object> CreateApp(v8::Local<v8::Context> context) {
  v8::Isolate* isolate = context->GetIsolate();
  v8::Local<v8::Object> app = v8::Object::New(isolate);
  SetMethod(context, app, "_setAppPath", AppSetAppPath);
  SetMethod(context, app, "getAppPath", AppGetAppPath);
  SetMethod(context, app, "_setIsPackaged", AppSetPackaged);
  SetMethod(context, app, "_setIsReady", AppSetReady);
  SetMethod(context, app, "isReady", AppIsReady);
  SetMethod(context, app, "setName", AppSetName);
  SetMethod(context, app, "getName", AppGetName);
  SetMethod(context, app, "setVersion", AppSetVersion);
  SetMethod(context, app, "getVersion", AppGetVersion);
  SetMethod(context, app, "getLocale", AppGetLocale);
  SetMethod(context, app, "getPreferredSystemLanguages", AppGetPreferredSystemLanguages);
  SetMethod(context, app, "getPath", AppGetPath);
  SetMethod(context, app, "setPath", AppSetPath);
  SetMethod(context, app, "focus", AppFocus);
  SetMethod(context, app, "hide", AppHide);
  SetMethod(context, app, "show", AppShow);
  SetMethod(context, app, "quit", AppQuit);
  SetMethod(context, app, "exit", AppExit);
  SetMethod(context, app, "getExitCode", AppGetExitCode);
  SetMethod(context, app, "requestSingleInstanceLock", AppRequestSingleInstanceLock);
  SetMethod(context, app, "releaseSingleInstanceLock", AppReleaseSingleInstanceLock);
  SetMethod(context, app, "getLoginItemSettings", AppGetLoginItemSettings);
  SetMethod(context, app, "setLoginItemSettings", AppSetLoginItemSettings);
  SetMethod(context, app, "setBadgeCount", AppSetBadgeCount);
  SetMethod(context, app, "getAppMetrics", AppGetAppMetrics);
  SetValue(context, app, "name", V8String(isolate, State().name));
  SetValue(context, app, "isPackaged", v8::Boolean::New(isolate, State().packaged));

  v8::Local<v8::Object> command_line = v8::Object::New(isolate);
  SetMethod(context, command_line, "appendSwitch", CommandLineAppendSwitch);
  SetMethod(context, command_line, "appendArgument", CommandLineAppendArgument);
  SetValue(context, app, "commandLine", command_line);
  return app;
}

// Anything that presents a WebContents frame in a MiniElectronRendererView.
class RendererSurfaceHost {
 public:
  virtual void Draw(NSRect dirty_rect) = 0;
  virtual void Resize() = 0;
  virtual void RefreshVisibility() = 0;
  virtual void Focus(bool focused) = 0;
  virtual bool SendInput(base::Value::Dict event) = 0;
  virtual bool SendNativeFileDrag(base::Value::Dict event,
                                  const std::vector<std::string>& paths) = 0;
  virtual bool CanPerformNativeFileDrop() const = 0;
  virtual bool HandleCaptionMouseDown(NSEvent*, NSPoint) { return false; }
  void ResetOcclusionObservation() { has_native_exposure_ = false; }

 protected:
  virtual ~RendererSurfaceHost() {
    DetachNativeLayer();
    if (frame_image_)
      CGImageRelease(frame_image_);
    if (frame_color_space_)
      CGColorSpaceRelease(frame_color_space_);
  }
  void DrawFrame(atom::WebContents* contents, NSView* view, NSRect dirty_rect);
  bool UpdateFrame(atom::WebContents* contents, NSView* view);
  void DetachNativeLayer();
  void InvalidateFrame() { frame_dirty_ = true; }
  uint64_t DrawnGeneration() const { return drawn_generation_; }
  bool IsRendererWindowVisible(NSWindow* window) {
    if (!window || !window.visible || window.miniaturized || NSApp.hidden) {
      has_native_exposure_ = false;
      return false;
    }
    // AppKit can leave the visible bit unset until the first native exposure.
    // Do not suspend an onscreen first frame based on that uninitialized state.
    const bool exposed =
        (window.occlusionState & NSWindowOcclusionStateVisible) != 0;
    has_native_exposure_ |= exposed;
    return exposed || !has_native_exposure_;
  }

 private:
  atom::RendererFrameSnapshot presented_frame_;
  atom::RendererFrameSnapshot pending_frame_;
  CGImageRef frame_image_ = nullptr;
  CGColorSpaceRef frame_color_space_ = nullptr;
  CALayer* native_clip_layer_ = nil;
  CALayerHost* native_host_layer_ = nil;
  bool frame_dirty_ = true;
  bool has_native_exposure_ = false;
  uint64_t drawn_generation_ = 0;
};

void ResizeWebContents(atom::WebContents* contents, NSView* view);

// BrowserView: an extra WebContents composited as a subview of a window's
// renderer view, positioned in window content (top-left origin) coordinates.
class MacBrowserView final : public mate::EventEmitter<MacBrowserView>,
                             public atom::WindowInterface,
                             public atom::WebContentsObserver,
                             public RendererSurfaceHost {
 public:
  MacBrowserView(v8::Isolate* isolate,
                 v8::Local<v8::Object> wrapper,
                 const gin_helper::Dictionary& options);
  ~MacBrowserView() override;

  static void Init(v8::Local<v8::Object> target);
  static MacBrowserView* FromValue(v8::Local<v8::Value> value);

  bool isClosed() override { return !web_contents_; }
  void close() override { destroy(); }
  void destroy() override;
  v8::Local<v8::Object> getWrapper() override { return wrapper_.Get(isolate()); }
  int getId() const override { return id_; }
  atom::WebContents* getWebContents() const override { return web_contents_; }
  NativeWindowHandle getNativeWindowHandle() const override {
    return (__bridge void*)view_.window;
  }

  void onWebContentsDeleted(atom::WebContents* contents) override;
  void onWebContentsPaint(atom::WebContents*, const gfx::Rect&) override;
  void onWebContentsTextInputState(
      atom::WebContents*, const base::Value::Dict& state) override;
  bool supportsFramePreparation() const override { return view_ && view_.window; }
  uint64_t prepareWebContentsFrame(atom::WebContents*, uint64_t) override;

  void Draw(NSRect dirty_rect) override;
  void Resize() override;
  void RefreshVisibility() override;
  void Focus(bool focused) override;
  bool SendInput(base::Value::Dict event) override;
  bool SendNativeFileDrag(base::Value::Dict event,
                          const std::vector<std::string>& paths) override;
  bool CanPerformNativeFileDrop() const override;

  void AttachTo(NSView* parent);
  void Detach();

  static v8::Persistent<v8::Function> constructor;
  static gin_helper::WrapperInfo kWrapperInfo;

 private:
  static void New(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void GetWebContents(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SetBounds(const v8::FunctionCallbackInfo<v8::Value>& info);

  int id_ = 0;
  NSView* view_ = nil;
  atom::WebContents* web_contents_ = nullptr;
  v8::Global<v8::Object> wrapper_;
};

class MacBrowserWindow final : public mate::EventEmitter<MacBrowserWindow>,
                               public atom::WindowInterface,
                               public atom::WebContentsObserver,
                               public RendererSurfaceHost {
 public:
  MacBrowserWindow(v8::Isolate* isolate,
                   v8::Local<v8::Object> wrapper,
                   const gin_helper::Dictionary& options);
  ~MacBrowserWindow() override;

  static void Init(v8::Local<v8::Object> target);
  static void New(const v8::FunctionCallbackInfo<v8::Value>& info);

  bool isClosed() override { return closed_; }
  void close() override;
  void destroy() override { DestroyNative(); }
  v8::Local<v8::Object> getWrapper() override {
    return wrapper_.Get(isolate());
  }
  int getId() const override { return id_; }
  atom::WebContents* getWebContents() const override { return web_contents_; }
  NativeWindowHandle getNativeWindowHandle() const override {
    return (__bridge void*)window_;
  }
  bool handleNativeWindowCommand(
      const std::string&, const base::Value::Dict&) override;

  void onWebContentsDeleted(atom::WebContents* contents) override;
  void onWebContentsPaint(atom::WebContents*, const gfx::Rect&) override;
  void onWebContentsTextInputState(
      atom::WebContents*, const base::Value::Dict& state) override;
  bool supportsFramePreparation() const override { return true; }
  uint64_t prepareWebContentsFrame(atom::WebContents*, uint64_t) override;
  void onWebContentsReadyToShow(atom::WebContents*) override;
  void onWebContentsDraggableRegions(
      atom::WebContents*, const base::Value::List& regions) override;
  void Draw(NSRect dirty_rect) override;
  void Resize() override;
  void RefreshVisibility() override;
  void Focus(bool focused) override;
  bool SendInput(base::Value::Dict event) override;
  bool SendNativeFileDrag(base::Value::Dict event,
                          const std::vector<std::string>& paths) override;
  bool CanPerformNativeFileDrop() const override;
  bool HandleCaptionMouseDown(NSEvent* event, NSPoint point) override;

  void ForgetBrowserView(MacBrowserView* view);
  bool RequestClose();
  void DidBecomeKey();
  void DidResignKey();
  void DidResize();
  void LayoutWindowButtons();

  static v8::Persistent<v8::Function> constructor;
  static gin_helper::WrapperInfo kWrapperInfo;

 private:
  static MacBrowserWindow* From(const v8::FunctionCallbackInfo<v8::Value>& info) {
    return static_cast<MacBrowserWindow*>(
        gin_helper::WrappableBase::GetNativePtr(info.This(), &kWrapperInfo));
  }
  static void GetWebContents(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void GetNativeWindowHandle(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void Close(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void Destroy(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void Show(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ShowInactive(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SetIgnoreMouseEvents(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SetVisibleOnAllWorkspaces(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void Hide(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void FocusWindow(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void BlurWindow(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void IsFocused(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void IsVisible(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void IsDestroyed(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void Maximize(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void Unmaximize(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void IsMaximized(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void Minimize(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void Restore(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void IsMinimized(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SetFullScreen(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void IsFullScreen(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SetTitle(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void GetTitle(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void GetBounds(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SetBounds(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void GetNormalBounds(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SetPosition(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SetSize(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SetMinimumSize(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SetBackgroundColor(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SetTitleBarOverlay(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SetWindowButtonPosition(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void Center(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SetAlwaysOnTop(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void IsAlwaysOnTop(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SetDocumentEdited(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void IsDocumentEdited(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void GetAllWindows(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void GetFocusedWindow(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void FromWebContents(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void FromId(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void AddBrowserView(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void RemoveBrowserView(const v8::FunctionCallbackInfo<v8::Value>& info);

  void SetNativeWindowButtonPosition(NSPoint position);
  void DestroyNative();

  int id_ = 0;
  NSWindow* window_ = nil;
  NSView* renderer_view_ = nil;
  NSRect normal_bounds_ = NSZeroRect;
  NSObject<NSWindowDelegate>* delegate_ = nil;
  NSPoint window_button_position_ = NSZeroPoint;
  CGFloat window_button_spacing_ = 0;
  bool has_window_button_position_ = false;
  bool has_window_button_spacing_ = false;
  atom::WebContents* web_contents_ = nullptr;
  std::vector<MacBrowserView*> browser_views_;
  struct DraggableRegion {
    NSRect bounds;
    bool draggable;
  };
  std::vector<DraggableRegion> draggable_regions_;
  v8::Global<v8::Object> wrapper_;
  bool closed_ = false;
  bool destroying_ = false;
};

}  // namespace
}  // namespace mini_electron::electron

using mini_electron::electron::MacBrowserWindow;
using mini_electron::electron::RendererSurfaceHost;

// Full-size content and frameless windows route their interior border to the
// renderer. Handle that border before either web input or caption dragging.
enum WindowResizeEdge : NSUInteger {
  kResizeTop = 1,
  kResizeLeft = 2,
  kResizeBottom = 4,
  kResizeRight = 8,
};
constexpr CGFloat kWindowResizeBorder = 5;

static bool HasContentResizeBorder(NSWindow* window) {
  const NSWindowStyleMask style = window.styleMask;
  return (style & NSWindowStyleMaskResizable) &&
         !(style & NSWindowStyleMaskFullScreen) &&
         (!(style & NSWindowStyleMaskTitled) ||
          (style & NSWindowStyleMaskFullSizeContentView));
}

static NSUInteger WindowResizeEdges(NSWindow* window, NSPoint point) {
  if (!HasContentResizeBorder(window))
    return 0;
  const NSRect bounds = [window convertRectFromScreen:window.frame];
  if (!NSPointInRect(point, bounds))
    return 0;
  for (NSWindowButton type :
       {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
    NSButton* button = [window standardWindowButton:type];
    if (button && !button.hidden && button.superview &&
        NSPointInRect([button convertPoint:point fromView:nil], button.bounds))
      return 0;
  }
  NSUInteger edges = 0;
  if (point.x < NSMinX(bounds) + kWindowResizeBorder)
    edges |= kResizeLeft;
  else if (point.x >= NSMaxX(bounds) - kWindowResizeBorder)
    edges |= kResizeRight;
  if (point.y < NSMinY(bounds) + kWindowResizeBorder)
    edges |= kResizeBottom;
  else if (point.y >= NSMaxY(bounds) - kWindowResizeBorder)
    edges |= kResizeTop;
  return edges;
}

static NSCursor* WindowResizeCursor(NSUInteger edges) {
  if (@available(macOS 15.0, *)) {
    return [NSCursor
        frameResizeCursorFromPosition:
            static_cast<NSCursorFrameResizePosition>(edges)
        inDirections:NSCursorFrameResizeDirectionsAll];
  }
  return (edges & (kResizeLeft | kResizeRight))
             ? NSCursor.resizeLeftRightCursor
             : NSCursor.resizeUpDownCursor;
}

static bool IsPointerEvent(NSEventType type) {
  switch (type) {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeLeftMouseUp:
    case NSEventTypeRightMouseDown:
    case NSEventTypeRightMouseUp:
    case NSEventTypeMouseMoved:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeMouseEntered:
    case NSEventTypeMouseExited:
    case NSEventTypeCursorUpdate:
    case NSEventTypeScrollWheel:
    case NSEventTypeTabletPoint:
    case NSEventTypeTabletProximity:
    case NSEventTypeOtherMouseDown:
    case NSEventTypeOtherMouseUp:
    case NSEventTypeOtherMouseDragged:
    case NSEventTypeGesture:
    case NSEventTypeMagnify:
    case NSEventTypeSwipe:
    case NSEventTypeRotate:
    case NSEventTypeBeginGesture:
    case NSEventTypeEndGesture:
    case NSEventTypeSmartMagnify:
    case NSEventTypeQuickLook:
    case NSEventTypePressure:
    case NSEventTypeDirectTouch:
    case NSEventTypeChangeMode:
      return true;
    default:
      return false;
  }
}
@interface MiniElectronWindow : NSPanel
@property(nonatomic) BOOL electronFocusable;
- (void)setElectronIgnoreMouseEvents:(BOOL)ignore forward:(BOOL)forward;
@end

@implementation MiniElectronWindow {
  NSUInteger _resizeEdges;
  NSPoint _resizeStartPoint;
  NSRect _resizeStartFrame;
  id _localMouseMonitor;
  id _globalMouseMonitor;
}

- (BOOL)canBecomeKeyWindow { return self.electronFocusable; }
- (BOOL)canBecomeMainWindow { return self.electronFocusable; }

- (void)forwardMouseMovement:(NSEvent*)event {
  if (!self.visible || !self.ignoresMouseEvents)
    return;
  // Forward movement only: the WindowServer still routes clicks to the window
  // underneath. Include out-of-bounds movement so Blink can dispatch mouseleave.
  NSPoint screen = event.window
      ? [event.window convertPointToScreen:event.locationInWindow]
      : NSEvent.mouseLocation;
  NSPoint point = [self convertPointFromScreen:screen];
  NSEvent* movement = [NSEvent mouseEventWithType:NSEventTypeMouseMoved
      location:point modifierFlags:event.modifierFlags timestamp:event.timestamp
      windowNumber:self.windowNumber context:nil eventNumber:0 clickCount:0 pressure:0];
  [self.contentView.subviews.firstObject mouseMoved:movement];
}

- (void)setElectronIgnoreMouseEvents:(BOOL)ignore forward:(BOOL)forward {
  self.ignoresMouseEvents = ignore;
  if (_localMouseMonitor) {
    [NSEvent removeMonitor:_localMouseMonitor];
    _localMouseMonitor = nil;
  }
  if (_globalMouseMonitor) {
    [NSEvent removeMonitor:_globalMouseMonitor];
    _globalMouseMonitor = nil;
  }
  if (!ignore || !forward)
    return;
  const NSEventMask mask = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged |
      NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged;
  _localMouseMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
      handler:^NSEvent*(NSEvent* event) {
        if (event.window != self)
          [self forwardMouseMovement:event];
        // Events explicitly targeted at this window are forwarded by sendEvent:
        // below. The monitor covers movement delivered to another owned window.
        return event;
      }];
  _globalMouseMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:mask
      handler:^(NSEvent* event) { [self forwardMouseMovement:event]; }];
}


- (void)sendEvent:(NSEvent*)event {
  const NSEventType type = event.type;
  if (self.ignoresMouseEvents && IsPointerEvent(type)) {
    if ((type == NSEventTypeMouseMoved || type == NSEventTypeLeftMouseDragged ||
         type == NSEventTypeRightMouseDragged || type == NSEventTypeOtherMouseDragged) &&
        _localMouseMonitor)
      [self forwardMouseMovement:event];
    return;
  }
  if (_resizeEdges &&
      (type == NSEventTypeLeftMouseDragged || type == NSEventTypeLeftMouseUp)) {
    if (HasContentResizeBorder(self)) {
      const NSPoint point = [self convertPointToScreen:event.locationInWindow];
      const CGFloat dx = point.x - _resizeStartPoint.x;
      const CGFloat dy = point.y - _resizeStartPoint.y;
      NSRect frame = _resizeStartFrame;
      const NSSize minimum = self.minSize;
      const NSSize maximum = self.maxSize;
      if (_resizeEdges & (kResizeLeft | kResizeRight)) {
        frame.size.width = std::clamp(
            frame.size.width + ((_resizeEdges & kResizeLeft) ? -dx : dx),
            std::max<CGFloat>(1, minimum.width),
            std::max<CGFloat>(std::max<CGFloat>(1, minimum.width), maximum.width));
        if (_resizeEdges & kResizeLeft)
          frame.origin.x = NSMaxX(_resizeStartFrame) - frame.size.width;
      }
      if (_resizeEdges & (kResizeTop | kResizeBottom)) {
        frame.size.height = std::clamp(
            frame.size.height + ((_resizeEdges & kResizeBottom) ? -dy : dy),
            std::max<CGFloat>(1, minimum.height),
            std::max<CGFloat>(std::max<CGFloat>(1, minimum.height), maximum.height));
        if (_resizeEdges & kResizeBottom)
          frame.origin.y = NSMaxY(_resizeStartFrame) - frame.size.height;
      }
      // AppKit still owns layout, renderer viewport updates and resize events.
      if (!NSEqualRects(frame, self.frame))
        [self setFrame:frame display:YES];
    }
    if (type == NSEventTypeLeftMouseUp) {
      _resizeEdges = 0;
      [NSCursor.arrowCursor set];
    }
    return;
  }
  if (type == NSEventTypeLeftMouseDown) {
    _resizeEdges = WindowResizeEdges(self, event.locationInWindow);
    if (_resizeEdges) {
      _resizeStartFrame = self.frame;
      _resizeStartPoint = [self convertPointToScreen:event.locationInWindow];
      [self makeKeyAndOrderFront:nil];
      [WindowResizeCursor(_resizeEdges) set];
      return;
    }
  }
  [super sendEvent:event];
  if (type == NSEventTypeMouseMoved) {
    const NSUInteger edges = WindowResizeEdges(self, event.locationInWindow);
    if (edges)
      [WindowResizeCursor(edges) set];
  }
}
@end

@interface MiniElectronRendererView : NSView <NSTextInputClient>
- (instancetype)initWithFrame:(NSRect)frame owner:(RendererSurfaceHost*)owner;
- (void)cancelNativeDrag;
- (void)updateTextInputState:(const base::Value::Dict&)state;
@end

@interface MiniElectronWindowDelegate : NSObject <NSWindowDelegate>
- (instancetype)initWithOwner:(MacBrowserWindow*)owner;
@end

@implementation MiniElectronRendererView {
  RendererSurfaceHost* _owner;
  NSRange _textSelection;
  NSRange _textMarkedRange;
  NSRect _textSelectionRect;
  NSRect _textMarkedRect;
  BOOL _textInputEditable;
  BOOL _composing;
  uint64_t _imeSerial;
  uint64_t _pendingImeSerial;
  NSEvent* _interpretingKey;
  BOOL _nativeDragActive;
}

- (instancetype)initWithFrame:(NSRect)frame owner:(RendererSurfaceHost*)owner {
  self = [super initWithFrame:frame];
  if (self) {
    _owner = owner;
    _textSelection = NSMakeRange(NSNotFound, 0);
    _textMarkedRange = NSMakeRange(NSNotFound, 0);
    self.wantsLayer = YES;
    [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
  }
  return self;
}
- (void)dealloc {
  [self unregisterDraggedTypes];
  [[NSNotificationCenter defaultCenter] removeObserver:self];
#if !__has_feature(objc_arc)
  [super dealloc];
#endif
}

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return self.window ? self.window.opaque : YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
  return !self.window.canBecomeKeyWindow || [super acceptsFirstMouse:event];
}
- (void)viewDidMoveToWindow {
  [super viewDidMoveToWindow];
  NSNotificationCenter* notifications = NSNotificationCenter.defaultCenter;
  [notifications removeObserver:self];
  _owner->ResetOcclusionObservation();
  if (self.window) {
    for (NSNotificationName name in @[
        NSWindowDidMoveNotification, NSWindowDidChangeBackingPropertiesNotification,
        NSWindowDidChangeScreenNotification, NSWindowDidBecomeKeyNotification,
        NSWindowDidResignKeyNotification ]) {
      [notifications addObserver:self selector:@selector(textInputWindowChanged:)
                           name:name object:self.window];
    }
    [notifications addObserver:self selector:@selector(textInputWindowChanged:)
                         name:NSApplicationDidChangeScreenParametersNotification
                       object:nil];
    for (NSNotificationName name in @[
        NSWindowDidChangeOcclusionStateNotification,
        NSWindowDidMiniaturizeNotification,
        NSWindowDidDeminiaturizeNotification ]) {
      [notifications addObserver:self selector:@selector(rendererVisibilityChanged:)
                           name:name object:self.window];
    }
    for (NSNotificationName name in @[
        NSApplicationDidHideNotification, NSApplicationDidUnhideNotification ]) {
      [notifications addObserver:self selector:@selector(rendererVisibilityChanged:)
                           name:name object:NSApp];
    }
    _owner->Resize();
  } else {
    [self updateTextInputState:base::Value::Dict()];
  }
  // A custom NSView is not automatically selected like an NSTextView. Seed
  // the responder chain on attachment, without stealing an existing child
  // renderer's focus or activating a nonfocusable companion panel.
  NSWindow* window = self.window;
  if (window.canBecomeKeyWindow &&
      (!window.firstResponder || window.firstResponder == window))
    [window makeFirstResponder:self];
  _owner->RefreshVisibility();
}
- (void)rendererVisibilityChanged:(NSNotification*)notification {
  HostState& state = State();
  if (!_owner || !CanEnterJavaScript(state) || state.exiting)
    return;
  NativeCallbackScope scope(state);
  _owner->RefreshVisibility();
  (void)notification;
}
- (void)viewDidHide {
  [super viewDidHide];
  [self rendererVisibilityChanged:nil];
}
- (void)viewDidUnhide {
  [super viewDidUnhide];
  [self rendererVisibilityChanged:nil];
}
- (void)textInputWindowChanged:(NSNotification*)notification {
  if ([notification.name isEqual:NSWindowDidChangeBackingPropertiesNotification] ||
      [notification.name isEqual:NSWindowDidChangeScreenNotification] ||
      [notification.name isEqual:NSApplicationDidChangeScreenParametersNotification])
    _owner->Resize();
  if ([notification.name isEqual:NSWindowDidResignKeyNotification]) {
    [self updateTextInputState:base::Value::Dict()];
    _owner->Focus(false);
  } else if ([notification.name isEqual:NSWindowDidBecomeKeyNotification]) {
    _owner->Focus(self.window.firstResponder == self);
  }
  [self.inputContext invalidateCharacterCoordinates];
}
- (void)drawRect:(NSRect)dirtyRect { _owner->Draw(dirtyRect); }
- (void)setFrameSize:(NSSize)newSize {
  [super setFrameSize:newSize];
  _owner->Resize();
  [self.inputContext invalidateCharacterCoordinates];
}
- (void)resetCursorRects {
  [super resetCursorRects];
  if (!HasContentResizeBorder(self.window))
    return;
  const NSRect frame = [self.window convertRectFromScreen:self.window.frame];
  const CGFloat border = kWindowResizeBorder;
  // Partition the border so corner cursors never compete with edge cursors.
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      if (row == 1 && column == 1)
        continue;
      const NSRect rect = NSMakeRect(
          column == 0 ? NSMinX(frame) :
              column == 1 ? NSMinX(frame) + border : NSMaxX(frame) - border,
          row == 0 ? NSMinY(frame) :
              row == 1 ? NSMinY(frame) + border : NSMaxY(frame) - border,
          column == 1 ? std::max<CGFloat>(0, frame.size.width - 2 * border) : border,
          row == 1 ? std::max<CGFloat>(0, frame.size.height - 2 * border) : border);
      const NSUInteger edges = WindowResizeEdges(
          self.window, NSMakePoint(NSMidX(rect), NSMidY(rect)));
      if (edges) {
        const NSRect visible = NSIntersectionRect(
            [self convertRect:rect fromView:nil], self.visibleRect);
        if (!NSIsEmptyRect(visible))
          [self addCursorRect:visible cursor:WindowResizeCursor(edges)];
      }
    }
  }
}
- (BOOL)becomeFirstResponder { _owner->Focus(true); return YES; }
- (BOOL)resignFirstResponder {
  if (_composing)
    [self unmarkText];
  [self updateTextInputState:base::Value::Dict()];
  _owner->Focus(false);
  return YES;
}

- (int)inputModifiers:(NSEvent*)event {
  NSEventModifierFlags flags = event.modifierFlags;
  int result = 0;
  if (flags & NSEventModifierFlagShift) result |= MINI_ELECTRON_SHIFT;
  if (flags & NSEventModifierFlagControl) result |= MINI_ELECTRON_CONTROL;
  if (flags & NSEventModifierFlagCommand) result |= MINI_ELECTRON_META;
  if (flags & NSEventModifierFlagOption) result |= MINI_ELECTRON_ALT;
  if ((event.type == NSEventTypeKeyDown || event.type == NSEventTypeKeyUp) && event.isARepeat)
    result |= MINI_ELECTRON_REPEAT;
  return result;
}

- (int)mouseModifiers:(NSEvent*)event {
  int result = [self inputModifiers:event];
  const NSUInteger buttons = [NSEvent pressedMouseButtons];
  if (buttons & 1) result |= MINI_ELECTRON_LBUTTON;
  if (buttons & 2) result |= MINI_ELECTRON_RBUTTON;
  if (buttons & 4) result |= MINI_ELECTRON_MBUTTON;
  // The global state may already reflect a later queued event. Preserve the
  // transition carried by this event, including synthetic AppKit events.
  const NSEventType type = event.type;
  if (type == NSEventTypeLeftMouseDown || type == NSEventTypeLeftMouseDragged)
    result |= MINI_ELECTRON_LBUTTON;
  else if (type == NSEventTypeLeftMouseUp)
    result &= ~MINI_ELECTRON_LBUTTON;
  if (type == NSEventTypeRightMouseDown || type == NSEventTypeRightMouseDragged)
    result |= MINI_ELECTRON_RBUTTON;
  else if (type == NSEventTypeRightMouseUp)
    result &= ~MINI_ELECTRON_RBUTTON;
  if (type == NSEventTypeOtherMouseDown || type == NSEventTypeOtherMouseDragged)
    result |= MINI_ELECTRON_MBUTTON;
  else if (type == NSEventTypeOtherMouseUp)
    result &= ~MINI_ELECTRON_MBUTTON;
  return result;
}

- (BOOL)readDraggedPaths:(id<NSDraggingInfo>)sender
                    into:(std::vector<std::string>*)paths {
  if (!paths)
    return NO;
  NSPasteboard* pasteboard = sender.draggingPasteboard;
  NSArray<NSURL*>* urls = [pasteboard
      readObjectsForClasses:@[ [NSURL class] ]
                     options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
  if (urls.count == 0 || urls.count > 1024)
    return NO;
  paths->reserve(urls.count);
  for (NSURL* url in urls) {
    if (!url.isFileURL)
      return NO;
    const char* path = url.fileSystemRepresentation;
    if (!path)
      return NO;
    const size_t length = std::strlen(path);
    if (length == 0 || length > 4096)
      return NO;
    paths->emplace_back(path, length);
  }
  return !paths->empty();
}

- (base::Value::Dict)nativeDragEvent:(id<NSDraggingInfo>)sender
                                type:(const char*)type {
  const NSPoint window_point = sender.draggingLocation;
  const NSPoint point = [self convertPoint:window_point fromView:nil];
  const NSPoint screen_point = [self.window convertPointToScreen:window_point];
  base::Value::Dict input;
  input.Set("type", type);
  input.Set("x", static_cast<int>(std::lround(point.x)));
  input.Set("y", static_cast<int>(std::lround(point.y)));
  input.Set("screenX", static_cast<int>(std::lround(screen_point.x)));
  input.Set("screenY", static_cast<int>(
      std::lround(mini_electron::electron::DesktopTop() - screen_point.y)));
  input.Set("modifiers", [self inputModifiers:NSApp.currentEvent]);
  return input;
}

- (void)cancelNativeDrag {
  if (!_nativeDragActive)
    return;
  base::Value::Dict event;
  event.Set("type", "dragLeave");
  event.Set("x", 0);
  event.Set("y", 0);
  event.Set("screenX", 0);
  event.Set("screenY", 0);
  event.Set("modifiers", 0);
  _owner->SendNativeFileDrag(std::move(event), {});
  _nativeDragActive = NO;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
  std::vector<std::string> paths;
  if (![self readDraggedPaths:sender into:&paths])
    return NSDragOperationNone;
  _nativeDragActive = _owner->SendNativeFileDrag(
      [self nativeDragEvent:sender type:"dragEnter"], paths);
  return _nativeDragActive ? NSDragOperationCopy : NSDragOperationNone;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
  if (!_nativeDragActive)
    return NSDragOperationNone;
  return _owner->SendNativeFileDrag(
             [self nativeDragEvent:sender type:"dragOver"], {})
      ? NSDragOperationCopy
      : NSDragOperationNone;
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
  if (_nativeDragActive) {
    _owner->SendNativeFileDrag(
        [self nativeDragEvent:sender type:"dragLeave"], {});
  }
  _nativeDragActive = NO;
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender {
  (void)sender;
  if (!_nativeDragActive || !_owner->CanPerformNativeFileDrop()) {
    [self cancelNativeDrag];
    return NO;
  }
  return YES;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
  if (!_nativeDragActive)
    return NO;
  const bool sent = _owner->SendNativeFileDrag(
      [self nativeDragEvent:sender type:"drop"], {});
  _nativeDragActive = NO;
  return sent;
}

- (void)sendMouse:(NSEvent*)event type:(const char*)type button:(const char*)button {
  NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
  base::Value::Dict input;
  input.Set("type", type);
  input.Set("x", static_cast<int>(std::lround(point.x)));
  input.Set("y", static_cast<int>(std::lround(point.y)));
  input.Set("button", button);
  input.Set("modifiers", [self mouseModifiers:event]);
  _owner->SendInput(std::move(input));
}

- (void)mouseMoved:(NSEvent*)event { [self sendMouse:event type:"mouseMove" button:"none"]; }
- (void)mouseDragged:(NSEvent*)event { [self sendMouse:event type:"mouseMove" button:"left"]; }
- (void)rightMouseDragged:(NSEvent*)event { [self sendMouse:event type:"mouseMove" button:"right"]; }
- (void)otherMouseDragged:(NSEvent*)event { [self sendMouse:event type:"mouseMove" button:"middle"]; }
- (void)mouseDown:(NSEvent*)event {
  [self.window makeFirstResponder:self];
  NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
  if (_owner->HandleCaptionMouseDown(event, point))
    return;
  [self sendMouse:event type:"mouseDown" button:"left"];
}
- (void)mouseUp:(NSEvent*)event { [self sendMouse:event type:"mouseUp" button:"left"]; }
- (void)rightMouseDown:(NSEvent*)event {
  [self.window makeFirstResponder:self];
  [self sendMouse:event type:"mouseDown" button:"right"];
}
- (void)rightMouseUp:(NSEvent*)event { [self sendMouse:event type:"mouseUp" button:"right"]; }
- (void)otherMouseDown:(NSEvent*)event {
  [self.window makeFirstResponder:self];
  [self sendMouse:event type:"mouseDown" button:"middle"];
}
- (void)otherMouseUp:(NSEvent*)event { [self sendMouse:event type:"mouseUp" button:"middle"]; }
- (void)scrollWheel:(NSEvent*)event {
  NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
  base::Value::Dict input;
  input.Set("type", "mouseWheel");
  input.Set("x", static_cast<int>(std::lround(point.x)));
  input.Set("y", static_cast<int>(std::lround(point.y)));
  if (event.hasPreciseScrollingDeltas) {
    // Trackpads already report logical pixels; do not turn them into wheel ticks.
    input.Set("precise", true);
    input.Set("deltaX", event.scrollingDeltaX);
    input.Set("deltaY", event.scrollingDeltaY);
  } else {
    input.Set("deltaY", event.scrollingDeltaY * 120.0);
  }
  input.Set("modifiers", [self mouseModifiers:event]);
  _owner->SendInput(std::move(input));
}
- (void)sendKey:(NSEvent*)event type:(const char*)type {
  base::Value::Dict input;
  input.Set("type", type);
  input.Set("keyCode", static_cast<int>(ui::KeyboardCodeFromNSEvent(event)));
  input.Set("nativeKeyCode", static_cast<int>(event.keyCode));
  input.Set("domCode", static_cast<int>(ui::DomCodeFromNSEvent(event)));
  input.Set("domKey", static_cast<int>(static_cast<uint32_t>(ui::DomKeyFromNSEvent(event))));
  input.Set("modifiers", [self inputModifiers:event]);
  input.Set("text", event.characters.UTF8String ?: "");
  input.Set("unmodifiedText", event.charactersIgnoringModifiers.UTF8String ?: "");
  input.Set("systemKey", false);
  _owner->SendInput(std::move(input));
}
- (void)keyDown:(NSEvent*)event {
  [self sendKey:event type:"keyDown"];
  if (!(event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl))) {
    _interpretingKey = event;
    [self interpretKeyEvents:@[ event ]];
    _interpretingKey = nil;
  }
}
- (void)keyUp:(NSEvent*)event { [self sendKey:event type:"keyUp"]; }
- (void)executeEditCommand:(const char*)command {
  base::Value::Dict input;
  input.Set("type", "editCommand");
  input.Set("command", command);
  _owner->SendInput(std::move(input));
}
- (void)undo:(id)sender { [self executeEditCommand:"Undo"]; }
- (void)redo:(id)sender { [self executeEditCommand:"Redo"]; }
- (void)cut:(id)sender { [self executeEditCommand:"Cut"]; }
- (void)copy:(id)sender { [self executeEditCommand:"Copy"]; }
- (void)paste:(id)sender { [self executeEditCommand:"Paste"]; }
- (void)selectAll:(id)sender { [self executeEditCommand:"SelectAll"]; }
- (BOOL)performKeyEquivalent:(NSEvent*)event {
  NSMenu* menu = [NSApplication sharedApplication].mainMenu;
  if ([menu performKeyEquivalent:event]) return YES;
  if ((event.modifierFlags & NSEventModifierFlagCommand) &&
      (event.modifierFlags & NSEventModifierFlagShift) && event.keyCode == 24) {
    NSEvent* alias = [NSEvent keyEventWithType:event.type location:event.locationInWindow
        modifierFlags:event.modifierFlags & ~NSEventModifierFlagShift
        timestamp:event.timestamp windowNumber:event.windowNumber context:nil
        characters:@"=" charactersIgnoringModifiers:@"=" isARepeat:event.isARepeat
        keyCode:event.keyCode];
    if ([menu performKeyEquivalent:alias]) return YES;
  }
  if (event.modifierFlags & NSEventModifierFlagCommand) {
    [self sendKey:event type:"keyDown"];
    return YES;
  }
  return [super performKeyEquivalent:event];
}


- (BOOL)hasNativeTextFocus {
  return self.window.isKeyWindow && self.window.firstResponder == self;
}

- (void)updateTextInputState:(const base::Value::Dict&)state {
  const bool editable = state.FindBool("editable").value_or(false);
  uint64_t serial = 0;
  if (const std::string* value = state.FindString("imeSerial"))
    base::StringToUint64(*value, &serial);
  // Native composition calls may outpace IPC delivery. An older renderer
  // snapshot must not undo a newer marked/selected range while it is in flight.
  if (editable && serial < _pendingImeSerial)
    return;
  const double scale = state.FindDouble("scale").value_or(0);
  auto read_rect = [scale](const base::Value::Dict* rect) {
    if (!rect || !std::isfinite(scale) || scale <= 0)
      return NSZeroRect;
    return NSMakeRect(rect->FindInt("x").value_or(0) / scale,
        rect->FindInt("y").value_or(0) / scale,
        rect->FindInt("width").value_or(0) / scale,
        rect->FindInt("height").value_or(0) / scale);
  };
  const int start = state.FindInt("selectionStart").value_or(-1);
  const int length = state.FindInt("selectionLength").value_or(0);
  const NSRect rect = read_rect(state.FindDict("selectionRect"));
  _textInputEditable = editable && [self hasNativeTextFocus]
      && start >= 0 && length >= 0 && rect.size.height > 0;
  _pendingImeSerial = 0;
  if (!_textInputEditable) {
    _textSelection = NSMakeRange(NSNotFound, 0);
    _textMarkedRange = NSMakeRange(NSNotFound, 0);
    _textSelectionRect = NSZeroRect;
    _textMarkedRect = NSZeroRect;
    _composing = NO;
    [self.inputContext discardMarkedText];
  } else {
    _textSelection = NSMakeRange(start, length);
    _textSelectionRect = rect;
    const int marked_start = state.FindInt("markedStart").value_or(-1);
    const int marked_length = state.FindInt("markedLength").value_or(0);
    _textMarkedRange = marked_start >= 0 && marked_length > 0
        ? NSMakeRange(marked_start, marked_length) : NSMakeRange(NSNotFound, 0);
    _textMarkedRect = read_rect(state.FindDict("markedRect"));
    _composing = _textMarkedRange.location != NSNotFound;
  }
  [self.inputContext invalidateCharacterCoordinates];
}

- (BOOL)sendImeInput:(base::Value::Dict)input replacementRange:(NSRange)range {
  const NSUInteger limit = std::numeric_limits<int>::max();
  if (range.location != NSNotFound) {
    if (range.location > limit || range.length > limit - range.location)
      return NO;
    input.Set("replacementStart", static_cast<int>(range.location));
    input.Set("replacementLength", static_cast<int>(range.length));
  }
  _pendingImeSerial = ++_imeSerial;
  input.Set("imeSerial", base::NumberToString(_pendingImeSerial));
  if (_owner->SendInput(std::move(input)))
    return YES;
  _pendingImeSerial = 0;
  return NO;
}

- (void)insertText:(id)value replacementRange:(NSRange)replacementRange {
  const BOOL was_composing = _composing;
  NSString* text = [value isKindOfClass:[NSAttributedString class]]
      ? [value string] : value;
  if (was_composing || replacementRange.location != NSNotFound || !_interpretingKey) {
    base::Value::Dict input;
    input.Set("type", "imeCommitText");
    input.Set("text", text.UTF8String ?: "");
    if (![self sendImeInput:std::move(input) replacementRange:replacementRange])
      return;
    const NSRange replaced = replacementRange.location != NSNotFound
        ? replacementRange : was_composing ? _textMarkedRange : _textSelection;
    if (replaced.location != NSNotFound)
      _textSelection = NSMakeRange(replaced.location + text.length, 0);
    _textMarkedRange = NSMakeRange(NSNotFound, 0);
    _composing = NO;
    return;
  }
  for (NSUInteger index = 0; index < text.length; ++index) {
    base::Value::Dict input;
    input.Set("type", "char");
    input.Set("charCode", static_cast<int>([text characterAtIndex:index]));
    if (_interpretingKey) {
      input.Set("modifiers", [self inputModifiers:_interpretingKey]);
      input.Set("nativeKeyCode", static_cast<int>(_interpretingKey.keyCode));
      input.Set("domCode", static_cast<int>(ui::DomCodeFromNSEvent(_interpretingKey)));
      input.Set("domKey", static_cast<int>(static_cast<uint32_t>(ui::DomKeyFromNSEvent(_interpretingKey))));
      input.Set("text", std::string([[text substringWithRange:NSMakeRange(index, 1)] UTF8String] ?: ""));
      input.Set("unmodifiedText", _interpretingKey.charactersIgnoringModifiers.UTF8String ?: "");
    }
    _owner->SendInput(std::move(input));
  }
}
- (void)insertText:(id)value { [self insertText:value replacementRange:NSMakeRange(NSNotFound, 0)]; }
- (void)setMarkedText:(id)value selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
  NSString* text = [value isKindOfClass:[NSAttributedString class]]
      ? [value string] : value;
  const NSUInteger start = std::min(selectedRange.location, text.length);
  const NSUInteger length = std::min(selectedRange.length, text.length - start);
  if (text.length > static_cast<NSUInteger>(std::numeric_limits<int>::max()))
    return;
  base::Value::Dict input;
  input.Set("type", "imeSetComposition");
  input.Set("text", text.UTF8String ?: "");
  input.Set("selectionStart", static_cast<int>(start));
  input.Set("selectionLength", static_cast<int>(length));
  if (![self sendImeInput:std::move(input) replacementRange:replacementRange])
    return;
  const NSRange replaced = replacementRange.location != NSNotFound
      ? replacementRange : _composing ? _textMarkedRange : _textSelection;
  if (replaced.location != NSNotFound) {
    _textMarkedRange = text.length ? NSMakeRange(replaced.location, text.length)
                                 : NSMakeRange(NSNotFound, 0);
    _textSelection = NSMakeRange(replaced.location + start, length);
  }
  _composing = text.length != 0;
}
- (void)unmarkText {
  if (_composing) {
    base::Value::Dict input;
    input.Set("type", "imeFinishComposition");
    [self sendImeInput:std::move(input) replacementRange:NSMakeRange(NSNotFound, 0)];
  }
  _textMarkedRange = NSMakeRange(NSNotFound, 0);
  _composing = NO;
}
- (BOOL)hasMarkedText { return [self markedRange].location != NSNotFound; }
- (NSRange)markedRange {
  return [self hasNativeTextFocus] ? _textMarkedRange : NSMakeRange(NSNotFound, 0);
}
- (NSRange)selectedRange {
  return _textInputEditable && [self hasNativeTextFocus]
      ? _textSelection : NSMakeRange(NSNotFound, 0);
}
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText { return @[]; }
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
  if (actualRange) *actualRange = NSMakeRange(NSNotFound, 0);
  return nil;
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point { return NSNotFound; }
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
  if (!_textInputEditable || ![self hasNativeTextFocus]) {
    if (actualRange) *actualRange = NSMakeRange(NSNotFound, 0);
    return NSZeroRect;
  }
  NSRect rect = _textSelectionRect;
  NSRange represented = _textSelection;
  if (_textMarkedRange.location != NSNotFound
      && NSEqualRanges(range, _textMarkedRange) && _textMarkedRect.size.height > 0) {
    rect = _textMarkedRect;
    represented = _textMarkedRange;
  }
  if (actualRange) *actualRange = represented;
  // The view is flipped (Blink's top-left origin). AppKit performs both the
  // flipped view/window conversion and the bottom-left screen conversion.
  // Resolve window placement at query time so moving a window never stales IPC.
  return [self.window convertRectToScreen:[self convertRect:rect toView:nil]];
}
- (void)doCommandBySelector:(SEL)selector { (void)selector; }
@end

@implementation MiniElectronWindowDelegate {
  MacBrowserWindow* _owner;
}
- (instancetype)initWithOwner:(MacBrowserWindow*)owner {
  self = [super init];
  if (self) _owner = owner;
  return self;
}
- (BOOL)windowShouldClose:(NSWindow*)sender {
  HostState& state = State();
  if (!CanEnterJavaScript(state))
    return YES;
  NativeCallbackScope scope(state);
  return _owner->RequestClose();
}
- (void)windowDidBecomeKey:(NSNotification*)notification {
  HostState& state = State();
  if (!CanEnterJavaScript(state))
    return;
  NativeCallbackScope scope(state);
  _owner->DidBecomeKey();
}
- (void)windowDidResignKey:(NSNotification*)notification {
  HostState& state = State();
  if (!CanEnterJavaScript(state))
    return;
  NativeCallbackScope scope(state);
  _owner->DidResignKey();
}
- (void)windowDidResize:(NSNotification*)notification {
  HostState& state = State();
  if (!CanEnterJavaScript(state))
    return;
  NativeCallbackScope scope(state);
  _owner->DidResize();
}
- (void)windowDidUpdate:(NSNotification*)notification {
  _owner->LayoutWindowButtons();
}
- (void)windowDidEnterFullScreen:(NSNotification*)notification {
  if (auto* contents = _owner->getWebContents())
    atom::WebContents::sendRendererMessage(contents->getIdApi(),
        "fullscreen-state", base::Value::Dict().Set("enabled", true));
}
- (void)windowDidExitFullScreen:(NSNotification*)notification {
  if (auto* contents = _owner->getWebContents())
    atom::WebContents::sendRendererMessage(contents->getIdApi(),
        "fullscreen-state", base::Value::Dict().Set("enabled", false));
}
@end

namespace mini_electron::electron {
namespace {

v8::Persistent<v8::Function> MacBrowserWindow::constructor;
gin_helper::WrapperInfo MacBrowserWindow::kWrapperInfo = {
    gin_helper::GinEmbedder::kEmbedderNativeGin};

NSColor* ParseBackgroundColor(const std::string& color) {
  unsigned rgb = 0;
  if (color.size() != 7 || color[0] != '#' ||
      std::string_view(color).substr(1).find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos ||
      !base::HexStringToUInt(std::string_view(color).substr(1), &rgb))
    return nil;
  return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xff) / 255.0
                           green:((rgb >> 8) & 0xff) / 255.0
                            blue:(rgb & 0xff) / 255.0
                           alpha:1.0];
}

void MacBrowserWindow::SetNativeWindowButtonPosition(NSPoint position) {
  window_button_position_ = position;
  has_window_button_position_ = true;
  LayoutWindowButtons();
}

void MacBrowserWindow::LayoutWindowButtons() {
  if (!window_ || !has_window_button_position_ ||
      (window_.styleMask & NSWindowStyleMaskFullScreen) != 0) {
    return;
  }

  NSButton* close_button =
      [window_ standardWindowButton:NSWindowCloseButton];
  NSButton* minimize_button =
      [window_ standardWindowButton:NSWindowMiniaturizeButton];
  NSButton* zoom_button =
      [window_ standardWindowButton:NSWindowZoomButton];
  if (!close_button || !minimize_button || !zoom_button)
    return;

  if (!has_window_button_spacing_) {
    window_button_spacing_ =
        std::max<CGFloat>(0, NSMinX(minimize_button.frame) -
                                NSMaxX(close_button.frame));
    has_window_button_spacing_ = true;
  }

  // The standard buttons remain children of AppKit's title-bar hierarchy.
  // Grow that hierarchy with the requested top margin so its hit testing still
  // contains buttons moved into a full-size content view.
  NSView* title_bar_container = close_button.superview.superview;
  if (!title_bar_container)
    return;
  const CGFloat button_height = NSHeight(close_button.frame);
  const CGFloat container_height =
      std::max(button_height,
               button_height + 2 * window_button_position_.y);
  NSRect container_frame = title_bar_container.frame;
  container_frame.size.height = container_height;
  container_frame.origin.y = NSHeight(window_.frame) - container_height;
  if (!NSEqualRects(title_bar_container.frame, container_frame))
    title_bar_container.frame = container_frame;

  CGFloat x = window_button_position_.x;
  for (NSButton* button :
       {close_button, minimize_button, zoom_button}) {
    NSRect frame = button.frame;
    frame.origin = NSMakePoint(x, (container_height - NSHeight(frame)) / 2);
    if (!NSEqualRects(button.frame, frame))
      button.frame = frame;
    x += NSWidth(frame) + window_button_spacing_;
  }
}


MacBrowserWindow::MacBrowserWindow(v8::Isolate* isolate,
                                   v8::Local<v8::Object> wrapper,
                                   const gin_helper::Dictionary& options) {
  gin_helper::Wrappable<MacBrowserWindow>::InitWith(isolate, wrapper);
  wrapper_.Reset(isolate, wrapper);
  id_ = State().next_window_id++;
  SetValue(isolate->GetCurrentContext(), wrapper, "id",
           v8::Integer::New(isolate, id_));

  int width = 800;
  int height = 600;
  int x = 0;
  int y = 0;
  bool show = true;
  bool resizable = true;
  bool minimizable = true;
  bool closable = true;
  bool frame = true;
  bool transparent = false;
  bool focusable = true;
  bool always_on_top = false;
  bool has_shadow = true;
  std::string title = State().name;
  std::string title_bar_style;
  std::string background_color;
  options.GetBydefaultVal("width", width, &width);
  options.GetBydefaultVal("height", height, &height);
  bool has_position = options.Get("x", &x) && options.Get("y", &y);
  options.GetBydefaultVal("show", show, &show);
  options.GetBydefaultVal("resizable", resizable, &resizable);
  options.GetBydefaultVal("minimizable", minimizable, &minimizable);
  options.GetBydefaultVal("closable", closable, &closable);
  options.GetBydefaultVal("frame", frame, &frame);
  options.GetBydefaultVal("transparent", transparent, &transparent);
  options.GetBydefaultVal("focusable", focusable, &focusable);
  options.GetBydefaultVal("alwaysOnTop", always_on_top, &always_on_top);
  options.GetBydefaultVal("hasShadow", has_shadow, &has_shadow);
  options.GetBydefaultVal("title", title, &title);
  options.GetBydefaultVal("titleBarStyle", "", &title_bar_style);
  options.Get("backgroundColor", &background_color);
  NSColor* initial_background = transparent ? NSColor.clearColor
      : (ParseBackgroundColor(background_color) ?: NSColor.whiteColor);

  NSWindowStyleMask style = frame ? NSWindowStyleMaskTitled : NSWindowStyleMaskBorderless;
  if (closable) style |= NSWindowStyleMaskClosable;
  if (minimizable) style |= NSWindowStyleMaskMiniaturizable;
  if (resizable) style |= NSWindowStyleMaskResizable;
  if (!focusable) style |= NSWindowStyleMaskNonactivatingPanel;
  if (title_bar_style == "hidden" ||
      title_bar_style == "hiddenInset" ||
      title_bar_style == "customButtonsOnHover") {
    style |= NSWindowStyleMaskFullSizeContentView;
  }
  NSRect bounds = CocoaRectFromElectron(
      NSMakeRect(x, y, std::max(1, width), std::max(1, height)));
  bounds = [NSWindow contentRectForFrameRect:bounds styleMask:style];
  window_ = [[MiniElectronWindow alloc] initWithContentRect:bounds
                                        styleMask:style
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  ((MiniElectronWindow*)window_).electronFocusable = focusable;
  ((NSPanel*)window_).hidesOnDeactivate = NO;
  window_.hasShadow = has_shadow;
  window_.level = always_on_top ? NSFloatingWindowLevel : NSNormalWindowLevel;
  window_.title = NSStringFromUtf8(title);
  window_.opaque = !transparent;
  window_.backgroundColor = initial_background;
  int min_width = 0;
  int min_height = 0;
  int max_width = 0;
  int max_height = 0;
  options.Get("minWidth", &min_width);
  options.Get("minHeight", &min_height);
  options.Get("maxWidth", &max_width);
  options.Get("maxHeight", &max_height);
  if (min_width > 0 || min_height > 0) {
    window_.minSize = NSMakeSize(std::max(1, min_width),
                                 std::max(1, min_height));
  }
  if (max_width > 0 || max_height > 0) {
    window_.maxSize = NSMakeSize(
        max_width > 0 ? std::max<CGFloat>(window_.minSize.width, max_width)
                      : window_.maxSize.width,
        max_height > 0 ? std::max<CGFloat>(window_.minSize.height, max_height)
                       : window_.maxSize.height);
  }
  if (!has_position)
    [window_ center];
  if (!title_bar_style.empty()) {
    window_.titleVisibility = NSWindowTitleHidden;
    window_.titlebarAppearsTransparent = YES;
  }
  gin_helper::Dictionary traffic_lights =
      gin_helper::Dictionary::CreateEmpty(isolate);
  if (options.Get("trafficLightPosition", &traffic_lights)) {
    double light_x = 0;
    double light_y = 0;
    if (traffic_lights.Get("x", &light_x) &&
        traffic_lights.Get("y", &light_y) &&
        std::isfinite(light_x) && std::isfinite(light_y)) {
      SetNativeWindowButtonPosition(NSMakePoint(light_x, light_y));
    }
  }
  normal_bounds_ = window_.frame;
  window_.releasedWhenClosed = NO;
  delegate_ = [[MiniElectronWindowDelegate alloc] initWithOwner:this];
  window_.delegate = delegate_;
  window_.acceptsMouseMovedEvents = YES;

  renderer_view_ = [[MiniElectronRendererView alloc]
      initWithFrame:window_.contentView.bounds owner:this];
  renderer_view_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  renderer_view_.layer.backgroundColor = initial_background.CGColor;
  [window_.contentView addSubview:renderer_view_];

  gin_helper::Dictionary web_preferences =
      gin_helper::Dictionary::CreateEmpty(isolate);
  options.Get("webPreferences", &web_preferences);
  web_preferences.Set("transparent", transparent);
  web_preferences.Set("backgroundColor", background_color);
  web_contents_ = atom::WebContents::create(isolate, web_preferences, this);
  if (!web_contents_) {
    DestroyNative();
    return;
  }
  web_contents_->addObserver(this);
  State().windows.push_back(this);
  atom::WindowList::addWindow(this);
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  Resize();
  if (show) {
    if (focusable) {
      [window_ makeKeyAndOrderFront:nil];
      [NSApp activateIgnoringOtherApps:YES];
    } else {
      [window_ orderFrontRegardless];
    }
  }
  RefreshVisibility();
  LayoutWindowButtons();

  v8::Local<v8::Value> argument = wrapper;
  Emit(AppObject(isolate), "browser-window-created", 1, &argument);
}

MacBrowserWindow::~MacBrowserWindow() {
  DestroyNative();
  wrapper_.Reset();
}

void MacBrowserWindow::DestroyNative() {
  if (destroying_ || closed_)
    return;
  destroying_ = true;
  closed_ = true;
  DetachNativeLayer();
  auto& windows = State().windows;
  windows.erase(std::remove(windows.begin(), windows.end(), this), windows.end());
  atom::WindowList::removeWindow(this);
  if (web_contents_) {
    web_contents_->removeObserver(this);
    web_contents_->destroyed();
    web_contents_ = nullptr;
  }
  for (MacBrowserView* view : browser_views_)
    view->Detach();
  browser_views_.clear();
  NSWindow* window = window_;
  MiniElectronWindowDelegate* delegate = delegate_;
  MiniElectronRendererView* renderer_view = renderer_view_;
  window_ = nil;
  delegate_ = nil;
  renderer_view_ = nil;
  if (window) {
    [(MiniElectronWindow*)window setElectronIgnoreMouseEvents:NO forward:NO];
    window.delegate = nil;
    [renderer_view removeFromSuperview];
    [window orderOut:nil];
    [window close];
  }
#if !__has_feature(objc_arc)
  [renderer_view release];
  [delegate release];
  [window release];
#endif
  destroying_ = false;
  Emit(getWrapper(), "closed");
  if (windows.empty() && !State().quitting)
    Emit(AppObject(isolate()), "window-all-closed");
}

void MacBrowserWindow::close() {
  if (!closed_ && !EmitCancelable(getWrapper(), "close"))
    DestroyNative();
}

bool MacBrowserWindow::handleNativeWindowCommand(
    const std::string& command, const base::Value::Dict& args) {
  if (!window_ || closed_)
    return false;
  if (command == "fullscreen") {
    const auto enabled = args.FindBool("enabled");
    if (!enabled)
      return false;
    if ((window_.styleMask & NSWindowStyleMaskFullScreen) !=
        (*enabled ? NSWindowStyleMaskFullScreen : 0)) {
      [window_ toggleFullScreen:nil];
    } else if (web_contents_) {
      atom::WebContents::sendRendererMessage(web_contents_->getIdApi(),
          "fullscreen-state", base::Value::Dict().Set("enabled", *enabled));
    }
  } else if (command == "focus" || command == "show") {
    if (command == "focus" &&
        !((MiniElectronWindow*)window_).electronFocusable)
      return false;
    [window_ orderFront:nil];
    if (((MiniElectronWindow*)window_).electronFocusable) {
      [NSApp activateIgnoringOtherApps:YES];
      [window_ makeKeyWindow];
      [window_ makeFirstResponder:renderer_view_];
    }
    RefreshVisibility();
  } else if (command == "take-focus") {
    const bool reverse = args.FindBool("reverse").value_or(false);
    NSView* next = reverse ? renderer_view_.previousValidKeyView
                           : renderer_view_.nextValidKeyView;
    if (!next || next == renderer_view_)
      return false;
    return [window_ makeFirstResponder:next];
  } else if (command == "bounds") {
    const auto x = args.FindInt("x");
    const auto y = args.FindInt("y");
    const auto width = args.FindInt("width");
    const auto height = args.FindInt("height");
    if (!x || !y || !width || !height || *width <= 0 || *height <= 0)
      return false;
    [window_ setFrame:CocoaRectFromElectron(NSMakeRect(
        *x, *y, *width, *height)) display:YES];
  } else if (command == "maximize") {
    if (!window_.zoomed)
      [window_ zoom:nil];
  } else if (command == "minimize") {
    [window_ miniaturize:nil];
    RefreshVisibility();
  } else if (command == "restore") {
    if (window_.miniaturized)
      [window_ deminiaturize:nil];
    if (window_.zoomed)
      [window_ zoom:nil];
    RefreshVisibility();
  } else if (command == "resizable") {
    const auto enabled = args.FindBool("enabled");
    if (!enabled)
      return false;
    window_.styleMask = *enabled
        ? window_.styleMask | NSWindowStyleMaskResizable
        : window_.styleMask & ~NSWindowStyleMaskResizable;
  } else if (command == "close") {
    return RequestClose();
  } else {
    return false;
  }
  return true;
}

bool MacBrowserWindow::RequestClose() {
  if (closed_)
    return YES;
  if (EmitCancelable(getWrapper(), "close"))
    return NO;
  DestroyNative();
  return YES;
}

void MacBrowserWindow::onWebContentsDeleted(atom::WebContents* contents) {
  if (web_contents_ == contents) {
    DetachNativeLayer();
    web_contents_ = nullptr;
  }
}

void MacBrowserWindow::onWebContentsPaint(atom::WebContents*, const gfx::Rect&) {
  InvalidateFrame();
  if (!UpdateFrame(web_contents_, renderer_view_))
    [renderer_view_ setNeedsDisplay:YES];
}

void MacBrowserWindow::onWebContentsReadyToShow(atom::WebContents*) {
  Emit(getWrapper(), "ready-to-show");
}

void MacBrowserWindow::onWebContentsDraggableRegions(
    atom::WebContents*, const base::Value::List& regions) {
  draggable_regions_.clear();
  draggable_regions_.reserve(regions.size());
  for (const base::Value& value : regions) {
    const base::Value::Dict* region = value.GetIfDict();
    if (!region)
      continue;
    const auto x = region->FindInt("x");
    const auto y = region->FindInt("y");
    const auto width = region->FindInt("width");
    const auto height = region->FindInt("height");
    const auto draggable = region->FindBool("draggable");
    if (!x || !y || !width || !height || !draggable ||
        *width <= 0 || *height <= 0)
      continue;
    draggable_regions_.push_back(
        {NSMakeRect(*x, *y, *width, *height), *draggable});
  }
}

bool MacBrowserWindow::HandleCaptionMouseDown(NSEvent* event, NSPoint point) {
  if (!window_)
    return false;
  bool draggable = false;
  for (const DraggableRegion& region : draggable_regions_) {
    if (!NSMouseInRect(point, region.bounds, YES))
      continue;
    // Match the Windows host: no-drag regions override every drag region,
    // regardless of DOM order, so titlebar controls remain interactive.
    if (!region.draggable)
      return false;
    draggable = true;
  }
  if (!draggable)
    return false;
  if ((window_.styleMask & NSWindowStyleMaskFullScreen) != 0)
    return true;
  if (event.clickCount == 2) {
    NSString* action = [[NSUserDefaults standardUserDefaults]
        stringForKey:@"AppleActionOnDoubleClick"];
    if ([action isEqualToString:@"Minimize"])
      [window_ miniaturize:nil];
    else if (![action isEqualToString:@"None"])
      [window_ zoom:nil];
  } else {
    [window_ performWindowDragWithEvent:event];
  }
  return true;
}

void RendererSurfaceHost::DetachNativeLayer() {
  NSCAssert(NSThread.isMainThread, @"Renderer layers belong to the main sequence");
  if (!native_clip_layer_)
    return;
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  native_host_layer_.contextId = 0;
  [native_clip_layer_ removeFromSuperlayer];
  [CATransaction commit];
#if !__has_feature(objc_arc)
  [native_host_layer_ release];
  [native_clip_layer_ release];
#endif
  native_host_layer_ = nil;
  native_clip_layer_ = nil;
  drawn_generation_ = 0;
}

bool RendererSurfaceHost::UpdateFrame(atom::WebContents* contents, NSView* view) {
  if (!view || !contents)
    return false;
  NSCAssert(NSThread.isMainThread, @"Renderer layers belong to the main sequence");
  if (frame_dirty_ && contents->copyFrame(&pending_frame_)) {
    if (pending_frame_.output_format == 1) {
      if (!native_host_layer_) {
        Class host_class = NSClassFromString(@"CALayerHost");
        if (!host_class)
          return false;
        native_host_layer_ = [[host_class alloc] init];
        native_clip_layer_ = [[CALayer alloc] init];
        // AppKit can store drawRect output in its own z=0 backing sublayer.
        native_clip_layer_.zPosition = 1;
        native_clip_layer_.masksToBounds = YES;
        native_clip_layer_.geometryFlipped = YES;
      }
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      view.wantsLayer = YES;
      if (native_clip_layer_.superlayer != view.layer)
        [view.layer insertSublayer:native_clip_layer_ atIndex:0];
      if (native_host_layer_.superlayer != native_clip_layer_)
        [native_clip_layer_ addSublayer:native_host_layer_];
      native_host_layer_.contextId = pending_frame_.native_context_id;
      // Host shared-context coordinates directly; the container owns clipping.
      native_host_layer_.anchorPoint = CGPointZero;
      [CATransaction commit];
      std::swap(presented_frame_, pending_frame_);
      if (frame_image_) {
        CGImageRelease(frame_image_);
        frame_image_ = nullptr;
      }
      frame_dirty_ = false;
    } else if (!pending_frame_.rgba.empty() && pending_frame_.width > 0 &&
               pending_frame_.height > 0 && pending_frame_.stride > 0) {
      DetachNativeLayer();
      if (frame_image_ && pending_frame_.generation == presented_frame_.generation) {
        frame_dirty_ = false;
      } else {
        if (!frame_color_space_)
          frame_color_space_ = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGDataProviderRef provider = CGDataProviderCreateWithData(
            nullptr, pending_frame_.rgba.data(), pending_frame_.rgba.size(), nullptr);
        CGImageRef image = provider ? CGImageCreate(
            pending_frame_.width, pending_frame_.height, 8, 32,
            pending_frame_.stride, frame_color_space_,
            kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast, provider,
            nullptr, false, kCGRenderingIntentDefault) : nullptr;
        if (provider)
          CGDataProviderRelease(provider);
        if (image) {
          if (frame_image_)
            CGImageRelease(frame_image_);
          std::swap(presented_frame_, pending_frame_);
          frame_image_ = image;
          frame_dirty_ = false;
        }
      }
    }
  }
  if (!native_host_layer_)
    return false;
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  native_clip_layer_.anchorPoint = CGPointZero;
  native_clip_layer_.position = CGPointZero;
  native_clip_layer_.bounds = CGRectMake(0, 0, view.bounds.size.width,
                                        view.bounds.size.height);
  // Remote backing coordinates are lower-left based. Keep its original
  // density and top-left position while a resized backing is still pending.
  native_host_layer_.position = CGPointMake(
      0, view.bounds.size.height -
          presented_frame_.height / presented_frame_.native_scale_factor);
  [CATransaction commit];
  // This commits a renderer GPU-completed backing into the local layer tree.
  // It is deliberately not a claim about display-server scanout.
  [CATransaction flush];
  drawn_generation_ = presented_frame_.presented_generation;
  return true;
}

void RendererSurfaceHost::DrawFrame(atom::WebContents* contents, NSView* view,
                                    NSRect dirty_rect) {
  if (!view)
    return;
  const bool native = UpdateFrame(contents, view);

  // A seqlock miss means a newer frame is being written, not that the window
  // is empty. Redraw the last complete image until a later paint publishes it.
  [(view.window.backgroundColor ?: NSColor.whiteColor) setFill];
  NSRectFillUsingOperation(dirty_rect, NSCompositingOperationCopy);
  if (native)
    return;
  if (!contents || !frame_image_)
    return;
  CGContextRef context = NSGraphicsContext.currentContext.CGContext;
  if (!context)
    return;
  CGContextSaveGState(context);
  CGContextTranslateCTM(context, 0, view.bounds.size.height);
  CGContextScaleCTM(context, 1, -1);
  const double scale = view.window ? view.window.backingScaleFactor
                                   : NSScreen.mainScreen.backingScaleFactor;
  const CGFloat width = presented_frame_.width / scale;
  const CGFloat height = presented_frame_.height / scale;
  // Retain original pixel density and clip while waiting for a new viewport.
  // Stretching a cached frame changes glyph and control aspect ratios.
  CGContextDrawImage(context,
                     CGRectMake(0, view.bounds.size.height - height, width, height),
                     frame_image_);
  CGContextRestoreGState(context);
  drawn_generation_ = presented_frame_.generation;
}

void ResizeWebContents(atom::WebContents* contents, NSView* view) {
  if (!contents || !view)
    return;
  NSScreen* screen = view.window.screen ?: NSScreen.mainScreen;
  NSNumber* display_number = screen.deviceDescription[@"NSScreenNumber"];
  contents->setVSyncDisplayId(
      display_number ? static_cast<int64_t>(display_number.unsignedIntValue) : -1);
  NSSize size = view.bounds.size;
  const double scale = view.window ? view.window.backingScaleFactor
                                   : NSScreen.mainScreen.backingScaleFactor;
  contents->resize(std::max(1, static_cast<int>(std::lround(size.width))),
                   std::max(1, static_cast<int>(std::lround(size.height))),
                   scale);
}

void MacBrowserWindow::Draw(NSRect dirty_rect) {
  DrawFrame(web_contents_, renderer_view_, dirty_rect);
}

void MacBrowserWindow::onWebContentsTextInputState(
    atom::WebContents*, const base::Value::Dict& state) {
  [(MiniElectronRendererView*)renderer_view_ updateTextInputState:state];
}

uint64_t MacBrowserWindow::prepareWebContentsFrame(atom::WebContents* contents,
                                                  uint64_t generation) {
  if (contents != web_contents_ || !renderer_view_ || !window_ || closed_)
    return 0;
  InvalidateFrame();
  if (UpdateFrame(contents, renderer_view_))
    return DrawnGeneration();
  [renderer_view_ display];
  return DrawnGeneration();
}

uint64_t MacBrowserView::prepareWebContentsFrame(atom::WebContents* contents,
                                                uint64_t generation) {
  if (contents != web_contents_ || !view_ || !view_.window)
    return 0;
  InvalidateFrame();
  if (UpdateFrame(contents, view_))
    return DrawnGeneration();
  [view_ display];
  return DrawnGeneration();
}

void MacBrowserWindow::Resize() {
  ResizeWebContents(web_contents_, renderer_view_);
  UpdateFrame(web_contents_, renderer_view_);
  RefreshVisibility();
}

void MacBrowserWindow::RefreshVisibility() {
  if (closed_ || destroying_ || !web_contents_)
    return;
  const bool visible = IsRendererWindowVisible(window_) && renderer_view_ &&
      !renderer_view_.hiddenOrHasHiddenAncestor;
  web_contents_->setVisible(visible);
  for (MacBrowserView* view : browser_views_)
    view->RefreshVisibility();
}

v8::Persistent<v8::Function> MacBrowserView::constructor;
gin_helper::WrapperInfo MacBrowserView::kWrapperInfo = {
    gin_helper::GinEmbedder::kEmbedderNativeGin};

MacBrowserView::MacBrowserView(v8::Isolate* isolate,
                               v8::Local<v8::Object> wrapper,
                               const gin_helper::Dictionary& options) {
  gin_helper::Wrappable<MacBrowserView>::InitWith(isolate, wrapper);
  wrapper_.Reset(isolate, wrapper);
  id_ = State().next_window_id++;
  view_ = [[MiniElectronRendererView alloc]
      initWithFrame:NSMakeRect(0, 0, 1, 1) owner:this];
  gin_helper::Dictionary web_preferences =
      gin_helper::Dictionary::CreateEmpty(isolate);
  options.Get("webPreferences", &web_preferences);
  web_contents_ = atom::WebContents::create(isolate, web_preferences, this);
  if (web_contents_) {
    web_contents_->addObserver(this);
    Resize();
  }
}

MacBrowserView::~MacBrowserView() {
  destroy();
  wrapper_.Reset();
}

void MacBrowserView::destroy() {
  for (MacBrowserWindow* window : State().windows)
    window->ForgetBrowserView(this);
  DetachNativeLayer();
  Detach();
  if (web_contents_) {
    web_contents_->removeObserver(this);
    web_contents_->destroyed();
    web_contents_ = nullptr;
  }
  NSView* view = view_;
  view_ = nil;
#if !__has_feature(objc_arc)
  [view release];
#else
  (void)view;
#endif
}

MacBrowserView* MacBrowserView::FromValue(v8::Local<v8::Value> value) {
  if (value.IsEmpty() || !value->IsObject())
    return nullptr;
  return static_cast<MacBrowserView*>(gin_helper::WrappableBase::GetNativePtr(
      value.As<v8::Object>(), &kWrapperInfo));
}

void MacBrowserView::onWebContentsDeleted(atom::WebContents* contents) {
  if (web_contents_ == contents) {
    DetachNativeLayer();
    web_contents_ = nullptr;
  }
}

void MacBrowserView::onWebContentsPaint(atom::WebContents*, const gfx::Rect&) {
  InvalidateFrame();
  if (!UpdateFrame(web_contents_, view_))
    [view_ setNeedsDisplay:YES];
}

void MacBrowserView::onWebContentsTextInputState(
    atom::WebContents*, const base::Value::Dict& state) {
  [(MiniElectronRendererView*)view_ updateTextInputState:state];
}

void MacBrowserView::Draw(NSRect dirty_rect) {
  DrawFrame(web_contents_, view_, dirty_rect);
}

void MacBrowserView::Resize() {
  ResizeWebContents(web_contents_, view_);
  UpdateFrame(web_contents_, view_);
  RefreshVisibility();
}

void MacBrowserView::RefreshVisibility() {
  if (!web_contents_)
    return;
  const bool visible = view_ && view_.superview &&
      IsRendererWindowVisible(view_.window) &&
      !view_.hiddenOrHasHiddenAncestor && !NSIsEmptyRect(view_.visibleRect);
  web_contents_->setVisible(visible);
}

void MacBrowserView::Focus(bool focused) {
  if (web_contents_)
    web_contents_->setFocus(focused);
}

bool MacBrowserView::SendInput(base::Value::Dict event) {
  return web_contents_ && web_contents_->sendInput(std::move(event));
}

bool MacBrowserView::SendNativeFileDrag(
    base::Value::Dict event, const std::vector<std::string>& paths) {
  return web_contents_ && web_contents_->sendNativeFileDrag(
      std::move(event), paths);
}

bool MacBrowserView::CanPerformNativeFileDrop() const {
  return web_contents_ && web_contents_->canPerformNativeFileDrop();
}

void MacBrowserView::AttachTo(NSView* parent) {
  if (!view_ || !parent)
    return;
  [parent addSubview:view_ positioned:NSWindowAbove relativeTo:nil];
  // BrowserViews must remain above the parent's native GPU surface (z=1).
  view_.layer.zPosition = 2;
  Resize();
}

void MacBrowserView::Detach() {
  [(MiniElectronRendererView*)view_ cancelNativeDrag];
  if (view_.superview)
    [view_ removeFromSuperview];
  RefreshVisibility();
}

void MacBrowserView::New(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (!info.IsConstructCall()) {
    info.GetIsolate()->ThrowException(v8::Exception::TypeError(
        V8String(info.GetIsolate(), "BrowserView must be constructed")));
    return;
  }
  gin_helper::Dictionary options =
      gin_helper::Dictionary::CreateEmpty(info.GetIsolate());
  if (info.Length() && info[0]->IsObject())
    options = gin_helper::Dictionary(info.GetIsolate(), info[0].As<v8::Object>());
  new MacBrowserView(info.GetIsolate(), info.This(), options);
  info.GetReturnValue().Set(info.This());
}

void MacBrowserView::GetWebContents(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserView* self = FromValue(info.This());
  if (self && self->web_contents_)
    info.GetReturnValue().Set(
        gin_helper::ConvertToV8(info.GetIsolate(), *self->web_contents_));
  else
    info.GetReturnValue().Set(v8::Null(info.GetIsolate()));
}

// _setBounds(x, y, width, height) in DIPs relative to the window content's
// top-left corner; the renderer view is flipped, so no conversion is needed.
void MacBrowserView::SetBounds(const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserView* self = FromValue(info.This());
  if (!self || !self->view_ || info.Length() < 4)
    return;
  v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
  double values[4];
  for (int index = 0; index < 4; ++index)
    values[index] = info[index]->NumberValue(context).FromMaybe(0);
  self->view_.frame = NSMakeRect(values[0], values[1], std::max(1.0, values[2]),
                                 std::max(1.0, values[3]));
  self->Resize();
  [self->view_ setNeedsDisplay:YES];
}

void MacBrowserView::Init(v8::Local<v8::Object> target) {
  v8::Isolate* isolate = target->GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if (constructor.IsEmpty()) {
    v8::Local<v8::FunctionTemplate> type =
        v8::FunctionTemplate::New(isolate, New);
    type->SetClassName(V8String(isolate, "BrowserView"));
    v8::Local<v8::ObjectTemplate> instance = type->InstanceTemplate();
    instance->SetInternalFieldCount(
        gin_helper::InternalFields::kNumberOfInternalFields);
    instance->Set(isolate, "_getWebContents",
                  v8::FunctionTemplate::New(isolate, GetWebContents));
    instance->Set(isolate, "_setBounds",
                  v8::FunctionTemplate::New(isolate, SetBounds));
    constructor.Reset(isolate, type->GetFunction(context).ToLocalChecked());
  }
  SetValue(context, target, "BrowserView", constructor.Get(isolate));
}

void MacBrowserWindow::ForgetBrowserView(MacBrowserView* view) {
  browser_views_.erase(
      std::remove(browser_views_.begin(), browser_views_.end(), view),
      browser_views_.end());
}

void MacBrowserWindow::AddBrowserView(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserWindow* self = From(info);
  MacBrowserView* view = info.Length() ? MacBrowserView::FromValue(info[0]) : nullptr;
  if (!self || !self->renderer_view_ || !view || !view->getWebContents())
    return;
  for (MacBrowserWindow* window : State().windows) {
    if (window != self)
      window->ForgetBrowserView(view);
  }
  self->ForgetBrowserView(view);
  self->browser_views_.push_back(view);
  view->AttachTo(self->renderer_view_);
}

void MacBrowserWindow::RemoveBrowserView(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserWindow* self = From(info);
  MacBrowserView* view = info.Length() ? MacBrowserView::FromValue(info[0]) : nullptr;
  if (!self || !view)
    return;
  if (std::find(self->browser_views_.begin(), self->browser_views_.end(), view) ==
      self->browser_views_.end())
    return;
  self->ForgetBrowserView(view);
  view->Detach();
}

void MacBrowserWindow::Focus(bool focused) {
  if (web_contents_)
    web_contents_->setFocus(focused);
}

bool MacBrowserWindow::SendInput(base::Value::Dict event) {
  return web_contents_ && web_contents_->sendInput(std::move(event));
}

bool MacBrowserWindow::SendNativeFileDrag(
    base::Value::Dict event, const std::vector<std::string>& paths) {
  return web_contents_ && web_contents_->sendNativeFileDrag(
      std::move(event), paths);
}

bool MacBrowserWindow::CanPerformNativeFileDrop() const {
  return web_contents_ && web_contents_->canPerformNativeFileDrop();
}

void MacBrowserWindow::DidBecomeKey() {
  LayoutWindowButtons();
  Focus(true);
  Emit(getWrapper(), "focus");
}
void MacBrowserWindow::DidResignKey() {
  LayoutWindowButtons();
  Focus(false);
  Emit(getWrapper(), "blur");
}
void MacBrowserWindow::DidResize() {
  if (window_ && !window_.zoomed &&
      (window_.styleMask & NSWindowStyleMaskFullScreen) == 0) {
    normal_bounds_ = window_.frame;
  }
  LayoutWindowButtons();
  Resize();
  Emit(getWrapper(), "resize");
}

void DestroyAllWindows() {
  std::vector<MacBrowserWindow*> windows = State().windows;
  for (MacBrowserWindow* window : windows) {
    if (window)
      window->destroy();
  }
}

bool CloseAllWindows() {
  std::vector<MacBrowserWindow*> windows = State().windows;
  for (MacBrowserWindow* window : windows) {
    if (window && !window->RequestClose())
      return false;
  }
  return true;
}

void MacBrowserWindow::GetWebContents(const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserWindow* self = From(info);
  if (self && self->web_contents_)
    info.GetReturnValue().Set(gin_helper::ConvertToV8(info.GetIsolate(), *self->web_contents_));
}
void MacBrowserWindow::Close(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info)) self->close(); }
void MacBrowserWindow::Destroy(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info)) self->DestroyNative(); }
void MacBrowserWindow::Show(const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserWindow* self = From(info);
  if (!self || !self->window_)
    return;
  // Populate the hidden window's backing store before ordering it onscreen.
  // This consumes the latest complete shared bitmap, not ready-to-show's
  // initial lifetime paint.
  self->InvalidateFrame();
  [self->renderer_view_ display];
  if (((MiniElectronWindow*)self->window_).electronFocusable) {
    [NSApp activateIgnoringOtherApps:YES];
    [self->window_ makeKeyAndOrderFront:nil];
  } else {
    [self->window_ orderFrontRegardless];
  }
  self->LayoutWindowButtons();
  self->RefreshVisibility();
}
void MacBrowserWindow::ShowInactive(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (auto* self = From(info); self && self->window_) {
    self->InvalidateFrame();
    [self->renderer_view_ display];
    [self->window_ orderFrontRegardless];
    self->LayoutWindowButtons();
    self->RefreshVisibility();
  }
}
void MacBrowserWindow::GetNativeWindowHandle(const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserWindow* self = From(info);
  if (!self || !self->window_)
    return;
  void* handle = self->getNativeWindowHandle();
  info.GetReturnValue().Set(node::Buffer::Copy(info.GetIsolate(),
      reinterpret_cast<const char*>(&handle), sizeof(handle)).ToLocalChecked());
}
void MacBrowserWindow::SetIgnoreMouseEvents(const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserWindow* self = From(info);
  if (!self || !self->window_ || !info.Length())
    return;
  bool forward = false;
  if (info.Length() > 1 && info[1]->IsObject()) {
    gin_helper::Dictionary options(info.GetIsolate(), info[1].As<v8::Object>());
    options.Get("forward", &forward);
  }
  [(MiniElectronWindow*)self->window_
      setElectronIgnoreMouseEvents:info[0]->BooleanValue(info.GetIsolate())
      forward:forward];
}
void MacBrowserWindow::SetVisibleOnAllWorkspaces(const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserWindow* self = From(info);
  if (!self || !self->window_ || !info.Length())
    return;
  bool full_screen = false;
  if (info.Length() > 1 && info[1]->IsObject()) {
    gin_helper::Dictionary options(info.GetIsolate(), info[1].As<v8::Object>());
    options.Get("visibleOnFullScreen", &full_screen);
  }
  NSWindowCollectionBehavior behavior = self->window_.collectionBehavior;
  behavior &= ~(NSWindowCollectionBehaviorCanJoinAllSpaces |
                NSWindowCollectionBehaviorFullScreenAuxiliary);
  if (info[0]->BooleanValue(info.GetIsolate())) {
    behavior &= ~NSWindowCollectionBehaviorMoveToActiveSpace;
    behavior |= NSWindowCollectionBehaviorCanJoinAllSpaces;
    if (full_screen) {
      behavior &= ~NSWindowCollectionBehaviorFullScreenPrimary;
      behavior |= NSWindowCollectionBehaviorFullScreenAuxiliary;
    }
  }
  self->window_.collectionBehavior = behavior;
}
void MacBrowserWindow::Hide(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (auto* self = From(info); self && self->window_) {
    [self->window_ orderOut:nil];
    self->RefreshVisibility();
  }
}
void MacBrowserWindow::FocusWindow(const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserWindow* self = From(info);
  if (!self || !self->window_ ||
      !((MiniElectronWindow*)self->window_).electronFocusable)
    return;
  self->InvalidateFrame();
  [self->renderer_view_ display];
  [NSApp activateIgnoringOtherApps:YES];
  [self->window_ makeKeyAndOrderFront:nil];
  self->RefreshVisibility();
}
void MacBrowserWindow::BlurWindow(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_) [self->window_ resignKeyWindow]; }
void MacBrowserWindow::IsFocused(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info)) info.GetReturnValue().Set(self->window_.keyWindow); }
void MacBrowserWindow::IsVisible(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info)) info.GetReturnValue().Set(self->window_.visible); }
void MacBrowserWindow::IsDestroyed(const v8::FunctionCallbackInfo<v8::Value>& info) { MacBrowserWindow* self = From(info); info.GetReturnValue().Set(!self || self->closed_); }
void MacBrowserWindow::Maximize(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_ && !self->window_.zoomed) [self->window_ zoom:nil]; }
void MacBrowserWindow::Unmaximize(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_ && self->window_.zoomed) [self->window_ zoom:nil]; }
void MacBrowserWindow::IsMaximized(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info)) info.GetReturnValue().Set(self->window_.zoomed); }
void MacBrowserWindow::Minimize(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (auto* self = From(info); self && self->window_) {
    [self->window_ miniaturize:nil];
    self->RefreshVisibility();
  }
}
void MacBrowserWindow::Restore(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (auto* self = From(info); self && self->window_) {
    [self->window_ deminiaturize:nil];
    self->RefreshVisibility();
  }
}
void MacBrowserWindow::IsMinimized(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info)) info.GetReturnValue().Set(self->window_.miniaturized); }
void MacBrowserWindow::SetFullScreen(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_ && info.Length() && (self->window_.styleMask & NSWindowStyleMaskFullScreen) != (info[0]->BooleanValue(info.GetIsolate()) ? NSWindowStyleMaskFullScreen : 0)) [self->window_ toggleFullScreen:nil]; }
void MacBrowserWindow::IsFullScreen(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info)) info.GetReturnValue().Set((self->window_.styleMask & NSWindowStyleMaskFullScreen) != 0); }
void MacBrowserWindow::SetTitle(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_ && info.Length()) self->window_.title = NSStringFromUtf8(Utf8(info.GetIsolate(), info[0])); }
void MacBrowserWindow::GetTitle(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_) info.GetReturnValue().Set(V8String(info.GetIsolate(), [self->window_.title UTF8String])); }

v8::Local<v8::Object> RectObject(v8::Isolate* isolate, NSRect rect) {
  rect = ElectronRectFromCocoa(rect);
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Object> result = v8::Object::New(isolate);
  SetValue(context, result, "x", v8::Integer::New(isolate, static_cast<int>(std::lround(rect.origin.x))));
  SetValue(context, result, "y", v8::Integer::New(isolate, static_cast<int>(std::lround(rect.origin.y))));
  SetValue(context, result, "width", v8::Integer::New(isolate, static_cast<int>(std::lround(rect.size.width))));
  SetValue(context, result, "height", v8::Integer::New(isolate, static_cast<int>(std::lround(rect.size.height))));
  return result;
}

void MacBrowserWindow::GetBounds(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_) info.GetReturnValue().Set(RectObject(info.GetIsolate(), self->window_.frame)); }
void MacBrowserWindow::SetBounds(const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserWindow* self = From(info);
  if (!self || !self->window_ || !info.Length() || !info[0]->IsObject()) return;
  gin_helper::Dictionary bounds(info.GetIsolate(), info[0].As<v8::Object>());
  NSRect frame = ElectronRectFromCocoa(self->window_.frame);
  int value;
  if (bounds.Get("x", &value)) frame.origin.x = value;
  if (bounds.Get("y", &value)) frame.origin.y = value;
  if (bounds.Get("width", &value)) frame.size.width = std::max(1, value);
  if (bounds.Get("height", &value)) frame.size.height = std::max(1, value);
  frame = CocoaRectFromElectron(frame);
  [self->window_ setFrame:frame display:YES];
}
void MacBrowserWindow::GetNormalBounds(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (auto* self = From(info); self && self->window_)
    info.GetReturnValue().Set(RectObject(info.GetIsolate(), self->normal_bounds_));
}
void MacBrowserWindow::SetPosition(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserWindow* self = From(info);
  if (!self || !self->window_ || info.Length() < 2)
    return;
  NSRect frame = ElectronRectFromCocoa(self->window_.frame);
  frame.origin.x = info[0]->Int32Value(info.GetIsolate()->GetCurrentContext()).FromMaybe(0);
  frame.origin.y = info[1]->Int32Value(info.GetIsolate()->GetCurrentContext()).FromMaybe(0);
  frame = CocoaRectFromElectron(frame);
  [self->window_ setFrameOrigin:frame.origin];
}
void MacBrowserWindow::SetSize(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserWindow* self = From(info);
  if (!self || !self->window_ || info.Length() < 2)
    return;
  NSRect frame = ElectronRectFromCocoa(self->window_.frame);
  frame.size.width = std::max(
      1, info[0]->Int32Value(info.GetIsolate()->GetCurrentContext()).FromMaybe(1));
  frame.size.height = std::max(
      1, info[1]->Int32Value(info.GetIsolate()->GetCurrentContext()).FromMaybe(1));
  frame = CocoaRectFromElectron(frame);
  [self->window_ setFrame:frame display:YES];
}
void MacBrowserWindow::SetMinimumSize(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserWindow* self = From(info);
  if (!self || !self->window_ || info.Length() < 2)
    return;
  self->window_.minSize = NSMakeSize(
      std::max(1, info[0]->Int32Value(info.GetIsolate()->GetCurrentContext()).FromMaybe(1)),
      std::max(1, info[1]->Int32Value(info.GetIsolate()->GetCurrentContext()).FromMaybe(1)));
}
void MacBrowserWindow::SetBackgroundColor(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserWindow* self = From(info);
  if (!self || !self->window_ || !info.Length())
    return;
  std::string color_string = Utf8(info.GetIsolate(), info[0]);
  NSColor* color = ParseBackgroundColor(color_string);
  if (!color)
    return;
  // Transparent companion windows keep a clear native backing surface.
  self->window_.backgroundColor = self->window_.opaque ? color : NSColor.clearColor;
  self->renderer_view_.layer.backgroundColor = self->window_.backgroundColor.CGColor;
  if (self->web_contents_) {
    base::Value::Dict params;
    params.Set("color", std::move(color_string));
    atom::WebContents::sendRendererMessage(
        self->web_contents_->getIdApi(), "set-background-color", std::move(params));
  }
  [self->renderer_view_ setNeedsDisplay:YES];
}
void MacBrowserWindow::SetTitleBarOverlay(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (auto* self = From(info); self && self->window_) {
    self->window_.titleVisibility = NSWindowTitleHidden;
    self->window_.titlebarAppearsTransparent = YES;
    self->LayoutWindowButtons();
  }
}
void MacBrowserWindow::SetWindowButtonPosition(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserWindow* self = From(info);
  if (!self || !self->window_ || !info.Length() || !info[0]->IsObject())
    return;
  gin_helper::Dictionary point(info.GetIsolate(), info[0].As<v8::Object>());
  double x = 0;
  double y = 0;
  if (!point.Get("x", &x) || !point.Get("y", &y) ||
      !std::isfinite(x) || !std::isfinite(y)) {
    return;
  }
  self->SetNativeWindowButtonPosition(NSMakePoint(x, y));
}
void MacBrowserWindow::Center(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_) [self->window_ center]; }
void MacBrowserWindow::SetAlwaysOnTop(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_ && info.Length()) self->window_.level = info[0]->BooleanValue(info.GetIsolate()) ? NSFloatingWindowLevel : NSNormalWindowLevel; }
void MacBrowserWindow::IsAlwaysOnTop(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info)) info.GetReturnValue().Set(self->window_.level > NSNormalWindowLevel); }
void MacBrowserWindow::SetDocumentEdited(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_ && info.Length()) self->window_.documentEdited = info[0]->BooleanValue(info.GetIsolate()); }
void MacBrowserWindow::IsDocumentEdited(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info)) info.GetReturnValue().Set(self->window_.documentEdited); }

void MacBrowserWindow::GetAllWindows(const v8::FunctionCallbackInfo<v8::Value>& info) {
  const auto& windows = State().windows;
  v8::Local<v8::Array> result = v8::Array::New(info.GetIsolate(), windows.size());
  v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();
  for (size_t index = 0; index < windows.size(); ++index)
    result->Set(context, index, windows[index]->getWrapper()).Check();
  info.GetReturnValue().Set(result);
}
void MacBrowserWindow::GetFocusedWindow(const v8::FunctionCallbackInfo<v8::Value>& info) {
  for (MacBrowserWindow* window : State().windows) if (window->window_.keyWindow) { info.GetReturnValue().Set(window->getWrapper()); return; }
  info.GetReturnValue().Set(v8::Null(info.GetIsolate()));
}
void MacBrowserWindow::FromWebContents(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (!info.Length() || !info[0]->IsObject()) return;
  for (MacBrowserWindow* window : State().windows) {
    if (window->web_contents_ &&
        window->web_contents_->getWrapper()->StrictEquals(info[0])) {
      info.GetReturnValue().Set(window->getWrapper());
      return;
    }
  }
  info.GetReturnValue().Set(v8::Null(info.GetIsolate()));
}
void MacBrowserWindow::FromId(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (!info.Length() || !info[0]->IsInt32()) return;
  int id = info[0].As<v8::Int32>()->Value();
  for (MacBrowserWindow* window : State().windows) if (window->id_ == id) { info.GetReturnValue().Set(window->getWrapper()); return; }
  info.GetReturnValue().Set(v8::Null(info.GetIsolate()));
}

void MacBrowserWindow::New(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (!info.IsConstructCall()) {
    info.GetIsolate()->ThrowException(v8::Exception::TypeError(V8String(info.GetIsolate(), "BrowserWindow must be constructed")));
    return;
  }
  gin_helper::Dictionary options = gin_helper::Dictionary::CreateEmpty(info.GetIsolate());
  if (info.Length() && info[0]->IsObject()) options = gin_helper::Dictionary(info.GetIsolate(), info[0].As<v8::Object>());
  new MacBrowserWindow(info.GetIsolate(), info.This(), options);
  info.GetReturnValue().Set(info.This());
}

void MacBrowserWindow::Init(v8::Local<v8::Object> target) {
  v8::Isolate* isolate = target->GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::FunctionTemplate> type = v8::FunctionTemplate::New(isolate, New);
  type->SetClassName(V8String(isolate, "BrowserWindow"));
  v8::Local<v8::ObjectTemplate> instance = type->InstanceTemplate();
  instance->SetInternalFieldCount(
      gin_helper::InternalFields::kNumberOfInternalFields);
  auto method = [&](const char* name, v8::FunctionCallback callback) {
    instance->Set(isolate, name, v8::FunctionTemplate::New(isolate, callback));
  };
  method("_getWebContents", &MacBrowserWindow::GetWebContents);
  method("close", &MacBrowserWindow::Close);
  method("destroy", &MacBrowserWindow::Destroy);
  method("show", &MacBrowserWindow::Show);
  method("showInactive", &MacBrowserWindow::ShowInactive);
  method("getNativeWindowHandle", &MacBrowserWindow::GetNativeWindowHandle);
  method("setIgnoreMouseEvents", &MacBrowserWindow::SetIgnoreMouseEvents);
  method("setVisibleOnAllWorkspaces", &MacBrowserWindow::SetVisibleOnAllWorkspaces);
  method("hide", &MacBrowserWindow::Hide);
  method("focus", &MacBrowserWindow::FocusWindow);
  method("blur", &MacBrowserWindow::BlurWindow);
  method("isFocused", &MacBrowserWindow::IsFocused);
  method("isVisible", &MacBrowserWindow::IsVisible);
  method("isDestroyed", &MacBrowserWindow::IsDestroyed);
  method("maximize", &MacBrowserWindow::Maximize);
  method("unmaximize", &MacBrowserWindow::Unmaximize);
  method("isMaximized", &MacBrowserWindow::IsMaximized);
  method("minimize", &MacBrowserWindow::Minimize);
  method("restore", &MacBrowserWindow::Restore);
  method("isMinimized", &MacBrowserWindow::IsMinimized);
  method("setFullScreen", &MacBrowserWindow::SetFullScreen);
  method("isFullScreen", &MacBrowserWindow::IsFullScreen);
  method("_setTitle", &MacBrowserWindow::SetTitle);
  method("getTitle", &MacBrowserWindow::GetTitle);
  method("getBounds", &MacBrowserWindow::GetBounds);
  method("setBounds", &MacBrowserWindow::SetBounds);
  method("getNormalBounds", &MacBrowserWindow::GetNormalBounds);
  method("setPosition", &MacBrowserWindow::SetPosition);
  method("setSize", &MacBrowserWindow::SetSize);
  method("setMinimumSize", &MacBrowserWindow::SetMinimumSize);
  method("setBackgroundColor", &MacBrowserWindow::SetBackgroundColor);
  method("setTitleBarOverlay", &MacBrowserWindow::SetTitleBarOverlay);
  method("setWindowButtonPosition", &MacBrowserWindow::SetWindowButtonPosition);
  method("center", &MacBrowserWindow::Center);
  method("setAlwaysOnTop", &MacBrowserWindow::SetAlwaysOnTop);
  method("isAlwaysOnTop", &MacBrowserWindow::IsAlwaysOnTop);
  method("setDocumentEdited", &MacBrowserWindow::SetDocumentEdited);
  method("isDocumentEdited", &MacBrowserWindow::IsDocumentEdited);
  method("setBrowserView", &MacBrowserWindow::AddBrowserView);
  method("addBrowserView", &MacBrowserWindow::AddBrowserView);
  method("removeBrowserView", &MacBrowserWindow::RemoveBrowserView);
  v8::Local<v8::Function> function = type->GetFunction(context).ToLocalChecked();
  SetMethod(context, function, "getAllWindows", GetAllWindows);
  SetMethod(context, function, "getFocusedWindow", GetFocusedWindow);
  SetMethod(context, function, "fromWebContents", FromWebContents);
  SetMethod(context, function, "fromId", FromId);
  constructor.Reset(isolate, function);
  SetValue(context, target, "BrowserWindow", function);
}

void InitializeWebContents(v8::Local<v8::Object> target,
                           v8::Local<v8::Value>,
                           v8::Local<v8::Context> context,
                           void*) {
  if (atom::WebContents::s_constructor.IsEmpty())
    atom::WebContents::init(context->GetIsolate(), target, State().environment);
  else
    SetValue(context, target, "WebContents",
             atom::WebContents::s_constructor.Get(context->GetIsolate()));
}

void InitializeBrowserWindow(v8::Local<v8::Object> target,
                             v8::Local<v8::Value>,
                             v8::Local<v8::Context>,
                             void*) {
  if (atom::WebContents::s_constructor.IsEmpty()) {
    v8::Local<v8::Object> ignored = v8::Object::New(target->GetIsolate());
    atom::WebContents::init(target->GetIsolate(), ignored, State().environment);
  }
  MacBrowserWindow::Init(target);
}

void InitializeBrowserView(v8::Local<v8::Object> target,
                           v8::Local<v8::Value>,
                           v8::Local<v8::Context>,
                           void*) {
  if (atom::WebContents::s_constructor.IsEmpty()) {
    v8::Local<v8::Object> ignored = v8::Object::New(target->GetIsolate());
    atom::WebContents::init(target->GetIsolate(), ignored, State().environment);
  }
  MacBrowserView::Init(target);
}

void InitializeMacHost(v8::Local<v8::Object> target,
                       v8::Local<v8::Value>,
                       v8::Local<v8::Context> context,
                       void*) {
  if (atom::WebContents::s_constructor.IsEmpty())
    atom::WebContents::init(context->GetIsolate(), target, State().environment);
  MacBrowserWindow::Init(target);
  v8::Local<v8::Object> app = CreateApp(context);
  State().app.Reset(context->GetIsolate(), app);
  SetValue(context, target, "app", app);
  SetValue(context, target, "webContents",
           atom::WebContents::s_constructor.Get(context->GetIsolate()));
}

// The continuous AppKit pump owns native input and Chromium work. Scope each
// delegate turn separately so Node microtasks do not wait for Run() to return.
class MacMainMessagePump final : public base::MessagePumpNSApplication {
 public:
  void Run(Delegate* delegate) override {
    ScopedJavaScriptDelegate scoped_delegate(delegate);
    base::MessagePumpNSApplication::Run(&scoped_delegate);
  }

 private:
  class ScopedJavaScriptDelegate final : public Delegate {
   public:
    explicit ScopedJavaScriptDelegate(Delegate* delegate) : delegate_(delegate) {}

    NextWorkInfo DoWork() override {
      HostState& state = State();
      if (!CanEnterJavaScript(state))
        return delegate_->DoWork();
      NativeCallbackScope scope(state);
      return delegate_->DoWork();
    }

    void DoIdleWork() override {
      HostState& state = State();
      if (!CanEnterJavaScript(state)) {
        delegate_->DoIdleWork();
        return;
      }
      NativeCallbackScope scope(state);
      delegate_->DoIdleWork();
    }

    void BeforeWait() override { delegate_->BeforeWait(); }
    void BeginNativeWorkBeforeDoWork() override {
      delegate_->BeginNativeWorkBeforeDoWork();
    }
    int RunDepth() override { return delegate_->RunDepth(); }

   private:
    void OnBeginWorkItem() override {
      native_work_items_.push_back(delegate_->BeginWorkItem());
    }
    void OnEndWorkItem(int) override {
      auto work_item = std::move(native_work_items_.back());
      native_work_items_.pop_back();
    }

    Delegate* delegate_;
    absl::InlinedVector<ScopedDoWorkItem, 8> native_work_items_;
  };
};

// Only main touches libuv. The waiter observes its backend fd without consuming
// events, then waits for acknowledgement; a native timer owns exact deadlines.
class MacNodeRunLoop final {
 public:
  explicit MacNodeRunLoop(uv_loop_t* loop) : loop_(loop) {}

  ~MacNodeRunLoop() {
    if (thread_started_) {
      stopping_.store(true, std::memory_order_release);
      const char byte = 0;
      ssize_t result;
      do {
        result = write(stop_pipe_[1], &byte, sizeof(byte));
      } while (result < 0 && errno == EINTR);
      uv_sem_post(&acknowledged_);
      uv_thread_join(&thread_);
    }
    if (before_wait_)
      CFRunLoopObserverInvalidate(before_wait_.get());
    if (deadline_)
      CFRunLoopTimerInvalidate(deadline_.get());
    if (work_source_)
      CFRunLoopSourceInvalidate(work_source_.get());
    if (keep_alive_) {
      uv_close(reinterpret_cast<uv_handle_t*>(keep_alive_), [](uv_handle_t* handle) {
        delete reinterpret_cast<uv_async_t*>(handle);
      });
    }
    if (stop_pipe_[0] >= 0)
      close(stop_pipe_[0]);
    if (stop_pipe_[1] >= 0)
      close(stop_pipe_[1]);
    if (semaphore_initialized_)
      uv_sem_destroy(&acknowledged_);
  }

  bool Initialize() {
    backend_fd_ = uv_backend_fd(loop_);
    if (backend_fd_ < 0 || pipe(stop_pipe_) != 0)
      return false;
    if (fcntl(stop_pipe_[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(stop_pipe_[1], F_SETFD, FD_CLOEXEC) < 0)
      return false;
    if (uv_sem_init(&acknowledged_, 0) != 0)
      return false;
    semaphore_initialized_ = true;

    // Browser windows, not user ref'ed Node handles, own application lifetime.
    // This referenced handle never sends periodic wakeups.
    keep_alive_ = new uv_async_t();
    if (uv_async_init(loop_, keep_alive_, [](uv_async_t*) {}) != 0) {
      delete keep_alive_;
      keep_alive_ = nullptr;
      return false;
    }

    run_loop_.reset(CFRunLoopGetCurrent(), base::scoped_policy::RETAIN);
    CFRunLoopSourceContext source_context = {};
    source_context.info = this;
    source_context.perform = [](void* info) {
      static_cast<MacNodeRunLoop*>(info)->PumpUv();
    };
    work_source_.reset(CFRunLoopSourceCreate(nullptr, 0, &source_context));

    CFRunLoopTimerContext timer_context = {};
    timer_context.info = this;
    deadline_.reset(CFRunLoopTimerCreate(
        nullptr, kNoDeadline, kNoDeadline, 0, 0,
        [](CFRunLoopTimerRef, void* info) {
          static_cast<MacNodeRunLoop*>(info)->PumpUv();
        },
        &timer_context));

    CFRunLoopObserverContext observer_context = {};
    observer_context.info = this;
    before_wait_.reset(CFRunLoopObserverCreate(
        nullptr, kCFRunLoopBeforeWaiting, true,
        std::numeric_limits<CFIndex>::max(),
        [](CFRunLoopObserverRef, CFRunLoopActivity, void* info) {
          static_cast<MacNodeRunLoop*>(info)->PumpUv();
        },
        &observer_context));
    if (!work_source_ || !deadline_ || !before_wait_)
      return false;

    if (uv_thread_create(&thread_, [](void* info) {
          static_cast<MacNodeRunLoop*>(info)->WaitForBackend();
        }, this) != 0)
      return false;
    thread_started_ = true;
    CFRunLoopAddSource(run_loop_.get(), work_source_.get(), kCFRunLoopCommonModes);
    CFRunLoopAddTimer(run_loop_.get(), deadline_.get(), kCFRunLoopCommonModes);
    CFRunLoopAddObserver(run_loop_.get(), before_wait_.get(), kCFRunLoopCommonModes);
    return true;
  }

  void Run() {
    if (State().exit_requested)
      return;
    base::RunLoop run_loop;
    main_run_loop_ = &run_loop;
    SignalWork();
    run_loop.Run();
    main_run_loop_ = nullptr;
  }

  void Quit() {
    uv_stop(loop_);
    if (main_run_loop_)
      main_run_loop_->Quit();
  }

 private:
  static constexpr CFTimeInterval kNoDeadline =
      std::numeric_limits<CFTimeInterval>::max();

  void SignalWork() {
    CFRunLoopSourceSignal(work_source_.get());
    CFRunLoopWakeUp(run_loop_.get());
  }

  void WaitForBackend() {
    pollfd descriptors[2] = {{backend_fd_, POLLIN, 0}, {stop_pipe_[0], POLLIN, 0}};
    while (!stopping_.load(std::memory_order_acquire)) {
      int result;
      do {
        result = poll(descriptors, 2, -1);
      } while (result < 0 && errno == EINTR &&
               !stopping_.load(std::memory_order_acquire));
      if (stopping_.load(std::memory_order_acquire))
        return;
      if (result < 0 || (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) ||
          descriptors[1].revents) {
        backend_error_.store(result < 0 ? errno : EIO, std::memory_order_release);
        SignalWork();
        return;
      }
      if (!(descriptors[0].revents & POLLIN))
        continue;
      backend_wakeup_pending_.store(true, std::memory_order_release);
      SignalWork();
      uv_sem_wait(&acknowledged_);
    }
  }

  void PumpUv() {
    // Native tracking may nest inside a Node callback; libuv is not reentrant.
    if (uv_running_ || State().exit_requested)
      return;
    const int error = backend_error_.load(std::memory_order_acquire);
    if (error) {
      std::fprintf(stderr, "mini-electron: libuv backend wait failed: %s\n",
                   std::strerror(error));
      ExitMacMainProcess(1);
      return;
    }
    uv_running_ = true;
    @autoreleasepool {
      // Node scopes its own callbacks. Chromium work uses the delegate above.
      uv_run(loop_, UV_RUN_NOWAIT);
    }
    uv_running_ = false;
    if (State().exit_requested)
      return;

    // Flush watcher changes and rearm after native/GCD/Chromium callbacks,
    // including timers or sockets created without a libuv async wake.
    uv_update_time(loop_);
    const int timeout = uv_backend_timeout(loop_);
    CFRunLoopTimerSetNextFireDate(
        deadline_.get(), timeout < 0 ? kNoDeadline
                                   : CFAbsoluteTimeGetCurrent() + timeout / 1000.0);
    if (backend_wakeup_pending_.exchange(false, std::memory_order_acq_rel))
      uv_sem_post(&acknowledged_);
  }

  uv_loop_t* loop_;
  uv_async_t* keep_alive_ = nullptr;
  int backend_fd_ = -1;
  int stop_pipe_[2] = {-1, -1};
  uv_sem_t acknowledged_;
  uv_thread_t thread_;
  bool semaphore_initialized_ = false;
  bool thread_started_ = false;
  bool uv_running_ = false;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> backend_wakeup_pending_{false};
  std::atomic<int> backend_error_{0};
  base::RunLoop* main_run_loop_ = nullptr;
  base::apple::ScopedCFTypeRef<CFRunLoopRef> run_loop_;
  base::apple::ScopedCFTypeRef<CFRunLoopSourceRef> work_source_;
  base::apple::ScopedCFTypeRef<CFRunLoopTimerRef> deadline_;
  base::apple::ScopedCFTypeRef<CFRunLoopObserverRef> before_wait_;
};

std::unique_ptr<MacNodeRunLoop> g_node_run_loop;

void RequestMainLoopExit() {
  State().exit_requested = true;
  if (g_node_run_loop)
    g_node_run_loop->Quit();
}

}  // namespace

v8::Local<v8::Object> GetFocusedBrowserWindow(v8::Isolate*) {
  for (MacBrowserWindow* window : State().windows) {
    NSWindow* native = (__bridge NSWindow*)window->getNativeWindowHandle();
    if (native.keyWindow)
      return window->getWrapper();
  }
  return {};
}

v8::Local<v8::Object> GetBrowserWindowForNativeWindow(void* native_window) {
  for (MacBrowserWindow* window : State().windows) {
    if (window->getNativeWindowHandle() == native_window)
      return window->getWrapper();
  }
  return {};
}

void InstallMacElectronBindings(node::Environment* environment,
                                const MacBindingOptions& options) {
  HostState& state = State();
  state.environment = environment;
  state.isolate = node::GetMainContext(environment)->GetIsolate();
  state.context.Reset(state.isolate, node::GetMainContext(environment));
  state.options = options;
  state.app_path.clear();
  state.packaged = false;
  if (!g_application_delegate) {
    g_application_delegate = [[MiniElectronApplicationDelegate alloc] init];
    NSApp.delegate = g_application_delegate;
  }
  RefreshNamedPaths();
  if (!atom::SessionMgr::get()->setRootDir(
          base::FilePath::FromUTF8Unsafe(state.paths["userData"]))) {
    std::fputs("mini-electron: cannot initialize the userData session root\n",
               stderr);
  }

  node::AddLinkedBinding(environment, "electron_browser_mac_host",
                         InitializeMacHost, nullptr);
  node::AddLinkedBinding(environment, "electron_browser_browserwindow",
                         InitializeBrowserWindow, nullptr);
  node::AddLinkedBinding(environment, "electron_browser_web_contents",
                         InitializeWebContents, nullptr);
  node::AddLinkedBinding(environment, "electron_browser_browserview",
                         InitializeBrowserView, nullptr);
  InstallMacPlatformApiBindings(environment);
}

int GetMacMainProcessExitCode() {
  return State().exit_code;
}

std::unique_ptr<base::MessagePump> CreateMacMainMessagePump() {
  return std::make_unique<MacMainMessagePump>();
}

void RunMacMainLoop() {
  if (g_node_run_loop)
    g_node_run_loop->Run();
}

void ExitMacMainProcess(int exit_code) {
  HostState& state = State();
  state.quitting = true;
  state.exiting = true;
  state.exit_code = exit_code;
  DestroyAllWindows();
  RequestMainLoopExit();
}

bool RunInMainJavaScriptScope(const std::function<void()>& callback) {
  HostState& state = State();
  if (!callback || !CanEnterJavaScript(state))
    return false;
  NativeCallbackScope scope(state);
  callback();
  return true;
}

bool InstallMacRunLoopIntegration(uv_loop_s* loop) {
  if (!loop || g_node_run_loop)
    return false;
  auto integration = std::make_unique<MacNodeRunLoop>(loop);
  if (!integration->Initialize())
    return false;
  g_node_run_loop = std::move(integration);
  return true;
}

void ShutdownMacRunLoopIntegration() {
  DestroyAllWindows();
  // Join and invalidate native callbacks before releasing the Environment.
  // Node teardown drains the async handle's delete-only close callback.
  g_node_run_loop.reset();
  HostState& state = State();
  ReleaseSingleInstanceLock();
  NSApp.delegate = nil;
#if !__has_feature(objc_arc)
  [g_application_delegate release];
#endif
  g_application_delegate = nil;
  ShutdownMacPlatformApiBindings();
  state.app.Reset();
  state.context.Reset();
  state.environment = nullptr;
  state.isolate = nullptr;
}

}  // namespace mini_electron::electron
