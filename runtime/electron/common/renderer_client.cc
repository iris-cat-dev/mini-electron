// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "runtime/electron/common/renderer_client.h"

#include <atomic>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#include "base/functional/bind.h"
#include "base/rand_util.h"
#include "base/task/sequenced_task_runner.h"
#include "runtime/electron/common/renderer_platform.h"
#include "runtime/electron/common/renderer_protocol.h"

namespace atom {
namespace {

using renderer_protocol::MessageKind;
using renderer_protocol::WireMessage;

struct ClientState {
    std::mutex lock;
    bool alive = true;
    int contents_id = 0;
    RendererClient::EventCallback event_callback;
    std::map<uint64_t, RendererClient::ReplyCallback> replies;
    scoped_refptr<base::SequencedTaskRunner> owner_runner;
};

class RendererClientImpl final : public RendererClient {
public:
    RendererClientImpl(RendererPlatformProcess process, uint64_t auth_high,
        uint64_t auth_low, std::shared_ptr<ClientState> state)
        : process_(process)
        , auth_high_(auth_high)
        , auth_low_(auth_low)
        , state_(std::move(state))
    {
    }

    ~RendererClientImpl() override { Close(); }

    bool Start(RendererLaunchOptions options)
    {
        base::Value::Dict launch;
        launch.Set("contentsId", options.contents_id);
        launch.Set("contextIsolation", options.context_isolation);
        launch.Set("webviewTag", options.webview_tag);
        launch.Set("preloadAllFrames", options.preload_all_frames);
        launch.Set("webSecurity", options.web_security);
        launch.Set("allowRunningInsecureContent", options.allow_running_insecure_content);
        launch.Set("transparent", options.transparent);
        launch.Set("process", std::move(options.preload_process_metadata));
        base::Value::List preloads;
        for (auto& preload : options.preloads) {
            base::Value::Dict item;
            item.Set("filename", std::move(preload.filename));
            item.Set("source", std::move(preload.source));
            preloads.Append(std::move(item));
        }
        launch.Set("preloads", std::move(preloads));
        base::Value::List schemes;
        for (auto& scheme : options.privileged_schemes) {
            base::Value::Dict item;
            item.Set("scheme", std::move(scheme.scheme));
            item.Set("standard", scheme.standard);
            item.Set("secure", scheme.secure);
            item.Set("supportFetchAPI", scheme.support_fetch_api);
            item.Set("corsEnabled", scheme.cors_enabled);
            item.Set("allowServiceWorkers", scheme.allow_service_workers);
            schemes.Append(std::move(item));
        }
        launch.Set("privilegedSchemes", std::move(schemes));

        std::string error;
        WireMessage hello { MessageKind::kHello, 0, std::move(launch) };
        if (!renderer_protocol::WriteMessage(process_.write_pipe, auth_high_, auth_low_, hello, &error))
            return false;
        WireMessage response;
        if (!renderer_protocol::ReadMessage(process_.read_pipe, auth_high_, auth_low_, &response, &error)
            || response.kind != MessageKind::kHello)
            return false;
        const std::optional<bool> ready = response.payload.FindBool("ready");
        if (!ready.value_or(false))
            return false;
        reader_ = std::thread([this, state = state_] { ReadLoop(std::move(state)); });
        return true;
    }

    uint64_t Request(std::string method, base::Value::Dict params,
        ReplyCallback callback) override
    {
        if (!renderer_protocol::IsBrowserToRendererMethod(method) || !callback)
            return 0;
        uint64_t id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
        if (!id)
            id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> state_lock(state_->lock);
            if (!state_->alive)
                return 0;
            state_->replies.emplace(id, std::move(callback));
        }
        base::Value::Dict payload;
        payload.Set("method", std::move(method));
        payload.Set("params", std::move(params));
        if (!Write({ MessageKind::kRequest, id, std::move(payload) })) {
            ReplyCallback failed;
            {
                std::lock_guard<std::mutex> state_lock(state_->lock);
                auto it = state_->replies.find(id);
                if (it != state_->replies.end()) {
                    failed = std::move(it->second);
                    state_->replies.erase(it);
                }
            }
            if (failed)
                PostReply(state_, std::move(failed), {}, "renderer IPC write failed");
            return 0;
        }
        return id;
    }

    bool Send(std::string method, base::Value::Dict params) override
    {
        if (!renderer_protocol::IsBrowserToRendererMethod(method))
            return false;
        base::Value::Dict payload;
        payload.Set("method", std::move(method));
        payload.Set("params", std::move(params));
        return Write({ MessageKind::kSend, 0, std::move(payload) });
    }

    bool Respond(uint64_t request_id, base::Value::Dict result,
        std::string error) override
    {
        if (!request_id)
            return false;
        base::Value::Dict payload;
        payload.Set("result", std::move(result));
        payload.Set("error", std::move(error));
        return Write({ MessageKind::kResponse, request_id, std::move(payload) });
    }

    int GetProcessId() const override { return process_.process_id; }

    bool IsAlive() const override
    {
        std::lock_guard<std::mutex> lock(state_->lock);
        return state_->alive && IsRendererProcessAlive(process_);
    }

    bool CopyFrame(RendererFrameSnapshot* snapshot) const override
    {
        if (!snapshot || !process_.frame_memory
            || process_.frame_mapping_size < sizeof(renderer_protocol::FrameBufferHeader))
            return false;
        const auto* header = static_cast<const renderer_protocol::FrameBufferHeader*>(process_.frame_memory);
        if (header->magic != renderer_protocol::kFrameMagic)
            return false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            uint64_t before = header->generation.load(std::memory_order_acquire);
            if (before & 1)
                continue;
            const uint32_t width = header->width;
            const uint32_t height = header->height;
            const uint32_t stride = header->stride;
            const uint32_t data_size = header->data_size;
            if (!width || !height || width > std::numeric_limits<uint32_t>::max() / 4
                || stride < width * 4 || data_size > header->capacity
                || data_size > renderer_protocol::kMaxFrameBytes
                || stride > data_size || height > data_size / stride
                || sizeof(*header) + data_size > process_.frame_mapping_size)
                return false;
            std::vector<uint8_t> pixels(data_size);
            std::memcpy(pixels.data(), header + 1, data_size);
            std::atomic_thread_fence(std::memory_order_acquire);
            uint64_t after = header->generation.load(std::memory_order_relaxed);
            if (before != after || (after & 1))
                continue;
            snapshot->width = static_cast<int>(width);
            snapshot->height = static_cast<int>(height);
            snapshot->stride = static_cast<int>(stride);
            snapshot->generation = after;
            snapshot->rgba = std::move(pixels);
            return true;
        }
        return false;
    }

    bool ShareFrameBufferWith(RendererClient& target,
        int guest_contents_id) const override
    {
        auto* target_impl = dynamic_cast<RendererClientImpl*>(&target);
        if (!target_impl || guest_contents_id <= 0 || !IsAlive() || !target_impl->IsAlive())
            return false;
        std::string error;
        return ShareRendererFrameBuffer(process_, &target_impl->process_,
            guest_contents_id, &error);
    }

    void Close() override
    {
        if (closed_.exchange(true, std::memory_order_acq_rel))
            return;
        {
            std::lock_guard<std::mutex> lock(state_->lock);
            state_->alive = false;
            state_->event_callback = {};
        }
        FailOutstanding(state_, "renderer closed");

        Write({ MessageKind::kShutdown, 0, {} }, true);
        if (!WaitForRendererProcess(&process_, 2000))
            TerminateRendererProcess(&process_);
        renderer_protocol::ClosePipe(process_.read_pipe);
        process_.read_pipe = renderer_protocol::kInvalidPipe;
        renderer_protocol::ClosePipe(process_.write_pipe);
        process_.write_pipe = renderer_protocol::kInvalidPipe;
        if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id())
            reader_.join();
        // Outstanding replies were failed before shutdown so a queued response
        // cannot win the close race and deliver a stale success.
        CloseRendererPlatformProcess(&process_);
    }

private:
    bool Write(WireMessage message, bool during_close = false)
    {
        if (!during_close) {
            std::lock_guard<std::mutex> state_lock(state_->lock);
            if (!state_->alive)
                return false;
        }
        std::lock_guard<std::mutex> lock(write_lock_);
        std::string error;
        return process_.write_pipe != renderer_protocol::kInvalidPipe
            && renderer_protocol::WriteMessage(process_.write_pipe, auth_high_, auth_low_, message, &error);
    }

    void ReadLoop(std::shared_ptr<ClientState> state)
    {
        std::string error;
        WireMessage message;
        while (renderer_protocol::ReadMessage(process_.read_pipe, auth_high_, auth_low_, &message, &error)) {
            if (message.kind == MessageKind::kResponse) {
                {
                    std::lock_guard<std::mutex> lock(state->lock);
                    if (!state->alive
                        || state->replies.find(message.request_id) == state->replies.end())
                        continue;
                }
                base::Value::Dict result;
                if (base::Value::Dict* found = message.payload.FindDict("result"))
                    result = found->Clone();
                std::string reply_error;
                if (const std::string* found = message.payload.FindString("error"))
                    reply_error = *found;
                PostPendingReply(state, message.request_id, std::move(result),
                    std::move(reply_error));
            } else if (message.kind == MessageKind::kEvent || message.kind == MessageKind::kRequest) {
                if (message.kind == MessageKind::kRequest)
                    message.payload.Set("requestId", static_cast<double>(message.request_id));
                PostEvent(state, std::move(message.payload));
            }
            message = WireMessage();
        }
        bool report_crash = false;
        {
            std::lock_guard<std::mutex> lock(state->lock);
            report_crash = state->alive;
            state->alive = false;
        }
        FailOutstanding(state, error.empty() ? "renderer crashed" : std::move(error));
        if (report_crash) {
            base::Value::Dict crash;
            crash.Set("type", "crashed");
            crash.Set("contentsId", state->contents_id);
            base::Value::Dict payload;
            payload.Set("reason", "process-exit");
            crash.Set("payload", std::move(payload));
            PostEvent(state, std::move(crash));
        }
    }

    static void PostPendingReply(const std::shared_ptr<ClientState>& state,
        uint64_t request_id, base::Value::Dict result, std::string error)
    {
        state->owner_runner->PostTask(FROM_HERE,
            base::BindOnce([](std::shared_ptr<ClientState> state, uint64_t request_id,
                               base::Value::Dict result, std::string error) {
                ReplyCallback callback;
                {
                    std::lock_guard<std::mutex> lock(state->lock);
                    if (!state->alive)
                        return;
                    auto found = state->replies.find(request_id);
                    if (found == state->replies.end())
                        return;
                    callback = std::move(found->second);
                    state->replies.erase(found);
                }
                callback(std::move(result), std::move(error));
            }, state, request_id, std::move(result), std::move(error)));
    }

    static void PostReply(const std::shared_ptr<ClientState>& state,
        ReplyCallback callback, base::Value::Dict result, std::string error)
    {
        state->owner_runner->PostTask(FROM_HERE,
            base::BindOnce([](ReplyCallback callback, base::Value::Dict result, std::string error) {
                callback(std::move(result), std::move(error));
            }, std::move(callback), std::move(result), std::move(error)));
    }

    static void PostEvent(const std::shared_ptr<ClientState>& state,
        base::Value::Dict event)
    {
        state->owner_runner->PostTask(FROM_HERE,
            base::BindOnce([](std::shared_ptr<ClientState> state, base::Value::Dict event) {
                EventCallback callback;
                {
                    std::lock_guard<std::mutex> lock(state->lock);
                    callback = state->event_callback;
                }
                // Close clears the state-owned callback before its owner can be
                // destroyed, so already queued event tasks become harmless.
                if (callback)
                    callback(std::move(event));
            }, state, std::move(event)));
    }

    static void FailOutstanding(const std::shared_ptr<ClientState>& state,
        std::string error)
    {
        std::map<uint64_t, ReplyCallback> replies;
        {
            std::lock_guard<std::mutex> lock(state->lock);
            replies.swap(state->replies);
        }
        for (auto& [id, callback] : replies)
            PostReply(state, std::move(callback), {}, error);
    }

    mutable RendererPlatformProcess process_;
    const uint64_t auth_high_;
    const uint64_t auth_low_;
    std::shared_ptr<ClientState> state_;
    std::atomic<uint64_t> next_request_id_ { 1 };
    std::atomic<bool> closed_ { false };
    std::mutex write_lock_;
    std::thread reader_;
};

} // namespace

std::unique_ptr<RendererClient> RendererClient::Create(RendererLaunchOptions options,
    EventCallback event_callback)
{
    if (!base::SequencedTaskRunner::HasCurrentDefault() || options.contents_id <= 0)
        return nullptr;
    uint64_t auth_high = base::RandUint64();
    uint64_t auth_low = base::RandUint64();
    if (!auth_high && !auth_low)
        auth_low = 1;
    RendererPlatformProcess process;
    std::string error;
    if (!LaunchRendererSandbox(auth_high, auth_low, &process, &error))
        return nullptr;
    auto state = std::make_shared<ClientState>();
    state->event_callback = std::move(event_callback);
    state->contents_id = options.contents_id;
    state->owner_runner = base::SequencedTaskRunner::GetCurrentDefault();
    auto client = std::make_unique<RendererClientImpl>(process, auth_high, auth_low, state);
    if (!client->Start(std::move(options))) {
        client->Close();
        return nullptr;
    }
    return client;
}

} // namespace atom
