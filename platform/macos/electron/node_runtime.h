// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef PLATFORM_MACOS_ELECTRON_NODE_RUNTIME_H_
#define PLATFORM_MACOS_ELECTRON_NODE_RUNTIME_H_

#include <string>
#include <vector>

namespace mini_electron::electron {

struct MainProcessOptions {
  std::string executable_path;
  std::string application_path;
  std::string browser_module_path;
  std::string resources_path;
  std::vector<std::string> application_arguments;
  bool is_packaged = false;
};

// Runs the binary with ordinary Node semantics. This path never initializes the
// browser host or renderer engine and is used by ELECTRON_RUN_AS_NODE children.
int RunAsNode(int argc, char** argv);

// Creates the trusted Node main process, installs only main-process linked
// bindings, loads the shared Electron JavaScript module, and then executes the
// application's unmodified package entry point. The Node/libuv loop is kept on
// the AppKit main thread.
int RunMainProcess(const MainProcessOptions& options);

}  // namespace mini_electron::electron

#endif  // PLATFORM_MACOS_ELECTRON_NODE_RUNTIME_H_
