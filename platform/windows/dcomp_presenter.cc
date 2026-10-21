// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "platform/windows/dcomp_presenter.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dcomp.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <iterator>
#include <map>
#include <utility>

#include "base/win/object_watcher.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/thread_pool.h"

namespace mini_electron::win {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint64_t kMaximumTextureBytes = 64ull * 1024 * 1024;

bool SameLuid(const LUID& left, const LUID& right) {
  return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

void ReportFailure(const char* operation, HRESULT result) {
  std::fprintf(stderr, "[mini-electron] D3D presentation %s failed (0x%08lx)\n",
               operation, static_cast<unsigned long>(result));
}

void ReportInvalidFrame(const char* reason) {
  std::fprintf(stderr, "[mini-electron] rejected renderer GPU frame: %s\n",
               reason);
}

bool ValidTextureSize(int width, int height) {
  if (width <= 0 || height <= 0)
    return false;
  const uint64_t row_bytes = static_cast<uint64_t>(width) * 4;
  return row_bytes <= kMaximumTextureBytes &&
         static_cast<uint64_t>(height) <= kMaximumTextureBytes / row_bytes;
}

struct DeviceCandidate {
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIFactory2> factory;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11Device5> device5;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11DeviceContext4> context4;
  LUID luid{};
};

bool CreateDeviceForAdapter(IDXGIFactory2* factory,
                            IDXGIAdapter1* adapter,
                            DeviceCandidate* candidate) {
  DXGI_ADAPTER_DESC1 adapter_description{};
  if (FAILED(adapter->GetDesc1(&adapter_description)) ||
      (adapter_description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
    return false;
  }

  static constexpr D3D_FEATURE_LEVEL kFeatureLevels[] = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL selected_level = D3D_FEATURE_LEVEL_9_1;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  HRESULT result = D3D11CreateDevice(
      adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      kFeatureLevels, static_cast<UINT>(std::size(kFeatureLevels)),
      D3D11_SDK_VERSION, &device, &selected_level, &context);
  if (result == E_INVALIDARG) {
    result = D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, &kFeatureLevels[1], 1,
        D3D11_SDK_VERSION, &device, &selected_level, &context);
  }
  if (FAILED(result) || selected_level < D3D_FEATURE_LEVEL_11_0)
    return false;

  D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
  if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,
                                         &options, sizeof(options))) ||
      !options.ExtendedResourceSharing) {
    return false;
  }

  ComPtr<ID3D11Device5> device5;
  ComPtr<ID3D11DeviceContext4> context4;
  if (FAILED(device.As(&device5)) || FAILED(context.As(&context4)))
    return false;

  // QueryInterface alone does not prove that this driver implements shared
  // fences. Exercise creation and NT-handle export before opting in.
  ComPtr<ID3D11Fence> test_fence;
  result = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                IID_PPV_ARGS(&test_fence));
  if (FAILED(result))
    return false;
  HANDLE test_handle = nullptr;
  result = test_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                          &test_handle);
  if (FAILED(result) || !test_handle) {
    if (test_handle)
      CloseHandle(test_handle);
    return false;
  }
  CloseHandle(test_handle);

  candidate->adapter = adapter;
  candidate->factory = factory;
  candidate->device = std::move(device);
  candidate->device5 = std::move(device5);
  candidate->context = std::move(context);
  candidate->context4 = std::move(context4);
  candidate->luid = adapter_description.AdapterLuid;
  return true;
}

bool FindHardwareDevice(const LUID* required_luid, DeviceCandidate* candidate) {
  ComPtr<IDXGIFactory6> factory;
  HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(result)) {
    ReportFailure("factory creation", result);
    return false;
  }

  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    result = required_luid
                 ? factory->EnumAdapters1(index, &adapter)
                 : factory->EnumAdapterByGpuPreference(
                       index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                       IID_PPV_ARGS(&adapter));
    if (result == DXGI_ERROR_NOT_FOUND)
      break;
    if (FAILED(result))
      continue;

    if (required_luid) {
      DXGI_ADAPTER_DESC1 description{};
      if (FAILED(adapter->GetDesc1(&description)) ||
          !SameLuid(description.AdapterLuid, *required_luid)) {
        continue;
      }
      if (CreateDeviceForAdapter(factory.Get(), adapter.Get(), candidate))
        return true;
      continue;
    }

    // Match the renderer's adapter choice exactly: it selects the first
    // hardware adapter, in high-performance order, on which D3D12 succeeds.
    ComPtr<ID3D12Device> producer_probe;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&producer_probe)))) {
      continue;
    }
    return CreateDeviceForAdapter(factory.Get(), adapter.Get(), candidate);
  }
  return false;
}

struct DeviceState {
  bool Initialize(const LUID* required_luid) {
    if (ready) {
      if (thread_id != GetCurrentThreadId()) {
        ReportInvalidFrame("D3D11 immediate context used from a non-UI thread");
        return false;
      }
      if (required_luid && !SameLuid(luid, *required_luid)) {
        ReportInvalidFrame("renderer and browser adapters do not match");
        return false;
      }
      return true;
    }
    if (attempted)
      return false;

    attempted = true;
    thread_id = GetCurrentThreadId();
    DeviceCandidate candidate;
    if (!FindHardwareDevice(required_luid, &candidate)) {
      std::fprintf(stderr,
                   "[mini-electron] no compatible hardware D3D11 shared-fence "
                   "device; using CPU composition\n");
      return false;
    }

    adapter = std::move(candidate.adapter);
    factory = std::move(candidate.factory);
    device = std::move(candidate.device);
    device5 = std::move(candidate.device5);
    context = std::move(candidate.context);
    context4 = std::move(candidate.context4);
    luid = candidate.luid;
    ready = true;
    return true;
  }

  bool OnDeviceThread() const {
    return ready && thread_id == GetCurrentThreadId();
  }

  bool attempted = false;
  bool ready = false;
  bool composition_capability_checked = false;
  bool composition_capable = false;
  DWORD thread_id = 0;
  LUID luid{};
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIFactory2> factory;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11Device5> device5;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11DeviceContext4> context4;
};

DeviceState& SharedDevice() {
  static DeviceState state;
  return state;
}

bool RealProcessHandle(HANDLE process, DWORD* process_id, bool in_process) {
  if (!process || process == INVALID_HANDLE_VALUE ||
      process == GetCurrentProcess() || process == GetCurrentThread()) {
    return false;
  }
  const DWORD id = GetProcessId(process);
  if (!id || (id == GetCurrentProcessId()) != in_process)
    return false;
  *process_id = id;
  return true;
}

bool ChildHandleValue(uint64_t value, HANDLE* handle) {
  if (!value || value > std::numeric_limits<uintptr_t>::max() ||
      value >= std::numeric_limits<uintptr_t>::max() - 15 ||
      (value & 3u) != 0) {
    return false;
  }
  *handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
  return true;
}

class ScopedHandle {
 public:
  ScopedHandle() = default;
  explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
  ~ScopedHandle() { reset(); }

  ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.release()) {}
  ScopedHandle& operator=(ScopedHandle&& other) noexcept {
    if (this != &other)
      reset(other.release());
    return *this;
  }

  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;

  HANDLE get() const { return handle_; }
  HANDLE* receive() {
    reset();
    return &handle_;
  }
  HANDLE release() {
    HANDLE handle = handle_;
    handle_ = nullptr;
    return handle;
  }
  void reset(HANDLE handle = nullptr) {
    if (handle_)
      CloseHandle(handle_);
    handle_ = handle;
  }

 private:
  HANDLE handle_ = nullptr;
};

struct CompositionSwapChain {
  explicit operator bool() const { return chain && ready.get(); }

  ComPtr<IDXGISwapChain1> chain;
  ScopedHandle ready;
};

bool DuplicateRendererHandle(HANDLE renderer_process,
                             HANDLE renderer_handle,
                             ScopedHandle* duplicate) {
  HRESULT result = DuplicateHandle(renderer_process, renderer_handle,
                                   GetCurrentProcess(), duplicate->receive(), 0,
                                   FALSE, DUPLICATE_SAME_ACCESS)
                       ? S_OK
                       : HRESULT_FROM_WIN32(GetLastError());
  if (FAILED(result)) {
    ReportFailure("renderer handle duplication", result);
    return false;
  }
  return true;
}

bool ValidateSourceTexture(ID3D11Texture2D* texture,
                           int width,
                           int height) {
  D3D11_TEXTURE2D_DESC description{};
  texture->GetDesc(&description);
  return description.Width == static_cast<UINT>(width) &&
         description.Height == static_cast<UINT>(height) &&
         description.MipLevels == 1 && description.ArraySize == 1 &&
         description.Format == DXGI_FORMAT_B8G8R8A8_UNORM &&
         description.SampleDesc.Count == 1 &&
         description.SampleDesc.Quality == 0 &&
         description.Usage == D3D11_USAGE_DEFAULT &&
         description.CPUAccessFlags == 0;
}

D3D11_TEXTURE2D_DESC RetainedTextureDescription(int width, int height) {
  D3D11_TEXTURE2D_DESC description{};
  description.Width = static_cast<UINT>(width);
  description.Height = static_cast<UINT>(height);
  description.MipLevels = 1;
  description.ArraySize = 1;
  description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  description.SampleDesc.Count = 1;
  description.Usage = D3D11_USAGE_DEFAULT;
  return description;
}

enum class PresentResult {
  kPresented,
  kPrimed,
  kTransient,
  kFailed,
};

class SwapChainWake final : public base::win::ObjectWatcher::Delegate {
 public:
  void SetWindow(HWND window) { window_ = window; }
  ~SwapChainWake() { Cancel(); }

  bool IsPriming(IDXGISwapChain1* chain) const {
    return priming_ && priming_->chain.Get() == chain;
  }

  PresentResult PrimingResult() const {
    if (!priming_->complete)
      return PresentResult::kTransient;
    if (priming_->failed) {
      ReportFailure("first-frame GPU completion", E_FAIL);
      return PresentResult::kFailed;
    }
    return PresentResult::kPrimed;
  }

  bool BeginPriming(IDXGISwapChain1* chain) {
    Cancel();
    ComPtr<IDXGIDevice2> device;
    HRESULT result = SharedDevice().device.As(&device);
    if (FAILED(result)) {
      ReportFailure("first-frame completion device", result);
      return false;
    }
    auto state = std::make_shared<Priming>();
    state->chain = chain;
    state->ready.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!state->ready.get()) {
      ReportFailure("first-frame completion event",
                    HRESULT_FROM_WIN32(GetLastError()));
      return false;
    }
    priming_ = state;
    // The task owns the event and swap chain through completion, including
    // when a newer resize cancels this candidate or destroys the presenter.
    if (!base::ThreadPool::PostTaskAndReplyWithResult(
            FROM_HERE,
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
             base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
            base::BindOnce([](std::shared_ptr<Priming> state) {
              return WaitForSingleObject(state->ready.get(), INFINITE)
                  == WAIT_OBJECT_0;
            }, state),
            base::BindOnce([](base::WeakPtr<SwapChainWake> self,
                              std::shared_ptr<Priming> state, bool ready) {
              if (!self || self->priming_ != state)
                return;
              state->complete = true;
              state->failed = !ready ||
                  FAILED(SharedDevice().device->GetDeviceRemovedReason());
              if (IsWindow(self->window_))
                InvalidateRect(self->window_, nullptr, FALSE);
            }, weak_factory_.GetWeakPtr(), state))) {
      priming_.reset();
      ReportFailure("first-frame completion task", E_FAIL);
      return false;
    }
    result = device->EnqueueSetEvent(state->ready.get());
    if (FAILED(result)) {
      priming_.reset();
      SetEvent(state->ready.get());
      ReportFailure("first-frame completion enqueue", result);
      return false;
    }
    SharedDevice().context->Flush();
    return true;
  }


  bool Arm(HANDLE object) {
    if (watcher_.IsWatching()) {
      if (watcher_.GetWatchedObject() == object)
        return true;
      watcher_.StopWatching();
    }
    return watcher_.StartWatchingOnce(object, this);
  }


  bool IsWatching() const { return watcher_.IsWatching(); }

  void Cancel() {
    if (watcher_.IsWatching())
      watcher_.StopWatching();
    priming_.reset();
  }

 private:
  struct Priming {
    ComPtr<IDXGISwapChain1> chain;
    ScopedHandle ready;
    bool complete = false;
    bool failed = false;
  };

  void OnObjectSignaled(HANDLE object) override {
    // A wake requests another non-blocking Present; DXGI remains the authority
    // on queue capacity, rather than a cached/consumed event token.
    if (IsWindow(window_))
      InvalidateRect(window_, nullptr, FALSE);
  }

  HWND window_ = nullptr;
  base::win::ObjectWatcher watcher_;
  std::shared_ptr<Priming> priming_;
  base::WeakPtrFactory<SwapChainWake> weak_factory_{this};
};

PresentResult PresentSwapChain(const CompositionSwapChain& swap_chain,
                               SwapChainWake* wake,
                               UINT interval) {
  const HRESULT result =
      swap_chain.chain->Present(interval, DXGI_PRESENT_DO_NOT_WAIT);
  if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
    if (!wake->Arm(swap_chain.ready.get())) {
      ReportFailure("swap-chain wake registration", E_FAIL);
      return PresentResult::kFailed;
    }
    return PresentResult::kTransient;
  }
  // A latency event can remain signaled while the target is occluded. Watching
  // it here would create an invalidate/present spin. Show/restore already
  // invalidates the HWND, so retain the last valid frame until then.
  if (result == DXGI_STATUS_OCCLUDED ||
      result == DXGI_STATUS_MODE_CHANGE_IN_PROGRESS) {
    return PresentResult::kTransient;
  }
  if (SUCCEEDED(result))
    return PresentResult::kPresented;
  ReportFailure("swap-chain present", result);
  return PresentResult::kFailed;
}

bool CreateCompositionSwapChain(int width,
                                int height,
                                CompositionSwapChain* swap_chain) {
  if (!ValidTextureSize(width, height))
    return false;
  if (swap_chain->chain || swap_chain->ready.get()) {
    ReportFailure("composition swap-chain replacement", E_UNEXPECTED);
    return false;
  }
  auto& shared = SharedDevice();
  DXGI_SWAP_CHAIN_DESC1 description{};
  description.Width = static_cast<UINT>(width);
  description.Height = static_cast<UINT>(height);
  description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  description.Stereo = FALSE;
  description.SampleDesc.Count = 1;
  description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  description.BufferCount = 2;
  description.Scaling = DXGI_SCALING_STRETCH;
  description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

  CompositionSwapChain candidate;
  HRESULT result = shared.factory->CreateSwapChainForComposition(
      shared.device.Get(), &description, nullptr,
      candidate.chain.ReleaseAndGetAddressOf());
  if (FAILED(result)) {
    ReportFailure("composition swap-chain creation", result);
    return false;
  }
  ComPtr<IDXGISwapChain2> swap_chain2;
  result = candidate.chain.As(&swap_chain2);
  if (FAILED(result) || FAILED(result = swap_chain2->SetMaximumFrameLatency(1))) {
    ReportFailure("swap-chain latency limit", result);
    return false;
  }
  candidate.ready.reset(swap_chain2->GetFrameLatencyWaitableObject());
  if (!candidate.ready.get()) {
    ReportFailure("swap-chain latency handle", E_HANDLE);
    return false;
  }
  *swap_chain = std::move(candidate);
  return true;
}

PresentResult CopyAndPresent(ID3D11Texture2D* texture,
                             const CompositionSwapChain& swap_chain,
                             SwapChainWake* wake,
                             bool first_present) {
  if (first_present && wake->IsPriming(swap_chain.chain.Get()))
    return wake->PrimingResult();
  if (first_present)
    wake->Cancel();
  else if (wake->IsWatching())
    return PresentResult::kTransient;

  HRESULT result = S_OK;

  ComPtr<ID3D11Texture2D> back_buffer;
  result = swap_chain.chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
  if (FAILED(result)) {
    ReportFailure("swap-chain back-buffer access", result);
    return PresentResult::kFailed;
  }

  D3D11_TEXTURE2D_DESC source_description{};
  D3D11_TEXTURE2D_DESC destination_description{};
  texture->GetDesc(&source_description);
  back_buffer->GetDesc(&destination_description);
  if (source_description.Width != destination_description.Width ||
      source_description.Height != destination_description.Height ||
      source_description.Format != destination_description.Format) {
    ReportInvalidFrame("retained texture does not match composition backing");
    return PresentResult::kFailed;
  }

  SharedDevice().context->CopyResource(back_buffer.Get(), texture);
  result = SharedDevice().device->GetDeviceRemovedReason();
  if (FAILED(result)) {
    ReportFailure("device copy", result);
    return PresentResult::kFailed;
  }
  if (!first_present)
    return PresentSwapChain(swap_chain, wake, 1);

  // DirectComposition can briefly show the other physical flip buffer on a
  // newly attached chain. Rotate once, initialize that buffer too, and wait
  // asynchronously for the GPU before replacing the retained visual.
  const PresentResult first = PresentSwapChain(swap_chain, wake, 0);
  if (first != PresentResult::kPresented)
    return first;
  ComPtr<ID3D11Texture2D> next_buffer;
  ComPtr<ID3D11Texture2D> presented_buffer;
  result = swap_chain.chain->GetBuffer(0, IID_PPV_ARGS(&next_buffer));
  if (SUCCEEDED(result))
    result = swap_chain.chain->GetBuffer(1, IID_PPV_ARGS(&presented_buffer));
  if (FAILED(result)) {
    ReportFailure("first-frame flip buffers", result);
    return PresentResult::kFailed;
  }
  SharedDevice().context->CopyResource(next_buffer.Get(), presented_buffer.Get());
  return wake->BeginPriming(swap_chain.chain.Get())
             ? PresentResult::kTransient
             : PresentResult::kFailed;
}


}  // namespace

struct D3DFrameConsumer::Impl {
  bool SameSource(DWORD renderer_pid, const D3DFrame& frame) const {
    return source_texture && source_fence && process_id == renderer_pid &&
           source_id == frame.source_id &&
           texture_handle_value == frame.texture_handle &&
           fence_handle_value == frame.fence_handle && width == frame.width &&
           height == frame.height;
  }

  DWORD process_id = 0;
  uint64_t texture_handle_value = 0;
  uint64_t fence_handle_value = 0;
  uint64_t frame_id = 0;
  uint64_t source_id = 0;
  uint64_t imported_frames = 0;
  int width = 0;
  int height = 0;
  ScopedHandle texture_handle;
  ScopedHandle fence_handle;
  ComPtr<ID3D11Texture2D> source_texture;
  ComPtr<ID3D11Fence> source_fence;
  ComPtr<ID3D11Texture2D> retained_texture;
};

D3DFrameConsumer::D3DFrameConsumer() : impl_(std::make_unique<Impl>()) {}

D3DFrameConsumer::~D3DFrameConsumer() = default;

bool D3DFrameConsumer::Import(HANDLE renderer_process, const D3DFrame& frame,
                            bool in_process) {
  if (frame.error) {
    ReportInvalidFrame("renderer reported a GPU error");
    return false;
  }
  if (!frame.frame_id || !frame.source_id ||
      frame.frame_id > (std::numeric_limits<uint64_t>::max() - 1) / 2 ||
      frame.ready_value != frame.frame_id * 2) {
    ReportInvalidFrame("invalid frame/source/fence sequence");
    return false;
  }
  if (!ValidTextureSize(frame.width, frame.height)) {
    ReportInvalidFrame("invalid or oversized texture dimensions");
    return false;
  }

  DWORD renderer_pid = 0;
  if (!RealProcessHandle(renderer_process, &renderer_pid, in_process)) {
    ReportInvalidFrame("invalid renderer process handle");
    return false;
  }
  HANDLE renderer_texture = nullptr;
  HANDLE renderer_fence = nullptr;
  if (!ChildHandleValue(frame.texture_handle, &renderer_texture) ||
      !ChildHandleValue(frame.fence_handle, &renderer_fence) ||
      renderer_texture == renderer_fence) {
    ReportInvalidFrame("invalid or pseudo shared handle");
    return false;
  }

  const LUID renderer_luid{frame.adapter_luid_low, frame.adapter_luid_high};
  auto& shared = SharedDevice();
  if (!shared.Initialize(&renderer_luid))
    return false;

  const bool same_source = impl_->SameSource(renderer_pid, frame);
  const bool new_process =
      impl_->process_id && impl_->process_id != renderer_pid;
  if (!new_process && impl_->process_id &&
      (frame.source_id < impl_->source_id ||
       (frame.source_id == impl_->source_id && !same_source) ||
       (same_source && frame.frame_id < impl_->frame_id))) {
    ReportInvalidFrame("stale or conflicting frame source");
    return false;
  }
  if (same_source && frame.frame_id == impl_->frame_id)
    return true;

  ScopedHandle texture_handle;
  ScopedHandle fence_handle;
  ComPtr<ID3D11Texture2D> source_texture;
  ComPtr<ID3D11Fence> source_fence;
  if (same_source) {
    source_texture = impl_->source_texture;
    source_fence = impl_->source_fence;
  } else {
    if (!DuplicateRendererHandle(renderer_process, renderer_texture,
                                 &texture_handle) ||
        !DuplicateRendererHandle(renderer_process, renderer_fence,
                                 &fence_handle)) {
      return false;
    }

    HRESULT result = shared.device5->OpenSharedResource1(
        texture_handle.get(), IID_PPV_ARGS(&source_texture));
    if (FAILED(result)) {
      ReportFailure("shared texture import", result);
      return false;
    }
    result = shared.device5->OpenSharedFence(
        fence_handle.get(), IID_PPV_ARGS(&source_fence));
    if (FAILED(result)) {
      ReportFailure("shared fence import", result);
      return false;
    }
    if (!ValidateSourceTexture(source_texture.Get(), frame.width,
                               frame.height)) {
      ReportInvalidFrame("shared resource is not the expected BGRA8 texture");
      return false;
    }
  }

  const uint64_t completed_value = source_fence->GetCompletedValue();
  if (completed_value == std::numeric_limits<uint64_t>::max() ||
      completed_value < frame.ready_value) {
    ReportInvalidFrame("shared fence has not actually reached ready value");
    return false;
  }

  ComPtr<ID3D11Texture2D> retained_texture = impl_->retained_texture;
  if (!retained_texture || frame.width != impl_->width ||
      frame.height != impl_->height) {
    const D3D11_TEXTURE2D_DESC description =
        RetainedTextureDescription(frame.width, frame.height);
    HRESULT result = shared.device->CreateTexture2D(
        &description, nullptr, &retained_texture);
    if (FAILED(result)) {
      ReportFailure("retained texture creation", result);
      return false;
    }
  }

  // Even though publication waits for the producer fence on the CPU, enqueue
  // the dependency on the D3D11 queue as well. The shared-fence wait is the
  // cross-API memory-ordering edge for the subsequent texture copy.
  HRESULT result =
      shared.context4->Wait(source_fence.Get(), frame.ready_value);
  if (FAILED(result)) {
    ReportFailure("producer-fence wait", result);
    return false;
  }
  shared.context->CopyResource(retained_texture.Get(), source_texture.Get());
  result =
      shared.context4->Signal(source_fence.Get(), frame.ready_value + 1);
  if (FAILED(result)) {
    ReportFailure("release-fence signal", result);
    return false;
  }
  shared.context->Flush();
  result = shared.device->GetDeviceRemovedReason();
  if (FAILED(result)) {
    ReportFailure("retained frame copy", result);
    return false;
  }

  if (!same_source) {
    impl_->texture_handle = std::move(texture_handle);
    impl_->fence_handle = std::move(fence_handle);
    impl_->source_texture = std::move(source_texture);
    impl_->source_fence = std::move(source_fence);
    impl_->process_id = renderer_pid;
    impl_->texture_handle_value = frame.texture_handle;
    impl_->fence_handle_value = frame.fence_handle;
    impl_->source_id = frame.source_id;
  }
  impl_->retained_texture = std::move(retained_texture);
  impl_->frame_id = frame.frame_id;
  ++impl_->imported_frames;
  impl_->width = frame.width;
  impl_->height = frame.height;
  return true;
}

uint64_t D3DFrameConsumer::source_id() const {
  return impl_->source_id;
}

uint64_t D3DFrameConsumer::frame_id() const {
  return impl_->frame_id;
}

uint64_t D3DFrameConsumer::imported_frames() const {
  return impl_->imported_frames;
}

int D3DFrameConsumer::width() const {
  return impl_->width;
}

int D3DFrameConsumer::height() const {
  return impl_->height;
}

ID3D11Texture2D* D3DFrameConsumer::texture() const {
  return impl_->retained_texture.Get();
}

ID3D11Device* D3DFrameConsumer::device() const {
  return impl_->retained_texture ? SharedDevice().device.Get() : nullptr;
}

bool CanUseD3DPresentation() {
  auto& shared = SharedDevice();
  if (!shared.Initialize(nullptr))
    return false;
  if (shared.composition_capability_checked)
    return shared.composition_capable;

  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IDCompositionDevice> composition_device;
  HRESULT result = shared.device.As(&dxgi_device);
  if (SUCCEEDED(result)) {
    result = DCompositionCreateDevice(dxgi_device.Get(),
                                      IID_PPV_ARGS(&composition_device));
  }
  shared.composition_capability_checked = true;
  shared.composition_capable = SUCCEEDED(result);
  if (!shared.composition_capable)
    ReportFailure("DirectComposition capability check", result);
  return shared.composition_capable;
}

struct DCompPresenter::Impl {
  struct LayerState {
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionRectangleClip> clip;
    ComPtr<IDCompositionScaleTransform> scale;
    CompositionSwapChain swap_chain;
    CompositionSwapChain pending_swap_chain;
    SwapChainWake wake;
    std::shared_ptr<D3DFrameConsumer> retained_frame;
    ComPtr<ID3D11Texture2D> cpu_texture;
    std::vector<uint8_t> cpu_upload;
    int backing_width = 0;
    int backing_height = 0;
    int pending_width = 0;
    int pending_height = 0;
    uint64_t presented_source_id = 0;
    uint64_t presented_frame_id = 0;
    int cpu_width = 0;
    int cpu_height = 0;
    uint64_t cpu_generation = 0;
    uint64_t presented_cpu_generation = 0;
    bool cpu_uploaded = false;
    bool backing_is_gpu = false;
    RECT bounds{};
  };

  struct OverlayState {
    ComPtr<IDCompositionVisual> visual;
    CompositionSwapChain swap_chain;
    CompositionSwapChain pending_swap_chain;
    SwapChainWake wake;
    ComPtr<ID3D11Texture2D> texture;
    int width = 0;
    int height = 0;
    int backing_width = 0;
    int backing_height = 0;
    int pending_width = 0;
    int pending_height = 0;
    uint64_t uploaded_generation = 0;
    uint64_t presented_generation = 0;
    bool uploaded = false;
    bool presented = false;
  };


  void CancelPendingPresents() {
    for (auto& entry : layers) {
      entry.second.wake.Cancel();
      entry.second.pending_swap_chain = {};
      entry.second.pending_width = 0;
      entry.second.pending_height = 0;
    }
    overlay.wake.Cancel();
    overlay.pending_swap_chain = {};
    overlay.pending_width = 0;
    overlay.pending_height = 0;
  }
  ~Impl() {
    CancelPendingPresents();
    if (diagnostic_property_set && IsWindow(window))
      RemovePropW(window, L"MiniElectronDirectComposition");
    if (target && composition_device) {
      target->SetRoot(nullptr);
      composition_device->Commit();
    }
  }

  bool Initialize(HWND target_window) {
    if (!IsWindow(target_window)) {
      ReportFailure("invalid presentation window", E_INVALIDARG);
      return false;
    }
    auto& shared = SharedDevice();
    if (!shared.Initialize(nullptr))
      return false;

    window = target_window;
    overlay.wake.SetWindow(window);
    // A prior presenter should have removed this. Clear it defensively so a
    // failed replacement can never leave stale diagnostics behind.
    RemovePropW(window, L"MiniElectronDirectComposition");

    ComPtr<IDXGIDevice> dxgi_device;
    HRESULT result = shared.device.As(&dxgi_device);
    if (SUCCEEDED(result)) {
      result = DCompositionCreateDevice(dxgi_device.Get(),
                                        IID_PPV_ARGS(&composition_device));
    }
    if (SUCCEEDED(result)) {
      result = composition_device->CreateTargetForHwnd(window, TRUE, &target);
    }
    if (SUCCEEDED(result))
      result = composition_device->CreateVisual(&root);
    if (SUCCEEDED(result))
      result = composition_device->CreateVisual(&overlay.visual);
    if (SUCCEEDED(result))
      result = target->SetRoot(root.Get());
    if (SUCCEEDED(result))
      result = composition_device->Commit();
    if (FAILED(result)) {
      ReportFailure("DirectComposition initialization", result);
      return false;
    }
    return true;
  }

  bool CreateLayerState(LayerState* state) {
    state->wake.SetWindow(window);
    HRESULT result = composition_device->CreateVisual(&state->visual);
    if (SUCCEEDED(result))
      result = composition_device->CreateRectangleClip(&state->clip);
    if (SUCCEEDED(result))
      result = composition_device->CreateScaleTransform(&state->scale);
    if (FAILED(result)) {
      ReportFailure("layer visual creation", result);
      return false;
    }
    return true;
  }

  bool SetLayerGeometry(LayerState* state, const DCompLayer& layer) {
    const int bounds_width = layer.bounds.right - layer.bounds.left;
    const int bounds_height = layer.bounds.bottom - layer.bounds.top;
    if (bounds_width <= 0 || bounds_height <= 0)
      return true;

    RECT effective_clip = layer.clip;
    if (effective_clip.right <= effective_clip.left ||
        effective_clip.bottom <= effective_clip.top) {
      effective_clip = layer.bounds;
    }
    RECT intersection{};
    if (!IntersectRect(&intersection, &layer.bounds, &effective_clip)) {
      intersection = {layer.bounds.left, layer.bounds.top, layer.bounds.left,
                      layer.bounds.top};
    }

    const int source_width = state->backing_width ? state->backing_width
                                                   : bounds_width;
    const int source_height = state->backing_height ? state->backing_height
                                                     : bounds_height;
    const float scale_x = static_cast<float>(bounds_width) / source_width;
    const float scale_y = static_cast<float>(bounds_height) / source_height;
    HRESULT result = state->scale->SetScaleX(scale_x);
    if (SUCCEEDED(result))
      result = state->scale->SetScaleY(scale_y);
    if (SUCCEEDED(result))
      result = state->scale->SetCenterX(0.f);
    if (SUCCEEDED(result))
      result = state->scale->SetCenterY(0.f);
    if (SUCCEEDED(result))
      result = state->clip->SetLeft(
          static_cast<float>(intersection.left - layer.bounds.left) / scale_x);
    if (SUCCEEDED(result))
      result = state->clip->SetTop(
          static_cast<float>(intersection.top - layer.bounds.top) / scale_y);
    if (SUCCEEDED(result))
      result = state->clip->SetRight(
          static_cast<float>(intersection.right - layer.bounds.left) / scale_x);
    if (SUCCEEDED(result))
      result = state->clip->SetBottom(
          static_cast<float>(intersection.bottom - layer.bounds.top) / scale_y);
    if (SUCCEEDED(result))
      result = state->visual->SetTransform(state->scale.Get());
    if (SUCCEEDED(result))
      result = state->visual->SetClip(state->clip.Get());
    if (SUCCEEDED(result))
      result = state->visual->SetOffsetX(static_cast<float>(layer.bounds.left));
    if (SUCCEEDED(result))
      result = state->visual->SetOffsetY(static_cast<float>(layer.bounds.top));
    if (FAILED(result)) {
      ReportFailure("layer geometry update", result);
      return false;
    }
    state->bounds = layer.bounds;
    return true;
  }

  PresentResult PresentLayerTexture(
      LayerState* state,
      ID3D11Texture2D* texture,
      int width,
      int height,
      bool is_gpu,
      uint64_t source_id,
      uint64_t generation,
      const std::shared_ptr<D3DFrameConsumer>& retained_frame) {
    const bool already_presented =
        state->swap_chain && state->backing_width == width &&
        state->backing_height == height &&
        state->backing_is_gpu == is_gpu &&
        (is_gpu ? state->presented_source_id == source_id &&
                      state->presented_frame_id == generation
                : state->presented_cpu_generation == generation);
    if (already_presented) {
      state->wake.Cancel();
      state->pending_swap_chain = {};
      state->pending_width = 0;
      state->pending_height = 0;
      if (is_gpu)
        state->retained_frame = retained_frame;
      return PresentResult::kPresented;
    }

    const bool replacement = !state->swap_chain ||
                             state->backing_width != width ||
                             state->backing_height != height;
    CompositionSwapChain* candidate = nullptr;
    if (replacement) {
      if (state->pending_swap_chain &&
          (state->pending_width != width ||
           state->pending_height != height)) {
        state->wake.Cancel();
        state->pending_swap_chain = {};
        state->pending_width = 0;
        state->pending_height = 0;
      }
      if (!state->pending_swap_chain) {
        if (!CreateCompositionSwapChain(width, height,
                                        &state->pending_swap_chain)) {
          return PresentResult::kFailed;
        }
        state->pending_width = width;
        state->pending_height = height;
      }
      candidate = &state->pending_swap_chain;
    } else {
      if (state->pending_swap_chain) {
        state->wake.Cancel();
        state->pending_swap_chain = {};
        state->pending_width = 0;
        state->pending_height = 0;
      }
      candidate = &state->swap_chain;
    }

    // A transient result below arms a one-shot watch before returning.
    const PresentResult present =
        CopyAndPresent(texture, *candidate, &state->wake, replacement);
    if (present == PresentResult::kTransient || present == PresentResult::kFailed)
      return present;

    state->wake.Cancel();
    if (replacement) {
      const HRESULT result = state->visual->SetContent(candidate->chain.Get());
      if (FAILED(result)) {
        ReportFailure("layer swap-chain attachment", result);
        return PresentResult::kFailed;
      }
      state->swap_chain = std::move(state->pending_swap_chain);
      state->backing_width = width;
      state->backing_height = height;
      state->pending_width = 0;
      state->pending_height = 0;
    }
    state->backing_is_gpu = is_gpu;
    if (present == PresentResult::kPrimed) {
      state->retained_frame = retained_frame;
      state->presented_source_id = 0;
      state->presented_frame_id = 0;
      state->presented_cpu_generation = 0;
      InvalidateRect(window, nullptr, FALSE);
      return PresentResult::kTransient;
    }
    if (is_gpu) {
      state->retained_frame = retained_frame;
      state->presented_source_id = source_id;
      state->presented_frame_id = generation;
      state->presented_cpu_generation = 0;
    } else {
      state->retained_frame.reset();
      state->presented_source_id = 0;
      state->presented_frame_id = 0;
      state->presented_cpu_generation = generation;
    }
    return PresentResult::kPresented;
  }

  PresentResult UpdateGpuLayerBacking(LayerState* state,
                                      const DCompLayer& layer) {
    if (!layer.frame->texture())
      return PresentResult::kTransient;
    if (layer.frame->device() != SharedDevice().device.Get()) {
      ReportInvalidFrame("retained texture belongs to another D3D11 device");
      return PresentResult::kFailed;
    }
    const int width = layer.frame->width();
    const int height = layer.frame->height();
    if (!ValidTextureSize(width, height)) {
      ReportInvalidFrame("retained texture dimensions are invalid");
      return PresentResult::kFailed;
    }
    return PresentLayerTexture(
        state, layer.frame->texture(), width, height, true,
        layer.frame->source_id(), layer.frame->frame_id(), layer.frame);
  }

  bool ValidateCpuLayer(const DCompLayer& layer) {
    if (!layer.rgba || !ValidTextureSize(layer.width, layer.height) ||
        layer.stride <= 0 ||
        static_cast<uint64_t>(layer.stride) <
            static_cast<uint64_t>(layer.width) * 4) {
      ReportFailure("invalid CPU fallback layer", E_INVALIDARG);
      return false;
    }
    const uint64_t addressed_bytes =
        static_cast<uint64_t>(layer.stride) * (layer.height - 1) +
        static_cast<uint64_t>(layer.width) * 4;
    if (addressed_bytes > kMaximumTextureBytes) {
      ReportFailure("oversized CPU fallback layer", E_INVALIDARG);
      return false;
    }
    return true;
  }

  PresentResult UpdateCpuLayerBacking(LayerState* state,
                                      const DCompLayer& layer) {
    const bool upload_needed =
        !state->cpu_uploaded || state->cpu_generation != layer.generation ||
        state->cpu_width != layer.width || state->cpu_height != layer.height;
    if (upload_needed) {
      if (!ValidateCpuLayer(layer))
        return PresentResult::kFailed;

      if (!state->cpu_texture || state->cpu_width != layer.width ||
          state->cpu_height != layer.height) {
        const D3D11_TEXTURE2D_DESC description =
            RetainedTextureDescription(layer.width, layer.height);
        ComPtr<ID3D11Texture2D> texture;
        const HRESULT result = SharedDevice().device->CreateTexture2D(
            &description, nullptr, &texture);
        if (FAILED(result)) {
          ReportFailure("CPU fallback texture creation", result);
          return PresentResult::kFailed;
        }
        state->cpu_texture = std::move(texture);
      }

      const size_t row_bytes = static_cast<size_t>(layer.width) * 4;
      state->cpu_upload.resize(row_bytes * static_cast<size_t>(layer.height));
      const auto* source = static_cast<const uint8_t*>(layer.rgba);
      for (int y = 0; y < layer.height; ++y) {
        const uint8_t* source_row =
            source + static_cast<size_t>(y) * layer.stride;
        uint8_t* destination_row =
            state->cpu_upload.data() + static_cast<size_t>(y) * row_bytes;
        for (int x = 0; x < layer.width; ++x) {
          destination_row[x * 4] = source_row[x * 4 + 2];
          destination_row[x * 4 + 1] = source_row[x * 4 + 1];
          destination_row[x * 4 + 2] = source_row[x * 4];
          destination_row[x * 4 + 3] = source_row[x * 4 + 3];
        }
      }
      SharedDevice().context->UpdateSubresource(
          state->cpu_texture.Get(), 0, nullptr, state->cpu_upload.data(),
          static_cast<UINT>(row_bytes), 0);
      const HRESULT result = SharedDevice().device->GetDeviceRemovedReason();
      if (FAILED(result)) {
        ReportFailure("CPU fallback texture upload", result);
        return PresentResult::kFailed;
      }
      state->cpu_width = layer.width;
      state->cpu_height = layer.height;
      state->cpu_generation = layer.generation;
      state->cpu_uploaded = true;
    }

    if (!state->cpu_texture)
      return PresentResult::kTransient;
    return PresentLayerTexture(
        state, state->cpu_texture.Get(), state->cpu_width, state->cpu_height,
        false, 0, state->cpu_generation, nullptr);
  }

  PresentResult UpdateLayerBacking(LayerState* state,
                                   const DCompLayer& layer) {
    if (layer.frame)
      return UpdateGpuLayerBacking(state, layer);
    if (layer.width <= 0 || layer.height <= 0)
      return PresentResult::kTransient;
    return UpdateCpuLayerBacking(state, layer);
  }

  bool ValidateOverlay(const DCompOverlay& input) {
    if (!input.bgra || !ValidTextureSize(input.width, input.height) ||
        input.stride <= 0 ||
        static_cast<uint64_t>(input.stride) <
            static_cast<uint64_t>(input.width) * 4) {
      ReportFailure("invalid caption overlay", E_INVALIDARG);
      return false;
    }
    const uint64_t addressed_bytes =
        static_cast<uint64_t>(input.stride) * (input.height - 1) +
        static_cast<uint64_t>(input.width) * 4;
    if (addressed_bytes > kMaximumTextureBytes) {
      ReportFailure("oversized caption overlay", E_INVALIDARG);
      return false;
    }
    return true;
  }

  PresentResult UpdateOverlay(const DCompOverlay& input) {
    const bool upload_needed = !overlay.uploaded ||
                               overlay.uploaded_generation != input.generation ||
                               overlay.width != input.width ||
                               overlay.height != input.height;
    if (upload_needed) {
      if (!ValidateOverlay(input))
        return PresentResult::kFailed;

      if (!overlay.texture || overlay.width != input.width ||
          overlay.height != input.height) {
        D3D11_TEXTURE2D_DESC description =
            RetainedTextureDescription(input.width, input.height);
        D3D11_SUBRESOURCE_DATA pixels{};
        pixels.pSysMem = input.bgra;
        pixels.SysMemPitch = static_cast<UINT>(input.stride);
        ComPtr<ID3D11Texture2D> texture;
        HRESULT result = SharedDevice().device->CreateTexture2D(
            &description, &pixels, &texture);
        if (FAILED(result)) {
          ReportFailure("caption texture creation", result);
          return PresentResult::kFailed;
        }
        overlay.texture = std::move(texture);
      } else {
        SharedDevice().context->UpdateSubresource(
            overlay.texture.Get(), 0, nullptr, input.bgra,
            static_cast<UINT>(input.stride), 0);
        const HRESULT result = SharedDevice().device->GetDeviceRemovedReason();
        if (FAILED(result)) {
          ReportFailure("caption texture upload", result);
          return PresentResult::kFailed;
        }
      }
      overlay.width = input.width;
      overlay.height = input.height;
      overlay.uploaded_generation = input.generation;
      overlay.uploaded = true;
    }

    const bool replacement = !overlay.swap_chain ||
                             overlay.backing_width != input.width ||
                             overlay.backing_height != input.height;
    const bool needs_present =
        !overlay.presented ||
        overlay.presented_generation != input.generation || replacement;
    if (needs_present) {
      CompositionSwapChain* candidate = nullptr;
      if (replacement) {
        if (overlay.pending_swap_chain &&
            (overlay.pending_width != input.width ||
             overlay.pending_height != input.height)) {
          overlay.wake.Cancel();
          overlay.pending_swap_chain = {};
          overlay.pending_width = 0;
          overlay.pending_height = 0;
        }
        if (!overlay.pending_swap_chain) {
          if (!CreateCompositionSwapChain(input.width, input.height,
                                          &overlay.pending_swap_chain)) {
            return PresentResult::kFailed;
          }
          overlay.pending_width = input.width;
          overlay.pending_height = input.height;
        }
        candidate = &overlay.pending_swap_chain;
      } else {
        if (overlay.pending_swap_chain) {
          overlay.wake.Cancel();
          overlay.pending_swap_chain = {};
          overlay.pending_width = 0;
          overlay.pending_height = 0;
        }
        candidate = &overlay.swap_chain;
      }

      const PresentResult present = CopyAndPresent(
          overlay.texture.Get(), *candidate, &overlay.wake, replacement);
      if (present == PresentResult::kTransient || present == PresentResult::kFailed)
        return present;

      overlay.wake.Cancel();
      if (replacement) {
        const HRESULT result =
            overlay.visual->SetContent(candidate->chain.Get());
        if (FAILED(result)) {
          ReportFailure("caption swap-chain attachment", result);
          return PresentResult::kFailed;
        }
        overlay.swap_chain = std::move(overlay.pending_swap_chain);
        overlay.backing_width = input.width;
        overlay.backing_height = input.height;
        overlay.pending_width = 0;
        overlay.pending_height = 0;
      }
      overlay.presented_generation = input.generation;
      overlay.presented = present == PresentResult::kPresented;
      if (!overlay.presented)
        InvalidateRect(window, nullptr, FALSE);
    } else {
      overlay.wake.Cancel();
      overlay.pending_swap_chain = {};
      overlay.pending_width = 0;
      overlay.pending_height = 0;
    }

    HRESULT result = overlay.visual->SetOffsetX(static_cast<float>(input.x));
    if (SUCCEEDED(result))
      result = overlay.visual->SetOffsetY(static_cast<float>(input.y));
    if (FAILED(result)) {
      ReportFailure("caption position update", result);
      return PresentResult::kFailed;
    }
    return PresentResult::kPresented;
  }


  bool PresentationFailed() {
    CancelPendingPresents();
    if (diagnostic_property_set && IsWindow(window)) {
      RemovePropW(window, L"MiniElectronDirectComposition");
      diagnostic_property_set = false;
    }
    return false;
  }

  bool Present(const std::vector<DCompLayer>& requested_layers,
               const DCompOverlay& requested_overlay) {
    if (!SharedDevice().OnDeviceThread()) {
      ReportFailure("presentation from non-UI thread", E_UNEXPECTED);
      return PresentationFailed();
    }
    if (!IsWindow(window)) {
      ReportFailure("destroyed presentation window", E_HANDLE);
      return PresentationFailed();
    }

    bool fatal_error = false;
    for (size_t index = 0; index < requested_layers.size(); ++index) {
      const auto& layer = requested_layers[index];
      for (size_t previous_index = 0; previous_index < index;
           ++previous_index) {
        if (requested_layers[previous_index].id == layer.id) {
          ReportFailure("duplicate layer id", E_INVALIDARG);
          return PresentationFailed();
        }
      }
      auto insertion = layers.try_emplace(layer.id);
      auto found = insertion.first;
      if (insertion.second && !CreateLayerState(&found->second)) {
        layers.erase(found);
        return PresentationFailed();
      }
      LayerState& state = found->second;
      const PresentResult update = UpdateLayerBacking(&state, layer);
      if (update == PresentResult::kFailed)
        fatal_error = true;
      if (!SetLayerGeometry(&state, layer))
        fatal_error = true;
    }

    const bool overlay_requested = requested_overlay.bgra &&
                                   requested_overlay.width > 0 &&
                                   requested_overlay.height > 0;
    if (overlay_requested) {
      if (UpdateOverlay(requested_overlay) == PresentResult::kFailed)
        fatal_error = true;
    } else {
      overlay.wake.Cancel();
      overlay.pending_swap_chain = {};
      overlay.pending_width = 0;
      overlay.pending_height = 0;
    }
    if (fatal_error)
      return PresentationFailed();

    HRESULT result = root->RemoveAllVisuals();
    IDCompositionVisual* previous = nullptr;
    bool visible_content = false;
    for (const auto& layer : requested_layers) {
      LayerState& state = layers.find(layer.id)->second;
      const int bounds_width = layer.bounds.right - layer.bounds.left;
      const int bounds_height = layer.bounds.bottom - layer.bounds.top;
      if (!state.swap_chain || bounds_width <= 0 || bounds_height <= 0)
        continue;
      if (SUCCEEDED(result))
        result = root->AddVisual(state.visual.Get(), TRUE, previous);
      previous = state.visual.Get();
      visible_content = true;
    }
    if (overlay_requested && overlay.swap_chain) {
      if (SUCCEEDED(result))
        result = root->AddVisual(overlay.visual.Get(), TRUE, previous);
      previous = overlay.visual.Get();
      visible_content = true;
    }
    if (SUCCEEDED(result))
      result = composition_device->Commit();
    if (FAILED(result)) {
      ReportFailure("DirectComposition commit", result);
      return PresentationFailed();
    }

    for (auto it = layers.begin(); it != layers.end();) {
      const bool requested = std::any_of(
          requested_layers.begin(), requested_layers.end(),
          [id = it->first](const DCompLayer& layer) { return layer.id == id; });
      if (!requested) {
        it->second.wake.Cancel();
        it->second.pending_swap_chain = {};
        it = layers.erase(it);
      } else {
        ++it;
      }
    }

    if (visible_content && !diagnostic_property_set) {
      SetLastError(ERROR_SUCCESS);
      if (!SetPropW(window, L"MiniElectronDirectComposition",
                    reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1)))) {
        ReportFailure("composition diagnostic property",
                      HRESULT_FROM_WIN32(GetLastError()));
        return PresentationFailed();
      }
      diagnostic_property_set = true;
    }
    if (!visible_content && diagnostic_property_set) {
      RemovePropW(window, L"MiniElectronDirectComposition");
      diagnostic_property_set = false;
    }
    return true;
  }

  HWND window = nullptr;
  bool diagnostic_property_set = false;
  ComPtr<IDCompositionDevice> composition_device;
  ComPtr<IDCompositionTarget> target;
  ComPtr<IDCompositionVisual> root;
  std::map<int, LayerState> layers;
  OverlayState overlay;
};

DCompPresenter::DCompPresenter(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

DCompPresenter::~DCompPresenter() = default;

std::unique_ptr<DCompPresenter> DCompPresenter::Create(HWND window) {
  auto impl = std::make_unique<Impl>();
  if (!impl->Initialize(window))
    return nullptr;
  return std::unique_ptr<DCompPresenter>(
      new DCompPresenter(std::move(impl)));
}

bool DCompPresenter::Present(const std::vector<DCompLayer>& layers,
                             const DCompOverlay& overlay) {
  return impl_->Present(layers, overlay);
}

uint64_t DCompPresenter::presented_frame_id(int id) const {
  const auto found = impl_->layers.find(id);
  return found == impl_->layers.end() ? 0 : found->second.presented_frame_id;
}


}  // namespace mini_electron::win
