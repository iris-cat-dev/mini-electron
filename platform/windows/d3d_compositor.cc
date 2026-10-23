// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "platform/windows/d3d_compositor.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include "base/check.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "components/viz/service/display/software_output_device.h"
#include "runtime/engine/browser/web_view_host.h"
#include "runtime/engine/common/live_id_detect.h"
#include "runtime/engine/common/thread_call.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/gpu/ShaderErrorHandler.h"
#include "third_party/skia/include/gpu/ganesh/GrBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/GrContextOptions.h"
#include "third_party/skia/include/gpu/ganesh/GrDirectContext.h"
#include "third_party/skia/include/gpu/ganesh/SkImageGanesh.h"
#include "third_party/skia/include/gpu/ganesh/SkSurfaceGanesh.h"
#include "third_party/skia/include/gpu/ganesh/d3d/GrD3DBackendContext.h"
#include "third_party/skia/include/gpu/ganesh/d3d/GrD3DTypes.h"

namespace mini_electron::win {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int64_t kMaximumSurfaceBytes = 256ll * 1024 * 1024;

uint64_t NextSourceId() {
  static std::atomic<uint64_t> next_id{0};
  const uint64_t id = next_id.fetch_add(1, std::memory_order_relaxed) + 1;
  CHECK_NE(id, 0u);
  return id;
}

struct D3DDevice {
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  LUID luid{};
  bool ready = false;
};

D3DDevice& Device() {
  static base::NoDestructor<D3DDevice> device;
  return *device;
}

bool GpuDisabled() {
  const char* disabled = std::getenv("MINI_ELECTRON_DISABLE_GPU");
  if (disabled && *disabled && std::strcmp(disabled, "0") != 0)
    return true;
  if (!base::CommandLine::InitializedForCurrentProcess())
    return false;
  const auto* command_line = base::CommandLine::ForCurrentProcess();
  return command_line->HasSwitch("disable-gpu") ||
         command_line->HasSwitch("disable-gpu-compositing");
}

bool PrepareD3DDevice() {
  if (GpuDisabled())
    return false;

  static std::once_flag initialized;
  std::call_once(initialized, [] {
    auto& state = Device();

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      std::fprintf(stderr,
                   "[mini-electron] DXGI factory creation failed; using CPU composition\n");
      return;
    }

    for (UINT index = 0;; ++index) {
      ComPtr<IDXGIAdapter1> adapter;
      HRESULT result = factory->EnumAdapterByGpuPreference(
          index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
      if (result == DXGI_ERROR_NOT_FOUND)
        break;
      if (FAILED(result))
        continue;

      DXGI_ADAPTER_DESC1 description{};
      if (FAILED(adapter->GetDesc1(&description)) ||
          (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        continue;
      }

      ComPtr<ID3D12Device> device;
      if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                   IID_PPV_ARGS(&device)))) {
        continue;
      }

      D3D12_COMMAND_QUEUE_DESC queue_description{};
      queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
      queue_description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
      ComPtr<ID3D12CommandQueue> queue;
      if (FAILED(device->CreateCommandQueue(&queue_description,
                                            IID_PPV_ARGS(&queue)))) {
        continue;
      }

      state.adapter = std::move(adapter);
      state.device = std::move(device);
      state.queue = std::move(queue);
      state.luid = description.AdapterLuid;
      state.ready = true;
      std::fwprintf(stderr,
                    L"[mini-electron] D3D12 compositor active: adapter=%ls\n",
                    description.Description);
      return;
    }

    std::fprintf(stderr,
                 "[mini-electron] no hardware D3D12 adapter; using CPU composition\n");
  });
  return Device().ready;
}

class D3DOutputDevice;
struct Endpoint {
  scoped_refptr<base::SingleThreadTaskRunner> runner;
  base::WeakPtr<D3DOutputDevice> device;
};

struct D3DFrameCopyResult {
  SkBitmap bitmap;
  float scale_factor = 1.f;
};

struct D3DFrameIdentity {
  uint64_t frame_id = 0;
  uint64_t source_id = 0;
};

struct Registry {
  std::mutex mutex;
  std::map<int64_t, Endpoint> endpoints;
  std::map<int64_t, base::RepeatingCallback<void(const D3DFrame&)>> callbacks;
  std::map<int64_t, D3DFrameIdentity> delivered_frames;
  std::map<int64_t, D3DFrame> deferred_frames;
  std::set<int64_t> active_readbacks;
};
Registry& GlobalRegistry() {
  static base::NoDestructor<Registry> registry;
  return *registry;
}

void AcknowledgeFrameInternal(int64_t view_id,
                              uint64_t frame_id,
                              uint64_t source_id,
                              bool unconsumed);

void DispatchFrame(int64_t view_id, D3DFrame frame) {
  content::ThreadCall::callUiThreadAsync(FROM_HERE, [view_id, frame] {
    base::RepeatingCallback<void(const D3DFrame&)> callback;
    {
      auto& registry = GlobalRegistry();
      std::lock_guard<std::mutex> lock(registry.mutex);
      if (!frame.error && registry.active_readbacks.count(view_id)) {
        registry.deferred_frames[view_id] = frame;
        return;
      }
      auto found = registry.callbacks.find(view_id);
      if (found != registry.callbacks.end()) {
        callback = found->second;
        if (!frame.error) {
          registry.delivered_frames[view_id] = {frame.frame_id,
                                                frame.source_id};
        }
      }
    }
    if (callback)
      callback.Run(frame);
    else if (!frame.error)
      AcknowledgeFrameInternal(view_id, frame.frame_id, frame.source_id, true);

    if (frame.error)
      return;
    auto* ids = common::LiveIdDetect::getWebViewIds();
    auto* view = static_cast<content::WebViewHost*>(ids->getPtrLocked(view_id));
    if (view)
      view->onFramePublishedInUiThread();
    ids->unlock(view_id, view);
  });
}

class D3DOutputDevice final : public viz::SoftwareOutputDevice,
                              private skgpu::ShaderErrorHandler {
 public:
  D3DOutputDevice(int64_t view_id, bool transparent)
      : view_id_(view_id),
        transparent_(transparent),
        runner_(base::SingleThreadTaskRunner::GetCurrentDefault()) {}

  bool Initialize() {
    DCHECK(runner_->RunsTasksInCurrentSequence());
    if (!PrepareD3DDevice())
      return false;

    GrD3DBackendContext backend;
    backend.fAdapter.retain(Device().adapter.Get());
    backend.fDevice.retain(Device().device.Get());
    backend.fQueue.retain(Device().queue.Get());
    GrContextOptions options;
    options.fShaderErrorHandler = this;
    context_ = GrDirectContext::MakeDirect3D(backend, options);
    if (!context_ || failed_)
      return false;

    if (FAILED(Device().device->CreateFence(
            0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_))) ||
        FAILED(Device().device->CreateSharedHandle(
            fence_.Get(), nullptr, GENERIC_ALL, nullptr, &fence_handle_))) {
      return false;
    }
    ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    readback_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready_event_ || !readback_event_)
      return false;

    {
      std::lock_guard<std::mutex> lock(GlobalRegistry().mutex);
      GlobalRegistry().endpoints[view_id_] = {
          runner_, weak_factory_.GetWeakPtr()};
    }
    return true;
  }

  ~D3DOutputDevice() override {
    DCHECK(runner_->RunsTasksInCurrentSequence());
    {
      auto& registry = GlobalRegistry();
      std::lock_guard<std::mutex> lock(registry.mutex);
      auto found = registry.endpoints.find(view_id_);
      if (found != registry.endpoints.end() &&
          found->second.device.get() == this) {
        registry.endpoints.erase(found);
      }
      registry.delivered_frames.erase(view_id_);
      registry.deferred_frames.erase(view_id_);
      registry.active_readbacks.erase(view_id_);
    }
    weak_factory_.InvalidateWeakPtrs();
    if (ready_wait_in_flight_)
      ready_wait_finished_.Wait();
    if (context_ && !context_->abandoned())
      context_->flushAndSubmit(GrSyncCpu::kYes);
    images_.clear();
    surface_.reset();
    backend_render_target_ = {};
    texture_.Reset();
    if (texture_handle_)
      CloseHandle(texture_handle_);
    ReleaseRetiredSource();
    if (fence_handle_)
      CloseHandle(fence_handle_);
    if (ready_event_)
      CloseHandle(ready_event_);
    if (readback_event_)
      CloseHandle(readback_event_);
    context_.reset();
  }

  // The offscreen backing can follow the submitted root frame. Resize before
  // that frame's damage, not after its ACK when a static page may already idle.
  bool SupportsOverridePlatformSize() const override { return true; }

  void Resize(const gfx::Size& size, float scale) override {
    DCHECK(runner_->RunsTasksInCurrentSequence());
    if (viewport_pixel_size_ == size && scale_factor_ == scale)
      return;
    viewport_pixel_size_ = size;
    scale_factor_ = std::isfinite(scale) && scale > 0.f ? scale : 1.f;
    root_has_drawn_ = false;
    surface_.reset();
    backend_render_target_ = {};
    if (texture_handle_) {
      if (!pending_swap_callback_.is_null() &&
          pending_frame_.source_id == source_id_) {
        DCHECK(!retired_texture_);
        DCHECK(!retired_texture_handle_);
        retired_texture_ = std::move(texture_);
        retired_texture_handle_ = texture_handle_;
      } else {
        texture_.Reset();
        CloseHandle(texture_handle_);
      }
      texture_handle_ = nullptr;
    } else {
      texture_.Reset();
    }
    if (size.IsEmpty() || !Usable())
      return;
    if (static_cast<int64_t>(size.width()) * size.height() * 4 >
        kMaximumSurfaceBytes) {
      Fail(7);
      return;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = size.width();
    description.Height = size.height();
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                        D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = description.Format;
    clear.Color[0] = clear.Color[1] = clear.Color[2] = 0.f;
    clear.Color[3] = transparent_ ? 0.f : 1.f;
    if (FAILED(Device().device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_SHARED, &description,
            D3D12_RESOURCE_STATE_COMMON, &clear,
            IID_PPV_ARGS(&texture_))) ||
        FAILED(Device().device->CreateSharedHandle(
            texture_.Get(), nullptr, GENERIC_ALL, nullptr, &texture_handle_))) {
      Fail(7);
      return;
    }

    GrD3DTextureResourceInfo resource_info;
    resource_info.fResource.retain(texture_.Get());
    resource_info.fResourceState = D3D12_RESOURCE_STATE_COMMON;
    resource_info.fFormat = description.Format;
    resource_info.fSampleCount = 1;
    resource_info.fLevelCount = 1;
    backend_render_target_ =
        GrBackendRenderTarget(size.width(), size.height(), resource_info);
    surface_ = SkSurfaces::WrapBackendRenderTarget(
        context_.get(), backend_render_target_, kTopLeft_GrSurfaceOrigin,
        kBGRA_8888_SkColorType, SkColorSpace::MakeSRGB(), nullptr);
    if (!surface_) {
      Fail(7);
      return;
    }
    source_id_ = NextSourceId();
    surface_->getCanvas()->clear(transparent_ ? SK_ColorTRANSPARENT
                                               : SK_ColorWHITE);
  }

  SkCanvas* BeginPaint(const gfx::Rect& damage) override {
    DCHECK(runner_->RunsTasksInCurrentSequence());
    damage_rect_ = damage;
    return Usable() && surface_ ? surface_->getCanvas() : nullptr;
  }

  void EndPaint() override { root_has_drawn_ = surface_ && !failed_; }

  sk_sp<SkSurface> CreateRenderPassSurface(const SkImageInfo& info) override {
    if (!Usable())
      return nullptr;
    return SkSurfaces::RenderTarget(context_.get(), skgpu::Budgeted::kYes,
                                    info, 0, kTopLeft_GrSurfaceOrigin, nullptr);
  }

  sk_sp<SkImage> PrepareImageForDraw(sk_sp<SkImage> image) override {
    if (!image || !Usable())
      return nullptr;
    if (image->isTextureBacked())
      return image;
    const uint32_t key = image->uniqueID();
    auto found = images_.find(key);
    if (found != images_.end()) {
      found->second.last_frame = frame_id_;
      return found->second.texture;
    }
    auto texture = SkImages::TextureFromImage(context_.get(), image);
    if (!texture) {
      Fail(6);
      return nullptr;
    }
    images_.emplace(key, Image{std::move(image), texture, frame_id_});
    return texture;
  }

  void OnSwapBuffers(SwapBuffersCallback callback, gfx::FrameData) override {
    DCHECK(runner_->RunsTasksInCurrentSequence());
    CHECK(pending_swap_callback_.is_null());
    if (!Usable() || !surface_) {
      std::move(callback).Run(viewport_pixel_size_);
      return;
    }

    if (!FlushRootForExternalAccess(GrSyncCpu::kNo)) {
      Fail(3);
      std::move(callback).Run(viewport_pixel_size_);
      return;
    }

    const uint64_t next_frame = ++frame_id_;
    const uint64_t ready_value = next_frame * 2;
    if (FAILED(Device().queue->Signal(fence_.Get(), ready_value))) {
      Fail(4);
      std::move(callback).Run(viewport_pixel_size_);
      return;
    }

    pending_frame_ = {reinterpret_cast<uint64_t>(texture_handle_),
                      reinterpret_cast<uint64_t>(fence_handle_),
                      ready_value,
                      next_frame,
                      source_id_,
                      static_cast<uint32_t>(Device().luid.LowPart),
                      static_cast<int32_t>(Device().luid.HighPart),
                      surface_->width(),
                      surface_->height(),
                      scale_factor_,
                      0};
    pending_swap_callback_ = std::move(callback);
    pending_published_ = false;
    pending_frame_counted_ = false;

    if (!AwaitReady(next_frame)) {
      Fail(4);
      auto failed_callback = std::move(pending_swap_callback_);
      pending_frame_ = {};
      std::move(failed_callback).Run(viewport_pixel_size_);
      return;
    }

    for (auto it = images_.begin(); it != images_.end();) {
      if (it->second.last_frame + 2 < frame_id_)
        it = images_.erase(it);
      else
        ++it;
    }
  }

  void CompleteReady(uint64_t frame_id, bool success) {
    DCHECK(runner_->RunsTasksInCurrentSequence());
    ready_wait_in_flight_ = false;
    if (pending_swap_callback_.is_null() || pending_published_ ||
        pending_frame_.frame_id != frame_id) {
      return;
    }
    if (!success || failed_) {
      if (!success)
        Fail(5);
      auto callback = std::move(pending_swap_callback_);
      const gfx::Size size(pending_frame_.width, pending_frame_.height);
      ReleaseRetiredSource();
      pending_frame_ = {};
      pending_published_ = false;
      std::move(callback).Run(size);
      return;
    }
    pending_published_ = true;
    if (!pending_frame_counted_) {
      ++completed_frames_;
      pending_frame_counted_ = true;
    }
    DispatchFrame(view_id_, pending_frame_);
  }

  void Acknowledge(uint64_t frame_id,
                   uint64_t source_id,
                   bool unconsumed) {
    DCHECK(runner_->RunsTasksInCurrentSequence());
    if (pending_swap_callback_.is_null() || !pending_published_ ||
        pending_frame_.frame_id != frame_id ||
        pending_frame_.source_id != source_id) {
      return;
    }

    HRESULT result = unconsumed
                         ? Device().queue->Signal(fence_.Get(),
                                                  pending_frame_.ready_value + 1)
                         : Device().queue->Wait(fence_.Get(),
                                                pending_frame_.ready_value + 1);
    auto callback = std::move(pending_swap_callback_);
    const gfx::Size size(pending_frame_.width, pending_frame_.height);
    ReleaseRetiredSource();
    pending_frame_ = {};
    pending_published_ = false;
    pending_frame_counted_ = false;
    if (FAILED(result))
      Fail(5);
    std::move(callback).Run(size);
  }

  SkBitmap Readback() {
    DCHECK(runner_->RunsTasksInCurrentSequence());
    SkBitmap result;
    if (!Usable() || !surface_ || !root_has_drawn_)
      return result;

    // Once the UI callback has begun, the frame remains externally owned until
    // the D3D11 consumer's odd release value completes. The asynchronous
    // capture boundary leaves that sequence free to import and release it.
    // Otherwise the registry defers publication, so producer-ready is safe;
    // Ganesh restores COMMON before deferred publication resumes.
    const bool callback_delivered = BeginReadback();
    base::ScopedClosureRunner readback_gate(base::BindOnce(
        &D3DOutputDevice::EndReadback, base::Unretained(this)));
    if (!pending_swap_callback_.is_null()) {
      const uint64_t safe_value =
          pending_frame_.ready_value + (callback_delivered ? 1 : 0);
      uint64_t completed_value = fence_->GetCompletedValue();
      if (completed_value == UINT64_MAX) {
        Fail(5);
        ReleasePendingSwap();
        return {};
      }
      if (completed_value < safe_value) {
        ResetEvent(readback_event_);
        if (FAILED(fence_->SetEventOnCompletion(safe_value,
                                                readback_event_)) ||
            WaitForSingleObject(readback_event_, INFINITE) != WAIT_OBJECT_0 ||
            (completed_value = fence_->GetCompletedValue()) == UINT64_MAX ||
            completed_value < safe_value) {
          Fail(5);
          ReleasePendingSwap();
          return {};
        }
      }
      if (!pending_frame_counted_) {
        ++completed_frames_;
        pending_frame_counted_ = true;
      }
    }

    const bool readback_succeeded =
        result.tryAllocPixels(SkImageInfo::MakeN32Premul(
            surface_->width(), surface_->height())) &&
        surface_->getCanvas()->readPixels(result, 0, 0);

    if (!FlushRootForExternalAccess(GrSyncCpu::kYes)) {
      Fail(3);
      ReleasePendingSwap();
      return {};
    }
    if (!readback_succeeded)
      return {};
    result.setImmutable();
    ++readbacks_;
    return result;
  }

  D3DFrameCopyResult ReadbackWithScale() {
    D3DFrameCopyResult result;
    result.scale_factor = scale_factor_;
    result.bitmap = Readback();
    return result;
  }

  D3DCompositorInfo Info() {
    return {!failed_ && context_ && !context_->abandoned(),
            completed_frames_, readbacks_};
  }

 private:
  struct Image {
    sk_sp<SkImage> source;
    sk_sp<SkImage> texture;
    uint64_t last_frame;
  };

  bool BeginReadback() {
    auto& registry = GlobalRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.active_readbacks.insert(view_id_);
    auto delivered = registry.delivered_frames.find(view_id_);
    return delivered != registry.delivered_frames.end() &&
           !pending_swap_callback_.is_null() &&
           delivered->second.frame_id == pending_frame_.frame_id &&
           delivered->second.source_id == pending_frame_.source_id;
  }

  void EndReadback() {
    D3DFrame deferred;
    bool has_deferred = false;
    {
      auto& registry = GlobalRegistry();
      std::lock_guard<std::mutex> lock(registry.mutex);
      registry.active_readbacks.erase(view_id_);
      auto found = registry.deferred_frames.find(view_id_);
      if (found != registry.deferred_frames.end()) {
        deferred = found->second;
        has_deferred = true;
        registry.deferred_frames.erase(found);
      }
    }
    if (has_deferred && !failed_)
      DispatchFrame(view_id_, deferred);
  }

  void ReleaseRetiredSource() {
    if (retired_texture_handle_) {
      CloseHandle(retired_texture_handle_);
      retired_texture_handle_ = nullptr;
    }
    retired_texture_.Reset();
  }

  void ReleasePendingSwap() {
    if (pending_swap_callback_.is_null())
      return;
    auto callback = std::move(pending_swap_callback_);
    const gfx::Size size(pending_frame_.width, pending_frame_.height);
    ReleaseRetiredSource();
    pending_frame_ = {};
    pending_published_ = false;
    pending_frame_counted_ = false;
    std::move(callback).Run(size);
  }

  bool AwaitReady(uint64_t frame_id) {
    DCHECK(!ready_wait_in_flight_);
    ready_wait_finished_.Reset();
    ResetEvent(ready_event_);
    const uint64_t ready_value = frame_id * 2;
    if (FAILED(fence_->SetEventOnCompletion(ready_value, ready_event_)))
      return false;

    auto runner = runner_;
    auto weak = weak_factory_.GetWeakPtr();
    auto* completion_fence = fence_.Get();
    auto* wait_finished = &ready_wait_finished_;
    ready_wait_in_flight_ = true;
    if (!base::ThreadPool::PostTask(
            FROM_HERE,
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
             base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
            base::BindOnce(
                [](ID3D12Fence* completion_fence, uintptr_t ready_event,
                   base::WaitableEvent* wait_finished,
                   scoped_refptr<base::SingleThreadTaskRunner> runner,
                   base::WeakPtr<D3DOutputDevice> weak, uint64_t frame_id) {
                  const DWORD wait_result =
                      WaitForSingleObject(reinterpret_cast<HANDLE>(ready_event), INFINITE);
                  const uint64_t completed_value =
                      completion_fence->GetCompletedValue();
                  const bool completed =
                      wait_result == WAIT_OBJECT_0 &&
                      completed_value != UINT64_MAX &&
                      SUCCEEDED(Device().device->GetDeviceRemovedReason()) &&
                      completed_value >= frame_id * 2;
                  wait_finished->Signal();
                  runner->PostTask(
                      FROM_HERE,
                      base::BindOnce(
                          [](base::WeakPtr<D3DOutputDevice> device,
                             uint64_t completed_frame_id, bool success) {
                            if (device)
                              device->CompleteReady(completed_frame_id, success);
                          },
                          weak, frame_id, completed));
                }, base::Unretained(completion_fence),
                reinterpret_cast<uintptr_t>(ready_event_),
                base::Unretained(wait_finished), std::move(runner), weak, frame_id))) {
      ready_wait_in_flight_ = false;
      return false;
    }
    return true;
  }

  // D3D12 PRESENT aliases COMMON. Ask Ganesh to emit the final transition so
  // its shared state tracker and the command list exposed to D3D11 cannot
  // disagree about the wrapped resource.
  bool FlushRootForExternalAccess(GrSyncCpu sync) {
    if (!surface_ || !context_)
      return false;
    context_->flush(surface_.get(),
                    SkSurfaces::BackendSurfaceAccess::kPresent,
                    GrFlushInfo());
    if (!context_->submit(sync))
      return false;
    GrD3DTextureResourceInfo resource_info;
    return backend_render_target_.getD3DTextureResourceInfo(&resource_info) &&
           resource_info.fResourceState == D3D12_RESOURCE_STATE_COMMON &&
           SUCCEEDED(Device().device->GetDeviceRemovedReason());
  }

  bool Usable() {
    if (!failed_ && context_ && !context_->abandoned())
      return true;
    if (!failed_)
      Fail(1);
    return false;
  }

  void Fail(int error) {
    if (failed_)
      return;
    failed_ = true;
    std::fprintf(stderr,
                 "[mini-electron] D3D12 compositor failure: view=%lld code=%d\n",
                 static_cast<long long>(view_id_), error);
    D3DFrame frame;
    frame.error = error;
    DispatchFrame(view_id_, frame);
  }

  void compileError(const char*, const char* errors, bool) override {
    std::fprintf(stderr,
                 "[mini-electron] D3D12 shader compilation failed: %s\n",
                 errors ? errors : "unknown error");
    Fail(8);
  }

  const int64_t view_id_;
  const bool transparent_;
  scoped_refptr<base::SingleThreadTaskRunner> runner_;
  sk_sp<GrDirectContext> context_;
  GrBackendRenderTarget backend_render_target_;
  sk_sp<SkSurface> surface_;
  ComPtr<ID3D12Resource> texture_;
  ComPtr<ID3D12Resource> retired_texture_;
  ComPtr<ID3D12Fence> fence_;
  HANDLE texture_handle_ = nullptr;
  HANDLE retired_texture_handle_ = nullptr;
  HANDLE fence_handle_ = nullptr;
  HANDLE ready_event_ = nullptr;
  HANDLE readback_event_ = nullptr;
  float scale_factor_ = 1.f;
  uint64_t frame_id_ = 0;
  uint64_t source_id_ = 0;
  uint64_t completed_frames_ = 0;
  uint64_t readbacks_ = 0;
  D3DFrame pending_frame_;
  SwapBuffersCallback pending_swap_callback_;
  bool root_has_drawn_ = false;
  bool pending_published_ = false;
  bool pending_frame_counted_ = false;
  bool failed_ = false;
  bool ready_wait_in_flight_ = false;
  base::WaitableEvent ready_wait_finished_;
  std::map<uint32_t, Image> images_;
  base::WeakPtrFactory<D3DOutputDevice> weak_factory_{this};
};

void AcknowledgeFrameInternal(int64_t view_id,
                              uint64_t frame_id,
                              uint64_t source_id,
                              bool unconsumed) {
  Endpoint endpoint;
  {
    std::lock_guard<std::mutex> lock(GlobalRegistry().mutex);
    auto found = GlobalRegistry().endpoints.find(view_id);
    if (found == GlobalRegistry().endpoints.end())
      return;
    endpoint = found->second;
  }
  if (endpoint.runner->RunsTasksInCurrentSequence()) {
    if (endpoint.device)
      endpoint.device->Acknowledge(frame_id, source_id, unconsumed);
    return;
  }
  endpoint.runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<D3DOutputDevice> device, uint64_t id,
             uint64_t source, bool was_unconsumed) {
            if (device)
              device->Acknowledge(id, source, was_unconsumed);
          },
          endpoint.device, frame_id, source_id, unconsumed));
}

}  // namespace

std::unique_ptr<viz::SoftwareOutputDevice> CreateD3DOutputDevice(
    int64_t view_id,
    bool transparent) {
  if (!view_id)
    return nullptr;
  {
    auto& registry = GlobalRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    auto callback = registry.callbacks.find(view_id);
    if (callback == registry.callbacks.end() || callback->second.is_null())
      return nullptr;
  }
  if (!PrepareD3DDevice())
    return nullptr;
  auto device = std::make_unique<D3DOutputDevice>(view_id, transparent);
  if (!device->Initialize()) {
    std::fprintf(stderr,
                 "[mini-electron] D3D12 surface initialization failed; using CPU composition\n");
    return nullptr;
  }
  return device;
}

void SetD3DFrameCallback(
    int64_t view_id,
    base::RepeatingCallback<void(const D3DFrame&)> callback) {
  std::lock_guard<std::mutex> lock(GlobalRegistry().mutex);
  if (callback)
    GlobalRegistry().callbacks[view_id] = std::move(callback);
  else
    GlobalRegistry().callbacks.erase(view_id);
}

void AcknowledgeD3DFrame(int64_t view_id,
                         uint64_t frame_id,
                         uint64_t source_id) {
  AcknowledgeFrameInternal(view_id, frame_id, source_id, false);
}

void CopyD3DFrameAsync(int64_t view_id, D3DFrameCopyCallback callback) {
  if (!callback)
    return;

  Endpoint endpoint;
  {
    std::lock_guard<std::mutex> lock(GlobalRegistry().mutex);
    auto found = GlobalRegistry().endpoints.find(view_id);
    if (found != GlobalRegistry().endpoints.end())
      endpoint = found->second;
  }

  if (!endpoint.runner) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](D3DFrameCopyCallback reply) {
              std::move(reply).Run(SkBitmap{}, 1.f);
            },
            std::move(callback)));
    return;
  }

  endpoint.runner->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<D3DOutputDevice> device) {
            return device ? device->ReadbackWithScale()
                          : D3DFrameCopyResult{};
          },
          endpoint.device),
      base::BindOnce(
          [](D3DFrameCopyCallback reply, D3DFrameCopyResult result) {
            std::move(reply).Run(std::move(result.bitmap),
                                 result.scale_factor);
          },
          std::move(callback)));
}

D3DCompositorInfo GetD3DCompositorInfo(int64_t view_id) {
  Endpoint endpoint;
  {
    std::lock_guard<std::mutex> lock(GlobalRegistry().mutex);
    auto found = GlobalRegistry().endpoints.find(view_id);
    if (found == GlobalRegistry().endpoints.end())
      return {};
    endpoint = found->second;
  }
  if (endpoint.runner->RunsTasksInCurrentSequence())
    return endpoint.device ? endpoint.device->Info() : D3DCompositorInfo{};

  D3DCompositorInfo result;
  base::WaitableEvent completed;
  if (!endpoint.runner->PostTask(
          FROM_HERE,
          base::BindOnce(
              [](base::WeakPtr<D3DOutputDevice> device,
                 D3DCompositorInfo* info, base::WaitableEvent* event) {
                if (device)
                  *info = device->Info();
                event->Signal();
              },
              endpoint.device, &result, &completed))) {
    return {};
  }
  completed.Wait();
  return result;
}

}  // namespace mini_electron::win
