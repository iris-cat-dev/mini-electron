// Copyright 2026 The miniblink132 Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef PLATFORM_MACOS_SMOKE_ENGINE_PROBE_H_
#define PLATFORM_MACOS_SMOKE_ENGINE_PROBE_H_

#include <optional>
#include <string>

namespace miniblink::mac {

std::optional<std::string> RunEngineProbe(const char* executable_path);

}  // namespace miniblink::mac

#endif  // PLATFORM_MACOS_SMOKE_ENGINE_PROBE_H_
