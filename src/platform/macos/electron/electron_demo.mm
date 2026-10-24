// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "src/platform/macos/electron/node_runtime.h"
#include "src/runtime/electron/common/renderer_server.h"
#include "src/runtime/engine/renderer/render_thread_impl.h"

#ifndef MINI_ELECTRON_SOURCE_ROOT
#define MINI_ELECTRON_SOURCE_ROOT ""
#endif

namespace {
int RunMacRendererProcess(int argc, char** argv) {
  @autoreleasepool {
    base::AtExitManager at_exit_manager;
    base::CommandLine::Init(argc, argv);

    // AppKit lazily creates block-backed methods while handling native events.
    // Load libobjc's executable trampolines before the renderer enters seatbelt.
    IMP trampoline = imp_implementationWithBlock(^(id) {});
    imp_removeBlock(trampoline);
    [NSApplication sharedApplication];

    // Even a windowless LSUIElement must consume AppKit events. An NSRunLoop
    // alone leaves WindowServer probes unanswered while Blink keeps rendering.
    // The engine borrows this UI sequence; its Blink thread keeps NS_RUNLOOP.
    base::SingleThreadTaskExecutor task_executor(base::MessagePumpType::UI);
    const int result = atom::RunRendererProcess(argc, argv);
    content::RenderThreadImpl::shutdown();
    if (base::ThreadPoolInstance::Get())
      base::ThreadPoolInstance::Get()->Shutdown();
    return result;
  }
}



bool IsRegularFile(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error);
}


std::filesystem::path Absolute(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::path absolute = std::filesystem::absolute(path, error);
  return error ? path.lexically_normal() : absolute.lexically_normal();
}

std::filesystem::path FindBrowserModule(
    const std::filesystem::path& resources_path) {
  if (const char* configured = std::getenv("MINI_ELECTRON_BROWSER_MODULE")) {
    std::filesystem::path path = Absolute(configured);
    if (IsRegularFile(path))
      return path;
  }

  std::filesystem::path packaged =
      resources_path / "mini-electron/lib/browser/electron.js";
  if (IsRegularFile(packaged))
    return packaged;

  std::filesystem::path source = std::filesystem::path(
      MINI_ELECTRON_SOURCE_ROOT) / "src/runtime/electron/lib/browser/electron.js";
  if (IsRegularFile(source))
    return source;

  source = Absolute("src/runtime/electron/lib/browser/electron.js");
  return IsRegularFile(source) ? source : std::filesystem::path();
}


}  // namespace

int main(int argc, char** argv) {
  // Select renderer mode before Node, linked bindings or trusted browser state.
  if (atom::IsRendererProcess(argc, argv))
    return RunMacRendererProcess(argc, argv);

  if (std::getenv("ELECTRON_RUN_AS_NODE"))
    return mini_electron::electron::RunAsNode(argc, argv);

  @autoreleasepool {
    base::AtExitManager at_exit_manager;
    base::CommandLine::Init(argc, argv);
    const std::filesystem::path executable = Absolute(argv[0]);
    const std::filesystem::path resources =
        Absolute([NSBundle.mainBundle.resourcePath fileSystemRepresentation]);
    const std::filesystem::path browser_module = FindBrowserModule(resources);
    if (browser_module.empty()) {
      std::fputs(
          "mini-electron: shared Electron browser module is missing from "
          "Contents/Resources/mini-electron\n",
          stderr);
      return 2;
    }

    // Like the Windows host, application discovery (packaged app.asar/app,
    // an app directory or script argument, --version/--abi/--help, and the
    // bundled default app) belongs to the shared browser init.js.
    mini_electron::electron::MainProcessOptions options;
    options.executable_path = executable.string();
    options.browser_module_path = browser_module.string();
    options.resources_path = resources.string();
    for (int index = 1; index < argc; ++index)
      options.arguments.emplace_back(argv[index]);

    [NSApplication sharedApplication];
    return mini_electron::electron::RunMainProcess(options);
  }
}
