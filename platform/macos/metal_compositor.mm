// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "platform/macos/metal_compositor.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "components/viz/service/display/software_output_device.h"
#include "runtime/engine/browser/web_view_host.h"
#include "runtime/engine/common/live_id_detect.h"
#include "runtime/engine/common/thread_call.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/gpu/ganesh/GrBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/GrDirectContext.h"
#include "third_party/skia/include/gpu/ShaderErrorHandler.h"
#include "third_party/skia/include/gpu/ganesh/GrContextOptions.h"
#include "third_party/skia/include/gpu/ganesh/SkImageGanesh.h"
#include "third_party/skia/include/gpu/ganesh/SkSurfaceGanesh.h"
#include "third_party/skia/include/gpu/ganesh/mtl/GrMtlBackendContext.h"
#include "third_party/skia/include/gpu/ganesh/mtl/GrMtlBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/mtl/GrMtlDirectContext.h"
#include "ui/base/cocoa/remote_layer_api.h"

namespace mini_electron::mac {
namespace {
struct MetalDevice {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  bool ready = false;
};
MetalDevice& Device() {
  static base::NoDestructor<MetalDevice> device;
  return *device;
}
class MetalOutputDevice;
struct Endpoint {
  scoped_refptr<base::SingleThreadTaskRunner> runner;
  base::WeakPtr<MetalOutputDevice> device;
};
struct Registry {
  std::mutex mutex;
  std::map<int64_t, Endpoint> endpoints;
  std::map<int64_t, base::RepeatingCallback<void(const MetalFrame&)>> callbacks;
};
Registry& GlobalRegistry() {
  // Engine threads may still be draining at process exit. Per-view resources
  // are destroyed on Viz; these process-wide capability tables do not race exit.
  static base::NoDestructor<Registry> registry;
  return *registry;
}
std::mutex& RegistryMutex() { return GlobalRegistry().mutex; }
std::map<int64_t, Endpoint>& Endpoints() { return GlobalRegistry().endpoints; }
std::map<int64_t, base::RepeatingCallback<void(const MetalFrame&)>>& Callbacks() {
  return GlobalRegistry().callbacks;
}
void DispatchFrame(int64_t view_id, MetalFrame frame) {
  content::ThreadCall::callUiThreadAsync(FROM_HERE, [view_id, frame] {
    base::RepeatingCallback<void(const MetalFrame&)> callback;
    {
      std::lock_guard<std::mutex> lock(RegistryMutex());
      auto found = Callbacks().find(view_id);
      if (found != Callbacks().end()) callback = found->second;
    }
    if (callback) callback.Run(frame);
    if (frame.error) return;
    // GPU completion releases the same resize gate as CPU publication, without
    // installing a bitmap or invoking the CPU pixel callback.
    auto* ids = common::LiveIdDetect::getWebViewIds();
    auto* view = static_cast<content::WebViewHost*>(ids->getPtrLocked(view_id));
    if (view) view->onFramePublishedInUiThread();
    ids->unlock(view_id, view);
  });
}

class MetalOutputDevice final : public viz::SoftwareOutputDevice,
                                private skgpu::ShaderErrorHandler {
 public:
  MetalOutputDevice(int64_t view_id, bool transparent)
      : view_id_(view_id), transparent_(transparent),
        runner_(base::SingleThreadTaskRunner::GetCurrentDefault()) {}
  bool Initialize() {
    DCHECK(runner_->RunsTasksInCurrentSequence());
    if (!ui::RemoteLayerAPISupported()) return false;
    @autoreleasepool {
      GrMtlBackendContext backend;
      backend.fDevice.retain((__bridge GrMTLHandle)Device().device);
      backend.fQueue.retain((__bridge GrMTLHandle)Device().queue);
      GrContextOptions options;
      options.fShaderErrorHandler = this;
      context_ = GrDirectContexts::MakeMetal(backend, options);
      if (!context_ || failed_) return false;
      // Persistent root storage keeps Viz damage/partial redraw semantics.
      // CAMetalLayer owns the separate presentation buffers and their fences.
      metal_layer_ = [CAMetalLayer layer];
      metal_layer_.device = Device().device;
      metal_layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
      metal_layer_.framebufferOnly = NO;
      metal_layer_.opaque = !transparent_;
      metal_layer_.maximumDrawableCount = 3;
      metal_layer_.allowsNextDrawableTimeout = YES;
      metal_layer_.presentsWithTransaction = YES;
      CGColorSpaceRef color_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
      metal_layer_.colorspace = color_space;
      CGColorSpaceRelease(color_space);
      root_layer_ = [CALayer layer];
      root_layer_.geometryFlipped = YES;
      root_layer_.masksToBounds = YES;
      [root_layer_ addSublayer:metal_layer_];
      Class context_class = NSClassFromString(@"CAContext");
      ca_context_ = [context_class contextWithCGSConnection:CGSMainConnectionID()
                                                   options:@{}];
      if (!ca_context_ || !ca_context_.contextId) return false;
      ca_context_.layer = root_layer_;
      {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        Endpoints()[view_id_] = {runner_, weak_factory_.GetWeakPtr()};
      }
      fprintf(stderr, "[mini-electron] Metal compositor active: device=%s view=%lld context=%u\n",
          Device().device.name.UTF8String, static_cast<long long>(view_id_), ca_context_.contextId);
      return true;
    }
  }
  ~MetalOutputDevice() override {
    DCHECK(runner_->RunsTasksInCurrentSequence());
    {
      std::lock_guard<std::mutex> lock(RegistryMutex());
      auto found = Endpoints().find(view_id_);
      if (found != Endpoints().end() && found->second.device.get() == this)
        Endpoints().erase(found);
    }
    weak_factory_.InvalidateWeakPtrs();
    // GPU completion never waits for this sequence, so teardown can drain work.
    if (context_ && !context_->abandoned())
      context_->flushAndSubmit(GrSyncCpu::kYes);
    images_.clear();
    surface_.reset();
    context_.reset();
    ca_context_.layer = nil;
  }
  void Resize(const gfx::Size& size, float scale) override {
    DCHECK(runner_->RunsTasksInCurrentSequence());
    if (viewport_pixel_size_ == size && scale_factor_ == scale) return;
    viewport_pixel_size_ = size;
    scale_factor_ = std::isfinite(scale) && scale > 0.f ? scale : 1.f;
    surface_.reset();
    root_has_drawn_ = false;
    if (size.IsEmpty() || !Usable()) return;
    if (static_cast<int64_t>(size.width()) * size.height() > 64 * 1024 * 1024 / 4) {
      Fail(7);
      return;
    }
    // Match CAMetalLayer's BGRA format; a raw GPU blit does not swizzle channels.
    surface_ = CreateRenderPassSurface(SkImageInfo::Make(size.width(), size.height(),
        kBGRA_8888_SkColorType, kPremul_SkAlphaType));
    if (!surface_) {
      Fail(7);
      return;
    }
    surface_->getCanvas()->clear(transparent_ ? SK_ColorTRANSPARENT : SK_ColorWHITE);
    if (!frame_number_) {
      root_layer_.frame = CGRectMake(0, 0, size.width() / scale_factor_, size.height() / scale_factor_);
      metal_layer_.frame = root_layer_.bounds;
    }
    // Keep existing remote geometry until a completed resized backing exists.
  }
  SkCanvas* BeginPaint(const gfx::Rect& damage) override {
    DCHECK(runner_->RunsTasksInCurrentSequence());
    damage_rect_ = damage;
    return Usable() && surface_ ? surface_->getCanvas() : nullptr;
  }
  void EndPaint() override { root_has_drawn_ = surface_ && !failed_; }
  sk_sp<SkSurface> CreateRenderPassSurface(const SkImageInfo& info) override {
    if (!Usable()) return nullptr;
    return SkSurfaces::RenderTarget(context_.get(), skgpu::Budgeted::kYes, info,
        0, kTopLeft_GrSurfaceOrigin, nullptr);
  }
  sk_sp<SkImage> PrepareImageForDraw(sk_sp<SkImage> image) override {
    if (!image || !Usable()) return nullptr;
    if (image->isTextureBacked()) return image;
    const uint32_t key = image->uniqueID();
    auto found = images_.find(key);
    if (found != images_.end()) {
      found->second.last_frame = frame_number_;
      return found->second.texture;
    }
    // The direct Metal path copies borrowed texels into owned staging storage
    // during this call, while DisplayResourceProvider's CPU read lock is held.
    auto texture = SkImages::TextureFromImage(context_.get(), image);
    if (!texture) {
      Fail(6);
      return nullptr;
    }
    images_.emplace(key, Image{std::move(image), texture, frame_number_});
    return texture;
  }
  void OnSwapBuffers(SwapBuffersCallback callback, gfx::FrameData) override {
    DCHECK(runner_->RunsTasksInCurrentSequence());
    CHECK(pending_swap_callback_.is_null());
    if (!Usable() || !surface_) {
      std::move(callback).Run(viewport_pixel_size_);
      return;
    }
    @autoreleasepool {
      metal_layer_.drawableSize = CGSizeMake(surface_->width(), surface_->height());
      id<CAMetalDrawable> drawable = [metal_layer_ nextDrawable];
      if (!drawable) {
        Fail(2);
        std::move(callback).Run(viewport_pixel_size_);
        return;
      }
      auto backend = SkSurfaces::GetBackendTexture(surface_.get(),
          SkSurfaces::BackendHandleAccess::kFlushRead);
      GrMtlTextureInfo info;
      if (!GrBackendTextures::GetMtlTextureInfo(backend, &info) || !context_->submit()) {
        Fail(3);
        std::move(callback).Run(viewport_pixel_size_);
        return;
      }
      id<MTLTexture> texture = (__bridge id<MTLTexture>)info.fTexture.get();
      id<MTLCommandBuffer> commands = [Device().queue commandBuffer];
      commands.label = @"mini-electron native presentation";
      id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
      if (!commands || !blit) {
        Fail(4);
        std::move(callback).Run(viewport_pixel_size_);
        return;
      }
      [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0
              sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(surface_->width(), surface_->height(), 1)
                 toTexture:drawable.texture destinationSlice:0 destinationLevel:0
         destinationOrigin:MTLOriginMake(0, 0, 0)];
      [blit endEncoding];
      MetalFrame frame{ca_context_.contextId, surface_->width(), surface_->height(),
                       scale_factor_, ++frame_number_, 0};
      // SoftwareOutputSurface allows one pending swap. Its weak callback is
      // canceled with the owning surface, so no per-frame shared allocation is needed.
      pending_swap_callback_ = std::move(callback);
      pending_frame_ = frame;
      pending_drawable_ = drawable;
      auto weak = weak_factory_.GetWeakPtr();
      auto runner = runner_;
      [commands addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        const bool success = completed.status == MTLCommandBufferStatusCompleted;
        runner->PostTask(FROM_HERE, base::BindOnce(
            [](base::WeakPtr<MetalOutputDevice> self, bool success) {
              if (self) self->CompleteSwap(success);
            }, weak, success));
      }];
      [commands commit];
      // Keep only recently sampled CPU resources; stable image identity, not
      // recycled resource IDs, keys the GPU upload cache. No idle eviction timer.
      for (auto it = images_.begin(); it != images_.end();) {
        if (it->second.last_frame + 2 < frame_number_) it = images_.erase(it);
        else ++it;
      }
    }
  }
  SkBitmap Readback() {
    DCHECK(runner_->RunsTasksInCurrentSequence());
    SkBitmap result;
    if (!Usable() || !surface_ || !root_has_drawn_ ||
        !result.tryAllocPixels(SkImageInfo::MakeN32Premul(surface_->width(), surface_->height())) ||
        !surface_->getCanvas()->readPixels(result, 0, 0)) return {};
    return result;
  }
  void UpdateGuests(std::vector<MetalGuestLayer> guests) {
    DCHECK(runner_->RunsTasksInCurrentSequence());
    @autoreleasepool {
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      const uint64_t update = ++guest_update_serial_;
      for (const auto& guest : guests) {
        if (guest.bounds.IsEmpty() || guest.scale_factor <= 0.f
            || (!guest.context_id && guest.cpu_bitmap.empty())) continue;
        auto& layers = guest_layers_[guest.id];
        layers.last_update = update;
        const bool native = guest.context_id != 0;
        if (!layers.clip) {
          layers.clip = [CALayer layer];
          layers.clip.masksToBounds = YES;
          [root_layer_ addSublayer:layers.clip];
        }
        if (!layers.content || layers.native != native) {
          [layers.content removeFromSuperlayer];
          layers.content = native ? [[NSClassFromString(@"CALayerHost") alloc] init] : [CALayer layer];
          layers.native = native;
          layers.generation = 0;
          [layers.clip addSublayer:layers.content];
        }
        if (native) {
          ((CALayerHost*)layers.content).contextId = guest.context_id;
        } else if (layers.generation != guest.generation) {
          auto* owner = new SkBitmap(guest.cpu_bitmap);
          CGDataProviderRef provider = CGDataProviderCreateWithData(owner,
              owner->getPixels(), owner->computeByteSize(),
              [](void* owner, const void*, size_t) { delete static_cast<SkBitmap*>(owner); });
          if (!provider) {
            delete owner;
            continue;
          }
          CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
          CGImageRef image = CGImageCreate(guest.pixel_width, guest.pixel_height,
              8, 32, guest.cpu_bitmap.rowBytes(), space,
              kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast,
              provider, nullptr, false, kCGRenderingIntentDefault);
          if (image) {
            layers.content.contents = (__bridge id)image;
            layers.generation = guest.generation;
            CGImageRelease(image);
          }
          CGColorSpaceRelease(space);
          CGDataProviderRelease(provider);
          if (!image) continue;
        }
        layers.clip.frame = CGRectMake(guest.bounds.x(), guest.bounds.y(), guest.bounds.width(), guest.bounds.height());
        layers.content.frame = CGRectMake(0, 0, guest.pixel_width / guest.scale_factor,
                                          guest.pixel_height / guest.scale_factor);
      }
      for (auto it = guest_layers_.begin(); it != guest_layers_.end();) {
        if (it->second.last_update != update) {
          [it->second.clip removeFromSuperlayer];
          it = guest_layers_.erase(it);
        } else ++it;
      }
      [CATransaction commit];
      [CATransaction flush];
    }
  }
 private:
  struct Image {
    sk_sp<SkImage> source;  // Retain identity; texels are never accessed after upload.
    sk_sp<SkImage> texture;
    uint64_t last_frame;
  };
  struct GuestLayers {
    CALayer* clip = nil;
    CALayer* content = nil;
    bool native = false;
    uint64_t generation = 0;
    uint64_t last_update = 0;
  };
  bool Usable() {
    if (!failed_ && context_ && !context_->abandoned()) return true;
    if (!failed_) Fail(1);
    return false;
  }
  void Fail(int error) {
    if (failed_) return;
    failed_ = true;
    fprintf(stderr, "[mini-electron] Metal compositor failure: view=%lld code=%d\n",
            static_cast<long long>(view_id_), error);
    MetalFrame frame;
    frame.error = error;
    if (ca_context_) DispatchFrame(view_id_, frame);
  }
  void compileError(const char*, const char* errors, bool) override {
    fprintf(stderr, "[mini-electron] Metal shader compilation failed: %s\n", errors);
    Fail(8);
  }
  void CompleteSwap(bool success) {
    auto callback = std::move(pending_swap_callback_);
    const MetalFrame frame = pending_frame_;
    id<CAMetalDrawable> drawable = pending_drawable_;
    pending_drawable_ = nil;
    if (!success || failed_) {
      if (!success) Fail(5);
      std::move(callback).Run(gfx::Size(frame.width, frame.height));
      return;
    }
    @autoreleasepool {
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      const CGRect bounds = CGRectMake(0, 0, frame.width / frame.scale_factor,
                                      frame.height / frame.scale_factor);
      root_layer_.frame = bounds;
      metal_layer_.frame = bounds;
      metal_layer_.contentsScale = frame.scale_factor;
      // Commit geometry and the completed backing atomically: a resize must
      // never scale an old drawable into the new layer's dimensions.
      [drawable present];
      [CATransaction commit];
      [CATransaction flush];
    }
    DispatchFrame(view_id_, frame);
    std::move(callback).Run(gfx::Size(frame.width, frame.height));
  }
  const int64_t view_id_;
  const bool transparent_;
  scoped_refptr<base::SingleThreadTaskRunner> runner_;
  sk_sp<GrDirectContext> context_;
  CAMetalLayer* metal_layer_ = nil;
  CALayer* root_layer_ = nil;
  CAContext* ca_context_ = nil;
  float scale_factor_ = 1.f;
  uint64_t frame_number_ = 0;
  uint64_t guest_update_serial_ = 0;
  MetalFrame pending_frame_;
  SwapBuffersCallback pending_swap_callback_;
  id<CAMetalDrawable> pending_drawable_ = nil;
  bool root_has_drawn_ = false;
  bool failed_ = false;
  std::map<uint32_t, Image> images_;
  std::map<int, GuestLayers> guest_layers_;
  base::WeakPtrFactory<MetalOutputDevice> weak_factory_{this};
};
}  // namespace

bool PrepareMetalDevice() {
  const char* disabled = std::getenv("MINI_ELECTRON_DISABLE_GPU");
  if (disabled && *disabled && std::strcmp(disabled, "0") != 0) return false;
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    @autoreleasepool {
      auto& state = Device();
      state.device = MTLCreateSystemDefaultDevice();
      state.queue = [state.device newCommandQueue];
      if (!state.device || !state.queue) {
        fprintf(stderr, "[mini-electron] Metal unavailable; using CPU composition\n");
        return;
      }
      // Hardware capabilities are opened before lockdown. Shader compilation
      // uses only the explicitly named compiler XPC service in the policy.
      state.ready = true;
    }
  });
  return Device().ready;
}

std::unique_ptr<viz::SoftwareOutputDevice> CreateMetalOutputDevice(int64_t view_id, bool transparent) {
  if (!view_id || !PrepareMetalDevice()) return nullptr;
  auto device = std::make_unique<MetalOutputDevice>(view_id, transparent);
  if (!device->Initialize()) {
    fprintf(stderr, "[mini-electron] Metal surface initialization failed; using CPU composition\n");
    return nullptr;
  }
  return device;
}

void SetMetalFrameCallback(int64_t view_id,
    base::RepeatingCallback<void(const MetalFrame&)> callback) {
  std::lock_guard<std::mutex> lock(RegistryMutex());
  if (callback) Callbacks()[view_id] = std::move(callback);
  else Callbacks().erase(view_id);
}

SkBitmap CopyMetalFrame(int64_t view_id) {
  Endpoint endpoint;
  {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    auto found = Endpoints().find(view_id);
    if (found == Endpoints().end()) return {};
    endpoint = found->second;
  }
  if (endpoint.runner->RunsTasksInCurrentSequence())
    return endpoint.device ? endpoint.device->Readback() : SkBitmap{};
  SkBitmap result;
  base::WaitableEvent completed;
  if (!endpoint.runner->PostTask(FROM_HERE, base::BindOnce(
          [](base::WeakPtr<MetalOutputDevice> device, SkBitmap* result,
             base::WaitableEvent* completed) {
            if (device) *result = device->Readback();
            completed->Signal();
          }, endpoint.device, &result, &completed))) return {};
  completed.Wait();
  return result;
}

void UpdateMetalGuestLayers(int64_t view_id, std::vector<MetalGuestLayer> guests) {
  Endpoint endpoint;
  {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    auto found = Endpoints().find(view_id);
    if (found == Endpoints().end()) return;
    endpoint = found->second;
  }
  endpoint.runner->PostTask(FROM_HERE, base::BindOnce(
      [](base::WeakPtr<MetalOutputDevice> device, std::vector<MetalGuestLayer> guests) {
        if (device) device->UpdateGuests(std::move(guests));
      }, endpoint.device, std::move(guests)));
}
}  // namespace mini_electron::mac
