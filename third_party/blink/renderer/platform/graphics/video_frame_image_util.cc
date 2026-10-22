// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/graphics/video_frame_image_util.h"

#include "base/logging.h"
#include "components/viz/common/gpu/raster_context_provider.h"
#include "media/base/video_frame.h"
#include "media/base/video_types.h"
#include "media/base/video_util.h"
#include "media/renderers/paint_canvas_video_renderer.h"
#include "third_party/blink/renderer/platform/graphics/canvas_resource_provider.h"
#include "third_party/blink/renderer/platform/graphics/gpu/shared_gpu_context.h"
#include "third_party/blink/renderer/platform/graphics/static_bitmap_image.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/skia_conversions.h"

namespace blink {


ImageOrientationEnum VideoTransformationToImageOrientation(media::VideoTransformation transform)
{
    if (!transform.mirrored) {
        switch (transform.rotation) {
        case media::VIDEO_ROTATION_0:
            return ImageOrientationEnum::kOriginTopLeft;
        case media::VIDEO_ROTATION_90:
            return ImageOrientationEnum::kOriginRightTop;
        case media::VIDEO_ROTATION_180:
            return ImageOrientationEnum::kOriginBottomRight;
        case media::VIDEO_ROTATION_270:
            return ImageOrientationEnum::kOriginLeftBottom;
        }
    }

    switch (transform.rotation) {
    case media::VIDEO_ROTATION_0:
        return ImageOrientationEnum::kOriginTopRight;
    case media::VIDEO_ROTATION_90:
        return ImageOrientationEnum::kOriginLeftTop;
    case media::VIDEO_ROTATION_180:
        return ImageOrientationEnum::kOriginBottomLeft;
    case media::VIDEO_ROTATION_270:
        return ImageOrientationEnum::kOriginRightBottom;
    }
}

media::VideoTransformation ImageOrientationToVideoTransformation(ImageOrientationEnum orientation)
{
    switch (orientation) {
    case ImageOrientationEnum::kOriginTopLeft:
        return media::kNoTransformation;
    case ImageOrientationEnum::kOriginTopRight:
        return media::VideoTransformation(media::VIDEO_ROTATION_0,
            /*mirrored=*/true);
    case ImageOrientationEnum::kOriginBottomRight:
        return media::VIDEO_ROTATION_180;
    case ImageOrientationEnum::kOriginBottomLeft:
        return media::VideoTransformation(media::VIDEO_ROTATION_180,
            /*mirrored=*/true);
    case ImageOrientationEnum::kOriginLeftTop:
        return media::VideoTransformation(media::VIDEO_ROTATION_90,
            /*mirrored=*/true);
    case ImageOrientationEnum::kOriginRightTop:
        return media::VIDEO_ROTATION_90;
    case ImageOrientationEnum::kOriginRightBottom:
        return media::VideoTransformation(media::VIDEO_ROTATION_270,
            /*mirrored=*/true);
    case ImageOrientationEnum::kOriginLeftBottom:
        return media::VIDEO_ROTATION_270;
    };
}

bool WillCreateAcceleratedImagesFromVideoFrame(const media::VideoFrame* frame)
{
    // The embedder exposes no SharedImage raster context. Video snapshots use
    // the bitmap provider, including RGB frames for which upstream uses a mailbox.
    return false;
}

scoped_refptr<StaticBitmapImage> CreateImageFromVideoFrame(scoped_refptr<media::VideoFrame> frame, bool allow_zero_copy_images,
    CanvasResourceProvider* resource_provider, media::PaintCanvasVideoRenderer* video_renderer, const gfx::Rect& dest_rect, bool prefer_tagged_orientation,
    bool reinterpret_video_as_srgb)
{
    DCHECK(frame);
    auto frame_sk_color_space = frame->CompatRGBColorSpace().ToSkColorSpace();
    if (!frame_sk_color_space)
        frame_sk_color_space = SkColorSpace::MakeSRGB();

    // This runtime has no SharedImage raster context. Even when zero-copy is
    // allowed, use the software provider rather than importing a GPU mailbox.
    const auto transform = frame->metadata().transformation.value_or(media::kNoTransformation);
    gfx::Rect final_dest_rect = dest_rect;
    if (final_dest_rect.IsEmpty()) {
        const auto& visible_rect = frame->visible_rect();
        final_dest_rect = gfx::Rect(0, 0, visible_rect.width(), visible_rect.height());
        if (transform.rotation == media::VIDEO_ROTATION_90 || transform.rotation == media::VIDEO_ROTATION_270)
            final_dest_rect.Transpose();
    } else if (!resource_provider) {
        DLOG(ERROR) << "An external CanvasResourceProvider must be provided when providing a custom destination rect.";
        return nullptr;
    } else if (!gfx::Rect(resource_provider->Size()).Contains(final_dest_rect)) {
        DLOG(ERROR) << "Provided CanvasResourceProvider is too small. Expected at least " << final_dest_rect.ToString()
                    << " got " << resource_provider->Size().ToString();
        return nullptr;
    }

    auto raster_context_provider = GetRasterContextProvider();
    const auto resource_provider_info
        = SkImageInfo::Make(gfx::SizeToSkISize(final_dest_rect.size()), kN32_SkColorType, kPremul_SkAlphaType, frame_sk_color_space);
    std::unique_ptr<CanvasResourceProvider> local_resource_provider;
    if (!resource_provider) {
        local_resource_provider = CreateResourceProviderForVideoFrame(resource_provider_info, raster_context_provider.get());
        if (!local_resource_provider) {
            DLOG(ERROR) << "Failed to create CanvasResourceProvider.";
            return nullptr;
        }
        resource_provider = local_resource_provider.get();
    }

    if (resource_provider->IsAccelerated())
        prefer_tagged_orientation = false;

    if (!DrawVideoFrameIntoResourceProvider(std::move(frame), resource_provider, raster_context_provider.get(), final_dest_rect, video_renderer,
            /*ignore_video_transformation=*/prefer_tagged_orientation, reinterpret_video_as_srgb)) {
        return nullptr;
    }
    return resource_provider->Snapshot(
        FlushReason::kNon2DCanvas, prefer_tagged_orientation ? VideoTransformationToImageOrientation(transform) : ImageOrientationEnum::kDefault);
}

bool DrawVideoFrameIntoResourceProvider(scoped_refptr<media::VideoFrame> frame, CanvasResourceProvider* resource_provider,
    viz::RasterContextProvider* raster_context_provider, const gfx::Rect& dest_rect, media::PaintCanvasVideoRenderer* video_renderer,
    bool ignore_video_transformation, bool reinterpret_video_as_srgb)
{
    DCHECK(frame);
    DCHECK(resource_provider);
    DCHECK(gfx::Rect(resource_provider->Size()).Contains(dest_rect));

    if (frame->HasSharedImage()) {
        if (!raster_context_provider) {
            DLOG(ERROR) << "Unable to process a texture backed VideoFrame without a RasterContextProvider.";
            return false;
        }
        if (!raster_context_provider->GrContext() && !raster_context_provider->ContextCapabilities().gpu_rasterization) {
            DLOG(ERROR) << "Unable to process a texture backed VideoFrame without a GrContext or OOP raster support.";
            return false;
        }
    }

    cc::PaintFlags media_flags;
    media_flags.setAlphaf(1.0f);
    media_flags.setFilterQuality(cc::PaintFlags::FilterQuality::kLow);
    media_flags.setBlendMode(SkBlendMode::kSrc);

    std::unique_ptr<media::PaintCanvasVideoRenderer> local_video_renderer;
    if (!video_renderer) {
        local_video_renderer = std::make_unique<media::PaintCanvasVideoRenderer>();
        video_renderer = local_video_renderer.get();
    }

    if (frame->HasMappableGpuBuffer() && !frame->IsMappable() && !resource_provider->IsAccelerated()) {
        frame = media::ConvertToMemoryMappedFrame(std::move(frame));
        if (!frame) {
            DLOG(ERROR) << "Failed to map VideoFrame.";
            return false;
        }
    }

    media::PaintCanvasVideoRenderer::PaintParams params;
    params.dest_rect = gfx::RectF(dest_rect);
    params.transformation = ignore_video_transformation ? media::kNoTransformation : frame->metadata().transformation.value_or(media::kNoTransformation);
    params.reinterpret_as_srgb = reinterpret_video_as_srgb;
    video_renderer->Paint(frame.get(), &resource_provider->Canvas(/*needs_will_draw=*/true), media_flags, params, raster_context_provider);
    return true;
}

void DrawVideoFrameIntoCanvas(scoped_refptr<media::VideoFrame> frame, cc::PaintCanvas* canvas, cc::PaintFlags& flags, bool ignore_video_transformation)
{
    auto raster_context_provider = GetRasterContextProvider();
    media::PaintCanvasVideoRenderer video_renderer;
    media::PaintCanvasVideoRenderer::PaintParams params;
    params.dest_rect = gfx::RectF(frame->natural_size().width(), frame->natural_size().height());
    params.transformation = ignore_video_transformation ? media::kNoTransformation : frame->metadata().transformation.value_or(media::kNoTransformation);
    video_renderer.Paint(std::move(frame), canvas, flags, params, raster_context_provider.get());
}

scoped_refptr<viz::RasterContextProvider> GetRasterContextProvider()
{
    auto wrapper = SharedGpuContext::ContextProviderWrapper();
    if (!wrapper)
        return nullptr;

    if (auto* provider = wrapper->ContextProvider())
        return base::WrapRefCounted(provider->RasterContextProvider());

    return nullptr;
}

std::unique_ptr<CanvasResourceProvider> CreateResourceProviderForVideoFrame(const SkImageInfo& info, viz::RasterContextProvider* raster_context_provider)
{
    return CanvasResourceProvider::CreateBitmapProvider(
        info, cc::PaintFlags::FilterQuality::kLow, CanvasResourceProvider::ShouldInitialize::kNo);
}

} // namespace blink
