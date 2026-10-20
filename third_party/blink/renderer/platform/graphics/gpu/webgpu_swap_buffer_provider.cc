// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/graphics/gpu/webgpu_swap_buffer_provider.h"

#include "base/functional/bind.h"
#include "base/location.h"
#include "cc/resources/cross_thread_shared_bitmap.h"
#include "cc/resources/shared_bitmap_id_registrar.h"
#include "components/viz/common/resources/bitmap_allocation.h"
#include "components/viz/common/resources/shared_bitmap.h"
#include "components/viz/common/resources/transferable_resource.h"
#include "third_party/blink/renderer/platform/graphics/canvas_color_params.h"
#include "third_party/blink/renderer/platform/graphics/canvas_resource.h"
#include "third_party/blink/renderer/platform/graphics/canvas_resource_dispatcher.h"
#include "third_party/blink/renderer/platform/graphics/unaccelerated_static_bitmap_image.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImage.h"
#include "ui/gfx/color_space.h"

namespace blink {
namespace {

SkColorType CanvasColorType(wgpu::TextureFormat format)
{
    switch (format) {
    case wgpu::TextureFormat::BGRA8Unorm:
        return kBGRA_8888_SkColorType;
    case wgpu::TextureFormat::RGBA8Unorm:
        return kRGBA_8888_SkColorType;
    case wgpu::TextureFormat::RGBA16Float:
        return kRGBA_F16_SkColorType;
    default:
        NOTREACHED();
    }
}

// Viz's software renderer consumes native N32 pixels, irrespective of the
// RGBA_8888 tag used by the shared-bitmap transport. Convert only at export;
// snapshots retain the canvas's original precision and color space.
scoped_refptr<cc::CrossThreadSharedBitmap> MakeBitmap(StaticBitmapImage* image)
{
    const auto info = image->GetSkImageInfo().makeColorType(kN32_SkColorType);
    const gfx::Size size(info.width(), info.height());
    constexpr auto format = viz::SinglePlaneFormat::kRGBA_8888;
    auto memory = viz::bitmap_allocation::AllocateSharedBitmap(size, format);
    if (!memory.IsValid())
        return nullptr;
    if (!image->PaintImageForCurrentFrame().readPixels(info, memory.mapping.memory(), info.minRowBytes(), 0, 0))
        return nullptr;
    return base::MakeRefCounted<cc::CrossThreadSharedBitmap>(viz::SharedBitmap::GenerateId(),
        std::move(memory.region), std::move(memory.mapping), size, format);
}

// Unlike CanvasResourceProviderBitmap, this is an actual exportable software
// resource. The dispatcher registration and mapping outlive every consumer.
class NativeCanvasResource final : public CanvasResource {
public:
    NativeCanvasResource(scoped_refptr<StaticBitmapImage> image, scoped_refptr<cc::CrossThreadSharedBitmap> bitmap,
        CanvasResourceDispatcher* dispatcher, cc::PaintFlags::FilterQuality filter_quality)
        : CanvasResource(nullptr, filter_quality, image->GetSkColorInfo().makeColorType(kN32_SkColorType))
        , image_(image->PaintImageForCurrentFrame().GetSwSkImage())
        , bitmap_(std::move(bitmap))
        , dispatcher_(dispatcher->GetWeakPtr())
    {
        dispatcher->DidAllocateSharedBitmap(bitmap_->shared_region().Duplicate(), bitmap_->id());
    }

    ~NativeCanvasResource() override
    {
        auto unregister = base::BindOnce([](base::WeakPtr<CanvasResourceDispatcher> dispatcher, viz::SharedBitmapId id) {
            if (dispatcher)
                dispatcher->DidDeleteSharedBitmap(id);
        }, dispatcher_, bitmap_->id());
        if (is_cross_thread())
            owning_thread_task_runner_->PostTask(FROM_HERE, std::move(unregister));
        else
            std::move(unregister).Run();
    }

    bool IsRecycleable() const override { return false; }
    bool SupportsAcceleratedCompositing() const override { return false; }
    bool IsValid() const override { return true; }
    gfx::Size Size() const override { return bitmap_->size(); }
    void NotifyResourceLost() override { }
    bool OriginClean() const override { return origin_clean_; }
    void SetOriginClean(bool clean) override { origin_clean_ = clean; }
    scoped_refptr<StaticBitmapImage> Bitmap() override
    {
        auto image = UnacceleratedStaticBitmapImage::Create(image_);
        image->SetOriginClean(origin_clean_);
        return image;
    }

private:
    bool UsesAcceleratedRaster() const override { return false; }
    const gpu::SyncToken GetSyncTokenWithOptionalVerification(bool) override { return {}; }
    base::WeakPtr<WebGraphicsContext3DProviderWrapper> ContextProviderWrapper() const override { return nullptr; }
    bool PrepareUnacceleratedTransferableResource(viz::TransferableResource* resource) override
    {
        *resource = viz::TransferableResource::MakeSoftwareSharedBitmap(bitmap_->id(), gpu::SyncToken(), Size(),
            viz::SinglePlaneFormat::kRGBA_8888, viz::TransferableResource::ResourceSource::kCanvas);
        resource->color_space = GetColorSpace();
        return true;
    }

    sk_sp<SkImage> image_;
    scoped_refptr<cc::CrossThreadSharedBitmap> bitmap_;
    base::WeakPtr<CanvasResourceDispatcher> dispatcher_;
    bool origin_clean_ = true;
};

} // namespace

WebGPUSwapBufferProvider::WebGPUSwapBufferProvider(Client* client, scoped_refptr<DawnControlClientHolder> dawn_control_client,
    const wgpu::Device& device, wgpu::TextureFormat format, PredefinedColorSpace color_space, const gfx::HDRMetadata& hdr_metadata)
    : dawn_control_client_(std::move(dawn_control_client))
    , client_(client)
    , device_(device)
    , format_(format)
    , color_space_(color_space)
    , hdr_metadata_(hdr_metadata)
{
}

WebGPUSwapBufferProvider::~WebGPUSwapBufferProvider()
{
    Neuter();
}

viz::SharedImageFormat WebGPUSwapBufferProvider::Format() const
{
    switch (format_) {
    case wgpu::TextureFormat::BGRA8Unorm:
        return viz::SinglePlaneFormat::kBGRA_8888;
    case wgpu::TextureFormat::RGBA8Unorm:
        return viz::SinglePlaneFormat::kRGBA_8888;
    case wgpu::TextureFormat::RGBA16Float:
        return viz::SinglePlaneFormat::kRGBA_F16;
    default:
        NOTREACHED();
    }
}

gfx::Size WebGPUSwapBufferProvider::Size() const
{
    return size_;
}

cc::Layer* WebGPUSwapBufferProvider::CcLayer()
{
    return layer_.get();
}

void WebGPUSwapBufferProvider::SetFilterQuality(cc::PaintFlags::FilterQuality filter_quality)
{
    filter_quality_ = filter_quality;
    if (layer_)
        layer_->SetNearestNeighbor(filter_quality == cc::PaintFlags::FilterQuality::kNone);
}

gpu::webgpu::NativeTextureInterface* WebGPUSwapBufferProvider::TextureInterface() const
{
    auto context = dawn_control_client_->GetContextProviderWeakPtr();
    return context ? context->ContextProvider()->WebGPUTextureInterface() : nullptr;
}

void WebGPUSwapBufferProvider::ReleaseTexture()
{
    if (reservation_.texture) {
        if (auto* interface = TextureInterface())
            interface->ReleaseCanvasTexture(reservation_);
    }
    reservation_ = {};
    texture_ = nullptr;
    alpha_clearer_ = nullptr;
}

void WebGPUSwapBufferProvider::DiscardCurrentSwapBuffer()
{
    if (texture_)
        texture_.Destroy();
    ReleaseTexture();
    last_image_ = nullptr;
    pending_frame_ = false;
    if (layer_)
        layer_->ClearTexture();
}

void WebGPUSwapBufferProvider::Neuter()
{
    if (neutered_)
        return;
    client_ = nullptr;
    DiscardCurrentSwapBuffer();
    if (layer_) {
        layer_->ClearClient();
        layer_ = nullptr;
    }
    neutered_ = true;
}

wgpu::Texture WebGPUSwapBufferProvider::GetNewTexture(const wgpu::TextureDescriptor& descriptor, SkAlphaType alpha_type,
    scoped_refptr<WebGPUTextureAlphaClearer> alpha_clearer)
{
    auto* interface = TextureInterface();
    if (neutered_ || !interface || !descriptor.size.width || !descriptor.size.height)
        return nullptr;
    ReleaseTexture();
    reservation_ = interface->CreateCanvasTexture(device_.Get(), static_cast<const WGPUTextureDescriptor&>(descriptor));
    if (!reservation_.texture)
        return nullptr;
    texture_ = wgpu::Texture::Acquire(reservation_.texture);
    size_ = gfx::Size(descriptor.size.width, descriptor.size.height);
    alpha_type_ = alpha_type;
    alpha_clearer_ = std::move(alpha_clearer);
    last_image_ = nullptr;
    pending_frame_ = true;
    if (!layer_) {
        layer_ = cc::TextureLayer::CreateForMailbox(this);
        layer_->SetIsDrawable(true);
        layer_->SetFlipped(false);
        layer_->SetPremultipliedAlpha(true);
        layer_->SetNearestNeighbor(filter_quality_ == cc::PaintFlags::FilterQuality::kNone);
        if (client_)
            client_->SetNeedsCompositingUpdate();
    }
    layer_->SetContentsOpaque(alpha_type == kOpaque_SkAlphaType);
    layer_->SetBlendBackgroundColor(alpha_type != kOpaque_SkAlphaType);
    layer_->SetNeedsDisplay();
    return texture_;
}

scoped_refptr<StaticBitmapImage> WebGPUSwapBufferProvider::Snapshot()
{
    return SnapshotInternal(false);
}

scoped_refptr<StaticBitmapImage> WebGPUSwapBufferProvider::SnapshotInternal(bool alpha_already_opaque)
{
    if (!texture_) {
        // Blink image wrappers carry thread-affinity state. Share only Skia's
        // immutable pixels with a consumer that may transfer its image.
        return last_image_ ? UnacceleratedStaticBitmapImage::Create(last_image_->PaintImageForCurrentFrame().GetSwSkImage()) : nullptr;
    }
    auto* interface = TextureInterface();
    if (!interface)
        return nullptr;
    SkBitmap pixels;
    auto info = SkImageInfo::Make(size_.width(), size_.height(), CanvasColorType(format_), alpha_type_,
        PredefinedColorSpaceToSkColorSpace(color_space_));
    if (!pixels.tryAllocPixels(info))
        return nullptr;
    if (!interface->ReadCanvasTexture(reservation_, pixels.getPixels(), pixels.rowBytes()))
        return nullptr;
    if (alpha_type_ == kOpaque_SkAlphaType && !alpha_already_opaque) {
        // Snapshotting must not change a texture that JS can still observe.
        // Set only alpha in the owned copy, preserving RGB and F16 precision.
        for (int y = 0; y < size_.height(); ++y) {
            if (format_ == wgpu::TextureFormat::RGBA16Float) {
                auto* row = static_cast<uint16_t*>(pixels.getAddr(0, y));
                for (int x = 0; x < size_.width(); ++x)
                    row[4 * x + 3] = 0x3c00; // IEEE 754 binary16 1.0.
            } else {
                auto* row = static_cast<uint8_t*>(pixels.getAddr(0, y));
                for (int x = 0; x < size_.width(); ++x)
                    row[4 * x + 3] = 255;
            }
        }
    }
    pixels.setImmutable();
    return UnacceleratedStaticBitmapImage::Create(SkImages::RasterFromBitmap(pixels));
}

void WebGPUSwapBufferProvider::FinalizeFrame()
{
    if (!texture_)
        return;
    // This is presentation/transfer expiry, not a snapshot request. No script
    // can observe the texture again before OnTextureTransferred destroys it.
    // Keep the native GPU alpha-clear path only for this unobservable frame.
    if (alpha_clearer_)
        alpha_clearer_->ClearAlpha(texture_);
    last_image_ = SnapshotInternal(!!alpha_clearer_);
    if (client_)
        client_->OnTextureTransferred();
    ReleaseTexture();
}

void WebGPUSwapBufferProvider::OnTextureDestroyed()
{
    last_image_ = Snapshot();
    ReleaseTexture();
}

scoped_refptr<StaticBitmapImage> WebGPUSwapBufferProvider::TransferToImageBitmap()
{
    FinalizeFrame();
    auto image = std::move(last_image_);
    pending_frame_ = false;
    if (layer_)
        layer_->ClearTexture();
    return image;
}

scoped_refptr<CanvasResource> WebGPUSwapBufferProvider::ExportCanvasResource(CanvasResourceDispatcher* dispatcher)
{
    if (!dispatcher || !pending_frame_)
        return nullptr;
    FinalizeFrame();
    if (!last_image_)
        return nullptr;
    auto bitmap = MakeBitmap(last_image_.get());
    if (!bitmap)
        return nullptr;
    pending_frame_ = false;
    return base::AdoptRef(new NativeCanvasResource(last_image_, std::move(bitmap), dispatcher, filter_quality_));
}

bool WebGPUSwapBufferProvider::PrepareTransferableResource(cc::SharedBitmapIdRegistrar* registrar,
    viz::TransferableResource* resource, viz::ReleaseCallback* release_callback)
{
    if (!registrar || !pending_frame_)
        return false;
    FinalizeFrame();
    if (!last_image_)
        return false;
    auto bitmap = MakeBitmap(last_image_.get());
    if (!bitmap)
        return false;
    auto registration = registrar->RegisterSharedBitmapId(bitmap->id(), bitmap);
    *resource = viz::TransferableResource::MakeSoftwareSharedBitmap(bitmap->id(), gpu::SyncToken(), size_,
        viz::SinglePlaneFormat::kRGBA_8888, viz::TransferableResource::ResourceSource::kCanvas);
    resource->color_space = PredefinedColorSpaceToGfxColorSpace(color_space_);
    resource->hdr_metadata = hdr_metadata_;
    // Both the registration and mapping must survive until Viz releases this
    // particular export, independently of resize, unconfigure, or context GC.
    *release_callback = base::BindOnce([](scoped_refptr<cc::CrossThreadSharedBitmap>, cc::SharedBitmapIdRegistration,
                                          const gpu::SyncToken&, bool) { }, std::move(bitmap), std::move(registration));
    pending_frame_ = false;
    return true;
}

} // namespace blink
