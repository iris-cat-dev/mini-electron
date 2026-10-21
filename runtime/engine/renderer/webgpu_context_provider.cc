// Copyright 2026 mini-electron contributors.
#include "runtime/engine/renderer/webgpu_context_provider.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <dawn/native/DawnNative.h>
#include <dawn/wire/WireClient.h>
#include <dawn/wire/WireServer.h>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/numerics/checked_math.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "gpu/command_buffer/client/webgpu_native_texture_interface.h"
#include "gpu/command_buffer/common/capabilities.h"
#include "gpu/config/gpu_feature_info.h"

namespace content {
namespace {

[[noreturn]] void Unsupported(const char* operation)
{
    LOG(FATAL) << "Renderer-local WebGPU has no Chromium command-buffer/shared-image contract for " << operation;
}

const DawnProcTable& NativeProcs()
{
    return dawn::native::GetProcs();
}

const DawnProcTable& WireProcs()
{
    return dawn::wire::client::GetProcs();
}

struct NativeLog {
    WGPULoggingType type;
    std::string message;
};

struct NativeLogger {
    WGPULoggingCallback callback;
    void* userdata;
    std::mutex mutex;
    std::vector<NativeLog> pending;
};

struct NativeState {
    // All native entrypoints, server object tables and native event dispatch are
    // serialized. A shared instance may deliver another worker's server response,
    // but never that worker's wire-client/Blink callback.
    std::recursive_mutex mutex;
    std::unique_ptr<dawn::native::Instance> instance;
    std::vector<dawn::native::Adapter> adapters;
    WGPUDevice warm_device = nullptr;
    int max_texture_size = 0;
    std::map<WGPUDevice, std::unique_ptr<NativeLogger>> loggers;
    std::mutex wake_mutex;
    std::map<const void*, base::RepeatingClosure> wakes;
};

NativeState& Native()
{
    // Metal discovery and the warmed device must survive every worker/provider.
    // Teardown at process exit could otherwise race still-live worker objects.
    static auto* state = new NativeState;
    return *state;
}

void CollectNativeLog(WGPULoggingType type, WGPUStringView message, void* userdata)
{
    auto* logger = static_cast<NativeLogger*>(userdata);
    {
        std::lock_guard lock(logger->mutex);
        logger->pending.push_back({ type, std::string(message.data ? message.data : "",
            message.data ? (message.length == WGPU_STRLEN ? std::strlen(message.data) : message.length) : 0) });
    }
    // Native compiler threads may emit logs while holding Dawn's logging lock.
    // Never call the wire server or acquire Native().mutex on that thread.
    std::lock_guard lock(Native().wake_mutex);
    for (const auto& [channel, wake] : Native().wakes)
        wake.Run();
}

void FlushNativeLogs()
{
    // Called only with Native().mutex held, like every other server callback.
    for (const auto& [device, logger] : Native().loggers) {
        std::vector<NativeLog> pending;
        {
            std::lock_guard lock(logger->mutex);
            pending.swap(logger->pending);
        }
        for (const auto& log : pending)
            logger->callback(log.type, { log.message.data(), log.message.size() }, logger->userdata);
    }
}

WGPUBackendType BackendType()
{
#if BUILDFLAG(IS_MAC)
    return WGPUBackendType_Metal;
#elif BUILDFLAG(IS_WIN)
    return WGPUBackendType_D3D12;
#else
    return WGPUBackendType_Undefined;
#endif
}

// Native object-producing completions must not depend on a Blink task runner:
// a worker can stop after submission but before its next progress task. Native
// callbacks only enqueue owned results. Once closed, the queue releases results
// immediately, even when a compiler thread is the last thread still running.
class NativeCompletionQueue {
public:
    using Completion = base::OnceCallback<void(bool server_alive)>;

    void SetWake(base::RepeatingClosure wake)
    {
        std::lock_guard lock(mutex_);
        wake_ = std::move(wake);
    }

    void Add(Completion completion)
    {
        {
            std::lock_guard lock(mutex_);
            if (!closed_) {
                pending_.push_back(std::move(completion));
                if (wake_)
                    wake_.Run();
                return;
            }
        }
        std::move(completion).Run(false);
    }

    void Drain()
    {
        std::vector<Completion> pending;
        {
            std::lock_guard lock(mutex_);
            pending.swap(pending_);
        }
        for (auto& completion : pending)
            std::move(completion).Run(true);
    }

    // The wire server must already be destroyed: its original callbacks then
    // only free weak-server userdata and cannot touch server object tables.
    void Close()
    {
        std::vector<Completion> pending;
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            wake_.Reset();
            pending.swap(pending_);
        }
        // Releasing a device may synchronously enqueue its loss notification.
        // No native release or callback is allowed under the queue mutex.
        for (auto& completion : pending)
            std::move(completion).Run(false);
    }

private:
    std::mutex mutex_;
    bool closed_ = false;
    base::RepeatingClosure wake_;
    std::vector<Completion> pending_;
};

thread_local std::shared_ptr<NativeCompletionQueue> g_native_completions;

class NativeCallbackScope {
public:
    explicit NativeCallbackScope(const std::shared_ptr<NativeCompletionQueue>& completions)
        : previous_(std::exchange(g_native_completions, completions)) { }
    ~NativeCallbackScope() { g_native_completions = std::move(previous_); }

private:
    std::shared_ptr<NativeCompletionQueue> previous_;
};

std::string CopyNativeMessage(WGPUStringView message)
{
    return std::string(message.data ? message.data : "",
        message.data ? (message.length == WGPU_STRLEN ? std::strlen(message.data) : message.length) : 0);
}

template <typename Info, typename Status, typename Object>
struct NativeObjectCompletion {
    std::shared_ptr<NativeCompletionQueue> queue;
    Info original;
    void (*release)(Object);
    // Bind the native completion as a value. Its opaque C handle is owned by
    // this result until delivered, not an Unretained() C++ object pointer.
    struct Result {
        Info original;
        Status status;
        Object object;
        void (*release)(Object);
        std::string message;
    };

    static void Complete(Status status, Object object, WGPUStringView message, void* userdata, void*)
    {
        std::unique_ptr<NativeObjectCompletion> data(static_cast<NativeObjectCompletion*>(userdata));
        data->queue->Add(base::BindOnce(
            [](Result result, bool server_alive) {
                auto& [original, status, object, release, message] = result;
                if (!server_alive && object) {
                    release(object);
                    object = nullptr;
                }
                // In the closed case this invokes Dawn's expired weak-server
                // trampoline solely to dispose of its callback userdata.
                original.callback(status, object, { message.data(), message.size() }, original.userdata1, original.userdata2);
            },
            Result { data->original, status, object, data->release, CopyNativeMessage(message) }));
    }
};

template <typename Status, typename Info, typename Object>
Info DeferOwnedCallback(Info original, void (*release)(Object))
{
    CHECK(g_native_completions);
    using Pending = NativeObjectCompletion<Info, Status, Object>;
    auto* pending = new Pending { g_native_completions, original, release };
    original.mode = WGPUCallbackMode_AllowSpontaneous;
    original.callback = &Pending::Complete;
    original.userdata1 = pending;
    original.userdata2 = nullptr;
    return original;
}

struct NativeDeviceLossCompletion {
    std::shared_ptr<NativeCompletionQueue> queue;
    WGPUDeviceLostCallbackInfo2 original;
    struct Result {
        WGPUDeviceLostCallbackInfo2 original;
        WGPUDevice device;
        WGPUDeviceLostReason reason;
        std::string message;
    };

    static void Complete(WGPUDevice const* device, WGPUDeviceLostReason reason, WGPUStringView message, void* userdata, void*)
    {
        std::unique_ptr<NativeDeviceLossCompletion> data(static_cast<NativeDeviceLossCompletion*>(userdata));
        data->queue->Add(base::BindOnce(
            [](Result result, bool server_alive) {
                auto& [original, device, reason, message] = result;
                original.callback(server_alive ? &device : nullptr, reason, { message.data(), message.size() }, original.userdata1, original.userdata2);
            },
            Result { data->original, device ? *device : nullptr, reason, CopyNativeMessage(message) }));
    }
};

// Keep descriptors, required features/limits and callback ownership intact while
// adding the native-only ability to read canvas textures with internal usages.
struct NativeDeviceDescriptor {
    explicit NativeDeviceDescriptor(const WGPUDeviceDescriptor* descriptor)
    {
        if (descriptor)
            value = *descriptor;
        if (value.deviceLostCallbackInfo2.callback) {
            CHECK(g_native_completions);
            auto* pending = new NativeDeviceLossCompletion { g_native_completions, value.deviceLostCallbackInfo2 };
            value.deviceLostCallbackInfo2.mode = WGPUCallbackMode_AllowSpontaneous;
            value.deviceLostCallbackInfo2.callback = &NativeDeviceLossCompletion::Complete;
            value.deviceLostCallbackInfo2.userdata1 = pending;
            value.deviceLostCallbackInfo2.userdata2 = nullptr;
        }
        if (value.requiredFeatureCount)
            features.assign(value.requiredFeatures, value.requiredFeatures + value.requiredFeatureCount);
        if (std::find(features.begin(), features.end(), WGPUFeatureName_DawnInternalUsages) == features.end())
            features.push_back(WGPUFeatureName_DawnInternalUsages);
        value.requiredFeatures = features.data();
        value.requiredFeatureCount = features.size();
        toggles.chain.sType = WGPUSType_DawnTogglesDescriptor;
        toggles.chain.next = value.nextInChain;
        toggles.enabledToggleCount = std::size(enabled_toggles);
        toggles.enabledToggles = enabled_toggles;
        value.nextInChain = &toggles.chain;
    }

    WGPUDeviceDescriptor value = {};
    std::vector<WGPUFeatureName> features;
    WGPUDawnTogglesDescriptor toggles = {};
    const char* enabled_toggles[2] = { "disallow_spirv", "disable_blob_cache" };
};

DawnProcTable ServerProcs()
{
    DawnProcTable procs = NativeProcs();
    procs.createInstance = [](const WGPUInstanceDescriptor*) -> WGPUInstance {
        Unsupported("createInstance (instances are reserved and injected)");
    };
    procs.instanceRequestAdapter = [](WGPUInstance instance, const WGPURequestAdapterOptions* options,
                                      WGPURequestAdapterCallback callback, void* userdata) {
        WGPURequestAdapterOptions native_options = {};
        if (options)
            native_options = *options;
        native_options.backendType = BackendType();
        NativeProcs().instanceRequestAdapter(instance, &native_options, callback, userdata);
    };
    procs.instanceRequestAdapter2 = [](WGPUInstance instance, const WGPURequestAdapterOptions* options,
                                       WGPURequestAdapterCallbackInfo2 callback) {
        WGPURequestAdapterOptions native_options = {};
        if (options)
            native_options = *options;
        native_options.backendType = BackendType();
        callback = DeferOwnedCallback<WGPURequestAdapterStatus>(callback, NativeProcs().adapterRelease);
        return NativeProcs().instanceRequestAdapter2(instance, &native_options, callback);
    };
    procs.adapterRequestDevice = [](WGPUAdapter adapter, const WGPUDeviceDescriptor* descriptor,
                                    WGPURequestDeviceCallback callback, void* userdata) {
        NativeDeviceDescriptor native_descriptor(descriptor);
        NativeProcs().adapterRequestDevice(adapter, &native_descriptor.value, callback, userdata);
    };
    procs.adapterRequestDevice2 = [](WGPUAdapter adapter, const WGPUDeviceDescriptor* descriptor,
                                     WGPURequestDeviceCallbackInfo2 callback) {
        NativeDeviceDescriptor native_descriptor(descriptor);
        callback = DeferOwnedCallback<WGPURequestDeviceStatus>(callback, NativeProcs().deviceRelease);
        return NativeProcs().adapterRequestDevice2(adapter, &native_descriptor.value, callback);
    };
    procs.deviceCreateComputePipelineAsync2 = [](WGPUDevice device, const WGPUComputePipelineDescriptor* descriptor,
                                                WGPUCreateComputePipelineAsyncCallbackInfo2 callback) {
        callback = DeferOwnedCallback<WGPUCreatePipelineAsyncStatus>(callback, NativeProcs().computePipelineRelease);
        return NativeProcs().deviceCreateComputePipelineAsync2(device, descriptor, callback);
    };
    procs.deviceCreateRenderPipelineAsync2 = [](WGPUDevice device, const WGPURenderPipelineDescriptor* descriptor,
                                               WGPUCreateRenderPipelineAsyncCallbackInfo2 callback) {
        callback = DeferOwnedCallback<WGPUCreatePipelineAsyncStatus>(callback, NativeProcs().renderPipelineRelease);
        return NativeProcs().deviceCreateRenderPipelineAsync2(device, descriptor, callback);
    };
    // Unlike the server's default spontaneous mapping mode, this confines every
    // native response write to a native entrypoint protected by Native().mutex.
    procs.bufferMapAsync2 = [](WGPUBuffer buffer, WGPUMapMode mode, size_t offset, size_t size,
                               WGPUBufferMapCallbackInfo2 callback) {
        callback.mode = WGPUCallbackMode_AllowProcessEvents;
        return NativeProcs().bufferMapAsync2(buffer, mode, offset, size, callback);
    };
    procs.deviceSetLoggingCallback = [](WGPUDevice device, WGPULoggingCallback callback, void* userdata) {
        // Uninstalling first waits for any native compiler log callback without
        // requiring that callback to enter the native/server critical section.
        NativeProcs().deviceSetLoggingCallback(device, nullptr, nullptr);
        Native().loggers.erase(device);
        if (callback) {
            auto logger = std::make_unique<NativeLogger>();
            logger->callback = callback;
            logger->userdata = userdata;
            auto* state = logger.get();
            Native().loggers.emplace(device, std::move(logger));
            NativeProcs().deviceSetLoggingCallback(device, CollectNativeLog, state);
        }
    };
    return procs;
}

class LocalChannel;

// The outgoing serializer can execute server commands when full, but it never
// executes wire-client responses. In particular Flush() is safe inside a canvas
// snapshot or while a wire entrypoint is still serializing a command.
class ClientSerializer final : public dawn::wire::CommandSerializer {
public:
    explicit ClientSerializer(LocalChannel* channel);
    void* GetCmdSpace(size_t size) override;
    bool Flush() override;
    size_t GetMaximumAllocationSize() const override { return kCapacity; }
    void OnSerializeError() override;
    bool HasCommands() const { return used_ != 0; }
    void Disconnect() { disconnected_ = true; used_ = 0; }

private:
    static constexpr size_t kCapacity = 1024 * 1024;
    LocalChannel* const channel_;
    std::unique_ptr<char[]> buffer_;
    size_t used_ = 0;
    bool disconnected_ = false;
};

// Server callbacks may originate in a different worker's instance pump. Blocks
// are stable while Dawn writes into them; draining also holds Native().mutex,
// after all such writes have returned. Only the owning Blink sequence consumes
// detached blocks. Recycling avoids a megabyte allocation for each task/flush.
class ServerSerializer final : public dawn::wire::CommandSerializer {
public:
    struct Block {
        std::unique_ptr<char[]> bytes;
        size_t used = 0;
    };

    explicit ServerSerializer(LocalChannel* channel) : channel_(channel) { }
    void* GetCmdSpace(size_t size) override;
    bool Flush() override;
    size_t GetMaximumAllocationSize() const override { return kCapacity; }
    void OnSerializeError() override;
    std::vector<Block> Drain();
    void Recycle(std::vector<Block> blocks);
    void Disconnect();

private:
    void Seal();
    static constexpr size_t kCapacity = 1024 * 1024;
    LocalChannel* const channel_;
    std::mutex mutex_;
    Block current_;
    std::vector<Block> ready_;
    std::vector<Block> recycled_;
    bool disconnected_ = false;
};

struct CanvasTexture {
    CanvasTexture(WGPUDevice device, WGPUTexture texture, const gpu::webgpu::ReservedTexture& reservation,
        const WGPUTextureDescriptor& descriptor)
        : device(device), texture(texture), reservation(reservation), width(descriptor.size.width),
          height(descriptor.size.height), format(descriptor.format)
    {
        NativeProcs().deviceAddRef(device);
    }
    ~CanvasTexture()
    {
        if (staging)
            NativeProcs().bufferRelease(staging);
        NativeProcs().textureRelease(texture);
        NativeProcs().deviceRelease(device);
    }

    WGPUDevice device;
    WGPUTexture texture;
    gpu::webgpu::ReservedTexture reservation;
    uint32_t width;
    uint32_t height;
    WGPUTextureFormat format;
    WGPUBuffer staging = nullptr;
    size_t staging_size = 0;
};

class LocalChannel final : public gpu::webgpu::APIChannel,
                           public gpu::webgpu::WebGPUInterface,
                           public gpu::webgpu::NativeTextureInterface {
public:
    LocalChannel() : outgoing_(this), incoming_(this)
    {
        std::lock_guard lock(Native().mutex);
        // The default transfer services really copy map/unmap payloads inline;
        // their mapped allocations belong to the retained wire client.
        client_ = std::make_unique<dawn::wire::WireClient>(dawn::wire::WireClientDescriptor { &outgoing_ });
        server_procs_ = ServerProcs();
        server_ = std::make_unique<dawn::wire::WireServer>(dawn::wire::WireServerDescriptor { &server_procs_, &incoming_ });
        // Wire instances do not support timed WaitAny. Only the native instance
        // enables it, for synchronous native staging-buffer readback.
        const auto reservation = client_->ReserveInstance();
        instance_ = reservation.instance;
        CHECK(instance_);
        CHECK(server_->InjectInstance(Native().instance->Get(), reservation.handle));
    }

    bool Bind()
    {
        if (bound_)
            return owner_ == std::this_thread::get_id() && !disconnected_;
        if (!base::SingleThreadTaskRunner::HasCurrentDefault())
            return false;
        owner_ = std::this_thread::get_id();
        bound_ = true;
        {
            std::lock_guard lock(task_mutex_);
            runner_ = base::SingleThreadTaskRunner::GetCurrentDefault();
            weak_ = weak_factory_.GetWeakPtr();
        }
        {
            std::lock_guard lock(Native().wake_mutex);
            Native().wakes.emplace(this, base::BindRepeating(&LocalChannel::SchedulePump, base::Unretained(this), base::TimeDelta()));
        }
        completions_->SetWake(base::BindRepeating(&LocalChannel::SchedulePump, base::Unretained(this), base::TimeDelta()));
        SchedulePump();
        return !disconnected_;
    }

    bool IsLost() const { return disconnected_ || failed_; }
    void SetLostCallback(base::RepeatingClosure callback) { lost_callback_ = std::move(callback); }
    void SetErrorCallback(base::RepeatingCallback<void(const char*, int32_t)> callback) { error_callback_ = std::move(callback); }

    WGPUInstance GetWGPUInstance() const override { return instance_; }
    scoped_refptr<gpu::webgpu::APIChannel> GetAPIChannel() const override
    {
        return scoped_refptr<gpu::webgpu::APIChannel>(const_cast<LocalChannel*>(this));
    }

    void Disconnect() override
    {
        CheckOwner();
        if (disconnected_.exchange(true))
            return;
        // Disconnect can resolve pending callbacks which release the provider.
        scoped_refptr<LocalChannel> keep_alive(this);
        weak_factory_.InvalidateWeakPtrs();
        {
            std::lock_guard lock(task_mutex_);
            weak_.reset();
            task_pending_ = false;
        }
        {
            std::lock_guard lock(Native().wake_mutex);
            Native().wakes.erase(this);
        }
        outgoing_.Disconnect();
        // Native destruction cannot call Blink: replies are disabled first.
        {
            std::lock_guard lock(Native().mutex);
            incoming_.Disconnect();
            canvas_textures_.clear();
            server_.reset();
            completions_->Close();
            // Server/device teardown makes outstanding maps and queue serials
            // ready. Flush their non-owning weak-server userdata once; owned
            // pipeline/device results remain independently self-cleaning.
            // This never handles wire-client responses or waits for new work.
            dawn::native::InstanceProcessEvents(Native().instance->Get());
        }
        // Retain the actual wire client and its inline mapped storage until the
        // last APIChannel owner (including mapped ArrayBuffers) goes away.
        client_->Disconnect();
    }

    void FlushCommands() override
    {
        CheckOwner();
        awaiting_flush_ = false;
        if (!disconnected_) {
            outgoing_.Flush();
            SchedulePump();
        }
    }
    bool EnsureAwaitingFlush() override
    {
        CheckOwner();
        if (disconnected_ || awaiting_flush_ || !outgoing_.HasCommands())
            return false;
        awaiting_flush_ = true;
        return true;
    }
    void FlushAwaitingCommands() override
    {
        CheckOwner();
        if (awaiting_flush_)
            FlushCommands();
    }

    gpu::webgpu::ReservedBuffer ReserveBuffer(WGPUDevice device, const WGPUBufferDescriptor* descriptor) override
    {
        CheckOwner();
        if (IsLost())
            return {};
        FlushCommands();
        WGPUBufferDescriptor placeholder = {};
        auto result = client_->ReserveBuffer(device, descriptor ? descriptor : &placeholder);
        return { result.buffer, result.handle.id, result.handle.generation, result.deviceHandle.id, result.deviceHandle.generation };
    }
    gpu::webgpu::ReservedTexture ReserveTexture(WGPUDevice device, const WGPUTextureDescriptor* descriptor) override
    {
        CheckOwner();
        if (IsLost())
            return {};
        FlushCommands();
        WGPUTextureDescriptor placeholder = {};
        auto result = client_->ReserveTexture(device, descriptor ? descriptor : &placeholder);
        return { result.texture, result.handle.id, result.handle.generation, result.deviceHandle.id, result.deviceHandle.generation };
    }
    WGPUDevice DeprecatedEnsureDefaultDeviceSync() override { Unsupported("DeprecatedEnsureDefaultDeviceSync"); }

    gpu::webgpu::ReservedTexture CreateCanvasTexture(WGPUDevice device, const WGPUTextureDescriptor& descriptor) override
    {
        CheckOwner();
        if (IsLost())
            return {};
        FlushCommands();
        if (IsLost())
            return {};
        const auto reserved = client_->ReserveTexture(device, &descriptor);
        gpu::webgpu::ReservedTexture result = { reserved.texture, reserved.handle.id, reserved.handle.generation,
            reserved.deviceHandle.id, reserved.deviceHandle.generation };
        bool injected = false;
        {
            std::lock_guard lock(Native().mutex);
            WGPUDevice native_device = server_->GetDevice(result.deviceId, result.deviceGeneration);
            if (native_device) {
                WGPUTextureDescriptor native_descriptor = descriptor;
                WGPUDawnTextureInternalUsageDescriptor internal = {};
                if (descriptor.nextInChain && descriptor.nextInChain->sType == WGPUSType_DawnTextureInternalUsageDescriptor)
                    internal = *reinterpret_cast<const WGPUDawnTextureInternalUsageDescriptor*>(descriptor.nextInChain);
                else
                    internal.chain.next = descriptor.nextInChain;
                internal.chain.sType = WGPUSType_DawnTextureInternalUsageDescriptor;
                internal.internalUsage |= WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
                native_descriptor.nextInChain = &internal.chain;
                WGPUTexture texture = NativeProcs().deviceCreateTexture(native_device, &native_descriptor);
                if (texture) {
                    injected = server_->InjectTexture(texture, reserved.handle, reserved.deviceHandle);
                    if (injected) {
                        const uint64_t key = TextureKey(result);
                        CHECK(!canvas_textures_.contains(key));
                        canvas_textures_.emplace(key, std::make_unique<CanvasTexture>(native_device, texture, result, descriptor));
                    } else {
                        NativeProcs().textureRelease(texture);
                    }
                }
            }
        }
        if (!injected) {
            // A missing device or failed injection is a broken wire contract,
            // not a WebGPU validation error (Dawn represents those by objects).
            // Disconnect before any unbacked unregister could reach the server.
            SerializationError();
            WireProcs().textureRelease(reserved.texture);
            return {};
        }
        SchedulePump();
        return result;
    }

    bool ReadCanvasTexture(const gpu::webgpu::ReservedTexture& reservation, void* pixels, size_t row_bytes) override
    {
        CheckOwner();
        if (IsLost() || !pixels)
            return false;
        // This flush only reaches the native server. Responses are dispatched by
        // a later owner task, never reentrantly in canvas snapshot/readback.
        FlushCommands();
        if (IsLost())
            return false;
        std::lock_guard lock(Native().mutex);
        CanvasTexture* texture = FindCanvasTexture(reservation);
        if (!texture || !texture->width || !texture->height)
            return false;
        size_t bytes_per_pixel;
        switch (texture->format) {
        case WGPUTextureFormat_RGBA8Unorm:
        case WGPUTextureFormat_RGBA8UnormSrgb:
        case WGPUTextureFormat_BGRA8Unorm:
        case WGPUTextureFormat_BGRA8UnormSrgb:
            bytes_per_pixel = 4;
            break;
        case WGPUTextureFormat_RGBA16Float:
            bytes_per_pixel = 8;
            break;
        default:
            return false;
        }
        size_t tight_row;
        if (!base::CheckMul(texture->width, bytes_per_pixel).AssignIfValid(&tight_row) || row_bytes < tight_row)
            return false;
        size_t aligned_row;
        if (!base::CheckAdd(tight_row, size_t(255)).AssignIfValid(&aligned_row))
            return false;
        aligned_row &= ~size_t(255);
        size_t staging_size;
        size_t last_output_byte;
        if (aligned_row > std::numeric_limits<uint32_t>::max() ||
            !base::CheckMul(aligned_row, texture->height).AssignIfValid(&staging_size) ||
            !base::CheckAdd(base::CheckMul(row_bytes, texture->height - 1), tight_row).AssignIfValid(&last_output_byte))
            return false;
        if (!texture->staging || texture->staging_size < staging_size) {
            if (texture->staging)
                NativeProcs().bufferRelease(texture->staging);
            WGPUBufferDescriptor buffer_descriptor = {};
            buffer_descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
            buffer_descriptor.size = staging_size;
            texture->staging = NativeProcs().deviceCreateBuffer(texture->device, &buffer_descriptor);
            texture->staging_size = staging_size;
        }
        if (!texture->staging)
            return false;
        // An invalid/destroyed canvas texture must fail the snapshot rather than
        // map and expose pixels left in the recycled staging buffer. This scope
        // is internal and does not disturb the application's outer error scopes.
        NativeProcs().devicePushErrorScope(texture->device, WGPUErrorFilter_Validation);
        WGPUDawnEncoderInternalUsageDescriptor internal = {};
        internal.chain.sType = WGPUSType_DawnEncoderInternalUsageDescriptor;
        internal.useInternalUsages = true;
        WGPUCommandEncoderDescriptor encoder_descriptor = {};
        encoder_descriptor.nextInChain = &internal.chain;
        WGPUCommandEncoder encoder = NativeProcs().deviceCreateCommandEncoder(texture->device, &encoder_descriptor);
        WGPUImageCopyTexture source = {};
        source.texture = texture->texture;
        source.aspect = WGPUTextureAspect_All;
        WGPUImageCopyBuffer destination = {};
        destination.buffer = texture->staging;
        destination.layout.bytesPerRow = static_cast<uint32_t>(aligned_row);
        destination.layout.rowsPerImage = texture->height;
        WGPUExtent3D extent = { texture->width, texture->height, 1 };
        NativeProcs().commandEncoderCopyTextureToBuffer(encoder, &source, &destination, &extent);
        WGPUCommandBuffer command = NativeProcs().commandEncoderFinish(encoder, nullptr);
        WGPUQueue queue = NativeProcs().deviceGetQueue(texture->device);
        NativeProcs().queueSubmit(queue, 1, &command);
        NativeProcs().queueRelease(queue);
        NativeProcs().commandBufferRelease(command);
        NativeProcs().commandEncoderRelease(encoder);
        bool valid_copy = false;
        WGPUPopErrorScopeCallbackInfo2 copy_callback = {};
        copy_callback.mode = WGPUCallbackMode_WaitAnyOnly;
        copy_callback.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType error, WGPUStringView, void* userdata, void*) {
            *static_cast<bool*>(userdata) = status == WGPUPopErrorScopeStatus_Success && error == WGPUErrorType_NoError;
        };
        copy_callback.userdata1 = &valid_copy;
        WGPUFutureWaitInfo wait = {};
        wait.future = NativeProcs().devicePopErrorScope2(texture->device, copy_callback);
        CHECK_EQ(NativeProcs().instanceWaitAny(Native().instance->Get(), 1, &wait, std::numeric_limits<uint64_t>::max()), WGPUWaitStatus_Success);
        if (!valid_copy)
            return false;

        bool mapped = false;
        WGPUBufferMapCallbackInfo2 callback = {};
        callback.mode = WGPUCallbackMode_WaitAnyOnly;
        callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* userdata, void*) {
            *static_cast<bool*>(userdata) = status == WGPUMapAsyncStatus_Success;
        };
        callback.userdata1 = &mapped;
        wait = {};
        wait.future = NativeProcs().bufferMapAsync2(texture->staging, WGPUMapMode_Read, 0, staging_size, callback);
        WGPUWaitStatus status = NativeProcs().instanceWaitAny(Native().instance->Get(), 1, &wait, std::numeric_limits<uint64_t>::max());
        // A single future on our timed-wait-enabled native instance must be
        // waitable. Never let a pending callback retain stack userdata.
        CHECK_EQ(status, WGPUWaitStatus_Success);
        if (!mapped)
            return false;
        const void* data = NativeProcs().bufferGetConstMappedRange(texture->staging, 0, staging_size);
        if (data) {
            auto* output = static_cast<uint8_t*>(pixels);
            const auto* input = static_cast<const uint8_t*>(data);
            for (uint32_t row = 0; row < texture->height; ++row)
                std::memcpy(output + size_t(row) * row_bytes, input + size_t(row) * aligned_row, tight_row);
        }
        NativeProcs().bufferUnmap(texture->staging);
        return data != nullptr;
    }

    void ReleaseCanvasTexture(const gpu::webgpu::ReservedTexture& reservation) override
    {
        CheckOwner();
        std::lock_guard lock(Native().mutex);
        if (FindCanvasTexture(reservation))
            canvas_textures_.erase(TextureKey(reservation));
        // The caller still owns the returned wire reference. Its eventual
        // unregister separately releases the wire server's injected reference.
    }

    void GenSyncTokenCHROMIUM(GLbyte*) override { Unsupported("GenSyncTokenCHROMIUM"); }
    void GenUnverifiedSyncTokenCHROMIUM(GLbyte*) override { Unsupported("GenUnverifiedSyncTokenCHROMIUM"); }
    void VerifySyncTokensCHROMIUM(GLbyte**, GLsizei) override { Unsupported("VerifySyncTokensCHROMIUM"); }
    void WaitSyncTokenCHROMIUM(const GLbyte*) override { Unsupported("WaitSyncTokenCHROMIUM"); }
    void ShallowFlushCHROMIUM() override { FlushCommands(); }
    void AssociateMailbox(GLuint, GLuint, GLuint, GLuint, uint64_t, uint64_t, const WGPUTextureFormat*, GLuint,
        gpu::webgpu::MailboxFlags, const gpu::Mailbox&) override { Unsupported("AssociateMailbox"); }
    void AssociateMailboxForBuffer(GLuint, GLuint, GLuint, GLuint, uint64_t, const gpu::Mailbox&) override { Unsupported("AssociateMailboxForBuffer"); }
    void DissociateMailbox(GLuint, GLuint) override { Unsupported("DissociateMailbox"); }
    void DissociateMailboxForBuffer(GLuint, GLuint) override { Unsupported("DissociateMailboxForBuffer"); }
    void DissociateMailboxForPresent(GLuint, GLuint, GLuint, GLuint) override { Unsupported("DissociateMailboxForPresent"); }
    void SetWebGPUExecutionContextToken(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override
    {
        Unsupported("SetWebGPUExecutionContextToken (GPU-process attribution)");
    }

    bool SendCommands(const char* bytes, size_t size)
    {
        if (IsLost())
            return false;
        std::lock_guard lock(Native().mutex);
        NativeCallbackScope callbacks(completions_);
        // Dawn returns a non-null success token, not an end-of-buffer cursor.
        if (!server_->HandleCommands(bytes, size)) {
            SerializationError();
            return false;
        }
        return true;
    }

    void SerializationError()
    {
        LOG(ERROR) << "Dawn wire command serialization failed";
        failed_ = true;
        SchedulePump();
    }

    void SchedulePump(base::TimeDelta delay = base::TimeDelta())
    {
        std::lock_guard lock(task_mutex_);
        if (disconnected_ || !runner_ || task_pending_)
            return;
        task_pending_ = true;
        runner_->PostDelayedTask(FROM_HERE, base::BindOnce(&LocalChannel::Pump, weak_), delay);
    }

private:
    ~LocalChannel() override
    {
        CheckOwner();
        CHECK(disconnected_);
        // Disconnect leaves these allocations alive for mapped buffers. Only
        // the final strong channel reference is allowed to destroy the client.
        if (instance_)
            WireProcs().instanceRelease(instance_);
        client_.reset();
    }

    void CheckOwner() const { CHECK(!bound_ || owner_ == std::this_thread::get_id()); }
    static uint64_t TextureKey(const gpu::webgpu::ReservedTexture& texture)
    {
        return uint64_t(texture.generation) << 32 | texture.id;
    }
    CanvasTexture* FindCanvasTexture(const gpu::webgpu::ReservedTexture& reservation)
    {
        auto found = canvas_textures_.find(TextureKey(reservation));
        if (found == canvas_textures_.end())
            return nullptr;
        const auto& actual = found->second->reservation;
        if (actual.deviceId != reservation.deviceId || actual.deviceGeneration != reservation.deviceGeneration || actual.texture != reservation.texture)
            return nullptr;
        return found->second.get();
    }

    void Pump()
    {
        CheckOwner();
        scoped_refptr<LocalChannel> keep_alive(this);
        {
            std::lock_guard lock(task_mutex_);
            task_pending_ = false;
        }
        if (disconnected_)
            return;
        if (failed_) {
            auto lost = lost_callback_;
            auto error = error_callback_;
            Disconnect();
            if (error)
                error.Run("Dawn wire command serialization failed", 0);
            if (lost)
                lost.Run();
            return;
        }
        // Flush outgoing traffic, advance actual native GPU work, then detach
        // the complete replies before invoking any Blink code outside the lock.
        outgoing_.Flush();
        awaiting_flush_ = false;
        bool pending;
        std::vector<ServerSerializer::Block> replies;
        {
            std::lock_guard lock(Native().mutex);
            pending = dawn::native::InstanceProcessEvents(Native().instance->Get());
            completions_->Drain();
            FlushNativeLogs();
            replies = incoming_.Drain();
        }
        for (const auto& block : replies) {
            if (disconnected_ || failed_)
                break;
            if (!client_->HandleCommands(block.bytes.get(), block.used))
                SerializationError();
        }
        incoming_.Recycle(std::move(replies));
        if (disconnected_)
            return;
        WireProcs().instanceProcessEvents(instance_);
        // Dawn reports whether genuine progressing work remains (device loss
        // futures alone are non-progressing). Idle channels have no timer.
        if (pending || failed_ || outgoing_.HasCommands())
            SchedulePump(base::Milliseconds(1));
    }

    std::shared_ptr<NativeCompletionQueue> completions_ = std::make_shared<NativeCompletionQueue>();
    ClientSerializer outgoing_;
    ServerSerializer incoming_;
    DawnProcTable server_procs_ = {};
    std::unique_ptr<dawn::wire::WireClient> client_;
    std::unique_ptr<dawn::wire::WireServer> server_;
    WGPUInstance instance_ = nullptr;
    std::map<uint64_t, std::unique_ptr<CanvasTexture>> canvas_textures_;
    bool bound_ = false;
    std::thread::id owner_;
    bool awaiting_flush_ = false;
    std::atomic<bool> disconnected_ = false;
    std::atomic<bool> failed_ = false;
    std::mutex task_mutex_;
    scoped_refptr<base::SingleThreadTaskRunner> runner_;
    base::WeakPtr<LocalChannel> weak_;
    bool task_pending_ = false;
    base::RepeatingClosure lost_callback_;
    base::RepeatingCallback<void(const char*, int32_t)> error_callback_;
    base::WeakPtrFactory<LocalChannel> weak_factory_ { this };
};

ClientSerializer::ClientSerializer(LocalChannel* channel)
    : channel_(channel), buffer_(std::make_unique<char[]>(kCapacity)) { }

void* ClientSerializer::GetCmdSpace(size_t size)
{
    if (disconnected_)
        return nullptr;
    CHECK_LE(size, kCapacity);
    if (size > kCapacity - used_ && !Flush())
        return nullptr;
    char* result = buffer_.get() + used_;
    used_ += size;
    channel_->SchedulePump();
    return result;
}

bool ClientSerializer::Flush()
{
    if (disconnected_)
        return false;
    if (!used_)
        return true;
    const size_t size = std::exchange(used_, 0);
    return channel_->SendCommands(buffer_.get(), size);
}

void ClientSerializer::OnSerializeError() { channel_->SerializationError(); }

void ServerSerializer::Seal()
{
    if (current_.used)
        ready_.push_back(std::move(current_));
    current_ = {};
}

void* ServerSerializer::GetCmdSpace(size_t size)
{
    std::lock_guard lock(mutex_);
    if (disconnected_)
        return nullptr;
    CHECK_LE(size, kCapacity);
    if (size > kCapacity - current_.used)
        Seal();
    if (!current_.bytes) {
        if (!recycled_.empty()) {
            current_ = std::move(recycled_.back());
            recycled_.pop_back();
        } else {
            current_.bytes = std::make_unique<char[]>(kCapacity);
        }
    }
    char* result = current_.bytes.get() + current_.used;
    current_.used += size;
    channel_->SchedulePump();
    return result;
}

bool ServerSerializer::Flush()
{
    std::lock_guard lock(mutex_);
    if (disconnected_)
        return false;
    Seal();
    channel_->SchedulePump();
    return true;
}

void ServerSerializer::OnSerializeError() { channel_->SerializationError(); }

std::vector<ServerSerializer::Block> ServerSerializer::Drain()
{
    std::lock_guard lock(mutex_);
    Seal();
    return std::exchange(ready_, {});
}

void ServerSerializer::Recycle(std::vector<Block> blocks)
{
    std::lock_guard lock(mutex_);
    if (disconnected_)
        return;
    for (auto& block : blocks) {
        block.used = 0;
        recycled_.push_back(std::move(block));
    }
}

void ServerSerializer::Disconnect()
{
    std::lock_guard lock(mutex_);
    disconnected_ = true;
    current_ = {};
    ready_.clear();
    recycled_.clear();
}

class WebGPUContextProvider final : public blink::WebGraphicsContext3DProvider {
public:
    WebGPUContextProvider() : channel_(base::MakeRefCounted<LocalChannel>())
    {
        caps_.max_texture_size = Native().max_texture_size;
        caps_.gpu_memory_buffer_formats.Clear();
        caps_.context_supports_distance_field_text = false;
        for (auto& status : features_.status_values)
            status = gpu::kGpuFeatureStatusDisabled;
        features_.status_values[gpu::GPU_FEATURE_TYPE_ACCELERATED_WEBGPU] = gpu::kGpuFeatureStatusEnabled;
#if BUILDFLAG(IS_MAC)
        features_.status_values[gpu::GPU_FEATURE_TYPE_METAL] = gpu::kGpuFeatureStatusEnabled;
#endif
    }
    ~WebGPUContextProvider() override
    {
        channel_->SetLostCallback({});
        channel_->SetErrorCallback({});
        channel_->Disconnect();
    }
    gpu::InterfaceBase* InterfaceBase() override { return channel_.get(); }
    gpu::gles2::GLES2Interface* ContextGL() override { return nullptr; }
    gpu::raster::RasterInterface* RasterInterface() override { return nullptr; }
    gpu::webgpu::WebGPUInterface* WebGPUInterface() override { return channel_.get(); }
    gpu::webgpu::NativeTextureInterface* WebGPUTextureInterface() override { return channel_.get(); }
    gpu::ContextSupport* ContextSupport() override { return nullptr; }
    bool IsContextLost() override { return channel_->IsLost(); }
    bool BindToCurrentSequence() override { return channel_->Bind(); }
    GrDirectContext* GetGrContext() override { return nullptr; }
    const gpu::Capabilities& GetCapabilities() const override { return caps_; }
    const gpu::GpuFeatureInfo& GetGpuFeatureInfo() const override { return features_; }
    const blink::WebglPreferences& GetWebglPreferences() const override { return preferences_; }
    gpu::GLHelper* GetGLHelper() override { Unsupported("GetGLHelper"); }
    void SetLostContextCallback(base::RepeatingClosure callback) override { channel_->SetLostCallback(std::move(callback)); }
    void SetErrorMessageCallback(base::RepeatingCallback<void(const char*, int32_t)> callback) override { channel_->SetErrorCallback(std::move(callback)); }
    cc::ImageDecodeCache* ImageDecodeCache(SkColorType) override { Unsupported("ImageDecodeCache"); }
    gpu::SharedImageInterface* SharedImageInterface() override { return nullptr; }
    void CopyVideoFrame(media::PaintCanvasVideoRenderer*, media::VideoFrame*, cc::PaintCanvas*) override { Unsupported("CopyVideoFrame"); }
    viz::RasterContextProvider* RasterContextProvider() const override { return nullptr; }
    unsigned int GetGrGLTextureFormat(viz::SharedImageFormat) const override { Unsupported("GetGrGLTextureFormat"); }

private:
    scoped_refptr<LocalChannel> channel_;
    gpu::Capabilities caps_;
    gpu::GpuFeatureInfo features_;
    blink::WebglPreferences preferences_;
};

} // namespace

bool InitializeWebGPU()
{
    static std::once_flag once;
    static bool initialized = false;
    std::call_once(once, [] {
        if (BackendType() == WGPUBackendType_Undefined)
            return;
        std::lock_guard lock(Native().mutex);
        WGPUInstanceDescriptor descriptor = {};
        descriptor.features.timedWaitAnyEnable = true;
        descriptor.features.timedWaitAnyMaxCount = 1;
        Native().instance = std::make_unique<dawn::native::Instance>(&descriptor);
        WGPURequestAdapterOptions options = {};
        options.backendType = BackendType();
        Native().adapters = Native().instance->EnumerateAdapters(&options);
        if (Native().adapters.empty()) {
            LOG(ERROR) << "WebGPU native backend has no adapter";
            return;
        }
        NativeDeviceDescriptor device_descriptor(nullptr);
        Native().warm_device = Native().adapters.front().CreateDevice(&device_descriptor.value);
        if (!Native().warm_device) {
            LOG(ERROR) << "WebGPU native backend device initialization failed";
            return;
        }
        WGPUSupportedLimits limits = {};
        if (!Native().adapters.front().GetLimits(&limits))
            return;
        Native().max_texture_size = static_cast<int>(std::min<uint32_t>(limits.limits.maxTextureDimension2D, std::numeric_limits<int>::max()));
        initialized = true;
    });
    return initialized;
}

std::unique_ptr<blink::WebGraphicsContext3DProvider> CreateWebGPUContextProvider()
{
    if (!InitializeWebGPU())
        return nullptr;
    return std::make_unique<WebGPUContextProvider>();
}

} // namespace content
