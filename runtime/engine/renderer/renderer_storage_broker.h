// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef RUNTIME_ENGINE_RENDERER_RENDERER_STORAGE_BROKER_H_
#define RUNTIME_ENGINE_RENDERER_RENDERER_STORAGE_BROKER_H_

#include <cstdint>
#include <functional>
#include <string>

#include "base/values.h"

namespace content {

using RendererStorageReply =
    std::function<void(base::Value::Dict, std::string)>;
using RendererStorageBroker =
    std::function<void(uint64_t, base::Value::Dict, RendererStorageReply)>;

void SetRendererStorageBroker(RendererStorageBroker broker);
bool RequestRendererStorage(uint64_t frame_id, base::Value::Dict request,
    base::Value::Dict* result, std::string* error);

} // namespace content

#endif // RUNTIME_ENGINE_RENDERER_RENDERER_STORAGE_BROKER_H_
