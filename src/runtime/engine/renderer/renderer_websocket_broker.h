// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef RUNTIME_ENGINE_RENDERER_RENDERER_WEBSOCKET_BROKER_H_
#define RUNTIME_ENGINE_RENDERER_RENDERER_WEBSOCKET_BROKER_H_

#include <cstdint>
#include <functional>
#include <string>

#include "base/values.h"

namespace content {

using RendererWebSocketCommand =
    std::function<void(uint64_t, base::Value::Dict)>;
using RendererWebSocketEvent = std::function<void(const base::Value::Dict&)>;

void SetRendererWebSocketCommand(RendererWebSocketCommand command);
uint64_t OpenRendererWebSocket(uint64_t frame_id,
    base::Value::Dict options, RendererWebSocketEvent event);
void SendRendererWebSocket(uint64_t socket_id, bool binary,
    base::Value data);
void CloseRendererWebSocket(uint64_t socket_id, int code,
    const std::string& reason);
void ForgetRendererWebSocket(uint64_t socket_id);
void DispatchRendererWebSocketEvent(base::Value::Dict event);

} // namespace content

#endif // RUNTIME_ENGINE_RENDERER_RENDERER_WEBSOCKET_BROKER_H_
