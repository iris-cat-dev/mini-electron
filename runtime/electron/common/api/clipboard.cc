// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/contains.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "runtime/electron/common/api/native_image.h"
#include "runtime/electron/common/gin_helper/dictionary.h"
#include "runtime/electron/common/gin_helper/object_template_builder.h"
#include "runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "runtime/electron/common/gin_helper/public/wrapper_info.h"
#include "runtime/electron/common/gin_helper/wrappable.h"
#include "runtime/electron/common/node_register_help.h"
#include "runtime/electron/node_bindings.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/libnode/src/node_buffer.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"

#if BUILDFLAG(IS_MAC)
#include "platform/macos/clipboard_mac.h"
#endif

namespace atom {

THREAD_LOCAL_CONSTRUCTOR(Clipboard)

class Clipboard : public mate::EventEmitter<Clipboard> {
public:
    Clipboard(v8::Isolate* isolate, v8::Local<v8::Object> wrapper)
    {
        gin_helper::Wrappable<Clipboard>::InitWith(isolate, wrapper);
    }

    static void init(v8::Isolate* isolate, v8::Local<v8::Object> target)
    {
        v8::Local<v8::Context> context = isolate->GetCurrentContext();
        v8::Local<v8::FunctionTemplate> prototype =
            v8::FunctionTemplate::New(isolate, newFunction);
        prototype->SetClassName(
            v8::String::NewFromUtf8Literal(isolate, "Clipboard"));
        gin_helper::ObjectTemplateBuilder builder(
            isolate, prototype->InstanceTemplate());
        builder.SetMethod("_readImage", &Clipboard::readImageApi);
        builder.SetMethod("_readText", &Clipboard::readTextApi);
        builder.SetMethod("_writeText", &Clipboard::writeTextApi);
        builder.SetMethod("_writeImage", &Clipboard::writeImageApi);
        builder.SetMethod("readBuffer", &Clipboard::readBufferApi);
        builder.SetMethod("writeBuffer", &Clipboard::writeBufferApi);
        builder.SetMethod("availableFormats", &Clipboard::availableFormatsApi);
        builder.SetMethod("has", &Clipboard::hasApi);
        builder.SetMethod("read", &Clipboard::readApi);
        builder.SetMethod("write", &Clipboard::writeApi);
        builder.SetMethod("readRTF", &Clipboard::readRTFApi);
        builder.SetMethod("writeRTF", &Clipboard::writeRTFApi);
        builder.SetMethod("readHTML", &Clipboard::readHTMLApi);
        builder.SetMethod("writeHTML", &Clipboard::writeHTMLApi);
        builder.SetMethod("readBookmark", &Clipboard::readBookmarkApi);
        builder.SetMethod("writeBookmark", &Clipboard::writeBookmarkApi);
        builder.SetMethod("readFindText", &Clipboard::readFindTextApi);
        builder.SetMethod("writeFindText", &Clipboard::writeFindTextApi);
        builder.SetMethod("_clear", &Clipboard::clearApi);

        v8::Local<v8::Function> constructor =
            prototype->GetFunction(context).ToLocalChecked();
        getClipboardConstructor().Reset(isolate, constructor);
        target
            ->Set(context, v8::String::NewFromUtf8Literal(isolate, "Clipboard"),
                constructor)
            .Check();
    }

    void writeFindTextApi(const std::u16string&) { }

    std::u16string readFindTextApi() { return {}; }

    std::u16string readHTMLApi()
    {
        std::u16string html;
        std::string url;
        uint32_t start = 0;
        uint32_t end = 0;
        ui::Clipboard::GetForCurrentThread()->ReadHTML(
            ui::ClipboardBuffer::kCopyPaste, nullptr, &html, &url, &start, &end);
        if (start > end || end > html.size())
            return {};
        return html.substr(start, end - start);
    }

    void writeHTMLApi(const std::u16string& html)
    {
        ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
        writer.WriteHTML(html, std::string());
    }

    v8::Local<v8::Value> readBookmarkApi(gin_helper::Arguments* args)
    {
        std::u16string title;
        std::string url;
        ui::Clipboard::GetForCurrentThread()->ReadBookmark(nullptr, &title, &url);
        auto result = gin_helper::Dictionary::CreateEmpty(args->isolate());
        result.Set("title", title);
        result.Set("url", url);
        return result.GetHandle();
    }

    void writeBookmarkApi(
        const std::u16string& title, const std::string& url)
    {
        ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
        writer.WriteBookmark(title, url);
    }

    void writeRTFApi(const std::string& text)
    {
        ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
        writer.WriteRTF(text);
    }

    std::u16string readRTFApi()
    {
        std::string data;
        ui::Clipboard::GetForCurrentThread()->ReadRTF(
            ui::ClipboardBuffer::kCopyPaste, nullptr, &data);
        return base::UTF8ToUTF16(data);
    }

    std::string readApi(const std::string& format)
    {
        return readImpl(format);
    }

    void writeApi(v8::Local<v8::Object> object)
    {
        gin_helper::Dictionary data(v8::Isolate::GetCurrent(), object);
        writeImpl(data);
    }

    void writeImpl(const gin_helper::Dictionary& data)
    {
        std::u16string text;
        std::u16string html;
        std::u16string bookmark;
        std::u16string rtf;
        NativeImage* image = nullptr;
        v8::Local<v8::Value> image_value;
        if (data.Get("image", &image_value) && image_value->IsObject())
            image = NativeImage::GetSelf(image_value.As<v8::Object>());

#if BUILDFLAG(IS_MAC)
        std::vector<uint8_t> png;
        if (image && !image->isEmpty()) {
            v8::Local<v8::Object> buffer = image->toPNG();
            const auto* bytes = reinterpret_cast<const uint8_t*>(
                node::Buffer::Data(buffer));
            png.assign(bytes, bytes + node::Buffer::Length(buffer));
        }
        const bool has_text = data.Get("text", &text);
        if ((has_text || !png.empty())
            && mini_electron::mac::WriteClipboard(
                has_text ? &text : nullptr, png.empty() ? nullptr : &png)) {
            return;
        }
#endif

        ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
        if (data.Get("text", &text)) {
            writer.WriteText(text);
            if (data.Get("bookmark", &bookmark))
                writer.WriteBookmark(bookmark, base::UTF16ToUTF8(text));
        }
        if (data.Get("rtf", &rtf))
            writer.WriteRTF(base::UTF16ToUTF8(rtf));
        if (data.Get("html", &html))
            writer.WriteHTML(html, std::string());
        if (image && !image->isEmpty())
            writer.WriteImage(image->bitmap());
    }

    bool hasApi(const std::string& format_name)
    {
        ui::ClipboardFormatType format(
            ui::ClipboardFormatType::GetType(format_name));
        if (format.GetName().empty())
            format = ui::ClipboardFormatType::CustomPlatformType(format_name);
        return ui::Clipboard::GetForCurrentThread()->IsFormatAvailable(
            format, ui::ClipboardBuffer::kCopyPaste, nullptr);
    }

    void clearApi(const std::string&)
    {
#if BUILDFLAG(IS_MAC)
        mini_electron::mac::ClearClipboard();
#else
        ui::Clipboard::GetForCurrentThread()->Clear(
            ui::ClipboardBuffer::kCopyPaste);
#endif
    }

    std::vector<std::u16string> availableFormatsApi()
    {
#if BUILDFLAG(IS_MAC)
        return mini_electron::mac::AvailableClipboardFormats();
#else
        return ui::Clipboard::GetForCurrentThread()
            ->ReadAvailableStandardAndCustomFormatNames(
                ui::ClipboardBuffer::kCopyPaste, nullptr);
#endif
    }

    std::string readImpl(const std::string& format_name)
    {
        ui::Clipboard* clipboard = ui::Clipboard::GetForCurrentThread();
        ui::ClipboardFormatType raw_format(
            ui::ClipboardFormatType::CustomPlatformType(format_name));
        if (clipboard->IsFormatAvailable(raw_format,
                ui::ClipboardBuffer::kCopyPaste, nullptr)) {
            std::string data;
            clipboard->ReadData(raw_format, nullptr, &data);
            return data;
        }
        const std::map<std::string, std::string> custom_names =
            clipboard->ExtractCustomPlatformNames(
                ui::ClipboardBuffer::kCopyPaste, nullptr);
        auto custom = custom_names.find(format_name);
        ui::ClipboardFormatType format =
            custom == custom_names.end()
            ? ui::ClipboardFormatType::CustomPlatformType(format_name)
            : ui::ClipboardFormatType::CustomPlatformType(custom->second);
        std::string data;
        clipboard->ReadData(format, nullptr, &data);
        return data;
    }

    v8::Local<v8::Value> readBufferApi(
        const std::string& format, gin_helper::Arguments* args)
    {
        const std::string data = readImpl(format);
        return node::Buffer::Copy(
                   args->isolate(), data.data(), data.size())
            .ToLocalChecked();
    }

    void writeBufferApi(
        const std::string& format, v8::Local<v8::Value> buffer)
    {
        v8::Isolate* isolate = v8::Isolate::GetCurrent();
        if (!node::Buffer::HasInstance(buffer)) {
            isolate->ThrowException(v8::Exception::TypeError(
                v8::String::NewFromUtf8Literal(isolate, "buffer must be a node Buffer")));
            return;
        }
        const auto* bytes = reinterpret_cast<const uint8_t*>(
            node::Buffer::Data(buffer));
        const size_t size = node::Buffer::Length(buffer);
        mojo_base::BigBuffer copy(size);
        std::copy(bytes, bytes + size, copy.data());
        ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
        writer.WriteData(base::UTF8ToUTF16(format), std::move(copy));
    }

    void writeImageApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        if (args.Length() < 1 || !args[0]->IsObject())
            return;
        NativeImage* image = NativeImage::GetSelf(args[0].As<v8::Object>());
        if (!image || image->isEmpty())
            return;
#if BUILDFLAG(IS_MAC)
        v8::Local<v8::Object> buffer = image->toPNG();
        const auto* bytes = reinterpret_cast<const uint8_t*>(
            node::Buffer::Data(buffer));
        std::vector<uint8_t> png(
            bytes, bytes + node::Buffer::Length(buffer));
        mini_electron::mac::WriteClipboard(nullptr, &png);
#else
        ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
        writer.WriteImage(image->bitmap());
#endif
    }

    std::u16string readTextApi(const std::string&)
    {
#if BUILDFLAG(IS_MAC)
        return mini_electron::mac::ReadClipboardText();
#else
        std::u16string text;
        ui::Clipboard::GetForCurrentThread()->ReadText(
            ui::ClipboardBuffer::kCopyPaste, nullptr, &text);
        return text;
#endif
    }

    void writeTextApi(const std::u16string& text, const std::string&)
    {
#if BUILDFLAG(IS_MAC)
        mini_electron::mac::WriteClipboard(&text, nullptr);
#else
        ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
        writer.WriteText(text);
#endif
    }

    v8::Local<v8::Object> readImageApi(const std::string&)
    {
#if BUILDFLAG(IS_MAC)
        const std::vector<uint8_t> png =
            mini_electron::mac::ReadClipboardPng();
        return NativeImage::createNativeImageFromBuffer(
            isolate(), png.data(), png.size());
#elif BUILDFLAG(IS_WIN)
        if (!::OpenClipboard(nullptr))
            return NativeImage::createEmpty(isolate());
        HANDLE dib_handle = ::GetClipboardData(CF_DIB);
        if (!dib_handle) {
            ::CloseClipboard();
            return NativeImage::createEmpty(isolate());
        }
        const SIZE_T dib_size = ::GlobalSize(dib_handle);
        if (dib_size < sizeof(BITMAPINFOHEADER)) {
            ::CloseClipboard();
            return NativeImage::createEmpty(isolate());
        }
        const void* dib = ::GlobalLock(dib_handle);
        if (!dib) {
            ::CloseClipboard();
            return NativeImage::createEmpty(isolate());
        }
        v8::Local<v8::Object> result =
            NativeImage::createFromDIB(isolate(), dib, dib_size);
        ::GlobalUnlock(dib_handle);
        ::CloseClipboard();
        return result;
#else
        return NativeImage::createEmpty(isolate());
#endif
    }

    static void newFunction(
        const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        if (!args.IsConstructCall())
            return;
        new Clipboard(args.GetIsolate(), args.This());
        args.GetReturnValue().Set(args.This());
    }

    static gin_helper::WrapperInfo kWrapperInfo;
};

gin_helper::WrapperInfo Clipboard::kWrapperInfo = {
    gin_helper::GinEmbedder::kEmbedderNativeGin
};

void initializeClipboardApi(v8::Local<v8::Object> exports,
    v8::Local<v8::Value>, v8::Local<v8::Context> context, void*)
{
    Clipboard::init(context->GetIsolate(), exports);
}

} // namespace atom

static const char CommonClipboardNative[] = "";
static NodeNative nativeCommonClipboard {
    "Clipboard", CommonClipboardNative, 0
};

NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_MANUAL(
    electron_common_clipboard, atom::initializeClipboardApi,
    &nativeCommonClipboard)
