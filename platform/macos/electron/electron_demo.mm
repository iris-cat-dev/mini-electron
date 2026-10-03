// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#import <AppKit/AppKit.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "platform/macos/electron/node_runtime.h"
#include "runtime/electron/common/renderer_server.h"

#ifndef MINI_ELECTRON_SOURCE_ROOT
#define MINI_ELECTRON_SOURCE_ROOT ""
#endif

namespace {


bool IsRegularFile(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error);
}

bool IsApplication(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_directory(path, error) || IsRegularFile(path);
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
      MINI_ELECTRON_SOURCE_ROOT) / "runtime/electron/lib/browser/electron.js";
  if (IsRegularFile(source))
    return source;

  source = Absolute("runtime/electron/lib/browser/electron.js");
  return IsRegularFile(source) ? source : std::filesystem::path();
}

std::filesystem::path FindPackagedApplication(
    const std::filesystem::path& resources_path) {
  const std::filesystem::path asar = resources_path / "app.asar";
  if (IsRegularFile(asar))
    return asar;
  const std::filesystem::path directory = resources_path / "app";
  return IsApplication(directory) ? directory : std::filesystem::path();
}

}  // namespace

int main(int argc, char** argv) {
  // Renderer mode is selected before Node, AppKit singleton creation, linked
  // binding registration, or any trusted main-process state.
  if (atom::IsRendererProcess(argc, argv))
    return atom::RunRendererProcess(argc, argv);

  if (std::getenv("ELECTRON_RUN_AS_NODE"))
    return mini_electron::electron::RunAsNode(argc, argv);

  @autoreleasepool {
    const std::filesystem::path executable = Absolute(argv[0]);
    const std::filesystem::path resources =
        Absolute([NSBundle.mainBundle.resourcePath fileSystemRepresentation]);
    std::filesystem::path application = FindPackagedApplication(resources);
    const bool packaged = !application.empty();

    int first_application_argument = 1;
    if (!packaged && argc > 1 && argv[1][0] != '-') {
      application = Absolute(argv[1]);
      first_application_argument = 2;
    }
    if (application.empty() || !IsApplication(application)) {
      std::fputs(
          "mini-electron: no application found; pass an app directory/main "
          "script or package Contents/Resources/app[.asar]\n",
          stderr);
      return 2;
    }

    const std::filesystem::path browser_module = FindBrowserModule(resources);
    if (browser_module.empty()) {
      std::fputs(
          "mini-electron: shared Electron browser module is missing from "
          "Contents/Resources/mini-electron\n",
          stderr);
      return 2;
    }

    mini_electron::electron::MainProcessOptions options;
    options.executable_path = executable.string();
    options.application_path = application.string();
    options.browser_module_path = browser_module.string();
    options.resources_path = resources.string();
    options.is_packaged = packaged;
    for (int index = first_application_argument; index < argc; ++index)
      options.application_arguments.emplace_back(argv[index]);

    [NSApplication sharedApplication];
    return mini_electron::electron::RunMainProcess(options);
  }
}
