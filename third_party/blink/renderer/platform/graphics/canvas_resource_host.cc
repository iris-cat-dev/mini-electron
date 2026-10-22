// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/graphics/canvas_resource_host.h"

#include "third_party/blink/public/common/features.h"
#include "third_party/blink/renderer/platform/graphics/canvas_resource_provider.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/renderer/platform/graphics/gpu/shared_context_rate_limiter.h"
#include "third_party/blink/renderer/platform/graphics/gpu/shared_gpu_context.h"
#endif

namespace blink {

#if !defined(MINI_UI_QUICKJS)
namespace {
constexpr unsigned kMaxCanvasAnimationBacklog = 2;

bool CanUseGPU()
{
    base::WeakPtr<WebGraphicsContext3DProviderWrapper> context_provider_wrapper =
        SharedGpuContext::ContextProviderWrapper();
    return context_provider_wrapper
        && !context_provider_wrapper->ContextProvider()->IsContextLost();
}
} // namespace
#endif

CanvasResourceHost::CanvasResourceHost(gfx::Size size)
    : size_(size)
{
}

CanvasResourceHost::~CanvasResourceHost()
{
#if !defined(MINI_UI_QUICKJS)
    ResetLayer();
#endif
}

std::unique_ptr<CanvasResourceProvider> CanvasResourceHost::ReplaceResourceProvider(std::unique_ptr<CanvasResourceProvider> new_resource_provider)
{
    std::unique_ptr<CanvasResourceProvider> old_resource_provider = std::move(resource_provider_);
    resource_provider_ = std::move(new_resource_provider);
    UpdateMemoryUsage();
#if !defined(MINI_UI_QUICKJS)
    if (resource_provider_) {
        resource_provider_->AlwaysEnableRasterTimersForTesting(always_enable_raster_timers_for_testing_);
    }
#endif
    if (old_resource_provider) {
        old_resource_provider->SetCanvasResourceHost(nullptr);
    }
    return old_resource_provider;
}

void CanvasResourceHost::DiscardResourceProvider()
{
    resource_provider_ = nullptr;
    UpdateMemoryUsage();
}

void CanvasResourceHost::SetFilterQuality(cc::PaintFlags::FilterQuality filter_quality)
{
    filter_quality_ = filter_quality;
    if (resource_provider_) {
        resource_provider_->SetFilterQuality(filter_quality);
    }
#if !defined(MINI_UI_QUICKJS)
    if (cc_layer_) {
        cc_layer_->SetNearestNeighbor(filter_quality == cc::PaintFlags::FilterQuality::kNone);
    }
#endif
}

void CanvasResourceHost::SetPreferred2DRasterMode(RasterModeHint hint)
{
#if defined(MINI_UI_QUICKJS)
    static_cast<void>(hint);
    preferred_2d_raster_mode_ = RasterModeHint::kPreferCPU;
#else
    preferred_2d_raster_mode_ = hint;
#endif
}

#if !defined(MINI_UI_QUICKJS)
bool CanvasResourceHost::ShouldTryToUseGpuRaster() const
{
    return preferred_2d_raster_mode_ == RasterModeHint::kPreferGPU
        && CanUseGPU();
}
#endif

#if !defined(MINI_UI_QUICKJS)
bool CanvasResourceHost::IsComposited() const
{
    if (IsHibernating()) {
        return false;
    }

    if (!resource_provider_) [[unlikely]] {
        return false;
    }

    return resource_provider_->SupportsDirectCompositing() && !LowLatencyEnabled();
}
#endif

void CanvasResourceHost::SetIsDisplayed(bool displayed)
{
    is_displayed_ = displayed;
#if !defined(MINI_UI_QUICKJS)
    // If the canvas is no longer being displayed, stop using the rate limiter.
    if (!is_displayed_) {
        frames_since_last_commit_ = 0;
        if (rate_limiter_) {
            rate_limiter_->Reset();
            rate_limiter_.reset(nullptr);
        }
    }
#endif
}

#if !defined(MINI_UI_QUICKJS)
SharedContextRateLimiter* CanvasResourceHost::RateLimiter() const
{
    return rate_limiter_.get();
}

void CanvasResourceHost::CreateRateLimiter()
{
    rate_limiter_ = std::make_unique<SharedContextRateLimiter>(kMaxCanvasAnimationBacklog);
}
#endif

RasterMode CanvasResourceHost::GetRasterMode() const
{
#if defined(MINI_UI_QUICKJS)
    return RasterMode::kCPU;
#else
    if (preferred_2d_raster_mode() == RasterModeHint::kPreferCPU)
        return RasterMode::kCPU;
    if (IsHibernating())
        return RasterMode::kCPU;
    if (resource_provider_) {
        return resource_provider_->IsAccelerated()
            ? RasterMode::kGPU
            : RasterMode::kCPU;
    }
    return ShouldTryToUseGpuRaster() ? RasterMode::kGPU : RasterMode::kCPU;
#endif
}

#if !defined(MINI_UI_QUICKJS)
void CanvasResourceHost::ResetLayer()
{
    if (cc_layer_) {
#if !defined(MINI_UI_QUICKJS)
        if (GetRasterMode() == RasterMode::kGPU) {
            cc_layer_->ClearTexture();
            cc_layer_->RemoveFromParent();
        }
#endif
        cc_layer_->ClearClient();
        cc_layer_ = nullptr;
    }
}

void CanvasResourceHost::ClearLayerTexture()
{
    if (cc_layer_) {
        cc_layer_->ClearTexture();
    }
}

void CanvasResourceHost::SetNeedsPushProperties()
{
    if (cc_layer_) {
        cc_layer_->SetNeedsSetTransferableResource();
    }
}
#endif

void CanvasResourceHost::SetHdrMetadata(const gfx::HDRMetadata& hdr_metadata)
{
    hdr_metadata_ = hdr_metadata;
}

#if !defined(MINI_UI_QUICKJS)
cc::TextureLayer* CanvasResourceHost::GetOrCreateCcLayerIfNeeded()
{
    if (!IsComposited()) {
        return nullptr;
    }
    if (!cc_layer_) [[unlikely]] {
        cc_layer_ = cc::TextureLayer::CreateForMailbox(this);
        cc_layer_->SetIsDrawable(true);
        cc_layer_->SetHitTestable(true);
        cc_layer_->SetContentsOpaque(opacity_mode_ == kOpaque);
        cc_layer_->SetBlendBackgroundColor(opacity_mode_ != kOpaque);
        cc_layer_->SetNearestNeighbor(FilterQuality() == cc::PaintFlags::FilterQuality::kNone);
        cc_layer_->SetFlipped(!resource_provider_->IsOriginTopLeft());
    }
    return cc_layer_.get();
}

namespace {

// Adapter for wrapping a CanvasResourceReleaseCallback into a
// viz::ReleaseCallback
void ReleaseCanvasResource(
    CanvasResource::ReleaseCallback callback, scoped_refptr<CanvasResource> canvas_resource, const gpu::SyncToken& sync_token, bool is_lost)
{
    std::move(callback).Run(std::move(canvas_resource), sync_token, is_lost);
}

} // unnamed namespace

bool CanvasResourceHost::PrepareTransferableResource(
    cc::SharedBitmapIdRegistrar* bitmap_registrar, viz::TransferableResource* out_resource, viz::ReleaseCallback* out_release_callback)
{
    CHECK(cc_layer_); // This explodes if FinalizeFrame() was not called.

    frames_since_last_commit_ = 0;
    if (rate_limiter_) {
        rate_limiter_->Reset();
    }

    // If hibernating but not hidden, we want to wake up from hibernation.
    if (IsHibernating() && !IsPageVisible()) {
        return false;
    }

    if (!IsResourceValid()) {
        return false;
    }

    // The beforeprint event listener is sometimes scheduled in the same task
    // as BeginFrame, which means that this code may sometimes be called between
    // the event listener and its associated FinalizeFrame call. So in order to
    // preserve the display list for printing, FlushRecording needs to know
    // whether any printing occurred in the current task.
    FlushReason reason = FlushReason::kCanvasPushFrame;
    if (PrintedInCurrentTask() || IsPrinting()) {
        reason = FlushReason::kCanvasPushFrameWhilePrinting;
    }
    FlushRecording(reason);

    // If the context is lost, we don't know if we should be producing GPU or
    // software frames, until we get a new context, since the compositor will
    // be trying to get a new context and may change modes.
    if (!GetOrCreateCanvasResourceProvider(preferred_2d_raster_mode_)) {
        return false;
    }

    scoped_refptr<CanvasResource> frame = resource_provider_->ProduceCanvasResource(reason);
    if (!frame || !frame->IsValid()) {
        return false;
    }

    CanvasResource::ReleaseCallback release_callback;
    if (!frame->PrepareTransferableResource(out_resource, &release_callback,
            /*needs_verified_synctoken=*/false)
        || *out_resource == cc_layer_->current_transferable_resource()) {
        // If the resource did not change, the release will be handled correctly
        // when the callback from the previous frame is dispatched. But run the
        // |release_callback| to release the ref acquired above.
        std::move(release_callback).Run(std::move(frame), gpu::SyncToken(), false /* is_lost */);
        return false;
    }
    // TODO(https://crbug.com/1475955): HDR metadata should be propagated to
    // `frame`, and should be populated by the above call to
    // CanvasResource::PrepareTransferableResource, rather than be inserted
    // here.
    out_resource->hdr_metadata = hdr_metadata_;
    // Note: frame is kept alive via a reference kept in out_release_callback.
    *out_release_callback = base::BindOnce(ReleaseCanvasResource, std::move(release_callback), std::move(frame));

    return true;
}

void CanvasResourceHost::DoPaintInvalidation(const gfx::Rect& dirty_rect)
{
    if (cc_layer_ && IsComposited()) {
        cc_layer_->SetNeedsDisplayRect(dirty_rect);
    }
}

void CanvasResourceHost::SetOpacityMode(OpacityMode opacity_mode)
{
    opacity_mode_ = opacity_mode;
    if (cc_layer_) {
        cc_layer_->SetContentsOpaque(opacity_mode_ == kOpaque);
        cc_layer_->SetBlendBackgroundColor(opacity_mode_ != kOpaque);
    }
}
#endif

void CanvasResourceHost::FlushRecording(FlushReason reason)
{
    if (resource_provider_) {
        resource_provider_->FlushCanvas(reason);
    }
}

bool CanvasResourceHost::IsResourceValid()
{
#if defined(MINI_UI_QUICKJS)
    return !resource_provider_ || resource_provider_->IsValid();
#else
    if (IsHibernating())
        return true;
    if (!cc_layer_)
        return true;
    if (!features::IsCanvasSharedBitmapConversionEnabled()
        || (resource_provider_
            && resource_provider_->GetType()
                == CanvasResourceProvider::kBitmap)) {
        if (preferred_2d_raster_mode_ == RasterModeHint::kPreferCPU)
            return true;
    }
    if (context_lost_ || shared_bitmap_gpu_channel_lost_)
        return false;
    if (resource_provider_ && resource_provider_->IsAccelerated()
        && resource_provider_->IsGpuContextLost()) {
        context_lost_ = true;
        ReplaceResourceProvider(nullptr);
        NotifyGpuContextLost();
        return false;
    }
    if (resource_provider_
        && resource_provider_->GetType()
            == CanvasResourceProvider::kSharedBitmap
        && resource_provider_->IsSharedBitmapGpuChannelLost()) {
        shared_bitmap_gpu_channel_lost_ = true;
        ReplaceResourceProvider(nullptr);
        NotifyGpuContextLost();
        return false;
    }
    return !!GetOrCreateCanvasResourceProvider(preferred_2d_raster_mode_);
#endif
}

} // namespace blink
