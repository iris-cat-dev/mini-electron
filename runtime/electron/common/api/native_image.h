// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef COMMON_API_API_NATIVE_IMAGE_EXPORT_H_
#define COMMON_API_API_NATIVE_IMAGE_EXPORT_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "build/build_config.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "v8.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace atom {

class NativeImage : public mate::EventEmitter<NativeImage> {
public:
    NativeImage(v8::Isolate* isolate, v8::Local<v8::Object> wrapper);
    ~NativeImage() override;

    static void init(v8::Isolate* isolate, v8::Local<v8::Object> target);

    v8::Local<v8::Object> toPNG();
    v8::Local<v8::Object> toJPEG(const base::Value::Dict& options);
    v8::Local<v8::Object> toBitmap();
    std::string toDataURL();
    v8::Local<v8::Object> getSize();
    v8::Local<v8::Object> crop(const base::Value::Dict& rect);
    v8::Local<v8::Object> resize(const base::Value::Dict& options);
    bool isEmpty() const;
    bool isTemplateImage() const;
    void setTemplateImage(bool is_template);

    static v8::Local<v8::Object> createEmpty(v8::Isolate* isolate);
    static void createEmptyApi(const v8::FunctionCallbackInfo<v8::Value>& info);
    static void createFromPathApi(const v8::FunctionCallbackInfo<v8::Value>& info);
    static void createFromBufferApi(const v8::FunctionCallbackInfo<v8::Value>& info);
    static void createFromDataURLApi(const v8::FunctionCallbackInfo<v8::Value>& info);

    // Creates an image from immutable, top-down, straight-alpha RGBA8 rows.
    // The pixels are deep-copied, so the caller may release them on return.
    static v8::Local<v8::Object> createFromRGBA(
        v8::Isolate* isolate, const uint8_t* rgba, int width, int height, int stride);
    static v8::Local<v8::Object> createNativeImageFromBuffer(
        v8::Isolate* isolate, const unsigned char* data, size_t size);
#if BUILDFLAG(IS_WIN)
    static v8::Local<v8::Object> createFromDIB(
        v8::Isolate* isolate, const void* dib, size_t dib_size);
    static v8::Local<v8::Object> createFromBITMAPINFO(
        v8::Isolate* isolate, const BITMAPINFO* info, const void* pixels);
#endif

    static NativeImage* GetSelf(v8::Local<v8::Object> handle);
    const SkBitmap& bitmap() const { return bitmap_; }

#if BUILDFLAG(IS_WIN)
    HICON getIcon();
    // The caller owns the returned bitmap.
    HBITMAP getBitmap() const;
#endif

    int getWidth() const;
    int getHeight() const;

    static gin::WrapperInfo kWrapperInfo;

private:
    static void newFunction(const v8::FunctionCallbackInfo<v8::Value>& args);
    static v8::Local<v8::Object> createFromBitmap(
        v8::Isolate* isolate, const SkBitmap& bitmap);
#if BUILDFLAG(IS_WIN)
    static v8::Local<v8::Object> createFromBITMAPINFO(
        v8::Isolate* isolate, const BITMAPINFO* info, const void* pixels,
        size_t pixel_size);
#endif
    bool decode(const uint8_t* data, size_t size);

    SkBitmap bitmap_;
    bool is_template_image_ = false;
#if BUILDFLAG(IS_WIN)
    HICON icon_ = nullptr;
#endif
};

} // namespace atom

#endif // COMMON_API_API_NATIVE_IMAGE_EXPORT_H_