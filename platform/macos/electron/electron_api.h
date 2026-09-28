// Copyright 2026 The miniblink132 Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef PLATFORM_MACOS_ELECTRON_ELECTRON_API_H_
#define PLATFORM_MACOS_ELECTRON_ELECTRON_API_H_

#include <optional>
#include <string>
#include <vector>

namespace miniblink::electron {

struct BrowserWindowOptions {
  int width = 800;
  int height = 600;
  bool resizable = true;
  bool visible = true;
  std::string title = "Electron";
  std::string url;
};

struct AppPlan {
  std::vector<BrowserWindowOptions> windows;
  bool quit_requested = false;
};

// Evaluates an Electron-style main-process script. The supported API surface is
// require('electron'), app.whenReady()/on('ready')/quit(), and BrowserWindow's
// constructor, loadFile/loadURL, visibility, title, bounds, and webContents URL.
std::optional<AppPlan> EvaluateMainScript(const std::string& script_path,
                                          std::string* error);

}  // namespace miniblink::electron

#endif  // PLATFORM_MACOS_ELECTRON_ELECTRON_API_H_
