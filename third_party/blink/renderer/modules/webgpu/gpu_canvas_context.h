// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_WEBGPU_GPU_CANVAS_CONTEXT_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_WEBGPU_GPU_CANVAS_CONTEXT_H_

#include "base/containers/heap_array.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_gpu_canvas_alpha_mode.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_gpu_canvas_tone_mapping_mode.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_typedefs.h"
#include "third_party/blink/renderer/core/html/canvas/canvas_rendering_context.h"
#include "third_party/blink/renderer/core/html/canvas/canvas_rendering_context_factory.h"
#include "third_party/blink/renderer/platform/bindings/script_wrappable.h"
#include "third_party/blink/renderer/platform/graphics/gpu/webgpu_cpp.h"
#include "third_party/blink/renderer/platform/graphics/gpu/webgpu_swap_buffer_provider.h"
#include "third_party/blink/renderer/platform/graphics/graphics_types.h"
#include "third_party/blink/renderer/platform/graphics/static_bitmap_image.h"

namespace blink {

class ExceptionState;
class GPUDevice;
class GPUCanvasConfiguration;
class GPUSwapChain;
class GPUTexture;
class WebGPUTextureAlphaClearer;
class V8UnionHTMLCanvasElementOrOffscreenCanvas;

// Implements WebGPU canvas configuration and frame lifetime. Native Dawn
// textures are presented and snapshotted by WebGPUSwapBufferProvider.
class GPUCanvasContext : public CanvasRenderingContext, public WebGPUSwapBufferProvider::Client {
    DEFINE_WRAPPERTYPEINFO();

public:
    class Factory : public CanvasRenderingContextFactory {
    public:
        Factory() = default;

        Factory(const Factory&) = delete;
        Factory& operator=(const Factory&) = delete;

        ~Factory() override;

        CanvasRenderingContext* Create(CanvasRenderingContextHost*, const CanvasContextCreationAttributesCore&) override;
        CanvasRenderingContext::CanvasRenderingAPI GetRenderingAPI() const override;
    };

    GPUCanvasContext(CanvasRenderingContextHost*, const CanvasContextCreationAttributesCore&);

    GPUCanvasContext(const GPUCanvasContext&) = delete;
    GPUCanvasContext& operator=(const GPUCanvasContext&) = delete;

    ~GPUCanvasContext() override;

    void Trace(Visitor*) const override;

    // CanvasRenderingContext implementation
    V8RenderingContext* AsV8RenderingContext() final;
    V8OffscreenRenderingContext* AsV8OffscreenRenderingContext() final;
    SkColorInfo CanvasRenderingContextSkColorInfo() const override;
    // Snapshots the current texture, or the last completed frame.
    scoped_refptr<StaticBitmapImage> GetImage(FlushReason) final;
    bool PaintRenderingResultsToCanvas(SourceDrawingBuffer) final;
    bool CopyRenderingResultsToVideoFrame(WebGraphicsContext3DVideoFramePool* frame_pool, SourceDrawingBuffer src_buffer,
        const gfx::ColorSpace& dst_color_space, VideoFrameCopyCompletedCallback callback) override;
    void PageVisibilityChanged() override
    {
    }
    bool isContextLost() const override
    {
        return false;
    }
    bool IsComposited() const final
    {
        return true;
    }
    bool IsOriginTopLeft() const final
    {
        return true;
    }
    void SetFilterQuality(cc::PaintFlags::FilterQuality) override;
    bool IsPaintable() const final
    {
        return true;
    }
    int ExternallyAllocatedBufferCountPerPixel() final
    {
        return 1;
    }
    void Stop() final;
    cc::Layer* CcLayer() const final;
    void Reshape(int width, int height) override;

    // OffscreenCanvas-specific methods
    bool PushFrame() final;
    ImageBitmap* TransferToImageBitmap(ScriptState*, ExceptionState&) final;

    bool IsOffscreenCanvas() const
    {
        if (Host())
            return Host()->IsOffscreenCanvas();
        return false;
    }

    // gpu_presentation_context.idl
    V8UnionHTMLCanvasElementOrOffscreenCanvas* getHTMLOrOffscreenCanvas() const;

    void configure(const GPUCanvasConfiguration* descriptor, ExceptionState&);
    void unconfigure();
    GPUCanvasConfiguration* getConfiguration();
    GPUTexture* getCurrentTexture(ScriptState*, ExceptionState&);

    // WebGPUSwapBufferProvider::Client implementation
    void OnTextureTransferred() override;
    void SetNeedsCompositingUpdate() override;

private:
    void ReplaceDrawingBuffer(bool destroy_swap_buffers);

    void FinalizeFrame(FlushReason) override;

    cc::PaintFlags::FilterQuality filter_quality_ = cc::PaintFlags::FilterQuality::kLow;
    Member<GPUDevice> device_;

    Member<GPUTexture> texture_;

    PredefinedColorSpace color_space_ = PredefinedColorSpace::kSRGB;
    V8GPUCanvasAlphaMode::Enum alpha_mode_ = V8GPUCanvasAlphaMode::Enum::kOpaque;
    V8GPUCanvasToneMappingMode::Enum tone_mapping_mode_ = V8GPUCanvasToneMappingMode::Enum::kStandard;
    scoped_refptr<WebGPUTextureAlphaClearer> alpha_clearer_;
    scoped_refptr<WebGPUSwapBufferProvider> swap_buffers_;

    bool new_texture_required_ = true;
    bool stopped_ = false;

    // Matches [[configuration]] != null in the WebGPU specification.
    bool configured_ = false;
    // Matches [[texture_descriptor]] in the WebGPU specification except that it
    // never becomes null.
    wgpu::TextureDescriptor texture_descriptor_;
    wgpu::DawnTextureInternalUsageDescriptor texture_internal_usage_;
    base::HeapArray<wgpu::TextureFormat> view_formats_;
};

} // namespace blink

#endif // THIRD_PARTY_BLINK_RENDERER_MODULES_WEBGPU_GPU_CANVAS_CONTEXT_H_
