// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
// TODO(crbug.com/351564777): Remove this and convert code to safer constructs.
#pragma allow_unsafe_buffers
#endif

#include "third_party/blink/renderer/modules/webgpu/external_texture_helper.h"

#include "media/base/video_frame.h"
#include "media/base/video_transformation.h"
#include "media/base/video_util.h"
#include "media/renderers/paint_canvas_video_renderer.h"
#include "third_party/blink/renderer/core/html/media/html_video_element.h"
#include "third_party/blink/renderer/modules/webcodecs/video_frame.h"
#include "third_party/blink/renderer/modules/webgpu/dawn_conversions.h"
#include "base/numerics/checked_math.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_device.h"
#include "third_party/blink/renderer/platform/graphics/canvas_color_params.h"
#include "third_party/blink/renderer/platform/graphics/gpu/dawn_control_client_holder.h"
#include "third_party/skia/include/effects/SkColorMatrix.h"
#include "third_party/skia/modules/skcms/skcms.h"

namespace blink {
namespace {
wgpu::ExternalTextureRotation FromVideoRotation(media::VideoRotation rotation)
{
    switch (rotation) {
    case media::VIDEO_ROTATION_0:
        return wgpu::ExternalTextureRotation::Rotate0Degrees;
    case media::VIDEO_ROTATION_90:
        return wgpu::ExternalTextureRotation::Rotate90Degrees;
    case media::VIDEO_ROTATION_180:
        return wgpu::ExternalTextureRotation::Rotate180Degrees;
    case media::VIDEO_ROTATION_270:
        return wgpu::ExternalTextureRotation::Rotate270Degrees;
    }
    NOTREACHED();
}

// Keep this in sync with the CPU converter's supported formats. Reject decoder
// surfaces and unsupported formats before the converter's NOTREACHED paths.
bool CanUploadVideoFrame(const media::VideoFrame& frame)
{
    if (!frame.IsMappable() && !frame.HasMappableGpuBuffer())
        return false;
    switch (frame.format()) {
    case media::PIXEL_FORMAT_ARGB:
    case media::PIXEL_FORMAT_XRGB:
    case media::PIXEL_FORMAT_ABGR:
    case media::PIXEL_FORMAT_XBGR:
    case media::PIXEL_FORMAT_YV12:
    case media::PIXEL_FORMAT_I420:
    case media::PIXEL_FORMAT_I422:
    case media::PIXEL_FORMAT_I444:
    case media::PIXEL_FORMAT_I420A:
    case media::PIXEL_FORMAT_I422A:
    case media::PIXEL_FORMAT_I444A:
    case media::PIXEL_FORMAT_YUV420P9:
    case media::PIXEL_FORMAT_YUV422P9:
    case media::PIXEL_FORMAT_YUV444P9:
    case media::PIXEL_FORMAT_YUV420P10:
    case media::PIXEL_FORMAT_YUV422P10:
    case media::PIXEL_FORMAT_YUV444P10:
    case media::PIXEL_FORMAT_YUV420P12:
    case media::PIXEL_FORMAT_YUV422P12:
    case media::PIXEL_FORMAT_YUV444P12:
    case media::PIXEL_FORMAT_YUV420AP10:
    case media::PIXEL_FORMAT_YUV422AP10:
    case media::PIXEL_FORMAT_YUV444AP10:
    case media::PIXEL_FORMAT_NV12:
    case media::PIXEL_FORMAT_P010LE:
    case media::PIXEL_FORMAT_Y16:
        return true;
    default:
        return false;
    }
}
} // namespace

std::array<float, 12> GetYUVToRGBMatrix(gfx::ColorSpace color_space, size_t bit_depth)
{
    // Get the appropriate YUV to RGB conversion matrix.
    SkYUVColorSpace src_sk_color_space;
    color_space.ToSkYUVColorSpace(static_cast<int>(bit_depth), &src_sk_color_space);
    SkColorMatrix sk_color_matrix = SkColorMatrix::YUVtoRGB(src_sk_color_space);
    float yuv_matrix[20];
    sk_color_matrix.getRowMajor(yuv_matrix);
    // Only use columns 1-3 (3x3 conversion matrix) and column 5 (bias values)
    return std::array<float, 12> { yuv_matrix[0], yuv_matrix[1], yuv_matrix[2], yuv_matrix[4], yuv_matrix[5], yuv_matrix[6], yuv_matrix[7], yuv_matrix[9],
        yuv_matrix[10], yuv_matrix[11], yuv_matrix[12], yuv_matrix[14] };
}

ColorSpaceConversionConstants GetColorSpaceConversionConstants(gfx::ColorSpace src_color_space, gfx::ColorSpace dst_color_space)
{
    ColorSpaceConversionConstants color_space_conversion_constants;
    // Get primary matrices for the source and destination color spaces.
    // Multiply the source primary matrix with the inverse destination primary
    // matrix to create a single transformation matrix.
    skcms_Matrix3x3 src_primary_matrix_to_XYZD50;
    skcms_Matrix3x3 dst_primary_matrix_to_XYZD50;
    src_color_space.GetPrimaryMatrix(&src_primary_matrix_to_XYZD50);
    dst_color_space.GetPrimaryMatrix(&dst_primary_matrix_to_XYZD50);

    skcms_Matrix3x3 dst_primary_matrix_from_XYZD50;
    skcms_Matrix3x3_invert(&dst_primary_matrix_to_XYZD50, &dst_primary_matrix_from_XYZD50);

    skcms_Matrix3x3 transform_matrix = skcms_Matrix3x3_concat(&dst_primary_matrix_from_XYZD50, &src_primary_matrix_to_XYZD50);
    // From row major matrix to col major matrix
    color_space_conversion_constants.gamut_conversion_matrix
        = std::array<float, 9> { transform_matrix.vals[0][0], transform_matrix.vals[1][0], transform_matrix.vals[2][0], transform_matrix.vals[0][1],
              transform_matrix.vals[1][1], transform_matrix.vals[2][1], transform_matrix.vals[0][2], transform_matrix.vals[1][2], transform_matrix.vals[2][2] };

    // Set constants for source transfer function.
    skcms_TransferFunction src_transfer_fn;
    src_color_space.GetTransferFunction(&src_transfer_fn);
    color_space_conversion_constants.src_transfer_constants = std::array<float, 7> { src_transfer_fn.g, src_transfer_fn.a, src_transfer_fn.b, src_transfer_fn.c,
        src_transfer_fn.d, src_transfer_fn.e, src_transfer_fn.f };

    // Set constants for destination transfer function.
    skcms_TransferFunction dst_transfer_fn;
    dst_color_space.GetInverseTransferFunction(&dst_transfer_fn);
    color_space_conversion_constants.dst_transfer_constants = std::array<float, 7> { dst_transfer_fn.g, dst_transfer_fn.a, dst_transfer_fn.b, dst_transfer_fn.c,
        dst_transfer_fn.d, dst_transfer_fn.e, dst_transfer_fn.f };

    return color_space_conversion_constants;
}

bool IsSameGamutAndGamma(gfx::ColorSpace src_color_space, gfx::ColorSpace dst_color_space)
{
    if (src_color_space.GetPrimaryID() == dst_color_space.GetPrimaryID()) {
        skcms_TransferFunction src;
        skcms_TransferFunction dst;
        if (src_color_space.GetTransferFunction(&src) && dst_color_space.GetTransferFunction(&dst)) {
            return (src.a == dst.a && src.b == dst.b && src.c == dst.c && src.d == dst.d && src.e == dst.e && src.f == dst.f && src.g == dst.g);
        }
    }
    return false;
}

ExternalTextureSource GetExternalTextureSourceFromVideoElement(HTMLVideoElement* video, ExceptionState& exception_state)
{
    ExternalTextureSource source;

    if (!video) {
        exception_state.ThrowDOMException(DOMExceptionCode::kOperationError, "Missing video source");
        return source;
    }

    if (video->WouldTaintOrigin()) {
        exception_state.ThrowSecurityError("Video element is tainted by cross-origin data and may not be "
                                           "loaded.");
        return source;
    }

    if (auto* wmp = video->GetWebMediaPlayer()) {
        source.media_video_frame = wmp->GetCurrentFrameThenUpdate();
        source.video_renderer = wmp->GetPaintCanvasVideoRenderer();
    }

    if (!source.media_video_frame) {
        exception_state.ThrowDOMException(DOMExceptionCode::kOperationError,
            "Failed to import texture from video "
            "element that doesn't have back "
            "resource.");
        return source;
    }
    if (!CanUploadVideoFrame(*source.media_video_frame)) {
        exception_state.ThrowDOMException(DOMExceptionCode::kOperationError,
            "Video frame is not CPU-readable or has an unsupported pixel format.");
        return source;
    }

    source.media_video_frame_unique_id = source.media_video_frame->unique_id();
    source.valid = true;

    return source;
}

ExternalTextureSource GetExternalTextureSourceFromVideoFrame(VideoFrame* frame, ExceptionState& exception_state)
{
    ExternalTextureSource source;

    if (!frame) {
        exception_state.ThrowDOMException(DOMExceptionCode::kOperationError, "Missing video frame");
        return source;
    }
    // Tainted blink::VideoFrames are not supposed to be possible.
    DCHECK(!frame->WouldTaintOrigin());

    source.media_video_frame = frame->frame();
    if (!source.media_video_frame) {
        exception_state.ThrowDOMException(DOMExceptionCode::kOperationError,
            "Failed to import texture from video "
            "frame that doesn't have back resource");
        return source;
    }

    if (!source.media_video_frame->coded_size().width() || !source.media_video_frame->coded_size().height()) {
        exception_state.ThrowDOMException(DOMExceptionCode::kOperationError, "Cannot import from zero sized video frame");
        return source;
    }
    if (!CanUploadVideoFrame(*source.media_video_frame)) {
        exception_state.ThrowDOMException(DOMExceptionCode::kOperationError,
            "Video frame is not CPU-readable or has an unsupported pixel format.");
        return source;
    }

    source.valid = true;

    return source;
}

ExternalTexture CreateExternalTexture(GPUDevice* device, PredefinedColorSpace dst_predefined_color_space, scoped_refptr<media::VideoFrame> media_video_frame,
    media::PaintCanvasVideoRenderer* video_renderer)
{
    DCHECK(media_video_frame);
    if (!media_video_frame->IsMappable() && media_video_frame->HasMappableGpuBuffer()) {
        media_video_frame = media::ConvertToMemoryMappedFrame(std::move(media_video_frame));
        if (!media_video_frame)
            return {};
    }
    // CPU upload owns its Dawn texture, not the decoder frame. In particular we
    // must not try a SharedImage import on a renderer-local native device.
    if (!CanUploadVideoFrame(*media_video_frame) || device->GetDawnControlClient()->IsContextLost())
        return {};

    const gfx::Rect visible_rect = media_video_frame->visible_rect();
    const gfx::Size natural_size = media_video_frame->natural_size();
    if (visible_rect.IsEmpty() || natural_size.IsEmpty())
        return {};
    // Normalize the converter's fallback and the shader constants together.
    if (!media_video_frame->ColorSpace().IsValid()) {
        media_video_frame = media::VideoFrame::WrapVideoFrame(
            media_video_frame, media_video_frame->format(), visible_rect, natural_size);
        if (!media_video_frame)
            return {};
        media_video_frame->set_color_space(gfx::ColorSpace::CreateREC709());
    }

    // The converter reads visible_data(), so the uploaded texture is already
    // cropped. Dawn applies the display scaling, rotation and mirroring.
    const uint32_t width = visible_rect.width();
    const uint32_t height = visible_rect.height();
    base::CheckedNumeric<uint32_t> row_bytes = width;
    row_bytes *= 4;
    row_bytes += 255;
    if (!row_bytes.IsValid())
        return {};
    const uint32_t padded_row_bytes = row_bytes.ValueOrDie();
    const uint32_t stride = padded_row_bytes & ~255u;
    base::CheckedNumeric<size_t> byte_size = stride;
    byte_size *= height;
    if (!byte_size.IsValid())
        return {};

    wgpu::BufferDescriptor buffer_desc = {
        .usage = wgpu::BufferUsage::CopySrc,
        .size = byte_size.ValueOrDie(),
        .mappedAtCreation = true,
    };
    wgpu::Buffer buffer = device->GetHandle().CreateBuffer(&buffer_desc);
    if (!buffer)
        return {};
    void* pixels = buffer.GetMappedRange(0, byte_size.ValueOrDie());
    if (!pixels)
        return {};
    media::PaintCanvasVideoRenderer::ConvertVideoFrameToRGBPixels(
        media_video_frame.get(), pixels, stride, false, media::PaintCanvasVideoRenderer::kFilterBilinear);
    buffer.Unmap();

    const bool rgba = media_video_frame->format() == media::PIXEL_FORMAT_Y16
        || media::PaintCanvasVideoRenderer::GetRGBPixelsOutputFormat() == viz::SinglePlaneFormat::kRGBA_8888;
    wgpu::TextureDescriptor texture_desc = {
        .usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::TextureBinding,
        .size = { width, height, 1 },
        .format = rgba ? wgpu::TextureFormat::RGBA8Unorm : wgpu::TextureFormat::BGRA8Unorm,
    };
    wgpu::Texture texture = device->GetHandle().CreateTexture(&texture_desc);
    wgpu::ImageCopyBuffer source = {
        .layout = { .bytesPerRow = stride, .rowsPerImage = height },
        .buffer = buffer,
    };
    wgpu::ImageCopyTexture destination = { .texture = texture };
    const wgpu::Extent3D copy_size = { width, height, 1 };
    wgpu::CommandEncoder encoder = device->GetHandle().CreateCommandEncoder();
    encoder.CopyBufferToTexture(&source, &destination, &copy_size);
    wgpu::CommandBuffer commands = encoder.Finish();
    device->GetHandle().GetQueue().Submit(1, &commands);

    gfx::ColorSpace src_color_space = media_video_frame->ColorSpace();
    // The CPU converter removes the YUV matrix/range, but not the primaries or
    // transfer function. Let Dawn perform that remaining RGB conversion.
    src_color_space = src_color_space.GetAsFullRangeRGB();
    const gfx::ColorSpace dst_color_space = PredefinedColorSpaceToGfxColorSpace(dst_predefined_color_space);
    ColorSpaceConversionConstants constants = GetColorSpaceConversionConstants(src_color_space, dst_color_space);
    wgpu::ExternalTextureDescriptor desc = {
        .plane0 = texture.CreateView(),
        .doYuvToRgbConversionOnly = IsSameGamutAndGamma(src_color_space, dst_color_space),
        .srcTransferFunctionParameters = constants.src_transfer_constants.data(),
        .dstTransferFunctionParameters = constants.dst_transfer_constants.data(),
        .gamutConversionMatrix = constants.gamut_conversion_matrix.data(),
    };
    desc.visibleSize = { width, height };
    desc.cropSize = { width, height };
    desc.apparentSize = { static_cast<uint32_t>(natural_size.width()), static_cast<uint32_t>(natural_size.height()) };
    if (const auto& transform = media_video_frame->metadata().transformation) {
        desc.rotation = FromVideoRotation(transform->rotation);
        desc.mirrored = transform->mirrored;
    }
    ExternalTexture result;
    result.texture = std::move(texture);
    result.wgpu_external_texture = device->GetHandle().CreateExternalTexture(&desc);
    device->GetDawnControlClient()->Flush();
    return result;
}

} // namespace blink
