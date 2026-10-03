// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "runtime/electron/common/api/native_image.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/strings/escape.h"
#include "base/strings/string_util.h"
#include "base/numerics/checked_math.h"
#include "runtime/electron/common/asar/asar_util.h"
#include "runtime/electron/common/gin_helper/dictionary.h"
#include "runtime/electron/common/gin_helper/object_template_builder.h"
#include "runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "runtime/electron/common/gin_helper/public/wrapper_info.h"
#include "runtime/electron/common/gin_helper/wrappable.h"
#include "runtime/electron/common/node_register_help.h"
#include "runtime/electron/node_bindings.h"
#include "skia/ext/image_operations.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/libnode/src/node_buffer.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/codec/SkCodec.h"
#include "third_party/skia/include/codec/SkIcoDecoder.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"

namespace atom {
namespace {

constexpr size_t kMaxEncodedImageBytes = 256u * 1024u * 1024u;
constexpr int kMaxImageDimension = 32768;

bool IsValidDimensions(int width, int height)
{
    if (width <= 0 || height <= 0 || width > kMaxImageDimension || height > kMaxImageDimension)
        return false;
    base::CheckedNumeric<size_t> bytes = static_cast<size_t>(width);
    bytes *= static_cast<size_t>(height);
    bytes *= 4u;
    return bytes.IsValid() && bytes.ValueOrDie() <= kMaxEncodedImageBytes;
}

v8::Local<v8::Object> CopyToNodeBuffer(v8::Isolate* isolate, base::span<const uint8_t> bytes)
{
    return node::Buffer::Copy(
               isolate, reinterpret_cast<const char*>(bytes.data()), bytes.size())
        .ToLocalChecked();
}

int ReadInt(const base::Value::Dict& options, const char* key, int fallback)
{
    const std::optional<int> value = options.FindInt(key);
    return value.value_or(fallback);
}

} // namespace

THREAD_LOCAL_CONSTRUCTOR(NativeImage)

NativeImage::NativeImage(v8::Isolate* isolate, v8::Local<v8::Object> wrapper)
{
    gin_helper::Wrappable<NativeImage>::InitWith(isolate, wrapper);
}

NativeImage::~NativeImage()
{
#if BUILDFLAG(IS_WIN)
    if (icon_)
        ::DestroyIcon(icon_);
#endif
}

void NativeImage::init(v8::Isolate* isolate, v8::Local<v8::Object> target)
{
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Local<v8::FunctionTemplate> prototype =
        v8::FunctionTemplate::New(isolate, newFunction);
    prototype->SetClassName(
        v8::String::NewFromUtf8Literal(isolate, "NativeImage"));

    gin_helper::ObjectTemplateBuilder builder(isolate, prototype->InstanceTemplate());
    builder.SetMethod("toPNG", &NativeImage::toPNG);
    builder.SetMethod("toJPEG", &NativeImage::toJPEG);
    builder.SetMethod("toBitmap", &NativeImage::toBitmap);
    builder.SetMethod("toDataURL", &NativeImage::toDataURL);
    builder.SetMethod("getSize", &NativeImage::getSize);
    builder.SetMethod("crop", &NativeImage::crop);
    builder.SetMethod("resize", &NativeImage::resize);
    builder.SetMethod("isEmpty", &NativeImage::isEmpty);
    builder.SetMethod("isTemplateImage", &NativeImage::isTemplateImage);
    builder.SetMethod("setTemplateImage", &NativeImage::setTemplateImage);

    v8::Local<v8::Function> constructor =
        prototype->GetFunction(context).ToLocalChecked();
    getNativeImageConstructor().Reset(isolate, constructor);
    target
        ->Set(context, v8::String::NewFromUtf8Literal(isolate, "NativeImage"),
            constructor)
        .Check();

    gin_helper::Dictionary native_image_class(isolate, constructor);
    native_image_class.SetMethod("createEmpty", &NativeImage::createEmptyApi);
    native_image_class.SetMethod("createFromPath", &NativeImage::createFromPathApi);
    native_image_class.SetMethod("createFromBuffer", &NativeImage::createFromBufferApi);
    native_image_class.SetMethod(
        "createFromDataURL", &NativeImage::createFromDataURLApi);
}

v8::Local<v8::Object> NativeImage::toPNG()
{
    if (isEmpty())
        return CopyToNodeBuffer(isolate(), base::span<const uint8_t>());
    std::optional<std::vector<uint8_t>> encoded =
        gfx::PNGCodec::EncodeBGRASkBitmap(bitmap_, false);
    if (!encoded)
        return CopyToNodeBuffer(isolate(), base::span<const uint8_t>());
    return CopyToNodeBuffer(isolate(), *encoded);
}

v8::Local<v8::Object> NativeImage::toJPEG(const base::Value::Dict& options)
{
    if (isEmpty())
        return CopyToNodeBuffer(isolate(), base::span<const uint8_t>());
    const int quality = std::clamp(ReadInt(options, "quality", 100), 0, 100);
    std::optional<std::vector<uint8_t>> encoded =
        gfx::JPEGCodec::Encode(bitmap_, quality);
    if (!encoded)
        return CopyToNodeBuffer(isolate(), base::span<const uint8_t>());
    return CopyToNodeBuffer(isolate(), *encoded);
}

v8::Local<v8::Object> NativeImage::toBitmap()
{
    if (isEmpty())
        return CopyToNodeBuffer(isolate(), base::span<const uint8_t>());
    base::CheckedNumeric<size_t> size = static_cast<size_t>(getWidth());
    size *= static_cast<size_t>(getHeight());
    size *= 4u;
    if (!size.IsValid())
        return CopyToNodeBuffer(isolate(), base::span<const uint8_t>());
    std::vector<uint8_t> bytes(size.ValueOrDie());
    const size_t row_bytes = static_cast<size_t>(getWidth()) * 4u;
    for (int y = 0; y < getHeight(); ++y) {
        std::memcpy(bytes.data() + static_cast<size_t>(y) * row_bytes,
            bitmap_.getAddr(0, y), row_bytes);
    }
    return CopyToNodeBuffer(isolate(), bytes);
}

std::string NativeImage::toDataURL()
{
    if (isEmpty())
        return "data:image/png;base64,";
    std::optional<std::vector<uint8_t>> encoded =
        gfx::PNGCodec::EncodeBGRASkBitmap(bitmap_, false);
    if (!encoded)
        return "data:image/png;base64,";
    std::string result("data:image/png;base64,");
    result.reserve(result.size() + 4u * ((encoded->size() + 2u) / 3u));
    base::Base64EncodeAppend(base::span<const uint8_t>(*encoded), &result);
    return result;
}

v8::Local<v8::Object> NativeImage::getSize()
{
    v8::Local<v8::Object> result = v8::Object::New(isolate());
    gin_helper::Dictionary dictionary(isolate(), result);
    dictionary.Set("width", getWidth());
    dictionary.Set("height", getHeight());
    return result;
}

v8::Local<v8::Object> NativeImage::crop(const base::Value::Dict& rect)
{
    const int x = ReadInt(rect, "x", -1);
    const int y = ReadInt(rect, "y", -1);
    const int width = ReadInt(rect, "width", 0);
    const int height = ReadInt(rect, "height", 0);
    base::CheckedNumeric<int> right = x;
    right += width;
    base::CheckedNumeric<int> bottom = y;
    bottom += height;
    if (isEmpty() || x < 0 || y < 0 || !IsValidDimensions(width, height)
        || !right.IsValid() || !bottom.IsValid()
        || right.ValueOrDie() > getWidth() || bottom.ValueOrDie() > getHeight()) {
        return createEmpty(isolate());
    }

    SkBitmap cropped;
    if (!cropped.tryAllocPixels(
            SkImageInfo::MakeN32Premul(width, height))) {
        return createEmpty(isolate());
    }
    if (!bitmap_.readPixels(cropped.info(), cropped.getPixels(),
            cropped.rowBytes(), x, y)) {
        return createEmpty(isolate());
    }
    return createFromBitmap(isolate(), cropped);
}

v8::Local<v8::Object> NativeImage::resize(const base::Value::Dict& options)
{
    if (isEmpty())
        return createEmpty(isolate());
    int width = ReadInt(options, "width", 0);
    int height = ReadInt(options, "height", 0);
    if (width <= 0 && height <= 0)
        return createEmpty(isolate());
    if (width <= 0)
        width = std::max(1, static_cast<int>(std::lround(
                                static_cast<double>(getWidth()) * height / getHeight())));
    if (height <= 0)
        height = std::max(1, static_cast<int>(std::lround(
                                 static_cast<double>(getHeight()) * width / getWidth())));
    if (!IsValidDimensions(width, height))
        return createEmpty(isolate());
    SkBitmap resized = skia::ImageOperations::Resize(
        bitmap_, skia::ImageOperations::RESIZE_LANCZOS3, width, height);
    if (resized.drawsNothing())
        return createEmpty(isolate());
    return createFromBitmap(isolate(), resized);
}

bool NativeImage::isTemplateImage() const
{
    return is_template_image_;
}

void NativeImage::setTemplateImage(bool is_template)
{
    is_template_image_ = is_template;
}

v8::Local<v8::Object> NativeImage::createEmpty(v8::Isolate* isolate)
{
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Local<v8::Function> constructor =
        v8::Local<v8::Function>::New(isolate, getNativeImageConstructor());
    return constructor->NewInstance(context).ToLocalChecked();
}

v8::Local<v8::Object> NativeImage::createFromBitmap(
    v8::Isolate* isolate, const SkBitmap& bitmap)
{
    v8::Local<v8::Object> result = createEmpty(isolate);
    NativeImage* self = GetSelf(result);
    if (self)
        self->bitmap_ = bitmap;
    return result;
}

void NativeImage::createEmptyApi(
    const v8::FunctionCallbackInfo<v8::Value>& info)
{
    info.GetReturnValue().Set(createEmpty(info.GetIsolate()));
}

void NativeImage::createFromPathApi(
    const v8::FunctionCallbackInfo<v8::Value>& info)
{
    std::string path;
    if (info.Length() > 0 && info[0]->IsString()) {
        v8::String::Utf8Value value(info.GetIsolate(), info[0]);
        if (*value)
            path.assign(*value, value.length());
    }
    std::string contents;
    if (path.empty()
        || !asar::readFileToString(base::FilePath::FromUTF8Unsafe(path), &contents)) {
        info.GetReturnValue().Set(createEmpty(info.GetIsolate()));
        return;
    }
    info.GetReturnValue().Set(createNativeImageFromBuffer(info.GetIsolate(),
        reinterpret_cast<const uint8_t*>(contents.data()), contents.size()));
}

void NativeImage::createFromBufferApi(
    const v8::FunctionCallbackInfo<v8::Value>& info)
{
    v8::Isolate* isolate = info.GetIsolate();
    if (info.Length() < 1 || !node::Buffer::HasInstance(info[0])) {
        isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(isolate, "buffer must be a node Buffer")));
        return;
    }
    const uint8_t* data =
        reinterpret_cast<const uint8_t*>(node::Buffer::Data(info[0]));
    const size_t size = node::Buffer::Length(info[0]);

    if (info.Length() > 1 && info[1]->IsObject()) {
        gin_helper::Dictionary options(
            isolate, info[1].As<v8::Object>());
        int width = 0;
        int height = 0;
        options.Get("width", &width);
        options.Get("height", &height);
        if (width > 0 || height > 0) {
            base::CheckedNumeric<size_t> required = static_cast<size_t>(width);
            required *= static_cast<size_t>(height);
            required *= 4u;
            if (!IsValidDimensions(width, height) || !required.IsValid()
                || size < required.ValueOrDie()) {
                info.GetReturnValue().Set(createEmpty(isolate));
                return;
            }
            SkBitmap bitmap;
            if (!bitmap.tryAllocPixels(SkImageInfo::MakeN32Premul(width, height))) {
                info.GetReturnValue().Set(createEmpty(isolate));
                return;
            }
            const size_t row_bytes = static_cast<size_t>(width) * 4u;
            for (int y = 0; y < height; ++y) {
                std::memcpy(bitmap.getAddr(0, y),
                    data + static_cast<size_t>(y) * row_bytes, row_bytes);
            }
            info.GetReturnValue().Set(createFromBitmap(isolate, bitmap));
            return;
        }
    }
    info.GetReturnValue().Set(
        createNativeImageFromBuffer(isolate, data, size));
}

void NativeImage::createFromDataURLApi(
    const v8::FunctionCallbackInfo<v8::Value>& info)
{
    v8::Isolate* isolate = info.GetIsolate();
    if (info.Length() < 1 || !info[0]->IsString()) {
        isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8Literal(isolate, "dataURL must be a string")));
        return;
    }
    v8::String::Utf8Value value(isolate, info[0]);
    std::string data_url(*value, value.length());
    const size_t comma = data_url.find(',');
    if (comma == std::string::npos || data_url.compare(0, 5, "data:") != 0) {
        info.GetReturnValue().Set(createEmpty(isolate));
        return;
    }
    const std::string metadata =
        base::ToLowerASCII(data_url.substr(5, comma - 5));
    if (metadata.rfind("image/png", 0) != 0
        && metadata.rfind("image/jpeg", 0) != 0
        && metadata.rfind("image/jpg", 0) != 0) {
        info.GetReturnValue().Set(createEmpty(isolate));
        return;
    }
    std::string decoded;
    const std::string_view payload(data_url.data() + comma + 1,
        data_url.size() - comma - 1);
    const bool is_base64 = metadata.find(";base64") != std::string::npos;
    if (is_base64) {
        if (!base::Base64Decode(payload, &decoded)) {
            info.GetReturnValue().Set(createEmpty(isolate));
            return;
        }
    } else {
        decoded = base::UnescapeBinaryURLComponent(payload);
    }
    if (decoded.size() > kMaxEncodedImageBytes) {
        info.GetReturnValue().Set(createEmpty(isolate));
        return;
    }
    info.GetReturnValue().Set(createNativeImageFromBuffer(isolate,
        reinterpret_cast<const uint8_t*>(decoded.data()), decoded.size()));
}

#if BUILDFLAG(IS_WIN)
v8::Local<v8::Object> NativeImage::createFromDIB(
    v8::Isolate* isolate, const void* dib, size_t dib_size)
{
    if (!dib || dib_size < sizeof(BITMAPINFOHEADER))
        return createEmpty(isolate);

    DWORD header_size = 0;
    std::memcpy(&header_size, dib, sizeof(header_size));
    if (header_size < sizeof(BITMAPINFOHEADER) || header_size > dib_size)
        return createEmpty(isolate);

    BITMAPINFOHEADER header = {};
    std::memcpy(&header, dib, sizeof(header));
    if (header.biSize != header_size || header.biPlanes != 1
        || (header.biBitCount != 24 && header.biBitCount != 32)
        || header.biCompression != BI_RGB) {
        // Masked DIBs are not decoded by this path. Reject them before deriving
        // a pixel address from their variable-size mask table.
        return createEmpty(isolate);
    }

    base::CheckedNumeric<size_t> pixel_offset =
        static_cast<size_t>(header_size);
    // A true-color DIB may still carry an explicit optimal color palette.
    base::CheckedNumeric<size_t> palette_bytes =
        static_cast<size_t>(header.biClrUsed);
    palette_bytes *= sizeof(RGBQUAD);
    pixel_offset += palette_bytes;
    if (!pixel_offset.IsValid()
        || pixel_offset.ValueOrDie() > dib_size) {
        return createEmpty(isolate);
    }

    if (header.biWidth <= 0
        || header.biHeight == std::numeric_limits<LONG>::min()) {
        return createEmpty(isolate);
    }
    const int height = std::abs(header.biHeight);
    if (!IsValidDimensions(header.biWidth, height))
        return createEmpty(isolate);

    base::CheckedNumeric<size_t> stride =
        static_cast<size_t>(header.biWidth);
    stride *= static_cast<size_t>(header.biBitCount / 8);
    stride += 3u;
    stride /= 4u;
    stride *= 4u;
    base::CheckedNumeric<size_t> pixel_bytes = stride;
    pixel_bytes *= static_cast<size_t>(height);
    if (!stride.IsValid() || !pixel_bytes.IsValid())
        return createEmpty(isolate);

    const size_t offset = pixel_offset.ValueOrDie();
    const size_t required_pixels = pixel_bytes.ValueOrDie();
    const size_t available_pixels = dib_size - offset;
    if (required_pixels > available_pixels)
        return createEmpty(isolate);
    if (header.biSizeImage != 0
        && (header.biSizeImage < required_pixels
            || header.biSizeImage > available_pixels)) {
        return createEmpty(isolate);
    }

    const auto* pixels = static_cast<const uint8_t*>(dib) + offset;
    return createFromBITMAPINFO(
        isolate, reinterpret_cast<const BITMAPINFO*>(dib), pixels,
        required_pixels);
}

v8::Local<v8::Object> NativeImage::createFromBITMAPINFO(
    v8::Isolate* isolate, const BITMAPINFO* info, const void* pixels)
{
    return createFromBITMAPINFO(
        isolate, info, pixels, std::numeric_limits<size_t>::max());
}

v8::Local<v8::Object> NativeImage::createFromBITMAPINFO(
    v8::Isolate* isolate, const BITMAPINFO* info, const void* pixels,
    size_t pixel_size)
{
    if (!info || !pixels)
        return createEmpty(isolate);
    const BITMAPINFOHEADER& header = info->bmiHeader;
    const int width = header.biWidth;
    if (header.biHeight == std::numeric_limits<LONG>::min())
        return createEmpty(isolate);
    const int height = std::abs(header.biHeight);
    const int bits_per_pixel = header.biBitCount;
    if (header.biSize < sizeof(BITMAPINFOHEADER) || header.biPlanes != 1
        || !IsValidDimensions(width, height)
        || (bits_per_pixel != 24 && bits_per_pixel != 32)
        || header.biCompression != BI_RGB) {
        return createEmpty(isolate);
    }
    base::CheckedNumeric<size_t> source_stride = static_cast<size_t>(width);
    source_stride *= static_cast<size_t>(bits_per_pixel / 8);
    source_stride += 3u;
    source_stride /= 4u;
    source_stride *= 4u;
    base::CheckedNumeric<size_t> source_size = source_stride;
    source_size *= static_cast<size_t>(height);
    if (!source_stride.IsValid() || !source_size.IsValid()
        || source_size.ValueOrDie() > pixel_size) {
        return createEmpty(isolate);
    }

    const size_t stride = source_stride.ValueOrDie();
    const auto* source_bytes = static_cast<const uint8_t*>(pixels);
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4u);
    bool has_nonzero_alpha = false;
    for (int y = 0; y < height; ++y) {
        const int source_y = header.biHeight > 0 ? height - 1 - y : y;
        const uint8_t* source =
            source_bytes + static_cast<size_t>(source_y) * stride;
        uint8_t* destination =
            rgba.data() + static_cast<size_t>(y) * width * 4u;
        for (int x = 0; x < width; ++x) {
            destination[x * 4] = source[x * bits_per_pixel / 8 + 2];
            destination[x * 4 + 1] = source[x * bits_per_pixel / 8 + 1];
            destination[x * 4 + 2] = source[x * bits_per_pixel / 8];
            destination[x * 4 + 3] =
                bits_per_pixel == 32 ? source[x * 4 + 3] : 255;
            has_nonzero_alpha |= destination[x * 4 + 3] != 0;
        }
    }
    if (bits_per_pixel == 32 && !has_nonzero_alpha) {
        for (size_t index = 3; index < rgba.size(); index += 4)
            rgba[index] = 255;
    }
    return createFromRGBA(isolate, rgba.data(), width, height, width * 4);
}
#endif

v8::Local<v8::Object> NativeImage::createFromRGBA(v8::Isolate* isolate,
    const uint8_t* rgba, int width, int height, int stride)
{
    if (!rgba || !IsValidDimensions(width, height)
        || stride < width * 4) {
        return createEmpty(isolate);
    }
    base::CheckedNumeric<size_t> input_size = static_cast<size_t>(stride);
    input_size *= static_cast<size_t>(height);
    if (!input_size.IsValid())
        return createEmpty(isolate);

    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(SkImageInfo::MakeN32Premul(width, height)))
        return createEmpty(isolate);
    for (int y = 0; y < height; ++y) {
        const uint8_t* source = rgba + static_cast<size_t>(y) * stride;
        SkPMColor* destination = bitmap.getAddr32(0, y);
        for (int x = 0; x < width; ++x) {
            destination[x] = SkPreMultiplyARGB(
                source[x * 4 + 3], source[x * 4], source[x * 4 + 1],
                source[x * 4 + 2]);
        }
    }
    return createFromBitmap(isolate, bitmap);
}

bool NativeImage::decode(const uint8_t* data, size_t size)
{
    if (!data || size == 0 || size > kMaxEncodedImageBytes)
        return false;
    base::span<const uint8_t> bytes(data, size);
    SkBitmap decoded = gfx::PNGCodec::Decode(bytes);
    if (decoded.drawsNothing())
        decoded = gfx::JPEGCodec::Decode(bytes);
    if (decoded.drawsNothing()) {
        sk_sp<SkData> encoded = SkData::MakeWithoutCopy(data, size);
        std::unique_ptr<SkCodec> codec = SkIcoDecoder::IsIco(data, size)
            ? SkIcoDecoder::Decode(std::move(encoded), nullptr)
            : SkCodec::MakeFromData(std::move(encoded));
        if (!codec || !IsValidDimensions(
                          codec->dimensions().width(),
                          codec->dimensions().height())) {
            return false;
        }
        const SkImageInfo info = SkImageInfo::MakeN32Premul(
            codec->dimensions().width(), codec->dimensions().height());
        if (!decoded.tryAllocPixels(info)
            || codec->getPixels(
                   info, decoded.getPixels(), decoded.rowBytes())
                != SkCodec::kSuccess) {
            return false;
        }
    }
    if (!IsValidDimensions(decoded.width(), decoded.height()))
        return false;
    bitmap_ = decoded;
    return true;
}

v8::Local<v8::Object> NativeImage::createNativeImageFromBuffer(
    v8::Isolate* isolate, const unsigned char* data, size_t size)
{
    v8::Local<v8::Object> result = createEmpty(isolate);
    NativeImage* self = GetSelf(result);
    if (self)
        self->decode(data, size);
    return result;
}

void NativeImage::newFunction(
    const v8::FunctionCallbackInfo<v8::Value>& args)
{
    if (!args.IsConstructCall())
        return;
    new NativeImage(args.GetIsolate(), args.This());
    args.GetReturnValue().Set(args.This());
}

NativeImage* NativeImage::GetSelf(v8::Local<v8::Object> handle)
{
    return static_cast<NativeImage*>(
        WrappableBase::GetNativePtr(handle, &kWrapperInfo));
}

#if BUILDFLAG(IS_WIN)
HBITMAP NativeImage::getBitmap() const
{
    if (isEmpty())
        return nullptr;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = getWidth();
    info.bmiHeader.biHeight = -getHeight();
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap =
        ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels)
        return nullptr;
    const size_t row_bytes = static_cast<size_t>(getWidth()) * 4u;
    for (int y = 0; y < getHeight(); ++y) {
        std::memcpy(static_cast<uint8_t*>(pixels) + static_cast<size_t>(y) * row_bytes,
            bitmap_.getAddr(0, y), row_bytes);
    }
    return bitmap;
}

HICON NativeImage::getIcon()
{
    if (icon_ || isEmpty())
        return icon_;
    HBITMAP color = getBitmap();
    if (!color)
        return nullptr;
    HBITMAP mask = ::CreateBitmap(getWidth(), getHeight(), 1, 1, nullptr);
    ICONINFO info = {};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    icon_ = ::CreateIconIndirect(&info);
    ::DeleteObject(color);
    ::DeleteObject(mask);
    return icon_;
}
#endif

int NativeImage::getWidth() const
{
    return isEmpty() ? 0 : bitmap_.width();
}

int NativeImage::getHeight() const
{
    return isEmpty() ? 0 : bitmap_.height();
}

bool NativeImage::isEmpty() const
{
    return bitmap_.drawsNothing();
}

gin_helper::WrapperInfo NativeImage::kWrapperInfo = {
    gin_helper::GinEmbedder::kEmbedderNativeGin
};

void initializeNativeImageApi(v8::Local<v8::Object> exports,
    v8::Local<v8::Value>, v8::Local<v8::Context> context, void*)
{
    NativeImage::init(context->GetIsolate(), exports);
}

} // namespace atom

static const char CommonNativeImageNative[] = "";
static NodeNative nativeCommonNativeImageNative {
    "NativeImage", CommonNativeImageNative, 0
};

NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_MANUAL(
    electron_common_nativeImage, atom::initializeNativeImageApi,
    &nativeCommonNativeImageNative)
