// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#import <AppKit/AppKit.h>
#import <ServiceManagement/ServiceManagement.h>

#include "platform/macos/electron/mac_electron_bindings.h"
#include "platform/macos/electron/mac_platform_apis.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "base/files/file_path.h"
#include "base/run_loop.h"
#include "base/values.h"
#include "runtime/electron/browser/api/session.h"
#include "runtime/electron/browser/api/web_contents.h"
#include "runtime/electron/browser/api/window_list.h"
#include "runtime/electron/browser/api/window_interface.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "runtime/electron/common/gin_helper/dictionary.h"
#include "runtime/electron/common/gin_helper/object_template_builder.h"
#include "runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "runtime/electron/common/gin_helper/public/wrapper_info.h"
#include "runtime/electron/common/gin_helper/wrappable.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libuv/include/uv.h"

@class MiniElectronApplicationDelegate;

namespace mini_electron::electron {
namespace {
extern MiniElectronApplicationDelegate* g_application_delegate;

class MacBrowserWindow;
void DestroyAllWindows();
bool CloseAllWindows();

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
  state.paths["exe"] = State().options.application_path.empty()
      ? std::string()
      : [[[NSBundle mainBundle] executablePath] fileSystemRepresentation];
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

void AppSetAppPath(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info.Length())
    State().app_path = Utf8(info.GetIsolate(), info[0]);
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

void AppReleaseSingleInstanceLock(
    const v8::FunctionCallbackInfo<v8::Value>&) {
  HostState& state = State();
  if (state.single_instance_fd < 0)
    return;
  flock(state.single_instance_fd, LOCK_UN);
  close(state.single_instance_fd);
  state.single_instance_fd = -1;
  if (!state.single_instance_notification.empty()) {
    [[NSDistributedNotificationCenter defaultCenter]
        removeObserver:g_application_delegate
                  name:NSStringFromUtf8(state.single_instance_notification)
                object:nil];
    state.single_instance_notification.clear();
  }
}

void StopMainEnvironment(int exit_code) {
  HostState& state = State();
  state.exit_code = exit_code;
  if (!state.environment)
    return;
  node::Environment* environment = state.environment;
  state.environment = nullptr;
  node::Stop(environment);
}

void AppGetExitCode(const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(State().exit_code);
}

void AppExit(const v8::FunctionCallbackInfo<v8::Value>& info) {
  int exit_code = 0;
  if (info.Length() && info[0]->IsInt32())
    exit_code = info[0].As<v8::Int32>()->Value();
  State().quitting = true;
  DestroyAllWindows();
  StopMainEnvironment(exit_code);
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
  StopMainEnvironment(0);
  return true;
}

void AppQuit(const v8::FunctionCallbackInfo<v8::Value>& info) {
  RequestApplicationQuit(info.GetIsolate());
}

@interface MiniElectronApplicationDelegate : NSObject <NSApplicationDelegate>
- (void)secondInstance:(NSNotification*)notification;
@end

MiniElectronApplicationDelegate* g_application_delegate = nil;

@implementation MiniElectronApplicationDelegate
- (void)secondInstance:(NSNotification*)notification {
  HostState& state = State();
  if (!state.isolate || state.app.IsEmpty())
    return;
  v8::HandleScope scope(state.isolate);
  v8::Local<v8::Context> context = state.context.Get(state.isolate);
  v8::Context::Scope context_scope(context);
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
  if (!state.isolate || state.app.IsEmpty())
    return;
  v8::HandleScope scope(state.isolate);
  v8::Local<v8::Context> context = state.context.Get(state.isolate);
  v8::Context::Scope context_scope(context);
  Emit(AppObject(state.isolate), "activate");
  (void)notification;
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)application
                    hasVisibleWindows:(BOOL)has_visible_windows {
  HostState& state = State();
  if (state.isolate && !state.app.IsEmpty()) {
    v8::HandleScope scope(state.isolate);
    v8::Local<v8::Context> context = state.context.Get(state.isolate);
    v8::Context::Scope context_scope(context);
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
  if (!state.isolate || state.app.IsEmpty())
    return;
  v8::HandleScope scope(state.isolate);
  v8::Local<v8::Context> context = state.context.Get(state.isolate);
  v8::Context::Scope context_scope(context);
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
  if (!state.isolate || state.app.IsEmpty())
    return;
  v8::HandleScope scope(state.isolate);
  v8::Local<v8::Context> context = state.context.Get(state.isolate);
  v8::Context::Scope context_scope(context);
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
  if (!state.isolate || state.app.IsEmpty())
    return NSTerminateNow;
  v8::HandleScope scope(state.isolate);
  v8::Local<v8::Context> context = state.context.Get(state.isolate);
  v8::Context::Scope context_scope(context);
  RequestApplicationQuit(state.isolate);
  (void)application;
  return NSTerminateCancel;
}
@end

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

class MacBrowserWindow final : public mate::EventEmitter<MacBrowserWindow>,
                               public atom::WindowInterface,
                               public atom::WebContentsObserver {
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

  void onWebContentsDeleted(atom::WebContents* contents) override;
  void onWebContentsPaint(atom::WebContents*) override;
  void onWebContentsReadyToShow(atom::WebContents*) override;
  void Draw(NSRect dirty_rect);
  void Resize();
  void Focus(bool focused);
  bool SendInput(base::Value::Dict event);

  bool RequestClose();
  void DidBecomeKey();
  void DidResignKey();
  void DidResize();

  static v8::Persistent<v8::Function> constructor;
  static gin_helper::WrapperInfo kWrapperInfo;

 private:
  static MacBrowserWindow* From(const v8::FunctionCallbackInfo<v8::Value>& info) {
    return static_cast<MacBrowserWindow*>(
        gin_helper::WrappableBase::GetNativePtr(info.This(), &kWrapperInfo));
  }
  static void GetWebContents(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void Close(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void Destroy(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void Show(const v8::FunctionCallbackInfo<v8::Value>& info);
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

  void DestroyNative();

  int id_ = 0;
  NSWindow* window_ = nil;
  NSView* renderer_view_ = nil;
  NSRect normal_bounds_ = NSZeroRect;
  NSObject<NSWindowDelegate>* delegate_ = nil;
  atom::WebContents* web_contents_ = nullptr;
  v8::Global<v8::Object> wrapper_;
  bool closed_ = false;
  bool destroying_ = false;
};

@interface MiniElectronRendererView : NSView <NSTextInputClient>
- (instancetype)initWithFrame:(NSRect)frame owner:(MacBrowserWindow*)owner;
@end

@interface MiniElectronWindowDelegate : NSObject <NSWindowDelegate>
- (instancetype)initWithOwner:(MacBrowserWindow*)owner;
@end

@implementation MiniElectronRendererView {
  MacBrowserWindow* _owner;
  NSMutableAttributedString* _markedText;
}

- (instancetype)initWithFrame:(NSRect)frame owner:(MacBrowserWindow*)owner {
  self = [super initWithFrame:frame];
  if (self) {
    _owner = owner;
    _markedText = [[NSMutableAttributedString alloc] init];
    self.wantsLayer = YES;
  }
  return self;
}
- (void)dealloc {
#if !__has_feature(objc_arc)
  [_markedText release];
  [super dealloc];
#endif
}

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (void)drawRect:(NSRect)dirtyRect { _owner->Draw(dirtyRect); }
- (void)setFrameSize:(NSSize)newSize {
  [super setFrameSize:newSize];
  _owner->Resize();
}
- (BOOL)becomeFirstResponder { _owner->Focus(true); return YES; }
- (BOOL)resignFirstResponder { _owner->Focus(false); return YES; }

- (int)inputModifiers:(NSEvent*)event {
  NSEventModifierFlags flags = event.modifierFlags;
  int result = 0;
  if (flags & NSEventModifierFlagShift) result |= 1;
  if (flags & NSEventModifierFlagControl) result |= 2;
  if (flags & NSEventModifierFlagOption) result |= 4;
  if (flags & NSEventModifierFlagCommand) result |= 8;
  return result;
}

- (void)sendMouse:(NSEvent*)event type:(const char*)type button:(const char*)button {
  NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
  base::Value::Dict input;
  input.Set("type", type);
  input.Set("x", static_cast<int>(std::lround(point.x)));
  input.Set("y", static_cast<int>(std::lround(point.y)));
  input.Set("button", button);
  input.Set("modifiers", [self inputModifiers:event]);
  _owner->SendInput(std::move(input));
}

- (void)mouseMoved:(NSEvent*)event { [self sendMouse:event type:"mouseMove" button:"none"]; }
- (void)mouseDragged:(NSEvent*)event { [self mouseMoved:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [self mouseMoved:event]; }
- (void)otherMouseDragged:(NSEvent*)event { [self mouseMoved:event]; }
- (void)mouseDown:(NSEvent*)event {
  [self.window makeFirstResponder:self];
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
  input.Set("deltaY", event.scrollingDeltaY);
  input.Set("modifiers", [self inputModifiers:event]);
  _owner->SendInput(std::move(input));
}
- (void)keyDown:(NSEvent*)event {
  base::Value::Dict input;
  input.Set("type", "keyDown");
  input.Set("keyCode", static_cast<int>(event.keyCode));
  input.Set("modifiers", [self inputModifiers:event]);
  input.Set("systemKey", (event.modifierFlags & NSEventModifierFlagCommand) != 0);
  _owner->SendInput(std::move(input));
  [self interpretKeyEvents:@[ event ]];
}
- (void)keyUp:(NSEvent*)event {
  base::Value::Dict input;
  input.Set("type", "keyUp");
  input.Set("keyCode", static_cast<int>(event.keyCode));
  input.Set("modifiers", [self inputModifiers:event]);
  input.Set("systemKey", (event.modifierFlags & NSEventModifierFlagCommand) != 0);
  _owner->SendInput(std::move(input));
}


- (void)insertText:(id)value replacementRange:(NSRange)replacementRange {
  [_markedText deleteCharactersInRange:NSMakeRange(0, _markedText.length)];
  NSString* text = [value isKindOfClass:[NSAttributedString class]]
      ? [value string] : value;
  for (NSUInteger index = 0; index < text.length; ++index) {
    base::Value::Dict input;
    input.Set("type", "char");
    input.Set("charCode", static_cast<int>([text characterAtIndex:index]));
    _owner->SendInput(std::move(input));
  }
  (void)replacementRange;
}
- (void)insertText:(id)value { [self insertText:value replacementRange:NSMakeRange(NSNotFound, 0)]; }
- (void)setMarkedText:(id)value selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
  NSAttributedString* text = [value isKindOfClass:[NSAttributedString class]]
      ? value : [[NSAttributedString alloc] initWithString:value];
  [_markedText setAttributedString:text];
  (void)selectedRange;
  (void)replacementRange;
#if !__has_feature(objc_arc)
  if (![value isKindOfClass:[NSAttributedString class]]) [text release];
#endif
}
- (void)unmarkText { [_markedText deleteCharactersInRange:NSMakeRange(0, _markedText.length)]; }
- (BOOL)hasMarkedText { return _markedText.length != 0; }
- (NSRange)markedRange { return self.hasMarkedText ? NSMakeRange(0, _markedText.length) : NSMakeRange(NSNotFound, 0); }
- (NSRange)selectedRange { return NSMakeRange(NSNotFound, 0); }
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText { return @[]; }
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange { return nil; }
- (NSUInteger)characterIndexForPoint:(NSPoint)point { return NSNotFound; }
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
  if (actualRange) *actualRange = range;
  return [self.window convertRectToScreen:[self convertRect:NSMakeRect(0, 0, 1, 20) toView:nil]];
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
- (BOOL)windowShouldClose:(NSWindow*)sender { return _owner->RequestClose(); }
- (void)windowDidBecomeKey:(NSNotification*)notification { _owner->DidBecomeKey(); }
- (void)windowDidResignKey:(NSNotification*)notification { _owner->DidResignKey(); }
- (void)windowDidResize:(NSNotification*)notification { _owner->DidResize(); }
@end

v8::Persistent<v8::Function> MacBrowserWindow::constructor;
gin_helper::WrapperInfo MacBrowserWindow::kWrapperInfo = {
    gin_helper::GinEmbedder::kEmbedderNativeGin};

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
  std::string title = State().name;
  std::string title_bar_style;
  options.GetBydefaultVal("width", width, &width);
  options.GetBydefaultVal("height", height, &height);
  bool has_position = options.Get("x", &x) && options.Get("y", &y);
  options.GetBydefaultVal("show", show, &show);
  options.GetBydefaultVal("resizable", resizable, &resizable);
  options.GetBydefaultVal("minimizable", minimizable, &minimizable);
  options.GetBydefaultVal("closable", closable, &closable);
  options.GetBydefaultVal("frame", frame, &frame);
  options.GetBydefaultVal("transparent", transparent, &transparent);
  options.GetBydefaultVal("title", title, &title);
  options.GetBydefaultVal("titleBarStyle", "", &title_bar_style);

  NSWindowStyleMask style = frame ? NSWindowStyleMaskTitled : NSWindowStyleMaskBorderless;
  if (closable) style |= NSWindowStyleMaskClosable;
  if (minimizable) style |= NSWindowStyleMaskMiniaturizable;
  if (resizable) style |= NSWindowStyleMaskResizable;
  if (title_bar_style == "hidden" ||
      title_bar_style == "hiddenInset" ||
      title_bar_style == "customButtonsOnHover") {
    style |= NSWindowStyleMaskFullSizeContentView;
  }
  NSRect bounds = CocoaRectFromElectron(
      NSMakeRect(x, y, std::max(1, width), std::max(1, height)));
  bounds = [NSWindow contentRectForFrameRect:bounds styleMask:style];
  window_ = [[NSWindow alloc] initWithContentRect:bounds
                                        styleMask:style
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  window_.title = NSStringFromUtf8(title);
  window_.opaque = !transparent;
  if (transparent) window_.backgroundColor = NSColor.clearColor;
  if (!has_position)
    [window_ center];
  if (!title_bar_style.empty()) {
    window_.titleVisibility = NSWindowTitleHidden;
    window_.titlebarAppearsTransparent = YES;
  }
  gin_helper::Dictionary traffic_lights =
      gin_helper::Dictionary::CreateEmpty(isolate);
  if (options.Get("trafficLightPosition", &traffic_lights)) {
    int light_x = 0;
    int light_y = 0;
    if (traffic_lights.Get("x", &light_x) &&
        traffic_lights.Get("y", &light_y)) {
      const NSWindowButton button_types[] = {
          NSWindowCloseButton,
          NSWindowMiniaturizeButton,
          NSWindowZoomButton};
      for (size_t index = 0; index < std::size(button_types); ++index) {
        NSButton* button = [window_ standardWindowButton:button_types[index]];
        if (button)
          [button setFrameOrigin:NSMakePoint(
              light_x + static_cast<int>(index) * 20, light_y)];
      }
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
  [window_.contentView addSubview:renderer_view_];

  gin_helper::Dictionary web_preferences =
      gin_helper::Dictionary::CreateEmpty(isolate);
  options.Get("webPreferences", &web_preferences);
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
    [window_ makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
  }

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
  auto& windows = State().windows;
  windows.erase(std::remove(windows.begin(), windows.end(), this), windows.end());
  atom::WindowList::removeWindow(this);
  if (web_contents_) {
    web_contents_->removeObserver(this);
    web_contents_->destroyed();
    web_contents_ = nullptr;
  }
  NSWindow* window = window_;
  MiniElectronWindowDelegate* delegate = delegate_;
  MiniElectronRendererView* renderer_view = renderer_view_;
  window_ = nil;
  delegate_ = nil;
  renderer_view_ = nil;
  if (window) {
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

bool MacBrowserWindow::RequestClose() {
  if (closed_)
    return YES;
  if (EmitCancelable(getWrapper(), "close"))
    return NO;
  DestroyNative();
  return YES;
}

void MacBrowserWindow::onWebContentsDeleted(atom::WebContents* contents) {
  if (web_contents_ == contents)
    web_contents_ = nullptr;
}

void MacBrowserWindow::onWebContentsPaint(atom::WebContents*) {
  [renderer_view_ setNeedsDisplay:YES];
}

void MacBrowserWindow::onWebContentsReadyToShow(atom::WebContents*) {
  Emit(getWrapper(), "ready-to-show");
}

void MacBrowserWindow::Draw(NSRect dirty_rect) {
  [[NSColor colorWithRed:0.047 green:0.067 blue:0.106 alpha:1.0] setFill];
  NSRectFill(dirty_rect);
  if (!web_contents_)
    return;
  atom::RendererFrameSnapshot frame;
  if (!web_contents_->copyFrame(&frame) || frame.rgba.empty() || frame.width <= 0 ||
      frame.height <= 0 || frame.stride <= 0) {
    return;
  }
  CGDataProviderRef provider = CGDataProviderCreateWithData(
      nullptr, frame.rgba.data(), frame.rgba.size(), nullptr);
  CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
  CGImageRef image = CGImageCreate(
      frame.width, frame.height, 8, 32, frame.stride, color_space,
      kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast, provider,
      nullptr, false, kCGRenderingIntentDefault);
  CGContextRef context = NSGraphicsContext.currentContext.CGContext;
  CGContextSaveGState(context);
  CGContextTranslateCTM(context, 0, renderer_view_.bounds.size.height);
  CGContextScaleCTM(context, 1, -1);
  CGContextDrawImage(context,
                     CGRectMake(0, 0, renderer_view_.bounds.size.width,
                                renderer_view_.bounds.size.height),
                     image);
  CGContextRestoreGState(context);
  CGImageRelease(image);
  CGColorSpaceRelease(color_space);
  CGDataProviderRelease(provider);
}

void MacBrowserWindow::Resize() {
  if (!web_contents_ || !renderer_view_)
    return;
  NSSize size = renderer_view_.bounds.size;
  const double scale = window_ ? window_.backingScaleFactor : 1.0;
  web_contents_->resize(std::max(1, static_cast<int>(std::lround(size.width))),
                        std::max(1, static_cast<int>(std::lround(size.height))),
                        scale);
}

void MacBrowserWindow::Focus(bool focused) {
  if (web_contents_)
    web_contents_->setFocus(focused);
}

bool MacBrowserWindow::SendInput(base::Value::Dict event) {
  return web_contents_ && web_contents_->sendInput(std::move(event));
}

void MacBrowserWindow::DidBecomeKey() { Focus(true); Emit(getWrapper(), "focus"); }
void MacBrowserWindow::DidResignKey() { Focus(false); Emit(getWrapper(), "blur"); }
void MacBrowserWindow::DidResize() {
  if (window_ && !window_.zoomed &&
      (window_.styleMask & NSWindowStyleMaskFullScreen) == 0) {
    normal_bounds_ = window_.frame;
  }
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
void MacBrowserWindow::Show(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_) [self->window_ makeKeyAndOrderFront:nil]; }
void MacBrowserWindow::Hide(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_) [self->window_ orderOut:nil]; }
void MacBrowserWindow::FocusWindow(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_) [self->window_ makeKeyAndOrderFront:nil]; }
void MacBrowserWindow::BlurWindow(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_) [self->window_ resignKeyWindow]; }
void MacBrowserWindow::IsFocused(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info)) info.GetReturnValue().Set(self->window_.keyWindow); }
void MacBrowserWindow::IsVisible(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info)) info.GetReturnValue().Set(self->window_.visible); }
void MacBrowserWindow::IsDestroyed(const v8::FunctionCallbackInfo<v8::Value>& info) { MacBrowserWindow* self = From(info); info.GetReturnValue().Set(!self || self->closed_); }
void MacBrowserWindow::Maximize(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_ && !self->window_.zoomed) [self->window_ zoom:nil]; }
void MacBrowserWindow::Unmaximize(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_ && self->window_.zoomed) [self->window_ zoom:nil]; }
void MacBrowserWindow::IsMaximized(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info)) info.GetReturnValue().Set(self->window_.zoomed); }
void MacBrowserWindow::Minimize(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_) [self->window_ miniaturize:nil]; }
void MacBrowserWindow::Restore(const v8::FunctionCallbackInfo<v8::Value>& info) { if (auto* self = From(info); self && self->window_) [self->window_ deminiaturize:nil]; }
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
  self->window_.contentMinSize = NSMakeSize(
      std::max(1, info[0]->Int32Value(info.GetIsolate()->GetCurrentContext()).FromMaybe(1)),
      std::max(1, info[1]->Int32Value(info.GetIsolate()->GetCurrentContext()).FromMaybe(1)));
}
void MacBrowserWindow::SetBackgroundColor(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserWindow* self = From(info);
  if (!self || !self->window_ || !info.Length())
    return;
  std::string color = Utf8(info.GetIsolate(), info[0]);
  unsigned value = 0;
  if (color.size() == 7 && color[0] == '#' &&
      std::sscanf(color.c_str() + 1, "%06x", &value) == 1) {
    self->window_.backgroundColor = [NSColor
        colorWithRed:((value >> 16) & 0xff) / 255.0
               green:((value >> 8) & 0xff) / 255.0
                blue:(value & 0xff) / 255.0
               alpha:1.0];
  }
}
void MacBrowserWindow::SetTitleBarOverlay(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (auto* self = From(info); self && self->window_) {
    self->window_.titleVisibility = NSWindowTitleHidden;
    self->window_.titlebarAppearsTransparent = YES;
  }
}
void MacBrowserWindow::SetWindowButtonPosition(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  MacBrowserWindow* self = From(info);
  if (!self || !self->window_ || !info.Length() || !info[0]->IsObject())
    return;
  gin_helper::Dictionary point(info.GetIsolate(), info[0].As<v8::Object>());
  int x = 0;
  int y = 0;
  if (!point.Get("x", &x) || !point.Get("y", &y))
    return;
  for (NSWindowButton button_type : {
           NSWindowCloseButton,
           NSWindowMiniaturizeButton,
           NSWindowZoomButton}) {
    NSButton* button = [self->window_ standardWindowButton:button_type];
    if (!button)
      continue;
    NSPoint origin = button.frame.origin;
    origin.y = y;
    [button setFrameOrigin:origin];
  }
  NSButton* close_button =
      [self->window_ standardWindowButton:NSWindowCloseButton];
  if (close_button)
    [close_button setFrameOrigin:NSMakePoint(x, close_button.frame.origin.y)];
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
  gin_helper::ObjectTemplateBuilder builder(isolate, type->InstanceTemplate());
  builder.SetMethod("_getWebContents", &MacBrowserWindow::GetWebContents);
  builder.SetMethod("close", &MacBrowserWindow::Close);
  builder.SetMethod("destroy", &MacBrowserWindow::Destroy);
  builder.SetMethod("show", &MacBrowserWindow::Show);
  builder.SetMethod("showInactive", &MacBrowserWindow::Show);
  builder.SetMethod("hide", &MacBrowserWindow::Hide);
  builder.SetMethod("focus", &MacBrowserWindow::FocusWindow);
  builder.SetMethod("blur", &MacBrowserWindow::BlurWindow);
  builder.SetMethod("isFocused", &MacBrowserWindow::IsFocused);
  builder.SetMethod("isVisible", &MacBrowserWindow::IsVisible);
  builder.SetMethod("isDestroyed", &MacBrowserWindow::IsDestroyed);
  builder.SetMethod("maximize", &MacBrowserWindow::Maximize);
  builder.SetMethod("unmaximize", &MacBrowserWindow::Unmaximize);
  builder.SetMethod("isMaximized", &MacBrowserWindow::IsMaximized);
  builder.SetMethod("minimize", &MacBrowserWindow::Minimize);
  builder.SetMethod("restore", &MacBrowserWindow::Restore);
  builder.SetMethod("isMinimized", &MacBrowserWindow::IsMinimized);
  builder.SetMethod("setFullScreen", &MacBrowserWindow::SetFullScreen);
  builder.SetMethod("isFullScreen", &MacBrowserWindow::IsFullScreen);
  builder.SetMethod("_setTitle", &MacBrowserWindow::SetTitle);
  builder.SetMethod("getTitle", &MacBrowserWindow::GetTitle);
  builder.SetMethod("getBounds", &MacBrowserWindow::GetBounds);
  builder.SetMethod("setBounds", &MacBrowserWindow::SetBounds);
  builder.SetMethod("getNormalBounds", &MacBrowserWindow::GetNormalBounds);
  builder.SetMethod("setPosition", &MacBrowserWindow::SetPosition);
  builder.SetMethod("setSize", &MacBrowserWindow::SetSize);
  builder.SetMethod("setMinimumSize", &MacBrowserWindow::SetMinimumSize);
  builder.SetMethod("setBackgroundColor", &MacBrowserWindow::SetBackgroundColor);
  builder.SetMethod("setTitleBarOverlay", &MacBrowserWindow::SetTitleBarOverlay);
  builder.SetMethod("setWindowButtonPosition", &MacBrowserWindow::SetWindowButtonPosition);
  builder.SetMethod("center", &MacBrowserWindow::Center);
  builder.SetMethod("setAlwaysOnTop", &MacBrowserWindow::SetAlwaysOnTop);
  builder.SetMethod("isAlwaysOnTop", &MacBrowserWindow::IsAlwaysOnTop);
  builder.SetMethod("setDocumentEdited", &MacBrowserWindow::SetDocumentEdited);
  builder.SetMethod("isDocumentEdited", &MacBrowserWindow::IsDocumentEdited);
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

uv_prepare_t* g_prepare = nullptr;
uv_check_t* g_check = nullptr;
uv_loop_t* g_loop = nullptr;

void PumpAppKit() {
  @autoreleasepool {
    constexpr int kMaximumEventsPerTurn = 64;
    for (int count = 0; count < kMaximumEventsPerTurn; ++count) {
      NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                          untilDate:NSDate.distantPast
                                             inMode:NSDefaultRunLoopMode
                                            dequeue:YES];
      if (!event)
        break;
      [NSApp sendEvent:event];
    }
    [NSApp updateWindows];
    base::RunLoop().RunUntilIdle();
  }
}

void PumpPrepare(uv_prepare_t*) { PumpAppKit(); }
void PumpCheck(uv_check_t*) { PumpAppKit(); }

}  // namespace

void InstallMacElectronBindings(node::Environment* environment,
                                const MacBindingOptions& options) {
  HostState& state = State();
  state.environment = environment;
  state.isolate = node::GetMainContext(environment)->GetIsolate();
  state.context.Reset(state.isolate, node::GetMainContext(environment));
  state.options = options;
  state.app_path = options.application_path;
  state.packaged = options.is_packaged;
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
  InstallMacPlatformApiBindings(environment);
}

int GetMacMainProcessExitCode() {
  return State().exit_code;
}

bool InstallMacRunLoopIntegration(uv_loop_s* loop) {
  if (!loop || g_loop)
    return false;
  g_prepare = new uv_prepare_t();
  g_check = new uv_check_t();
  if (uv_prepare_init(loop, g_prepare) != 0 ||
      uv_check_init(loop, g_check) != 0) {
    delete g_prepare;
    delete g_check;
    g_prepare = nullptr;
    g_check = nullptr;
    return false;
  }
  if (uv_prepare_start(g_prepare, PumpPrepare) != 0 ||
      uv_check_start(g_check, PumpCheck) != 0) {
    uv_close(reinterpret_cast<uv_handle_t*>(g_prepare), nullptr);
    uv_close(reinterpret_cast<uv_handle_t*>(g_check), nullptr);
    uv_run(loop, UV_RUN_NOWAIT);
    delete g_prepare;
    delete g_check;
    g_prepare = nullptr;
    g_check = nullptr;
    return false;
  }
  g_loop = loop;
  return true;
}

void ShutdownMacRunLoopIntegration() {
  DestroyAllWindows();
  HostState& state = State();
  if (!state.single_instance_notification.empty()) {
    [[NSDistributedNotificationCenter defaultCenter]
        removeObserver:g_application_delegate
                  name:NSStringFromUtf8(state.single_instance_notification)
                object:nil];
    state.single_instance_notification.clear();
  }
  NSApp.delegate = nil;
#if !__has_feature(objc_arc)
  [g_application_delegate release];
#endif
  g_application_delegate = nil;
  if (state.single_instance_fd >= 0) {
    flock(state.single_instance_fd, LOCK_UN);
    close(state.single_instance_fd);
    state.single_instance_fd = -1;
  }
  state.app.Reset();
  state.context.Reset();
  state.environment = nullptr;
  state.isolate = nullptr;

  if (!g_loop)
    return;
  uv_prepare_stop(g_prepare);
  uv_check_stop(g_check);
  uv_close(reinterpret_cast<uv_handle_t*>(g_prepare),
           [](uv_handle_t* handle) { delete reinterpret_cast<uv_prepare_t*>(handle); });
  uv_close(reinterpret_cast<uv_handle_t*>(g_check),
           [](uv_handle_t* handle) { delete reinterpret_cast<uv_check_t*>(handle); });
  uv_run(g_loop, UV_RUN_NOWAIT);
  g_prepare = nullptr;
  g_check = nullptr;
  g_loop = nullptr;
}

}  // namespace mini_electron::electron
