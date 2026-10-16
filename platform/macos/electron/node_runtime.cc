// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "platform/macos/electron/node_runtime.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "base/message_loop/message_pump_type.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "runtime/electron/common/gin_helper/per_isolate_data.h"
#include "runtime/electron/common/atom_version.h"
#include "runtime/electron/common/chrome_version.h"

#include "platform/macos/electron/desktop_api_mac.h"
#include "platform/macos/electron/mac_auto_updater.h"
#include "platform/macos/electron/mac_electron_bindings.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libuv/include/uv.h"

extern "C" void _register_electron_common_asar();
extern "C" void _register_electron_browser_commandline();
extern "C" void _register_electron_browser_downloaditem();
extern "C" void _register_electron_browser_protocol();
extern "C" void _register_electron_browser_session();
extern "C" void _register_electron_browser_safe_storage();
extern "C" void _register_electron_browser_webrequest();

namespace mini_electron::electron {
namespace {

std::string QuoteJavaScriptString(const std::string& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() + 2);
  result.push_back('"');
  for (unsigned char character : value) {
    switch (character) {
      case '\\': result += "\\\\"; break;
      case '"': result += "\\\""; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (character < 0x20) {
          result += "\\u00";
          result.push_back(kHex[character >> 4]);
          result.push_back(kHex[character & 0x0f]);
        } else {
          result.push_back(static_cast<char>(character));
        }
    }
  }
  result.push_back('"');
  return result;
}

std::string BrowserBootstrap(const MainProcessOptions& options) {
  std::string source = "'use strict';\nObject.defineProperties(process.versions, {"
      "electron: {value: '" ATOM_VERSION_STRING "'},"
      "miniElectron: {value: '" MINI_ELECTRON_VERSION_STRING "'},"
      "chrome: {value: '" CHROME_VERSION_STRING "'}"
      "});\nObject.defineProperty(process, 'resourcesPath', { configurable: true, value: ";
  source += QuoteJavaScriptString(options.resources_path);
  source += " });\nconst filename = require('path').join(require('path').dirname(";
  source += QuoteJavaScriptString(options.browser_module_path);
  source += "), 'init.js');\nrequire('module').createRequire(filename)(filename);\n";
  return source;
}

void PrintErrors(const char* phase, const std::vector<std::string>& errors) {
  for (const std::string& error : errors)
    std::fprintf(stderr, "mini-electron: %s: %s\n", phase, error.c_str());
}

// Runs libuv until app.quit()/app.exit()/process.exit() requests shutdown.
// The AppKit pump handles keep the loop alive like the Windows UI message loop.
void RunMainLoop(node::MultiIsolatePlatform* platform,
                 v8::Isolate* isolate,
                 uv_loop_t* loop) {
  while (!MacMainLoopExitRequested()) {
    uv_run(loop, UV_RUN_DEFAULT);
    if (MacMainLoopExitRequested())
      break;
    platform->DrainTasks(isolate);
    if (!uv_loop_alive(loop))
      break;
  }
}

}  // namespace

int RunAsNode(int argc, char** argv) {
  _register_electron_common_asar();
  return node::Start(argc, argv);
}

int RunMainProcess(const MainProcessOptions& options) {
  _register_electron_common_asar();
  _register_electron_browser_commandline();
  _register_electron_browser_downloaditem();
  _register_electron_browser_protocol();
  _register_electron_browser_safe_storage();
  _register_electron_browser_session();
  _register_electron_browser_webrequest();
  mini_electron::mac::RegisterMacDesktopApiModule();
  // Linked modules must be registered before node::InitializeOncePerProcess;
  // afterwards node_module_register treats them as pending dlopen addons.
  // Same process options as the Windows host: Electron's main process warns
  // on unhandled rejections instead of crashing, and CommonJS detection stays
  // off.
  std::vector<std::string> initialization_args = {
      options.executable_path, "--no-experimental-detect-module",
      "--unhandled-rejections=warn"};
  std::shared_ptr<node::InitializationResult> initialization =
      node::InitializeOncePerProcess(
          initialization_args,
          node::ProcessInitializationFlags::kEnableStdioInheritance);
  if (!initialization) {
    std::fputs("mini-electron: Node process initialization returned no result\n",
               stderr);
    return 1;
  }
  PrintErrors("Node initialization", initialization->errors());
  if (initialization->early_return())
    return initialization->exit_code();

  base::SingleThreadTaskExecutor task_executor(base::MessagePumpType::UI);
  // Session network/file brokers run blocking work on the Chromium thread
  // pool, matching the Windows host's main-process lifecycle.
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("mini-electron-main");

  std::vector<std::string> process_args = {options.executable_path};
  process_args.insert(process_args.end(), options.arguments.begin(),
                      options.arguments.end());

  std::vector<std::string> setup_errors;
  std::unique_ptr<node::CommonEnvironmentSetup> setup =
      node::CommonEnvironmentSetup::Create(
          initialization->platform(), &setup_errors, process_args,
          initialization->exec_args());
  if (!setup) {
    PrintErrors("Node environment", setup_errors);
    base::ThreadPoolInstance::Get()->Shutdown();
    node::TearDownOncePerProcess();
    return 1;
  }

  std::optional<int> process_exit;
  const int exit_code = [&]() {
    v8::Isolate* isolate = setup->isolate();
    v8::Locker locker(isolate);
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handles(isolate);
    v8::Context::Scope context_scope(setup->context());
    gin_helper::PerIsolateData gin_isolate_data(
        isolate, setup->array_buffer_allocator().get());
    MacBindingOptions binding_options;
    binding_options.resources_path = options.resources_path;
    InstallMacElectronBindings(setup->env(), binding_options);
    InstallMacAutoUpdaterBinding(setup->env());
    if (!InstallMacRunLoopIntegration(setup->event_loop())) {
      ShutdownMacAutoUpdaterBinding();
      std::fputs("mini-electron: failed to integrate AppKit and Node event loops\n", stderr);
      return 1;
    }
    // process.exit() has already emitted 'exit'; like the Windows host, stop
    // the loop and tear down instead of calling exit() mid-JavaScript.
    node::SetProcessExitHandler(
        setup->env(), [&](node::Environment* environment, int code) {
          process_exit = code;
          ExitMacMainProcess(code);
          node::Stop(environment);
        });
    const bool loaded =
        !node::LoadEnvironment(setup->env(), BrowserBootstrap(options)).IsEmpty();
    if (loaded && !process_exit)
      RunMainLoop(initialization->platform(), isolate, setup->event_loop());
    else if (!process_exit)
      std::fputs("mini-electron: browser bootstrap failed\n", stderr);
    const int result =
        process_exit.value_or(loaded ? GetMacMainProcessExitCode() : 1);
    if (!process_exit)
      (void)node::EmitProcessExit(setup->env());
    ShutdownMacAutoUpdaterBinding();
    ShutdownMacRunLoopIntegration();
    node::Stop(setup->env(), node::StopFlags::kDoNotTerminateIsolate);
    return result;
  }();
  base::ThreadPoolInstance::Get()->Shutdown();
  setup.reset();
  node::TearDownOncePerProcess();
  return exit_code;
}

}  // namespace mini_electron::electron
