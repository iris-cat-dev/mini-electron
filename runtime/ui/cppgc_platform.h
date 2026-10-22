// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef MINI_UI_CPPGC_PLATFORM_H_
#define MINI_UI_CPPGC_PLATFORM_H_

#include <memory>

namespace cppgc {
class Platform;
}

namespace mini_ui {

// Process initialization is paired with shutdown after the last static UI
// owner is destroyed. No V8 JavaScript platform or isolate is initialized.
void InitializeCppgcProcess();
std::shared_ptr<cppgc::Platform> GetCppgcPlatform();

// Runs due foreground GC work on the thread which initialized the platform.
// Uses the same monotonic clock as cppgc task scheduling.
void RunCppgcTasks();

} // namespace mini_ui

#endif // MINI_UI_CPPGC_PLATFORM_H_
