// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif

#include "media/renderers/paint_canvas_video_renderer.h"

#include <GLES3/gl3.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <numeric>

#include "base/barrier_closure.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/numerics/checked_math.h"
#include "base/synchronization/waitable_event.h"
#include "base/system/sys_info.h"
#include "base/task/thread_pool.h"
#include "base/threading/thread_restrictions.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "media/base/data_buffer.h"
#include "media/base/video_util.h"
#include "third_party/libyuv/include/libyuv.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkImageInfo.h"

// Skia internal format depends on a platform. On Android it is ABGR, on others
// it's ARGB. YUV_ORDER() conditionally remap YUV to YVU for ABGR.
#if SK_B32_SHIFT == 0 && SK_G32_SHIFT == 8 && SK_R32_SHIFT == 16 && SK_A32_SHIFT == 24
#define OUTPUT_ARGB 1
#define LIBYUV_ABGR_TO_ARGB libyuv::ABGRToARGB
#define YUV_ORDER(y, y_stride, u, u_stride, v, v_stride) (y), (y_stride), (u), (u_stride), (v), (v_stride)
#define GBR_TO_RGB_ORDER(y, y_stride, u, u_stride, v, v_stride) (v), (v_stride), (y), (y_stride), (u), (u_stride)
#define LIBYUV_NV12_TO_ARGB_MATRIX libyuv::NV12ToARGBMatrix
#define SHARED_IMAGE_FORMAT viz::SinglePlaneFormat::kBGRA_8888
#elif SK_R32_SHIFT == 0 && SK_G32_SHIFT == 8 && SK_B32_SHIFT == 16 && SK_A32_SHIFT == 24
#define OUTPUT_ARGB 0
#define LIBYUV_ABGR_TO_ARGB libyuv::ARGBToABGR
#define YUV_ORDER(y, y_stride, u, u_stride, v, v_stride) (y), (y_stride), (v), (v_stride), (u), (u_stride)
#define GBR_TO_RGB_ORDER(y, y_stride, u, u_stride, v, v_stride) (u), (u_stride), (y), (y_stride), (v), (v_stride)
#define LIBYUV_NV12_TO_ARGB_MATRIX libyuv::NV21ToARGBMatrix
#define SHARED_IMAGE_FORMAT viz::SinglePlaneFormat::kRGBA_8888
#else
#error Unexpected Skia ARGB_8888 layout!
#endif

namespace media {
namespace {

libyuv::FilterMode ToLibyuvFilterMode(PaintCanvasVideoRenderer::FilterMode filter)
{
    switch (filter) {
    case PaintCanvasVideoRenderer::kFilterNone:
        return libyuv::kFilterNone;
    case PaintCanvasVideoRenderer::kFilterBilinear:
        return libyuv::kFilterBilinear;
    }
}

size_t NumConvertVideoFrameToRGBPixelsTasks(const VideoFrame* video_frame)
{
    constexpr size_t kTaskBytes = 1024 * 1024; // 1 MiB
    const size_t frame_size = VideoFrame::AllocationSize(video_frame->format(), video_frame->visible_rect().size());
    const size_t n_tasks = std::max<size_t>(1, frame_size / kTaskBytes);
    return std::min<size_t>(n_tasks, base::SysInfo::NumberOfProcessors());
}

void ConvertVideoFrameToRGBPixelsTask(const VideoFrame* video_frame, void* rgb_pixels, size_t row_bytes, bool premultiply_alpha, libyuv::FilterMode filter,
    size_t task_index, size_t n_tasks, base::RepeatingClosure* done)
{
    const VideoPixelFormat format = video_frame->format();
    const int width = video_frame->visible_rect().width();
    const int height = video_frame->visible_rect().height();

    size_t rows_per_chunk = 1;
    for (size_t plane = 0; plane < VideoFrame::kMaxPlanes; ++plane) {
        if (VideoFrame::IsValidPlane(format, plane)) {
            rows_per_chunk = std::lcm(rows_per_chunk, VideoFrame::SampleSize(format, plane).height());
        }
    }

    base::CheckedNumeric<size_t> chunks = height / rows_per_chunk;
    const size_t chunk_start = (chunks * task_index / n_tasks).ValueOrDie();
    const size_t chunk_end = (chunks * (task_index + 1) / n_tasks).ValueOrDie();

    // Indivisible heights must process any remaining rows in the last task.
    size_t rows = (chunk_end - chunk_start) * rows_per_chunk;
    if (task_index + 1 == n_tasks)
        rows += height % rows_per_chunk;

    struct PlaneMetaData {
        int stride;
        raw_ptr<const uint8_t> data;
    };
    std::array<PlaneMetaData, VideoFrame::kMaxPlanes> plane_meta;

    for (size_t plane = 0; plane < VideoFrame::kMaxPlanes; ++plane) {
        if (VideoFrame::IsValidPlane(format, plane)) {
            plane_meta[plane] = { // Note: Unlike |data|, stride does not need to be adjusted by the
                // visible rect and sample size. Adding the full frame stride to a
                // pixel on row N and column M will wrap to column M on row N + 1.
                .stride = video_frame->stride(plane),

                .data = video_frame->visible_data(plane)
                    + video_frame->stride(plane) * (chunk_start * rows_per_chunk) / VideoFrame::SampleSize(format, plane).height()
            };
        } else {
            plane_meta[plane] = { .stride = 0, .data = nullptr };
        }
    }

    uint8_t* pixels = static_cast<uint8_t*>(rgb_pixels) + row_bytes * chunk_start * rows_per_chunk;

    if (format == PIXEL_FORMAT_ARGB || format == PIXEL_FORMAT_XRGB || format == PIXEL_FORMAT_ABGR || format == PIXEL_FORMAT_XBGR) {
        DCHECK_LE(width, static_cast<int>(row_bytes));
        const uint8_t* data = plane_meta[VideoFrame::Plane::kARGB].data;

        // Handle order swapping depending on the source and destination formats.
        if ((OUTPUT_ARGB && (format == PIXEL_FORMAT_ARGB || format == PIXEL_FORMAT_XRGB))
            || (!OUTPUT_ARGB && (format == PIXEL_FORMAT_ABGR || format == PIXEL_FORMAT_XBGR))) {
            uint8_t* dest = pixels;
            for (size_t i = 0; i < rows; i++) {
                memcpy(dest, data, width * 4);
                dest += row_bytes;
                data += plane_meta[VideoFrame::Plane::kARGB].stride;
            }
        } else {
            LIBYUV_ABGR_TO_ARGB(plane_meta[VideoFrame::Plane::kARGB].data, plane_meta[VideoFrame::Plane::kARGB].stride, pixels, row_bytes, width, rows);
        }
        if (format == PIXEL_FORMAT_XRGB || format == PIXEL_FORMAT_XBGR) {
            for (size_t y = 0; y < rows; ++y) {
                uint8_t* alpha = pixels + y * row_bytes + 3;
                for (int x = 0; x < width; ++x)
                    alpha[x * 4] = 255;
            }
        }

        // Handle `premultiply_alpha` if the source format has alpha. This could
        // be more efficient if combined with order swapping (in the case that no
        // swap is performed).
        if (premultiply_alpha && (format == PIXEL_FORMAT_ARGB || format == PIXEL_FORMAT_ABGR)) {
            libyuv::ARGBAttenuate(pixels, row_bytes, pixels, row_bytes, width, rows);
        }

        done->Run();
        return;
    }

    // TODO(crbug.com/41380578): This should default to BT.709 color space.
    auto yuv_cs = kRec601_SkYUVColorSpace;
    video_frame->ColorSpace().ToSkYUVColorSpace(video_frame->BitDepth(), &yuv_cs);
    const libyuv::YuvConstants* matrix = GetYuvContantsForColorSpace(yuv_cs, OUTPUT_ARGB);

    if (!video_frame->data(VideoFrame::Plane::kU) && !video_frame->data(VideoFrame::Plane::kV)) {
        DCHECK_EQ(format, PIXEL_FORMAT_I420);
        // For monochrome content ARGB and ABGR are interchangeable.
        libyuv::I400ToARGBMatrix(plane_meta[VideoFrame::Plane::kY].data, plane_meta[VideoFrame::Plane::kY].stride, pixels, row_bytes, matrix, width, rows);
        done->Run();
        return;
    }

    auto convert_yuv = [&](const libyuv::YuvConstants* matrix, auto&& func) {
        func(YUV_ORDER(plane_meta[VideoFrame::Plane::kY].data, plane_meta[VideoFrame::Plane::kY].stride, plane_meta[VideoFrame::Plane::kU].data,
                 plane_meta[VideoFrame::Plane::kU].stride, plane_meta[VideoFrame::Plane::kV].data, plane_meta[VideoFrame::Plane::kV].stride),
            pixels, row_bytes, matrix, width, rows);
    };

    auto convert_yuv_with_filter = [&](const libyuv::YuvConstants* matrix, auto&& func) {
        func(YUV_ORDER(plane_meta[VideoFrame::Plane::kY].data, plane_meta[VideoFrame::Plane::kY].stride, plane_meta[VideoFrame::Plane::kU].data,
                 plane_meta[VideoFrame::Plane::kU].stride, plane_meta[VideoFrame::Plane::kV].data, plane_meta[VideoFrame::Plane::kV].stride),
            pixels, row_bytes, matrix, width, rows, filter);
    };

    auto convert_yuva = [&](const libyuv::YuvConstants* matrix, auto&& func) {
        func(YUV_ORDER(plane_meta[VideoFrame::Plane::kY].data, plane_meta[VideoFrame::Plane::kY].stride, plane_meta[VideoFrame::Plane::kU].data,
                 plane_meta[VideoFrame::Plane::kU].stride, plane_meta[VideoFrame::Plane::kV].data, plane_meta[VideoFrame::Plane::kV].stride),
            plane_meta[VideoFrame::Plane::kA].data, plane_meta[VideoFrame::Plane::kA].stride, pixels, row_bytes, matrix, width, rows, premultiply_alpha);
    };

    auto convert_yuva_with_filter = [&](const libyuv::YuvConstants* matrix, auto&& func) {
        func(YUV_ORDER(plane_meta[VideoFrame::Plane::kY].data, plane_meta[VideoFrame::Plane::kY].stride, plane_meta[VideoFrame::Plane::kU].data,
                 plane_meta[VideoFrame::Plane::kU].stride, plane_meta[VideoFrame::Plane::kV].data, plane_meta[VideoFrame::Plane::kV].stride),
            plane_meta[VideoFrame::Plane::kA].data, plane_meta[VideoFrame::Plane::kA].stride, pixels, row_bytes, matrix, width, rows, premultiply_alpha,
            filter);
    };

    auto convert_yuv16 = [&](const libyuv::YuvConstants* matrix, auto&& func) {
        func(YUV_ORDER(reinterpret_cast<const uint16_t*>(plane_meta[VideoFrame::Plane::kY].data.get()), plane_meta[VideoFrame::Plane::kY].stride / 2,
                 reinterpret_cast<const uint16_t*>(plane_meta[VideoFrame::Plane::kU].data.get()), plane_meta[VideoFrame::Plane::kU].stride / 2,
                 reinterpret_cast<const uint16_t*>(plane_meta[VideoFrame::Plane::kV].data.get()), plane_meta[VideoFrame::Plane::kV].stride / 2),
            pixels, row_bytes, matrix, width, rows);
    };

    auto convert_yuv16_with_filter = [&](const libyuv::YuvConstants* matrix, auto&& func) {
        func(YUV_ORDER(reinterpret_cast<const uint16_t*>(plane_meta[VideoFrame::Plane::kY].data.get()), plane_meta[VideoFrame::Plane::kY].stride / 2,
                 reinterpret_cast<const uint16_t*>(plane_meta[VideoFrame::Plane::kU].data.get()), plane_meta[VideoFrame::Plane::kU].stride / 2,
                 reinterpret_cast<const uint16_t*>(plane_meta[VideoFrame::Plane::kV].data.get()), plane_meta[VideoFrame::Plane::kV].stride / 2),
            pixels, row_bytes, matrix, width, rows, filter);
    };

    auto convert_yuva16 = [&](const libyuv::YuvConstants* matrix, auto&& func) {
        func(YUV_ORDER(reinterpret_cast<const uint16_t*>(plane_meta[VideoFrame::Plane::kY].data.get()), plane_meta[VideoFrame::Plane::kY].stride / 2,
                 reinterpret_cast<const uint16_t*>(plane_meta[VideoFrame::Plane::kU].data.get()), plane_meta[VideoFrame::Plane::kU].stride / 2,
                 reinterpret_cast<const uint16_t*>(plane_meta[VideoFrame::Plane::kV].data.get()), plane_meta[VideoFrame::Plane::kV].stride / 2),
            reinterpret_cast<const uint16_t*>(plane_meta[VideoFrame::Plane::kA].data.get()), plane_meta[VideoFrame::Plane::kA].stride / 2, pixels, row_bytes,
            matrix, width, rows, premultiply_alpha);
    };

    auto convert_yuva16_with_filter = [&](const libyuv::YuvConstants* matrix, auto&& func) {
        func(YUV_ORDER(reinterpret_cast<const uint16_t*>(plane_meta[VideoFrame::Plane::kY].data.get()), plane_meta[VideoFrame::Plane::kY].stride / 2,
                 reinterpret_cast<const uint16_t*>(plane_meta[VideoFrame::Plane::kU].data.get()), plane_meta[VideoFrame::Plane::kU].stride / 2,
                 reinterpret_cast<const uint16_t*>(plane_meta[VideoFrame::Plane::kV].data.get()), plane_meta[VideoFrame::Plane::kV].stride / 2),
            reinterpret_cast<const uint16_t*>(plane_meta[VideoFrame::Plane::kA].data.get()), plane_meta[VideoFrame::Plane::kA].stride / 2, pixels, row_bytes,
            matrix, width, rows, premultiply_alpha, filter);
    };

    switch (format) {
    case PIXEL_FORMAT_YV12:
    case PIXEL_FORMAT_I420:
        convert_yuv_with_filter(matrix, libyuv::I420ToARGBMatrixFilter);
        break;
    case PIXEL_FORMAT_I422:
        convert_yuv_with_filter(matrix, libyuv::I422ToARGBMatrixFilter);
        break;
    case PIXEL_FORMAT_I444:
        // Special case for 4:4:4 RGB encoded videos.
        if (video_frame->ColorSpace().GetMatrixID() == gfx::ColorSpace::MatrixID::GBR) {
            libyuv::MergeARGBPlane(
                GBR_TO_RGB_ORDER(plane_meta[VideoFrame::Plane::kY].data, plane_meta[VideoFrame::Plane::kY].stride, plane_meta[VideoFrame::Plane::kU].data,
                    plane_meta[VideoFrame::Plane::kU].stride, plane_meta[VideoFrame::Plane::kV].data, plane_meta[VideoFrame::Plane::kV].stride),
                plane_meta[VideoFrame::Plane::kA].data, plane_meta[VideoFrame::Plane::kA].stride, pixels, row_bytes, width, rows);
        } else {
            convert_yuv(matrix, libyuv::I444ToARGBMatrix);
        }
        break;
    case PIXEL_FORMAT_YUV420P10:
        convert_yuv16_with_filter(matrix, libyuv::I010ToARGBMatrixFilter);
        break;
    case PIXEL_FORMAT_YUV422P10:
        convert_yuv16_with_filter(matrix, libyuv::I210ToARGBMatrixFilter);
        break;
    case PIXEL_FORMAT_YUV444P10:
        convert_yuv16(matrix, libyuv::I410ToARGBMatrix);
        break;
    case PIXEL_FORMAT_YUV420P12:
        convert_yuv16(matrix, libyuv::I012ToARGBMatrix);
        break;
    case PIXEL_FORMAT_I420A:
        convert_yuva_with_filter(matrix, libyuv::I420AlphaToARGBMatrixFilter);
        break;
    case PIXEL_FORMAT_I422A:
        convert_yuva_with_filter(matrix, libyuv::I422AlphaToARGBMatrixFilter);
        break;
    case PIXEL_FORMAT_I444A:
        convert_yuva(matrix, libyuv::I444AlphaToARGBMatrix);
        break;
    case PIXEL_FORMAT_YUV420AP10:
        convert_yuva16_with_filter(matrix, libyuv::I010AlphaToARGBMatrixFilter);
        break;
    case PIXEL_FORMAT_YUV422AP10:
        convert_yuva16_with_filter(matrix, libyuv::I210AlphaToARGBMatrixFilter);
        break;
    case PIXEL_FORMAT_YUV444AP10:
        convert_yuva16(matrix, libyuv::I410AlphaToARGBMatrix);
        break;

    case PIXEL_FORMAT_NV12:
        LIBYUV_NV12_TO_ARGB_MATRIX(plane_meta[VideoFrame::Plane::kY].data, plane_meta[VideoFrame::Plane::kY].stride, plane_meta[VideoFrame::Plane::kUV].data,
            plane_meta[VideoFrame::Plane::kUV].stride, pixels, row_bytes, matrix, width, rows);
        break;
    case PIXEL_FORMAT_P010LE:
        libyuv::P010ToARGBMatrix(reinterpret_cast<const uint16_t*>(plane_meta[VideoFrame::Plane::kY].data.get()), plane_meta[VideoFrame::Plane::kY].stride / 2,
            reinterpret_cast<const uint16_t*>(plane_meta[VideoFrame::Plane::kUV].data.get()), plane_meta[VideoFrame::Plane::kUV].stride / 2, pixels, row_bytes,
            GetYuvContantsForColorSpace(yuv_cs, /*output_argb_matrix=*/true), width, rows);
        if (!OUTPUT_ARGB)
            libyuv::ARGBToABGR(pixels, row_bytes, pixels, row_bytes, width, rows);
        break;

    case PIXEL_FORMAT_YUV420P9:
    case PIXEL_FORMAT_YUV422P9:
    case PIXEL_FORMAT_YUV444P9:
    case PIXEL_FORMAT_YUV422P12:
    case PIXEL_FORMAT_YUV444P12:
    case PIXEL_FORMAT_Y16:
        NOTREACHED() << "These cases should be handled in ConvertVideoFrameToRGBPixels";

    case PIXEL_FORMAT_UYVY:
    case PIXEL_FORMAT_NV21:
    case PIXEL_FORMAT_NV16:
    case PIXEL_FORMAT_NV24:
    case PIXEL_FORMAT_NV12A:
    case PIXEL_FORMAT_P210LE:
    case PIXEL_FORMAT_P410LE:
    case PIXEL_FORMAT_YUY2:
    case PIXEL_FORMAT_ARGB:
    case PIXEL_FORMAT_BGRA:
    case PIXEL_FORMAT_XRGB:
    case PIXEL_FORMAT_RGB24:
    case PIXEL_FORMAT_MJPEG:
    case PIXEL_FORMAT_ABGR:
    case PIXEL_FORMAT_XBGR:
    case PIXEL_FORMAT_XR30:
    case PIXEL_FORMAT_XB30:
    case PIXEL_FORMAT_RGBAF16:
    case PIXEL_FORMAT_UNKNOWN:
        NOTREACHED() << "Only YUV formats and Y16 are supported, got: " << media::VideoPixelFormatToString(format);
    }
    done->Run();
}

// libyuv doesn't support all 9-, 10- nor 12-bit pixel formats yet. This
// function creates a regular 8-bit video frame which we can give to libyuv.
scoped_refptr<VideoFrame> DownShiftHighbitVideoFrame(const VideoFrame* video_frame)
{
    VideoPixelFormat format;
    switch (video_frame->format()) {
    case PIXEL_FORMAT_YUV420P12:
    case PIXEL_FORMAT_YUV420P10:
    case PIXEL_FORMAT_YUV420P9:
        format = PIXEL_FORMAT_I420;
        break;

    case PIXEL_FORMAT_YUV422P12:
    case PIXEL_FORMAT_YUV422P10:
    case PIXEL_FORMAT_YUV422P9:
        format = PIXEL_FORMAT_I422;
        break;

    case PIXEL_FORMAT_YUV444P12:
    case PIXEL_FORMAT_YUV444P10:
    case PIXEL_FORMAT_YUV444P9:
        format = PIXEL_FORMAT_I444;
        break;

    default:
        NOTREACHED();
    }
    const int scale = 1 << (24 - video_frame->BitDepth());
    scoped_refptr<VideoFrame> ret
        = VideoFrame::CreateFrame(format, video_frame->coded_size(), video_frame->visible_rect(), video_frame->natural_size(), video_frame->timestamp());

    ret->set_color_space(video_frame->ColorSpace());
    // Copy all metadata.
    // (May be enough to copy color space)
    ret->metadata().MergeMetadataFrom(video_frame->metadata());

    for (int plane = VideoFrame::Plane::kY; plane <= VideoFrame::Plane::kV; ++plane) {
        int width = VideoFrame::Columns(plane, video_frame->format(), video_frame->visible_rect().width());
        int height = VideoFrame::Rows(plane, video_frame->format(), video_frame->visible_rect().height());
        const uint16_t* src = reinterpret_cast<const uint16_t*>(video_frame->visible_data(plane));
        uint8_t* dst = ret->GetWritableVisibleData(plane);
        if (!src) {
            // An AV1 monochrome (grayscale) frame has no U and V planes. Set all U
            // and V samples to the neutral value (128).
            DCHECK_NE(plane, VideoFrame::Plane::kY);
            memset(dst, 128, height * ret->stride(plane));
            continue;
        }
        libyuv::Convert16To8Plane(src, video_frame->stride(plane) / 2, dst, ret->stride(plane), scale, width, height);
    }
    return ret;
}

// Converts 16-bit data to |out| buffer of specified GL |type|.
// When the |format| is RGBA, the converted value is fed as luminance.
void FlipAndConvertY16(const VideoFrame* video_frame, uint8_t* out, unsigned format, unsigned type, bool flip_y, size_t output_row_bytes)
{
    const uint8_t* row_head = video_frame->visible_data(0);
    const int stride = video_frame->stride(0);
    const int height = video_frame->visible_rect().height();
    for (int i = 0; i < height; ++i, row_head += stride) {
        uint8_t* out_row_head = flip_y ? out + output_row_bytes * (height - i - 1) : out + output_row_bytes * i;
        const uint16_t* row = reinterpret_cast<const uint16_t*>(row_head);
        const uint16_t* row_end = row + video_frame->visible_rect().width();
        if (type == GL_FLOAT) {
            float* out_row = reinterpret_cast<float*>(out_row_head);
            if (format == GL_RGBA) {
                while (row < row_end) {
                    float gray_value = *row++ / 65535.f;
                    *out_row++ = gray_value;
                    *out_row++ = gray_value;
                    *out_row++ = gray_value;
                    *out_row++ = 1.0f;
                }
                continue;
            } else if (format == GL_RED) {
                while (row < row_end)
                    *out_row++ = *row++ / 65535.f;
                continue;
            }
            // For other formats, hit NOTREACHED below.
        } else if (type == GL_UNSIGNED_BYTE) {
            // We take the upper 8 bits of 16-bit data and convert it as luminance to
            // ARGB.  We loose the precision here, but it is important not to render
            // Y16 as RG_88.  To get the full precision use float textures with WebGL1
            // and e.g. R16UI or R32F textures with WebGL2.
            DCHECK_EQ(static_cast<unsigned>(GL_RGBA), format);
            uint32_t* rgba = reinterpret_cast<uint32_t*>(out_row_head);
            while (row < row_end) {
                uint32_t gray_value = *row++ >> 8;
                *rgba++ = SkColorSetRGB(gray_value, gray_value, gray_value);
            }
            continue;
        }
        NOTREACHED() << "Unsupported Y16 conversion for format: 0x" << std::hex << format << " and type: 0x" << std::hex << type;
    }
}

// Common functionality of PaintCanvasVideoRenderer's TexImage2D and
// TexSubImage2D. Allocates a buffer required for conversion and converts
// |frame| content to desired |format|. Returns true if calling glTex(Sub)Image
// is supported for provided |frame| format and parameters.
bool TexImageHelper(VideoFrame* frame, unsigned format, unsigned type, bool flip_y, scoped_refptr<DataBuffer>* temp_buffer)
{
    if (!frame->IsMappable())
        return false;
    unsigned output_bytes_per_pixel = 0;
    switch (frame->format()) {
    case PIXEL_FORMAT_Y16:
        // Converting single component unsigned short here to FLOAT luminance.
        switch (format) {
        case GL_RGBA:
            if (type == GL_FLOAT) {
                output_bytes_per_pixel = 4 * sizeof(GLfloat);
                break;
            }
            return false;
        case GL_RED:
            if (type == GL_FLOAT) {
                output_bytes_per_pixel = sizeof(GLfloat);
                break;
            }
            return false;
        default:
            return false;
        }
        break;
    default:
        return false;
    }

    const size_t output_row_bytes = static_cast<size_t>(frame->visible_rect().width()) * output_bytes_per_pixel;
    const base::CheckedNumeric<size_t> output_size = base::CheckedNumeric<size_t>(output_row_bytes) * frame->visible_rect().height();
    if (!output_size.IsValid() || output_size.ValueOrDie() > static_cast<size_t>(std::numeric_limits<int>::max()))
        return false;
    *temp_buffer = new DataBuffer(output_size.ValueOrDie<int>());
    FlipAndConvertY16(frame, (*temp_buffer)->writable_data(), format, type, flip_y, output_row_bytes);
    return true;
}

} // namespace

// static
void PaintCanvasVideoRenderer::ConvertVideoFrameToRGBPixels(
    const VideoFrame* video_frame, void* rgb_pixels, size_t row_bytes, bool premultiply_alpha, FilterMode filter, bool disable_threading)
{
    if (!video_frame->IsMappable()) {
        NOTREACHED() << "Cannot extract pixels from non-CPU frame formats.";
    }

    scoped_refptr<VideoFrame> temporary_frame;
    // TODO(thomasanderson): Parallelize converting these formats.
    switch (video_frame->format()) {
    case PIXEL_FORMAT_YUV420P9:
    case PIXEL_FORMAT_YUV422P9:
    case PIXEL_FORMAT_YUV444P9:
    case PIXEL_FORMAT_YUV422P12:
    case PIXEL_FORMAT_YUV444P12:
        temporary_frame = DownShiftHighbitVideoFrame(video_frame);
        video_frame = temporary_frame.get();
        break;
    case PIXEL_FORMAT_YUV420P10:
    case PIXEL_FORMAT_YUV420P12:
        // In AV1, a monochrome (grayscale) frame is represented as a YUV 4:2:0
        // frame with no U and V planes. Since there are no 10-bit and 12-bit
        // versions of libyuv::I400ToARGBMatrix(), convert the frame to an 8-bit
        // YUV 4:2:0 frame with U and V planes.
        if (!video_frame->data(VideoFrame::Plane::kU) && !video_frame->data(VideoFrame::Plane::kV)) {
            temporary_frame = DownShiftHighbitVideoFrame(video_frame);
            video_frame = temporary_frame.get();
        }
        break;
    case PIXEL_FORMAT_Y16:
        // Since it is grayscale conversion, we disregard
        // SK_PMCOLOR_BYTE_ORDER and always use GL_RGBA.
        FlipAndConvertY16(video_frame, static_cast<uint8_t*>(rgb_pixels), GL_RGBA, GL_UNSIGNED_BYTE, false /*flip_y*/, row_bytes);
        return;
    default:
        break;
    }

    const size_t n_tasks = disable_threading ? 1 : NumConvertVideoFrameToRGBPixelsTasks(video_frame);
    base::WaitableEvent event;
    base::RepeatingClosure barrier = base::BarrierClosure(n_tasks, base::BindOnce(&base::WaitableEvent::Signal, base::Unretained(&event)));

    const libyuv::FilterMode libyuv_filter = ToLibyuvFilterMode(filter);
    for (size_t i = 1; i < n_tasks; ++i) {
        base::ThreadPool::PostTask(FROM_HERE,
            base::BindOnce(ConvertVideoFrameToRGBPixelsTask, base::Unretained(video_frame), rgb_pixels, row_bytes, premultiply_alpha, libyuv_filter, i, n_tasks,
                &barrier));
    }
    ConvertVideoFrameToRGBPixelsTask(video_frame, rgb_pixels, row_bytes, premultiply_alpha, libyuv_filter, 0, n_tasks, &barrier);
    {
        base::ScopedAllowBaseSyncPrimitivesOutsideBlockingScope allow_wait;
        event.Wait();
    }
}

// static
viz::SharedImageFormat PaintCanvasVideoRenderer::GetRGBPixelsOutputFormat()
{
    return SHARED_IMAGE_FORMAT;
}

bool PaintCanvasVideoRenderer::TexImage2D(unsigned target, unsigned /*texture*/, gpu::gles2::GLES2Interface* gl,
    const gpu::Capabilities& /*gpu_capabilities*/, VideoFrame* frame, int level, int internalformat, unsigned format, unsigned type,
    bool flip_y, bool /*premultiply_alpha*/)
{
    DCHECK(frame);
    if (!frame->IsMappable())
        return false;

    scoped_refptr<DataBuffer> temp_buffer;
    if (!TexImageHelper(frame, format, type, flip_y, &temp_buffer))
        return false;

    gl->TexImage2D(target, level, internalformat, frame->visible_rect().width(), frame->visible_rect().height(), 0, format, type, temp_buffer->data());
    return true;
}

bool PaintCanvasVideoRenderer::TexSubImage2D(unsigned target, gpu::gles2::GLES2Interface* gl, VideoFrame* frame, int level, unsigned format, unsigned type,
    int xoffset, int yoffset, bool flip_y, bool /*premultiply_alpha*/)
{
    DCHECK(frame);
    if (!frame->IsMappable())
        return false;

    scoped_refptr<DataBuffer> temp_buffer;
    if (!TexImageHelper(frame, format, type, flip_y, &temp_buffer))
        return false;

    gl->TexSubImage2D(target, level, xoffset, yoffset, frame->visible_rect().width(), frame->visible_rect().height(), format, type, temp_buffer->data());
    return true;
}

} // namespace media
