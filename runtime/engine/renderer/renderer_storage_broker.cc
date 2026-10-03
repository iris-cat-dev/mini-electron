// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "runtime/engine/renderer/renderer_storage_broker.h"

#include <condition_variable>
#include <mutex>
#include <utility>

namespace content {
namespace {
std::mutex g_broker_lock;
RendererStorageBroker g_broker;
} // namespace

void SetRendererStorageBroker(RendererStorageBroker broker)
{
    std::lock_guard<std::mutex> lock(g_broker_lock);
    g_broker = std::move(broker);
}

bool RequestRendererStorage(uint64_t frame_id, base::Value::Dict request,
    base::Value::Dict* result, std::string* error)
{
    RendererStorageBroker broker;
    {
        std::lock_guard<std::mutex> lock(g_broker_lock);
        broker = g_broker;
    }
    if (!frame_id || !broker) {
        *error = !frame_id ? "storage request has no frame"
                          : "storage broker is unavailable";
        return false;
    }
    std::mutex reply_lock;
    std::condition_variable replied;
    bool done = false;
    broker(frame_id, std::move(request),
        [&](base::Value::Dict value, std::string broker_error) {
        {
            std::lock_guard<std::mutex> lock(reply_lock);
            *result = std::move(value);
            *error = std::move(broker_error);
            done = true;
        }
        replied.notify_one();
        });
    std::unique_lock<std::mutex> lock(reply_lock);
    replied.wait(lock, [&] { return done; });
    return error->empty();
}

} // namespace content
