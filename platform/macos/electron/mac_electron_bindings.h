// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef PLATFORM_MACOS_ELECTRON_MAC_ELECTRON_BINDINGS_H_
#define PLATFORM_MACOS_ELECTRON_MAC_ELECTRON_BINDINGS_H_

#include <functional>
#include <memory>
#include <string>
#include "v8.h"

struct uv_loop_s;

namespace base {
class MessagePump;
}

namespace node {
class Environment;
}

namespace mini_electron::electron {

struct MacBindingOptions {
  std::string resources_path;
};

// Registration is per Environment so Worker threads do not accidentally gain
// browser authority. Renderer processes never call this function.
void InstallMacElectronBindings(node::Environment* environment,
                                const MacBindingOptions& options);
int GetMacMainProcessExitCode();

// AppKit's continuous UI pump, entering Node's callback scope for Chromium work.
std::unique_ptr<base::MessagePump> CreateMacMainMessagePump();

// Runs AppKit, Chromium and Node until app.quit()/app.exit()/process.exit().
// Call after InstallMacRunLoopIntegration() and browser bootstrap.
void RunMacMainLoop();
// process.exit() handler: destroys windows without app events, matching the
// Windows host, and stops the main loop.
void ExitMacMainProcess(int exit_code);

// Bridges libuv backend readiness and its next deadline into AppKit's run loop.
// All libuv callbacks run on the main thread. Shutdown joins the readiness
// waiter and removes native sources before the JavaScript Environment is gone.
bool InstallMacRunLoopIntegration(uv_loop_s* loop);
void ShutdownMacRunLoopIntegration();

// Runs |callback| inside the main Environment's HandleScope, context, and a
// Node CallbackScope (microtasks and async hooks). AppKit delegates, GCD main
// queue blocks, and NSNotification observers run outside any V8 scope and must
// enter JavaScript through this. Returns false when the Environment is gone.
bool RunInMainJavaScriptScope(const std::function<void()>& callback);
// Caller must already be inside the main JavaScript scope.
v8::Local<v8::Object> GetFocusedBrowserWindow(v8::Isolate* isolate);
v8::Local<v8::Object> GetBrowserWindowForNativeWindow(void* native_window);

}  // namespace mini_electron::electron

#endif  // PLATFORM_MACOS_ELECTRON_MAC_ELECTRON_BINDINGS_H_
