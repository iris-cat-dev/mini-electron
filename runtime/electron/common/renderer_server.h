// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef RUNTIME_ELECTRON_COMMON_RENDERER_SERVER_H_
#define RUNTIME_ELECTRON_COMMON_RENDERER_SERVER_H_

namespace atom {

bool IsRendererProcess(int argc, const char* const* argv);
bool IsRendererProcess(int argc, const wchar_t* const* argv);
int RunRendererProcess(int argc, char* argv[]);
int RunRendererProcess(int argc, wchar_t* argv[]);

} // namespace atom

#endif // RUNTIME_ELECTRON_COMMON_RENDERER_SERVER_H_
