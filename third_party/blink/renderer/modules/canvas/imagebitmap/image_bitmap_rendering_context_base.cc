// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/canvas/imagebitmap/image_bitmap_rendering_context_base.h"

#include "third_party/blink/renderer/bindings/core/v8/v8_union_htmlcanvaselement_offscreencanvas.h"
#include "third_party/blink/renderer/core/imagebitmap/image_bitmap.h"
#include "third_party/blink/renderer/core/offscreencanvas/offscreen_canvas.h"
#include "third_party/blink/renderer/platform/graphics/canvas_resource_provider.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/renderer/platform/graphics/gpu/image_layer_bridge.h"
#endif
#include "third_party/blink/renderer/platform/graphics/graphics_context.h"
#include "third_party/blink/renderer/platform/graphics/static_bitmap_image.h"
#include "third_party/blink/renderer/platform/graphics/unaccelerated_static_bitmap_image.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkSurface.h"

namespace blink {

ImageBitmapRenderingContextBase::ImageBitmapRenderingContextBase(CanvasRenderingContextHost* host, const CanvasContextCreationAttributesCore& attrs)
    : CanvasRenderingContext(host, attrs, CanvasRenderingAPI::kBitmaprenderer)
#if !defined(MINI_UI_QUICKJS)
    , image_layer_bridge_(MakeGarbageCollected<ImageLayerBridge>(attrs.alpha ? kNonOpaque : kOpaque))
#endif
{
}

ImageBitmapRenderingContextBase::~ImageBitmapRenderingContextBase() = default;

V8UnionHTMLCanvasElementOrOffscreenCanvas* ImageBitmapRenderingContextBase::getHTMLOrOffscreenCanvas() const
{
    if (Host()->IsOffscreenCanvas()) {
        return MakeGarbageCollected<V8UnionHTMLCanvasElementOrOffscreenCanvas>(static_cast<OffscreenCanvas*>(Host()));
    }
    return MakeGarbageCollected<V8UnionHTMLCanvasElementOrOffscreenCanvas>(static_cast<HTMLCanvasElement*>(Host()));
}

void ImageBitmapRenderingContextBase::Reset()
{
    CHECK(Host());
    CHECK(Host()->IsOffscreenCanvas());
    Host()->DiscardResourceProvider();
}

void ImageBitmapRenderingContextBase::Stop()
{
#if defined(MINI_UI_QUICKJS)
    image_ = nullptr;
#else
    image_layer_bridge_->Dispose();
#endif
}

void ImageBitmapRenderingContextBase::Dispose()
{
    Stop();
    CanvasRenderingContext::Dispose();
}

void ImageBitmapRenderingContextBase::ResetInternalBitmapToBlackTransparent(int width, int height)
{
    SkBitmap black_bitmap;
    if (black_bitmap.tryAllocN32Pixels(width, height)) {
        black_bitmap.eraseARGB(0, 0, 0, 0);
        auto image = SkImages::RasterFromBitmap(black_bitmap);
        if (image) {
#if defined(MINI_UI_QUICKJS)
            image_ = UnacceleratedStaticBitmapImage::Create(image);
#else
            image_layer_bridge_->SetImage(UnacceleratedStaticBitmapImage::Create(image));
#endif
        }
    }
}

void ImageBitmapRenderingContextBase::SetImage(ImageBitmap* image_bitmap)
{
    DCHECK(!image_bitmap || !image_bitmap->IsNeutered());

    // According to the standard TransferFromImageBitmap(null) has to reset the
    // internal bitmap and create a black transparent one.
    if (image_bitmap) {
#if defined(MINI_UI_QUICKJS)
        image_ = image_bitmap->BitmapImage();
#else
        image_layer_bridge_->SetImage(image_bitmap->BitmapImage());
#endif
    }
    else
        ResetInternalBitmapToBlackTransparent(Host()->width(), Host()->height());

    DidDraw(CanvasPerformanceMonitor::DrawType::kOther);

    if (image_bitmap)
        image_bitmap->close();
}

scoped_refptr<StaticBitmapImage> ImageBitmapRenderingContextBase::GetImage(FlushReason)
{
#if defined(MINI_UI_QUICKJS)
    return image_;
#else
    return image_layer_bridge_->GetImage();
#endif
}

scoped_refptr<StaticBitmapImage> ImageBitmapRenderingContextBase::GetImageAndResetInternal()
{
    scoped_refptr<StaticBitmapImage> copy_image = GetImage(FlushReason::kNon2DCanvas);
    if (!copy_image)
        return nullptr;
    ResetInternalBitmapToBlackTransparent(copy_image->width(), copy_image->height());

    return copy_image;
}

#if !defined(MINI_UI_QUICKJS)
void ImageBitmapRenderingContextBase::SetUV(const gfx::PointF& left_top, const gfx::PointF& right_bottom)
{
    image_layer_bridge_->SetUV(left_top, right_bottom);
}
#endif

void ImageBitmapRenderingContextBase::SetFilterQuality(
    cc::PaintFlags::FilterQuality filter_quality)
{
#if defined(MINI_UI_QUICKJS)
    static_cast<void>(filter_quality);
#else
    image_layer_bridge_->SetFilterQuality(filter_quality);
#endif
}

#if !defined(MINI_UI_QUICKJS)
cc::Layer* ImageBitmapRenderingContextBase::CcLayer() const
{
    return image_layer_bridge_->CcLayer();
}
#endif

bool ImageBitmapRenderingContextBase::IsPaintable() const
{
#if defined(MINI_UI_QUICKJS)
    return !!image_;
#else
    return !!image_layer_bridge_->GetImage();
#endif
}

void ImageBitmapRenderingContextBase::Trace(Visitor* visitor) const
{
#if !defined(MINI_UI_QUICKJS)
    visitor->Trace(image_layer_bridge_);
#endif
    CanvasRenderingContext::Trace(visitor);
}

bool ImageBitmapRenderingContextBase::CanCreateCanvas2dResourceProvider() const
{
    DCHECK(Host());
    DCHECK(Host()->IsOffscreenCanvas());
    return !!static_cast<OffscreenCanvas*>(Host())->GetOrCreateResourceProvider();
}

bool ImageBitmapRenderingContextBase::PushFrame()
{
    DCHECK(Host());
    DCHECK(Host()->IsOffscreenCanvas());
    if (!CanCreateCanvas2dResourceProvider())
        return false;

    scoped_refptr<StaticBitmapImage> image = GetImage(FlushReason::kNon2DCanvas);
    if (!image) {
        return false;
    }
    cc::PaintFlags paint_flags;
    paint_flags.setBlendMode(SkBlendMode::kSrc);
    Host()->ResourceProvider()->Canvas().drawImage(image->PaintImageForCurrentFrame(), 0, 0, SkSamplingOptions(), &paint_flags);
    scoped_refptr<CanvasResource> resource = Host()->ResourceProvider()->ProduceCanvasResource(FlushReason::kNon2DCanvas);
    Host()->PushFrame(std::move(resource), SkIRect::MakeWH(image->Size().width(), image->Size().height()));
    return true;
}

bool ImageBitmapRenderingContextBase::IsOriginTopLeft() const
{
#if defined(MINI_UI_QUICKJS)
    return true;
#else
    if (Host()->IsOffscreenCanvas())
        return false;
    return Host()->IsAccelerated();
#endif
}

} // namespace blink
