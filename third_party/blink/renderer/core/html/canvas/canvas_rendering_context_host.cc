// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/canvas/canvas_rendering_context_host.h"

#if !defined(MINI_UI_QUICKJS)
#include "base/feature_list.h"
#endif
#include "base/metrics/histogram_functions.h"
#include "base/metrics/histogram_macros.h"
#include "base/notreached.h"
#if !defined(MINI_UI_QUICKJS)
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/config/gpu_feature_info.h"
#include "third_party/blink/public/common/features.h"
#endif
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/public/common/privacy_budget/identifiability_study_settings.h"
#endif
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/renderer/bindings/core/v8/script_promise_resolver.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_image_encode_options.h"
#include "third_party/blink/renderer/core/html/canvas/canvas_async_blob_creator.h"
#include "third_party/blink/renderer/core/html/canvas/canvas_rendering_context.h"
#include "third_party/blink/renderer/platform/graphics/canvas_resource_dispatcher.h"
#include "third_party/blink/renderer/platform/graphics/canvas_resource_provider.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/renderer/platform/graphics/gpu/shared_gpu_context.h"
#endif
#include "third_party/blink/renderer/platform/graphics/skia/skia_utils.h"
#include "third_party/blink/renderer/platform/graphics/static_bitmap_image.h"
#include "third_party/blink/renderer/platform/graphics/unaccelerated_static_bitmap_image.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "ui/gfx/geometry/skia_conversions.h"

namespace blink {

CanvasRenderingContextHost::CanvasRenderingContextHost(HostType host_type, const gfx::Size& size)
    : CanvasResourceHost(size)
    , host_type_(host_type)
{
}

void CanvasRenderingContextHost::RecordCanvasSizeToUMA()
{
    if (did_record_canvas_size_to_uma_)
        return;
    did_record_canvas_size_to_uma_ = true;

    switch (host_type_) {
    case HostType::kNone:
        NOTREACHED();
    case HostType::kCanvasHost:
        UMA_HISTOGRAM_CUSTOM_COUNTS("Blink.Canvas.SqrtNumberOfPixels", std::sqrt(Size().Area64()), 1, 5000, 100);
        break;
    case HostType::kOffscreenCanvasHost:
        UMA_HISTOGRAM_CUSTOM_COUNTS("Blink.OffscreenCanvas.SqrtNumberOfPixels", std::sqrt(Size().Area64()), 1, 5000, 100);
        break;
    }
}

scoped_refptr<StaticBitmapImage> CanvasRenderingContextHost::CreateTransparentImage(const gfx::Size& size) const
{
    if (!IsValidImageSize(size))
        return nullptr;
    SkImageInfo info = SkImageInfo::Make(gfx::SizeToSkISize(size), GetRenderingContextSkColorInfo().makeAlphaType(kPremul_SkAlphaType));
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info, info.minRowBytes(), nullptr);
    if (!surface)
        return nullptr;
    return UnacceleratedStaticBitmapImage::Create(surface->makeImageSnapshot());
}

void CanvasRenderingContextHost::Commit(scoped_refptr<CanvasResource>&&, const SkIRect&)
{
    NOTIMPLEMENTED();
}

bool CanvasRenderingContextHost::IsPaintable() const
{
    return (RenderingContext() && RenderingContext()->IsPaintable()) || IsValidImageSize(Size());
}

bool CanvasRenderingContextHost::PrintedInCurrentTask() const
{
    return RenderingContext() && RenderingContext()->did_print_in_current_task();
}

void CanvasRenderingContextHost::InitializeForRecording(cc::PaintCanvas* canvas) const
{
    if (RenderingContext())
        RenderingContext()->RestoreCanvasMatrixClipStack(canvas);
}

#if !defined(MINI_UI_QUICKJS)
bool CanvasRenderingContextHost::IsWebGL() const
{
    return RenderingContext() && RenderingContext()->IsWebGL();
}

bool CanvasRenderingContextHost::IsWebGPU() const
{
    return RenderingContext() && RenderingContext()->IsWebGPU();
}
#endif

bool CanvasRenderingContextHost::IsRenderingContext2D() const
{
    return RenderingContext() && RenderingContext()->IsRenderingContext2D();
}

bool CanvasRenderingContextHost::IsImageBitmapRenderingContext() const
{
    return RenderingContext() && RenderingContext()->IsImageBitmapRenderingContext();
}

CanvasResourceProvider* CanvasRenderingContextHost::GetOrCreateCanvasResourceProvider(RasterModeHint hint)
{
    return GetOrCreateCanvasResourceProviderImpl(hint);
}

CanvasResourceProvider* CanvasRenderingContextHost::GetOrCreateCanvasResourceProviderImpl(RasterModeHint hint)
{
    if (!ResourceProvider() && !did_fail_to_create_resource_provider_) {
        if (IsValidImageSize(Size())) {
#if !defined(MINI_UI_QUICKJS)
            if (IsWebGPU()) {
                CreateCanvasResourceProviderWebGPU();
            } else if (IsWebGL()) {
                CreateCanvasResourceProviderWebGL();
            } else
#endif
            {
                CreateCanvasResourceProvider2D(hint);
            }
        }
        if (!ResourceProvider())
            did_fail_to_create_resource_provider_ = true;
    }
    return ResourceProvider();
}

#if !defined(MINI_UI_QUICKJS)
void CanvasRenderingContextHost::CreateCanvasResourceProviderWebGPU()
{
    const SkImageInfo resource_info = SkImageInfo::Make(SkISize::Make(Size().width(), Size().height()), GetRenderingContextSkColorInfo());
    // Renderer-local WebGPU exports owned CPU pixels, not SharedImages.
    ReplaceResourceProvider(CanvasResourceProvider::CreateBitmapProvider(
        resource_info, FilterQuality(), CanvasResourceProvider::ShouldInitialize::kNo, this));
    if (ResourceProvider() && ResourceProvider()->IsValid()) {
        base::UmaHistogramBoolean("Blink.Canvas.ResourceProviderIsAccelerated", ResourceProvider()->IsAccelerated());
        base::UmaHistogramEnumeration("Blink.Canvas.ResourceProviderType", ResourceProvider()->GetType());
    }
}

void CanvasRenderingContextHost::CreateCanvasResourceProviderWebGL()
{
    DCHECK(IsWebGL());

    base::WeakPtr<CanvasResourceDispatcher> dispatcher = GetOrCreateResourceDispatcher() ? GetOrCreateResourceDispatcher()->GetWeakPtr() : nullptr;

    std::unique_ptr<CanvasResourceProvider> provider;
    const SkImageInfo resource_info = SkImageInfo::Make(SkISize::Make(Size().width(), Size().height()), GetRenderingContextSkColorInfo());
    // Do not initialize the CRP using Skia. The CRP can have bottom left origin
    // in which case Skia Graphite won't be able to render into it, and WebGL is
    // responsible for clearing the CRP when it renders anyway and we have clear
    // rect tracking in the shared image system to enforce this.
    constexpr auto kShouldInitialize = CanvasResourceProvider::ShouldInitialize::kNo;
    if (SharedGpuContext::IsGpuCompositingEnabled() && LowLatencyEnabled()) {
        // If LowLatency is enabled, we need a resource that is able to perform well
        // in such mode. It will first try a PassThrough provider and, if that is
        // not possible, it will try a SharedImage with the appropriate flags.
        bool using_swapchain = RenderingContext() && RenderingContext()->UsingSwapChain();
        bool using_webgl_image_chromium = SharedGpuContext::MaySupportImageChromium()
            && (RuntimeEnabledFeatures::WebGLImageChromiumEnabled() || base::FeatureList::IsEnabled(features::kLowLatencyWebGLImageChromium));
        if (using_swapchain || using_webgl_image_chromium) {
            // If either SwapChain is enabled or WebGLImage mode is enabled, we can
            // try a passthrough provider.
            DCHECK(LowLatencyEnabled());
            provider = CanvasResourceProvider::CreatePassThroughProvider(
                resource_info, FilterQuality(), SharedGpuContext::ContextProviderWrapper(), dispatcher, RenderingContext()->IsOriginTopLeft(), this);
        }
        if (!provider) {
            // If PassThrough failed, try a SharedImage with usage display enabled,
            // and if WebGLImageChromium is enabled, add concurrent read write and
            // usage scanout (overlay).
            gpu::SharedImageUsageSet shared_image_usage_flags = gpu::SHARED_IMAGE_USAGE_DISPLAY_READ;
            if (using_webgl_image_chromium) {
                shared_image_usage_flags |= gpu::SHARED_IMAGE_USAGE_SCANOUT;
                shared_image_usage_flags |= gpu::SHARED_IMAGE_USAGE_CONCURRENT_READ_WRITE;
            }
            provider = CanvasResourceProvider::CreateSharedImageProvider(resource_info, FilterQuality(), kShouldInitialize,
                SharedGpuContext::ContextProviderWrapper(), RasterMode::kGPU, shared_image_usage_flags, this);
        }
    } else if (SharedGpuContext::IsGpuCompositingEnabled()) {
        // If there is no LowLatency mode, and GPU is enabled, will try a GPU
        // SharedImage that should support Usage Display and probably Usage Scanout
        // if WebGLImageChromium is enabled.
        gpu::SharedImageUsageSet shared_image_usage_flags = gpu::SHARED_IMAGE_USAGE_DISPLAY_READ;
        if (SharedGpuContext::MaySupportImageChromium() && RuntimeEnabledFeatures::WebGLImageChromiumEnabled()) {
            shared_image_usage_flags |= gpu::SHARED_IMAGE_USAGE_SCANOUT;
        }
        provider = CanvasResourceProvider::CreateSharedImageProvider(
            resource_info, FilterQuality(), kShouldInitialize, SharedGpuContext::ContextProviderWrapper(), RasterMode::kGPU, shared_image_usage_flags, this);
    }

    // If either of the other modes failed and / or it was not possible to do, we
    // will backup with a SharedBitmap, and if that was not possible with a Bitmap
    // provider.
    if (!provider) {
        provider = CanvasResourceProvider::CreateSharedBitmapProvider(
            resource_info, FilterQuality(), kShouldInitialize, dispatcher, SharedGpuContext::SharedImageInterfaceProvider(), this);
    }
    if (!provider) {
        provider = CanvasResourceProvider::CreateBitmapProvider(resource_info, FilterQuality(), kShouldInitialize, this);
    }

    ReplaceResourceProvider(std::move(provider));
    if (ResourceProvider() && ResourceProvider()->IsValid()) {
        base::UmaHistogramBoolean("Blink.Canvas.ResourceProviderIsAccelerated", ResourceProvider()->IsAccelerated());
        base::UmaHistogramEnumeration("Blink.Canvas.ResourceProviderType", ResourceProvider()->GetType());
    }
}
#endif

void CanvasRenderingContextHost::CreateCanvasResourceProvider2D(RasterModeHint hint)
{
    DCHECK(IsRenderingContext2D() || IsImageBitmapRenderingContext());
    const SkImageInfo resource_info = SkImageInfo::Make(
        SkISize::Make(Size().width(), Size().height()),
        GetRenderingContextSkColorInfo());
    constexpr auto kShouldInitialize =
        CanvasResourceProvider::ShouldInitialize::kCallClear;
    std::unique_ptr<CanvasResourceProvider> provider;
#if defined(MINI_UI_QUICKJS)
    static_cast<void>(hint);
    provider = CanvasResourceProvider::CreateBitmapProvider(
        resource_info, FilterQuality(), kShouldInitialize, this);
#else
    base::WeakPtr<CanvasResourceDispatcher> dispatcher =
        GetOrCreateResourceDispatcher()
        ? GetOrCreateResourceDispatcher()->GetWeakPtr()
        : nullptr;
    const bool use_gpu =
        hint == RasterModeHint::kPreferGPU && ShouldAccelerate2dContext();
    if (use_gpu && LowLatencyEnabled()) {
        provider = CanvasResourceProvider::CreateSwapChainProvider(
            resource_info, FilterQuality(), kShouldInitialize,
            SharedGpuContext::ContextProviderWrapper(), dispatcher, this);
        if (!provider) {
            gpu::SharedImageUsageSet shared_image_usage_flags =
                gpu::SHARED_IMAGE_USAGE_DISPLAY_READ;
            if (SharedGpuContext::MaySupportImageChromium()
                && (RuntimeEnabledFeatures::Canvas2dImageChromiumEnabled()
                    || base::FeatureList::IsEnabled(
                        features::kLowLatencyCanvas2dImageChromium))) {
                shared_image_usage_flags |= gpu::SHARED_IMAGE_USAGE_SCANOUT;
                shared_image_usage_flags |=
                    gpu::SHARED_IMAGE_USAGE_CONCURRENT_READ_WRITE;
            }
            provider = CanvasResourceProvider::CreateSharedImageProvider(
                resource_info, FilterQuality(), kShouldInitialize,
                SharedGpuContext::ContextProviderWrapper(), RasterMode::kGPU,
                shared_image_usage_flags, this);
        }
    } else if (use_gpu) {
        gpu::SharedImageUsageSet shared_image_usage_flags =
            gpu::SHARED_IMAGE_USAGE_DISPLAY_READ;
        if (SharedGpuContext::MaySupportImageChromium()
            && RuntimeEnabledFeatures::Canvas2dImageChromiumEnabled()) {
            shared_image_usage_flags |= gpu::SHARED_IMAGE_USAGE_SCANOUT;
        }
        provider = CanvasResourceProvider::CreateSharedImageProvider(
            resource_info, FilterQuality(), kShouldInitialize,
            SharedGpuContext::ContextProviderWrapper(), RasterMode::kGPU,
            shared_image_usage_flags, this);
    } else if (SharedGpuContext::MaySupportImageChromium()
        && RuntimeEnabledFeatures::Canvas2dImageChromiumEnabled()) {
        const gpu::SharedImageUsageSet shared_image_usage_flags =
            gpu::SHARED_IMAGE_USAGE_DISPLAY_READ
            | gpu::SHARED_IMAGE_USAGE_SCANOUT;
        provider = CanvasResourceProvider::CreateSharedImageProvider(
            resource_info, FilterQuality(), kShouldInitialize,
            SharedGpuContext::ContextProviderWrapper(), RasterMode::kCPU,
            shared_image_usage_flags, this);
    }
    if (!provider) {
        provider = CanvasResourceProvider::CreateSharedBitmapProvider(
            resource_info, FilterQuality(), kShouldInitialize, dispatcher,
            SharedGpuContext::SharedImageInterfaceProvider(), this);
    }
    if (!provider) {
        provider = CanvasResourceProvider::CreateBitmapProvider(
            resource_info, FilterQuality(), kShouldInitialize, this);
    }
#endif
    ReplaceResourceProvider(std::move(provider));
    if (ResourceProvider()) {
        if (ResourceProvider()->IsValid()) {
            base::UmaHistogramBoolean(
                "Blink.Canvas.ResourceProviderIsAccelerated",
                ResourceProvider()->IsAccelerated());
            base::UmaHistogramEnumeration(
                "Blink.Canvas.ResourceProviderType",
                ResourceProvider()->GetType());
        }
        ResourceProvider()->SetFilterQuality(FilterQuality());
        ResourceProvider()->SetResourceRecyclingEnabled(true);
    }
}

SkColorInfo CanvasRenderingContextHost::GetRenderingContextSkColorInfo() const
{
    if (RenderingContext())
        return RenderingContext()->CanvasRenderingContextSkColorInfo();
    return SkColorInfo(kN32_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
}

bool CanvasRenderingContextHost::IsOffscreenCanvas() const
{
    return host_type_ == HostType::kOffscreenCanvasHost;
}

#if !defined(MINI_UI_QUICKJS)
IdentifiableToken CanvasRenderingContextHost::IdentifiabilityInputDigest(const CanvasRenderingContext* const context) const
{
    const uint64_t context_digest = context ? context->IdentifiableTextToken().ToUkmMetricValue() : 0;
    const uint64_t context_type = static_cast<uint64_t>(context ? context->GetRenderingAPI() : CanvasRenderingContext::CanvasRenderingAPI::kUnknown);
    const bool encountered_skipped_ops = context && context->IdentifiabilityEncounteredSkippedOps();
    const bool encountered_sensitive_ops = context && context->IdentifiabilityEncounteredSensitiveOps();
    const bool encountered_partially_digested_image = context && context->IdentifiabilityEncounteredPartiallyDigestedImage();
    // Bits [0-3] are the context type, bits [4-6] are skipped ops, sensitive
    // ops, and partial image ops bits, respectively. The remaining bits are
    // for the canvas digest.
    uint64_t final_digest = (context_digest << 7) | context_type;
    if (encountered_skipped_ops)
        final_digest |= IdentifiableSurface::CanvasTaintBit::kSkipped;
    if (encountered_sensitive_ops)
        final_digest |= IdentifiableSurface::CanvasTaintBit::kSensitive;
    if (encountered_partially_digested_image)
        final_digest |= IdentifiableSurface::CanvasTaintBit::kPartiallyDigested;
    return final_digest;
}
#endif

void CanvasRenderingContextHost::PageVisibilityChanged()
{
    bool page_visible = IsPageVisible();
    if (RenderingContext()) {
        RenderingContext()->PageVisibilityChanged();
        if (page_visible) {
            RenderingContext()->SendContextLostEventIfNeeded();
        }
    }
#if !defined(MINI_UI_QUICKJS)
    if (!page_visible && (IsWebGL() || IsWebGPU())) {
        DiscardResourceProvider();
    }
#endif
}

bool CanvasRenderingContextHost::ContextHasOpenLayers(const CanvasRenderingContext* context) const
{
    return context != nullptr && context->IsRenderingContext2D() && context->LayerCount() != 0;
}

} // namespace blink
