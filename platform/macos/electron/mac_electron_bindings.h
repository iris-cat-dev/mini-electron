// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef PLATFORM_MACOS_ELECTRON_MAC_ELECTRON_BINDINGS_H_
#define PLATFORM_MACOS_ELECTRON_MAC_ELECTRON_BINDINGS_H_

#include <string>

struct uv_loop_s;

namespace node {
class Environment;
}

namespace mini_electron::electron {

struct MacBindingOptions {
  std::string resources_path;
  std::string application_path;
  bool is_packaged = false;
};

// Registration is per Environment so Worker threads do not accidentally gain
// browser authority. Renderer processes never call this function.
void InstallMacElectronBindings(node::Environment* environment,
                                const MacBindingOptions& options);
int GetMacMainProcessExitCode();

// Installs AppKit event pumping on Node's libuv loop. The returned handles are
// owned by the loop and are closed by ShutdownMacRunLoopIntegration().
bool InstallMacRunLoopIntegration(uv_loop_s* loop);
void ShutdownMacRunLoopIntegration();

}  // namespace mini_electron::electron

#endif  // PLATFORM_MACOS_ELECTRON_MAC_ELECTRON_BINDINGS_H_
