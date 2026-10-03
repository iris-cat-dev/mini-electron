// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "runtime/engine/renderer/renderer_websocket_broker.h"

#include <map>
#include <utility>
#include <optional>

#include "base/no_destructor.h"
#include "runtime/engine/common/thread_call.h"

namespace content {
namespace {

RendererWebSocketCommand& Command()
{
    static base::NoDestructor<RendererWebSocketCommand> command;
    return *command;
}

struct SocketEvent {
    uint64_t frame_id = 0;
    RendererWebSocketEvent callback;
};

std::map<uint64_t, SocketEvent>& Events()
{
    static base::NoDestructor<std::map<uint64_t, SocketEvent>> events;
    return *events;
}

uint64_t& NextSocketId()
{
    static uint64_t next = 1;
    return next;
}

void Send(uint64_t socket_id, base::Value::Dict command)
{
    auto found = Events().find(socket_id);
    if (Command() && found != Events().end())
        Command()(found->second.frame_id, std::move(command));
}

} // namespace

void SetRendererWebSocketCommand(RendererWebSocketCommand command)
{
    CHECK(ThreadCall::isBlinkThread());
    Command() = std::move(command);
    if (!Command())
        Events().clear();
}

uint64_t OpenRendererWebSocket(uint64_t frame_id,
    base::Value::Dict options, RendererWebSocketEvent event)
{
    CHECK(ThreadCall::isBlinkThread());
    if (!frame_id || !Command())
        return 0;
    uint64_t id = NextSocketId()++;
    Events()[id] = { frame_id, std::move(event) };
    options.Set("kind", "network");
    options.Set("operation", "websocket-open");
    options.Set("socketId", static_cast<double>(id));
    Send(id, std::move(options));
    return id;
}

void SendRendererWebSocket(uint64_t socket_id, bool binary,
    base::Value data)
{
    CHECK(ThreadCall::isBlinkThread());
    base::Value::Dict command;
    command.Set("kind", "network");
    command.Set("operation", "websocket-send");
    command.Set("socketId", static_cast<double>(socket_id));
    command.Set("binary", binary);
    command.Set("data", std::move(data));
    Send(socket_id, std::move(command));
}

void CloseRendererWebSocket(uint64_t socket_id, int code,
    const std::string& reason)
{
    CHECK(ThreadCall::isBlinkThread());
    base::Value::Dict command;
    command.Set("kind", "network");
    command.Set("operation", "websocket-close");
    command.Set("socketId", static_cast<double>(socket_id));
    command.Set("code", code);
    command.Set("reason", reason);
    Send(socket_id, std::move(command));
}

void ForgetRendererWebSocket(uint64_t socket_id)
{
    CHECK(ThreadCall::isBlinkThread());
    Events().erase(socket_id);
}

void DispatchRendererWebSocketEvent(base::Value::Dict event)
{
    CHECK(ThreadCall::isBlinkThread());
    std::optional<double> raw_id = event.FindDouble("socketId");
    if (!raw_id || *raw_id <= 0)
        return;
    uint64_t id = static_cast<uint64_t>(*raw_id);
    auto found = Events().find(id);
    if (found == Events().end())
        return;
    found->second.callback(event);
    const std::string* type = event.FindString("event");
    if (type && (*type == "close" || *type == "error"))
        Events().erase(found);
}

} // namespace content
