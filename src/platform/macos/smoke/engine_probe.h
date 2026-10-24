// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef PLATFORM_MACOS_SMOKE_ENGINE_PROBE_H_
#define PLATFORM_MACOS_SMOKE_ENGINE_PROBE_H_

#include <optional>
#include <string>

namespace mini_electron::mac {

std::optional<std::string> RunEngineProbe(const char* executable_path);

}  // namespace mini_electron::mac

#endif  // PLATFORM_MACOS_SMOKE_ENGINE_PROBE_H_
