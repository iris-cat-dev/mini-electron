// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "src/runtime/engine/renderer/renderer_websocket_broker.h"

#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "src/runtime/electron/common/renderer_protocol.h"
#include "src/runtime/engine/common/thread_call.h"
namespace content {
namespace {

RendererWebSocketCommand& Command()
{
    static base::NoDestructor<RendererWebSocketCommand> command;
    return *command;
}

struct ReceiveCredit {
    uint64_t event_id = 0;
    size_t bytes = 0;
};

struct SocketEvent {
    uint64_t frame_id = 0;
    RendererWebSocketEvent callback;
    bool paused = false;
    bool forgotten = false;
    bool receive_cancelled = false;
    std::deque<base::Value::Dict> pending;
    std::vector<ReceiveCredit> delivered_while_paused;
};

using SocketEventPtr = std::shared_ptr<SocketEvent>;

std::map<uint64_t, SocketEventPtr>& Events()
{
    static base::NoDestructor<std::map<uint64_t, SocketEventPtr>> events;
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
        Command()(found->second->frame_id, std::move(command));
}

void Acknowledge(uint64_t frame_id, uint64_t socket_id,
    ReceiveCredit credit, bool cancelled)
{
    if (!Command() || !frame_id || !credit.event_id || !credit.bytes)
        return;
    base::Value::Dict command;
    command.Set("kind", "network");
    command.Set("operation", "websocket-receive-ack");
    command.Set("socketId", static_cast<double>(socket_id));
    command.Set("eventId", base::NumberToString(credit.event_id));
    command.Set("credit", static_cast<int>(credit.bytes));
    command.Set("cancelled", cancelled);
    Command()(frame_id, std::move(command));
}

bool ReceiveMetadata(const base::Value::Dict& event, ReceiveCredit* credit)
{
    return atom::renderer_protocol::ParseWebSocketReceiveEvent(
        event, &credit->event_id, &credit->bytes);
}

void CancelPending(uint64_t socket_id, const SocketEventPtr& socket)
{
    std::deque<base::Value::Dict> pending;
    pending.swap(socket->pending);
    for (base::Value::Dict& event : pending) {
        ReceiveCredit credit;
        const bool received = ReceiveMetadata(event, &credit);
        event = {};
        if (received)
            Acknowledge(socket->frame_id, socket_id, credit, true);
    }
    std::vector<ReceiveCredit> delivered;
    delivered.swap(socket->delivered_while_paused);
    for (ReceiveCredit credit : delivered)
        Acknowledge(socket->frame_id, socket_id, credit, true);
}

void Deliver(uint64_t socket_id, const SocketEventPtr& socket,
    base::Value::Dict event)
{
    const std::string* raw_type = event.FindString("event");
    const std::string type = raw_type ? *raw_type : std::string();
    const bool terminal_event = type == "close"
        || (type == "error" && !event.FindBool("operationError").value_or(false));
    ReceiveCredit credit;
    const bool received = ReceiveMetadata(event, &credit);
    if ((!received || !socket->receive_cancelled)
        && !socket->callback(event) && !socket->forgotten
        && !socket->receive_cancelled) {
        socket->paused = true;
        socket->pending.push_front(std::move(event));
        return;
    }
    // The acknowledgement must never race the native payload destructor.
    event = {};
    if (received) {
        if (socket->paused && !socket->forgotten && !socket->receive_cancelled)
            socket->delivered_while_paused.push_back(credit);
        else
            Acknowledge(socket->frame_id, socket_id, credit,
                socket->forgotten || socket->receive_cancelled);
    }
    if (terminal_event) {
        auto found = Events().find(socket_id);
        if (found != Events().end() && found->second == socket)
            Events().erase(found);
        socket->forgotten = true;
        CancelPending(socket_id, socket);
    }
}

} // namespace

void SetRendererWebSocketCommand(RendererWebSocketCommand command)
{
    CHECK(ThreadCall::isBlinkThread());
    Command() = std::move(command);
    if (!Command()) {
        Events().clear();
    }
}

uint64_t OpenRendererWebSocket(uint64_t frame_id,
    base::Value::Dict options, RendererWebSocketEvent event)
{
    CHECK(ThreadCall::isBlinkThread());
    if (!frame_id || !Command())
        return 0;
    uint64_t id = NextSocketId()++;
    auto socket = std::make_shared<SocketEvent>();
    socket->frame_id = frame_id;
    socket->callback = std::move(event);
    Events()[id] = std::move(socket);
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
    auto found = Events().find(socket_id);
    if (found == Events().end())
        return;
    SocketEventPtr socket = found->second;
    Events().erase(found);
    socket->forgotten = true;
    CancelPending(socket_id, socket);
}

void CancelRendererWebSocketReceive(uint64_t socket_id)
{
    CHECK(ThreadCall::isBlinkThread());
    auto found = Events().find(socket_id);
    if (found == Events().end())
        return;
    SocketEventPtr socket = found->second;
    socket->receive_cancelled = true;
    socket->paused = false;
    std::vector<ReceiveCredit> delivered;
    delivered.swap(socket->delivered_while_paused);
    for (ReceiveCredit credit : delivered)
        Acknowledge(socket->frame_id, socket_id, credit, true);
    std::deque<base::Value::Dict> pending;
    pending.swap(socket->pending);
    for (base::Value::Dict& event : pending)
        Deliver(socket_id, socket, std::move(event));
}

void ApplyRendererWebSocketBackpressure(uint64_t socket_id)
{
    CHECK(ThreadCall::isBlinkThread());
    auto found = Events().find(socket_id);
    if (found != Events().end() && !found->second->receive_cancelled)
        found->second->paused = true;
}

void RemoveRendererWebSocketBackpressure(uint64_t socket_id)
{
    CHECK(ThreadCall::isBlinkThread());
    auto found = Events().find(socket_id);
    if (found == Events().end())
        return;
    SocketEventPtr socket = found->second;
    socket->paused = false;
    std::vector<ReceiveCredit> delivered;
    delivered.swap(socket->delivered_while_paused);
    for (ReceiveCredit credit : delivered)
        Acknowledge(socket->frame_id, socket_id, credit, false);
    while (!socket->paused && !socket->forgotten
        && !socket->pending.empty()) {
        base::Value::Dict event = std::move(socket->pending.front());
        socket->pending.pop_front();
        Deliver(socket_id, socket, std::move(event));
    }
}

void DispatchRendererWebSocketEvent(base::Value::Dict event)
{
    CHECK(ThreadCall::isBlinkThread());
    std::optional<double> raw_id = event.FindDouble("socketId");
    if (!raw_id || !std::isfinite(*raw_id) || *raw_id < 1
        || *raw_id > 9007199254740991.0
        || *raw_id != static_cast<uint64_t>(*raw_id)) {
        return;
    }
    const uint64_t id = static_cast<uint64_t>(*raw_id);
    const std::string* type = event.FindString("event");
    const bool data_event =
        type && (*type == "text" || *type == "binary");
    const bool terminal_event = type && (*type == "close"
        || (*type == "error" && !event.FindBool("operationError").value_or(false)));
    ReceiveCredit credit;
    const bool received = ReceiveMetadata(event, &credit);
    if (data_event && !received)
        return;
    auto found = Events().find(id);
    if (found == Events().end()) {
        uint64_t frame_id = 0;
        if (const std::string* frame = event.FindString("frameId"))
            base::StringToUint64(*frame, &frame_id);
        event = {};
        if (received)
            Acknowledge(frame_id, id, credit, true);
        return;
    }
    SocketEventPtr socket = found->second;
    if (socket->paused && (received || terminal_event)) {
        socket->pending.push_back(std::move(event));
        return;
    }
    Deliver(id, socket, std::move(event));
}

} // namespace content
