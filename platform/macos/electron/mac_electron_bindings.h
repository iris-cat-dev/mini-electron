// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef PLATFORM_MACOS_ELECTRON_MAC_ELECTRON_BINDINGS_H_
#define PLATFORM_MACOS_ELECTRON_MAC_ELECTRON_BINDINGS_H_

#include <functional>
#include <string>

struct uv_loop_s;

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

// app.quit()/app.exit() and process.exit() request the main loop to stop;
// RunMainProcess polls this between libuv turns.
bool MacMainLoopExitRequested();
// process.exit() handler: destroys windows without app events, matching the
// Windows host, and stops the main loop.
void ExitMacMainProcess(int exit_code);

// Installs AppKit event pumping on Node's libuv loop. The returned handles are
// owned by the loop and are closed by ShutdownMacRunLoopIntegration().
bool InstallMacRunLoopIntegration(uv_loop_s* loop);
void ShutdownMacRunLoopIntegration();

// Runs |callback| inside the main Environment's HandleScope, context, and a
// Node CallbackScope (microtasks and async hooks). AppKit delegates, GCD main
// queue blocks, and NSNotification observers run outside any V8 scope and must
// enter JavaScript through this. Returns false when the Environment is gone.
bool RunInMainJavaScriptScope(const std::function<void()>& callback);

}  // namespace mini_electron::electron

#endif  // PLATFORM_MACOS_ELECTRON_MAC_ELECTRON_BINDINGS_H_
