// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/webgpu/gpu_canvas_context.h"

#include "components/viz/common/resources/shared_image_format_utils.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_union_htmlcanvaselement_offscreencanvas.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_gpu_canvas_alpha_mode.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_gpu_canvas_configuration.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_gpu_canvas_tone_mapping.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_gpu_canvas_tone_mapping_mode.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_union_canvasrenderingcontext2d_gpucanvascontext_imagebitmaprenderingcontext_webgl2renderingcontext_webglrenderingcontext.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_union_gpucanvascontext_imagebitmaprenderingcontext_offscreencanvasrenderingcontext2d_webgl2renderingcontext_webglrenderingcontext.h"
#include "third_party/blink/renderer/core/frame/web_feature.h"
#include "third_party/blink/renderer/core/html/canvas/predefined_color_space.h"
#include "third_party/blink/renderer/core/imagebitmap/image_bitmap.h"
#include "third_party/blink/renderer/core/offscreencanvas/offscreen_canvas.h"
#include "third_party/blink/renderer/modules/webgpu/dawn_conversions.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_device.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_texture.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/graphics/canvas_color_params.h"
#include "third_party/blink/renderer/platform/graphics/canvas_resource_provider.h"
#include "third_party/blink/renderer/platform/graphics/gpu/webgpu_texture_alpha_clearer.h"
#include "third_party/blink/renderer/platform/graphics/unaccelerated_static_bitmap_image.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkPixmap.h"

namespace blink {

namespace {

bool IsContextFormatSupported(V8GPUTextureFormat::Enum format)
{
    switch (format) {
    case V8GPUTextureFormat::Enum::kBgra8Unorm:
    case V8GPUTextureFormat::Enum::kRgba8Unorm:
    case V8GPUTextureFormat::Enum::kRgba16Float:
        return true;
    default:
        return false;
    }
}

} // namespace

GPUCanvasContext::Factory::~Factory() = default;

CanvasRenderingContext* GPUCanvasContext::Factory::Create(CanvasRenderingContextHost* host, const CanvasContextCreationAttributesCore& attrs)
{
    CanvasRenderingContext* rendering_context = MakeGarbageCollected<GPUCanvasContext>(host, attrs);
    DCHECK(host);
    return rendering_context;
}

CanvasRenderingContext::CanvasRenderingAPI GPUCanvasContext::Factory::GetRenderingAPI() const
{
    return CanvasRenderingContext::CanvasRenderingAPI::kWebgpu;
}

GPUCanvasContext::GPUCanvasContext(CanvasRenderingContextHost* host, const CanvasContextCreationAttributesCore& attrs)
    : CanvasRenderingContext(host, attrs, CanvasRenderingAPI::kWebgpu)
{
    texture_descriptor_ = {};
}

GPUCanvasContext::~GPUCanvasContext()
{
    // Perform destruction that's safe to do inside a GC (as in it doesn't touch
    // other GC objects).
    if (swap_buffers_) {
        swap_buffers_->Neuter();
    }
}

void GPUCanvasContext::Trace(Visitor* visitor) const
{
    visitor->Trace(device_);
    visitor->Trace(texture_);
    CanvasRenderingContext::Trace(visitor);
}

// CanvasRenderingContext implementation
V8RenderingContext* GPUCanvasContext::AsV8RenderingContext()
{
    return MakeGarbageCollected<V8RenderingContext>(this);
}

V8OffscreenRenderingContext* GPUCanvasContext::AsV8OffscreenRenderingContext()
{
    return MakeGarbageCollected<V8OffscreenRenderingContext>(this);
}

SkColorInfo GPUCanvasContext::CanvasRenderingContextSkColorInfo() const
{
    if (!swap_buffers_)
        return CanvasRenderingContext::CanvasRenderingContextSkColorInfo();
    return SkColorInfo(viz::ToClosestSkColorType(
                           /*gpu_compositing=*/true, swap_buffers_->Format()),
        alpha_mode_ == V8GPUCanvasAlphaMode::Enum::kOpaque ? kOpaque_SkAlphaType : kPremul_SkAlphaType, PredefinedColorSpaceToSkColorSpace(color_space_));
}

void GPUCanvasContext::Stop()
{
    ReplaceDrawingBuffer(/*destroy_swap_buffers*/ true);
    stopped_ = true;
}

cc::Layer* GPUCanvasContext::CcLayer() const
{
    if (swap_buffers_) {
        return swap_buffers_->CcLayer();
    }
    return nullptr;
}

void GPUCanvasContext::Reshape(int width, int height)
{
    if (stopped_) {
        return;
    }

    // Steps for canvas context resizing:
    // 1. Replace the drawing buffer of context.
    ReplaceDrawingBuffer(/* destroy_swap_buffers */ false);

    // 2. Let configuration be context.[[configuration]]
    // 3. If configuration is not null:
    //   1. Set context.[[textureDescriptor]] to the GPUTextureDescriptor for the
    //      canvas and configuration(canvas, configuration).
    texture_descriptor_.size = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };

    // If we don't notify the host that something has changed it may never check
    // for the new cc::Layer.
    Host()->SetNeedsCompositingUpdate();
}

scoped_refptr<StaticBitmapImage> GPUCanvasContext::GetImage(FlushReason)
{
    if (!swap_buffers_)
        return nullptr;
    if (auto image = swap_buffers_->Snapshot())
        return image;
    // A newly configured, resized, or transferred drawing buffer starts black,
    // with the configured alpha interpretation and color space.
    const gfx::Size size = Host()->Size();
    SkBitmap pixels;
    if (size.IsEmpty() || !pixels.tryAllocPixels(SkImageInfo::Make(SkISize::Make(size.width(), size.height()), CanvasRenderingContextSkColorInfo())))
        return nullptr;
    pixels.eraseColor(alpha_mode_ == V8GPUCanvasAlphaMode::Enum::kOpaque ? SK_ColorBLACK : SK_ColorTRANSPARENT);
    pixels.setImmutable();
    return UnacceleratedStaticBitmapImage::Create(SkImages::RasterFromBitmap(pixels));
}

bool GPUCanvasContext::PaintRenderingResultsToCanvas(SourceDrawingBuffer)
{
    auto image = GetImage(FlushReason::kNone);
    if (!image)
        return false;
    if (Host()->ResourceProvider() && Host()->ResourceProvider()->Size() != Host()->Size())
        Host()->DiscardResourceProvider();
    auto* provider = Host()->GetOrCreateCanvasResourceProvider(RasterModeHint::kPreferCPU);
    if (!provider)
        return false;
    auto sk_image = image->PaintImageForCurrentFrame().GetSwSkImage();
    SkPixmap pixels;
    return sk_image && sk_image->peekPixels(&pixels)
        && provider->WritePixels(pixels.info(), pixels.addr(), pixels.rowBytes(), 0, 0);
}

bool GPUCanvasContext::CopyRenderingResultsToVideoFrame(WebGraphicsContext3DVideoFramePool*, SourceDrawingBuffer,
    const gfx::ColorSpace&, VideoFrameCopyCompletedCallback)
{
    // This hook is the accelerated video conversion path. Returning false asks
    // the caller to use the owned CPU snapshot instead of a GPU-service pool.
    return false;
}

void GPUCanvasContext::SetFilterQuality(cc::PaintFlags::FilterQuality filter_quality)
{
    if (filter_quality != filter_quality_) {
        filter_quality_ = filter_quality;
        if (swap_buffers_) {
            swap_buffers_->SetFilterQuality(filter_quality);
        }
    }
}

bool GPUCanvasContext::PushFrame()
{
    DCHECK(Host());
    DCHECK(Host()->IsOffscreenCanvas());
    if (!swap_buffers_)
        return false;
    auto* offscreen = static_cast<OffscreenCanvas*>(Host());
    if (!offscreen->HasPlaceholderCanvas())
        return false;
    auto resource = swap_buffers_->ExportCanvasResource(offscreen->GetOrCreateResourceDispatcher());
    if (!resource)
        return false;
    const gfx::Size size = resource->Size();
    return Host()->PushFrame(std::move(resource), SkIRect::MakeWH(size.width(), size.height()));
}

ImageBitmap* GPUCanvasContext::TransferToImageBitmap(ScriptState* script_state, ExceptionState& exception_state)
{
    auto MakeFallbackImageBitmap = [this](V8GPUCanvasAlphaMode::Enum alpha_mode) -> ImageBitmap* {
        // It is not possible to create an empty image bitmap, return null in that
        // case which will fail ImageBitmap creation with an exception instead.
        gfx::Size size = Host()->Size();
        if (size.IsEmpty()) {
            return nullptr;
        }

        // We intentionally leave the image in legacy color space.
        SkBitmap black_bitmap;
        if (!black_bitmap.tryAllocN32Pixels(size.width(), size.height())) {
            // It is not possible to create such a big image bitmap, return null in
            // that case which will fail ImageBitmap creation with an exception
            // instead.
            return nullptr;
        }

        if (alpha_mode == V8GPUCanvasAlphaMode::Enum::kOpaque) {
            black_bitmap.eraseARGB(255, 0, 0, 0);
        } else {
            black_bitmap.eraseARGB(0, 0, 0, 0);
        }

        return MakeGarbageCollected<ImageBitmap>(UnacceleratedStaticBitmapImage::Create(SkImages::RasterFromBitmap(black_bitmap)));
    };

    // If the canvas configuration is invalid, WebGPU requires that we give a
    // fallback black ImageBitmap if possible.
    if (!swap_buffers_) {
        return MakeFallbackImageBitmap(V8GPUCanvasAlphaMode::Enum::kOpaque);
    }

    auto image = swap_buffers_->TransferToImageBitmap();
    // Transfer resets the drawing buffer even when the previous texture was
    // explicitly destroyed, and the returned bitmap owns independent pixels.
    OnTextureTransferred();
    new_texture_required_ = true;
    return image ? MakeGarbageCollected<ImageBitmap>(std::move(image)) : MakeFallbackImageBitmap(alpha_mode_);
}

// gpu_presentation_context.idl
V8UnionHTMLCanvasElementOrOffscreenCanvas* GPUCanvasContext::getHTMLOrOffscreenCanvas() const
{
    if (Host()->IsOffscreenCanvas()) {
        return MakeGarbageCollected<V8UnionHTMLCanvasElementOrOffscreenCanvas>(static_cast<OffscreenCanvas*>(Host()));
    }
    return MakeGarbageCollected<V8UnionHTMLCanvasElementOrOffscreenCanvas>(static_cast<HTMLCanvasElement*>(Host()));
}

void GPUCanvasContext::configure(const GPUCanvasConfiguration* descriptor, ExceptionState& exception_state)
{
    DCHECK(descriptor);

    if (stopped_ || !Host()) {
        // This is probably not possible, or at least would only happen during page
        // shutdown.
        exception_state.ThrowDOMException(DOMExceptionCode::kUnknownError, "canvas has been destroyed");
        return;
    }

    if (!descriptor->device()->ValidateTextureFormatUsage(descriptor->format(), exception_state)) {
        return;
    }

    for (auto view_format : descriptor->viewFormats()) {
        if (!descriptor->device()->ValidateTextureFormatUsage(view_format, exception_state)) {
            return;
        }
    }

    if (!IsContextFormatSupported(descriptor->format().AsEnum())) {
        exception_state.ThrowTypeError(String::Format("Unsupported canvas context format '%s'.", V8GPUTextureFormat(descriptor->format()).AsCStr()));
        return;
    }

    // As soon as the validation for extensions for usage and formats passes, the
    // canvas is "configured" and calls to getNextTexture() will return GPUTexture
    // objects (valid or invalid) and not throw.
    configured_ = true;

    view_formats_ = AsDawnEnum<wgpu::TextureFormat>(descriptor->viewFormats());
    gfx::Size host_size = Host()->Size();

    // Set the default values of the member corresponding to
    // GPUCanvasContext.[[texture_descriptor]] in the WebGPU spec.
    texture_descriptor_ = {
        // Set the values from the configuration descriptor
        .usage = AsDawnFlags<wgpu::TextureUsage>(descriptor->usage()),
        .size = { static_cast<uint32_t>(host_size.width()), static_cast<uint32_t>(host_size.height()) },
        .format = AsDawnEnum(descriptor->format()),
        .viewFormatCount = descriptor->viewFormats().size(),
        .viewFormats = view_formats_.data(),
        // Set the size of the texture in case there was no Reshape() since the
        // creation of the context.
    };

    // Reconfiguring the context discards previous drawing buffers but we also
    // destroy the swap buffers so that any validation error below will cause
    // swap_buffers_ to be nullptr and getCurrentTexture() to fail.
    ReplaceDrawingBuffer(/*destroy_swap_buffers*/ true);

    // Store the configured device separately, even if the configuration fails, so
    // that errors can be generated in the appropriate error scope.
    device_ = descriptor->device();

    // Configuration validation belongs to the configured device's error scope.
    device_->GetHandle().ValidateTextureDescriptor(&texture_descriptor_);

    alpha_mode_ = descriptor->alphaMode().AsEnum();
    texture_internal_usage_ = { {
        .internalUsage = wgpu::TextureUsage::CopySrc,
    } };
    if (alpha_mode_ == V8GPUCanvasAlphaMode::Enum::kOpaque)
        texture_internal_usage_.internalUsage |= wgpu::TextureUsage::RenderAttachment;
    texture_descriptor_.nextInChain = &texture_internal_usage_;

    if (!ValidateAndConvertColorSpace(descriptor->colorSpace(), color_space_, exception_state)) {
        return;
    }

    gfx::HDRMetadata hdr_metadata;
    tone_mapping_mode_ = V8GPUCanvasToneMappingMode::Enum::kStandard;
    if (descriptor->hasToneMapping() && descriptor->toneMapping()->hasMode()) {
        tone_mapping_mode_ = descriptor->toneMapping()->mode().AsEnum();
        switch (tone_mapping_mode_) {
        case V8GPUCanvasToneMappingMode::Enum::kStandard:
            break;
        case V8GPUCanvasToneMappingMode::Enum::kExtended:
            hdr_metadata.extended_range.emplace(
                /*current_headroom=*/gfx::HdrMetadataExtendedRange::kDefaultHdrHeadroom,
                /*desired_headroom=*/gfx::HdrMetadataExtendedRange::kDefaultHdrHeadroom);
            break;
        }
    }

    swap_buffers_ = base::AdoptRef(new WebGPUSwapBufferProvider(this, device_->GetDawnControlClient(), device_->GetHandle(),
        texture_descriptor_.format, color_space_, hdr_metadata));
    swap_buffers_->SetFilterQuality(filter_quality_);

    // Note: SetContentsOpaque is only an optimization hint. It doesn't
    // actually make the contents opaque.
    switch (alpha_mode_) {
    case V8GPUCanvasAlphaMode::Enum::kOpaque: {
        if (!alpha_clearer_ || !alpha_clearer_->IsCompatible(device_->GetHandle(), texture_descriptor_.format)) {
            alpha_clearer_
                = base::MakeRefCounted<WebGPUTextureAlphaClearer>(device_->GetDawnControlClient(), device_->GetHandle(), texture_descriptor_.format);
        }
        break;
    }
    case V8GPUCanvasAlphaMode::Enum::kPremultiplied:
        alpha_clearer_ = nullptr;
        break;
    }

    // If we don't notify the host that something has changed it may never check
    // for the new cc::Layer.
    Host()->SetNeedsCompositingUpdate();
}

void GPUCanvasContext::unconfigure()
{
    if (stopped_) {
        return;
    }

    ReplaceDrawingBuffer(/*destroy_swap_buffers*/ true);

    // When developers call unconfigure from the page, one of the reasons for
    // doing so is to expressly release the GPUCanvasContext's device reference.
    // In order to fully release it, any TextureAlphaClearer that has been created
    // also needs to be released.
    alpha_clearer_ = nullptr;
    device_ = nullptr;
    configured_ = false;
}

GPUCanvasConfiguration* GPUCanvasContext::getConfiguration()
{
    if (!configured_) {
        return nullptr;
    }

    GPUCanvasConfiguration* configuration = GPUCanvasConfiguration::Create();
    configuration->setDevice(device_);
    configuration->setFormat(FromDawnEnum(texture_descriptor_.format));
    configuration->setUsage(static_cast<uint32_t>(texture_descriptor_.usage));

    Vector<V8GPUTextureFormat> view_formats;
    for (size_t i = 0; i < texture_descriptor_.viewFormatCount; ++i) {
        view_formats.push_back(FromDawnEnum(view_formats_[i]));
    }
    configuration->setViewFormats(view_formats);

    configuration->setColorSpace(PredefinedColorSpaceToV8(color_space_));
    configuration->setAlphaMode(alpha_mode_);

    GPUCanvasToneMapping* tone_mapping = GPUCanvasToneMapping::Create();
    tone_mapping->setMode(tone_mapping_mode_);
    configuration->setToneMapping(tone_mapping);

    return configuration;
}

GPUTexture* GPUCanvasContext::getCurrentTexture(ScriptState* script_state, ExceptionState& exception_state)
{
    if (!configured_) {
        exception_state.ThrowDOMException(DOMExceptionCode::kInvalidStateError, "context is not configured");
        return nullptr;
    }
    DCHECK(device_);

    // Calling getCurrentTexture returns a texture that is valid until the
    // animation frame it gets presented. If getCurrentTexture is called multiple
    // time, the same texture should be returned. |texture_| is set to
    // null when presented so that we know we should create a new one.
    if (texture_ && !new_texture_required_) {
        return texture_.Get();
    }
    new_texture_required_ = false;

    // Error textures also obey per-frame identity and expiry.
    DidDraw(CanvasPerformanceMonitor::DrawType::kOther);

    if (!swap_buffers_) {
        device_->InjectError(wgpu::ErrorType::Validation, "context configuration is invalid.");
        texture_ = GPUTexture::CreateError(device_.Get(), &texture_descriptor_);
        return texture_.Get();
    }

    SkAlphaType alpha_type = alpha_mode_ == V8GPUCanvasAlphaMode::Enum::kOpaque ? kOpaque_SkAlphaType : kPremul_SkAlphaType;
    wgpu::Texture native_texture = swap_buffers_->GetNewTexture(texture_descriptor_, alpha_type, alpha_clearer_);
    if (!native_texture) {
        // Preserve the configured device's validation behavior on allocation failure.
        if (texture_descriptor_.size.width == 0 || texture_descriptor_.size.height == 0) {
            device_->InjectError(wgpu::ErrorType::Validation, "Could not create a swapchain texture of size 0.");
        } else {
            device_->InjectError(wgpu::ErrorType::Validation, "Could not create the swapchain texture.");
        }
        texture_ = GPUTexture::CreateError(device_, &texture_descriptor_);
        return texture_.Get();
    }

    texture_ = MakeGarbageCollected<GPUTexture>(device_, std::move(native_texture), String::FromUTF8(texture_descriptor_.label));
    // Preserve actual GPU pixels before an application explicitly destroys its
    // current texture. Presentation still expires the JS object at frame end.
    texture_->SetBeforeDestroyCallback(WTF::BindOnce(
        [](scoped_refptr<WebGPUSwapBufferProvider> buffers) { buffers->OnTextureDestroyed(); }, swap_buffers_));

    ExecutionContext* execution_context = ExecutionContext::From(script_state);
    UseCounter::Count(execution_context, WebFeature::kWebGPUCanvasContextGetCurrentTexture);

    return texture_.Get();
}

void GPUCanvasContext::ReplaceDrawingBuffer(bool destroy_swap_buffers)
{
    if (texture_) {
        texture_->ClearBeforeDestroyCallback();
        texture_->destroy();
        texture_ = nullptr;
    }
    if (swap_buffers_) {
        swap_buffers_->DiscardCurrentSwapBuffer();
        if (destroy_swap_buffers) {
            swap_buffers_->Neuter();
            swap_buffers_ = nullptr;
        }
    }
    new_texture_required_ = true;
}

void GPUCanvasContext::FinalizeFrame(FlushReason reason)
{
    // createImageBitmap, convertToBlob, and other readbacks flush/snapshot
    // without transferring the drawing buffer or expiring its current texture.
    if (reason != FlushReason::kCanvasPushFrame && reason != FlushReason::kCanvasPushFrameWhilePrinting)
        return;
    if (swap_buffers_)
        swap_buffers_->FinalizeFrame();
    // Also expire error textures and explicitly destroyed textures, neither of
    // which necessarily has a live native bridge record.
    OnTextureTransferred();
    new_texture_required_ = true;
}

void GPUCanvasContext::OnTextureTransferred()
{
    if (texture_) {
        texture_->ClearBeforeDestroyCallback();
        texture_->destroy();
        texture_ = nullptr;
    }
    new_texture_required_ = true;
}

void GPUCanvasContext::SetNeedsCompositingUpdate()
{
    if (Host()) {
        Host()->SetNeedsCompositingUpdate();
    }
}

} // namespace blink
