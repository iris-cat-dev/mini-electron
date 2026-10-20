// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_GRAPHICS_GPU_WEBGPU_SWAP_BUFFER_PROVIDER_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_GRAPHICS_GPU_WEBGPU_SWAP_BUFFER_PROVIDER_H_

#include "base/memory/raw_ptr.h"
#include "cc/layers/texture_layer.h"
#include "cc/layers/texture_layer_client.h"
#include "components/viz/common/resources/shared_image_format.h"
#include "gpu/command_buffer/client/webgpu_native_texture_interface.h"
#include "third_party/blink/renderer/platform/graphics/gpu/dawn_control_client_holder.h"
#include "third_party/blink/renderer/platform/graphics/gpu/webgpu_texture_alpha_clearer.h"
#include "third_party/blink/renderer/platform/graphics/graphics_types.h"
#include "third_party/blink/renderer/platform/graphics/static_bitmap_image.h"
#include "third_party/blink/renderer/platform/platform_export.h"
#include "third_party/blink/renderer/platform/wtf/ref_counted.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/hdr_metadata.h"

namespace blink {

class CanvasResource;
class CanvasResourceDispatcher;

// Owns genuine Dawn canvas textures and exports their completed pixels through
// the software compositor resource protocol. No Chromium GPU service is needed.
class PLATFORM_EXPORT WebGPUSwapBufferProvider : public cc::TextureLayerClient, public RefCounted<WebGPUSwapBufferProvider> {
public:
    class Client {
    public:
        virtual void OnTextureTransferred() = 0;
        virtual void SetNeedsCompositingUpdate() = 0;
    };

    WebGPUSwapBufferProvider(Client*, scoped_refptr<DawnControlClientHolder>, const wgpu::Device&,
        wgpu::TextureFormat, PredefinedColorSpace, const gfx::HDRMetadata&);
    ~WebGPUSwapBufferProvider() override;

    viz::SharedImageFormat Format() const;
    gfx::Size Size() const;
    cc::Layer* CcLayer();
    void SetFilterQuality(cc::PaintFlags::FilterQuality);
    void Neuter();
    void DiscardCurrentSwapBuffer();
    wgpu::Texture GetNewTexture(const wgpu::TextureDescriptor&, SkAlphaType, scoped_refptr<WebGPUTextureAlphaClearer>);

    // Snapshots own their pixels, never staging-buffer mappings. Reading does
    // not expire the current texture; completing a frame does.
    scoped_refptr<StaticBitmapImage> Snapshot();
    void FinalizeFrame();
    void OnTextureDestroyed();
    scoped_refptr<StaticBitmapImage> TransferToImageBitmap();
    scoped_refptr<CanvasResource> ExportCanvasResource(CanvasResourceDispatcher*);

    bool PrepareTransferableResource(cc::SharedBitmapIdRegistrar*, viz::TransferableResource*, viz::ReleaseCallback*) override;

private:
    gpu::webgpu::NativeTextureInterface* TextureInterface() const;
    void ReleaseTexture();
    scoped_refptr<StaticBitmapImage> SnapshotInternal(bool alpha_already_opaque);

    scoped_refptr<DawnControlClientHolder> dawn_control_client_;
    raw_ptr<Client> client_;
    wgpu::Device device_;
    scoped_refptr<cc::TextureLayer> layer_;
    const wgpu::TextureFormat format_;
    const PredefinedColorSpace color_space_;
    const gfx::HDRMetadata hdr_metadata_;
    cc::PaintFlags::FilterQuality filter_quality_ = cc::PaintFlags::FilterQuality::kLow;
    gfx::Size size_;
    SkAlphaType alpha_type_ = kPremul_SkAlphaType;
    gpu::webgpu::ReservedTexture reservation_ = {};
    wgpu::Texture texture_;
    scoped_refptr<WebGPUTextureAlphaClearer> alpha_clearer_;
    scoped_refptr<StaticBitmapImage> last_image_;
    bool pending_frame_ = false;
    bool neutered_ = false;
};

} // namespace blink

#endif // THIRD_PARTY_BLINK_RENDERER_PLATFORM_GRAPHICS_GPU_WEBGPU_SWAP_BUFFER_PROVIDER_H_
