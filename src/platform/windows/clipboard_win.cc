// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "src/platform/clipboard.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "third_party/skia/include/codec/SkCodec.h"
#include "third_party/skia/include/codec/SkEncodedImageFormat.h"
#include "third_party/skia/include/codec/SkPngDecoder.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/clipboard_constants.h"
#include "ui/base/clipboard/clipboard_format_type.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"

namespace mini_electron {
namespace {

std::mutex clipboard_broker_lock;
ClipboardBroker clipboard_broker;
bool renderer_broker_required = false;

struct BrokerResult {
  bool required = false;
  bool succeeded = false;
  base::Value::Dict value;
};

ClipboardBroker SnapshotClipboardBroker(bool* required) {
  std::lock_guard<std::mutex> lock(clipboard_broker_lock);
  *required = renderer_broker_required;
  return clipboard_broker;
}

BrokerResult BrokerRequest(const char* operation) {
  BrokerResult result;
  ClipboardBroker broker = SnapshotClipboardBroker(&result.required);
  if (!broker)
    return result;
  base::Value::Dict request;
  request.Set("operation", operation);
  result.succeeded = broker(std::move(request), &result.value);
  return result;
}

bool RendererBrokerRequired() {
  std::lock_guard<std::mutex> lock(clipboard_broker_lock);
  return renderer_broker_required;
}

bool IsValidPng(const std::vector<uint8_t>& png, SkBitmap* decoded) {
  if (png.empty() || png.size() > kMaximumClipboardImageBytes)
    return false;
  sk_sp<SkData> encoded = SkData::MakeWithoutCopy(png.data(), png.size());
  std::unique_ptr<SkCodec> codec = SkPngDecoder::Decode(std::move(encoded), nullptr);
  if (!codec || codec->getEncodedFormat() != SkEncodedImageFormat::kPNG
      || !ClipboardImageDimensionsAreValid(
          codec->dimensions().width(), codec->dimensions().height())) {
    return false;
  }
  const SkImageInfo info = SkImageInfo::MakeN32Premul(
      codec->dimensions().width(), codec->dimensions().height());
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(info)
      || codec->getPixels(info, bitmap.getPixels(), bitmap.rowBytes())
          != SkCodec::kSuccess) {
    return false;
  }
  if (decoded)
    *decoded = std::move(bitmap);
  return true;
}

void AppendUnique(std::vector<std::u16string>* formats,
                  std::u16string format) {
  if (std::find(formats->begin(), formats->end(), format) == formats->end())
    formats->push_back(std::move(format));
}

std::optional<ui::ClipboardFormatType> FormatType(
    const std::string& format) {
  if (format == ui::kMimeTypeText)
    return ui::ClipboardFormatType::PlainTextType();
  if (format == ui::kMimeTypeHTML)
    return ui::ClipboardFormatType::HtmlType();
  if (format == ui::kMimeTypeRTF)
    return ui::ClipboardFormatType::RtfType();
  if (format == ui::kMimeTypePNG)
    return ui::ClipboardFormatType::PngType();
  if (format == ui::kMimeTypeURIList)
    return ui::ClipboardFormatType::FilenameType();
  return std::nullopt;
}

}  // namespace

void SetRendererClipboardBroker(ClipboardBroker broker) {
  std::lock_guard<std::mutex> lock(clipboard_broker_lock);
  if (broker)
    renderer_broker_required = true;
  clipboard_broker = std::move(broker);
}

ClipboardSequence GetClipboardSequence() {
  BrokerResult broker = BrokerRequest("sequence");
  if (broker.required) {
    ClipboardSequence sequence;
    const std::string* high = broker.succeeded
        ? broker.value.FindString("high") : nullptr;
    const std::string* low = broker.succeeded
        ? broker.value.FindString("low") : nullptr;
    if (!high || !low
        || !base::StringToUint64(*high, &sequence.high)
        || !base::StringToUint64(*low, &sequence.low)) {
      return {};
    }
    return sequence;
  }

  const auto& token = ui::Clipboard::GetForCurrentThread()->GetSequenceNumber(
      ui::ClipboardBuffer::kCopyPaste).value();
  return {token.GetHighForSerialization(), token.GetLowForSerialization()};
}

bool ClipboardHasText() {
  return ClipboardHasFormat(ui::kMimeTypeText);
}

bool ClipboardHasHtml() {
  return ClipboardHasFormat(ui::kMimeTypeHTML);
}

std::u16string ReadClipboardText() {
  BrokerResult broker = BrokerRequest("readText");
  if (broker.required) {
    const std::string* value = broker.succeeded
        ? broker.value.FindString("value") : nullptr;
    return value ? base::UTF8ToUTF16(*value) : std::u16string();
  }

  std::u16string text;
  ui::Clipboard::GetForCurrentThread()->ReadText(
      ui::ClipboardBuffer::kCopyPaste, nullptr, &text);
  return text;
}

ClipboardHtml ReadClipboardHtml() {
  BrokerResult broker = BrokerRequest("readHtml");
  if (broker.required) {
    ClipboardHtml html;
    if (!broker.succeeded)
      return html;
    if (const std::string* value = broker.value.FindString("markup"))
      html.markup = base::UTF8ToUTF16(*value);
    if (const std::string* value = broker.value.FindString("sourceUrl"))
      html.source_url = *value;
    html.fragment_start = static_cast<uint32_t>(
        broker.value.FindInt("fragmentStart").value_or(0));
    html.fragment_end = static_cast<uint32_t>(
        broker.value.FindInt("fragmentEnd").value_or(0));
    if (html.fragment_start > html.fragment_end
        || html.fragment_end > html.markup.size()) {
      return {};
    }
    return html;
  }

  ClipboardHtml html;
  ui::Clipboard::GetForCurrentThread()->ReadHTML(
      ui::ClipboardBuffer::kCopyPaste, nullptr, &html.markup,
      &html.source_url, &html.fragment_start, &html.fragment_end);
  if (html.fragment_start > html.fragment_end
      || html.fragment_end > html.markup.size()) {
    return {};
  }
  return html;
}

std::vector<uint8_t> ReadClipboardPng() {
  BrokerResult broker = BrokerRequest("readPng");
  if (broker.required) {
    base::Value::BlobStorage* data = broker.succeeded
        ? broker.value.FindBlob("data") : nullptr;
    // The authenticated browser already performed bounded PNG decode.
    if (!data || data->empty() || data->size() > kMaximumClipboardImageBytes)
      return {};
    return std::move(*data);
  }

  std::vector<uint8_t> png;
  ui::Clipboard::GetForCurrentThread()->ReadPng(
      ui::ClipboardBuffer::kCopyPaste, nullptr,
      base::BindOnce(
          [](std::vector<uint8_t>* output, const std::vector<uint8_t>& value) {
            *output = value;
          },
          &png));
  // ClipboardWin validates PNG pixels or converts a bounded native DIB. Avoid
  // decoding those pixels a second time; bound the encoded DIB conversion too.
  if (png.size() > kMaximumClipboardImageBytes)
    return {};
  return png;
}

std::vector<std::u16string> AvailableClipboardFormats() {
  BrokerResult broker = BrokerRequest("availableFormats");
  if (broker.required) {
    std::vector<std::u16string> formats;
    if (!broker.succeeded)
      return formats;
    if (const auto* values = broker.value.FindList("formats")) {
      for (const auto& value : *values) {
        if (value.is_string())
          AppendUnique(&formats, base::UTF8ToUTF16(value.GetString()));
      }
    }
    return formats;
  }

  ui::Clipboard* clipboard = ui::Clipboard::GetForCurrentThread();
  std::vector<std::u16string> formats;
  clipboard->ReadAvailableTypes(
      ui::ClipboardBuffer::kCopyPaste, nullptr, &formats);
  if (clipboard->IsFormatAvailable(ui::ClipboardFormatType::PngType(),
          ui::ClipboardBuffer::kCopyPaste, nullptr)) {
    AppendUnique(&formats, base::UTF8ToUTF16(ui::kMimeTypePNG));
  }
  return formats;
}

bool ClipboardHasFormat(const std::string& format) {
  if (RendererBrokerRequired()) {
    const std::u16string requested = base::UTF8ToUTF16(format);
    const auto formats = AvailableClipboardFormats();
    return std::find(formats.begin(), formats.end(), requested) != formats.end();
  }
  std::optional<ui::ClipboardFormatType> type = FormatType(format);
  return type && ui::Clipboard::GetForCurrentThread()->IsFormatAvailable(
      *type, ui::ClipboardBuffer::kCopyPaste, nullptr);
}

std::string ReadClipboardData(const std::string& format) {
  if (RendererBrokerRequired())
    return {};
  std::optional<ui::ClipboardFormatType> type = FormatType(format);
  if (!type)
    return {};
  std::string data;
  ui::Clipboard::GetForCurrentThread()->ReadData(*type, nullptr, &data);
  return data;
}

std::string ReadClipboardRtf() {
  if (RendererBrokerRequired())
    return {};
  std::string rtf;
  ui::Clipboard::GetForCurrentThread()->ReadRTF(
      ui::ClipboardBuffer::kCopyPaste, nullptr, &rtf);
  return rtf;
}

void ReadClipboardBookmark(std::u16string* title, std::string* url) {
  if (RendererBrokerRequired()) {
    title->clear();
    url->clear();
    return;
  }
  ui::Clipboard::GetForCurrentThread()->ReadBookmark(nullptr, title, url);
}

bool WriteClipboard(const ClipboardContent& content) {
  bool broker_required = false;
  ClipboardBroker broker = SnapshotClipboardBroker(&broker_required);
  if (broker_required) {
    if (!broker || content.rtf || content.bookmark_title
        || content.bookmark_url
        || (content.png
            && (content.png->empty()
                || content.png->size() > kMaximumClipboardImageBytes))) {
      return false;
    }
    base::Value::Dict request;
    request.Set("operation", "write");
    if (content.text)
      request.Set("text", base::UTF16ToUTF8(*content.text));
    if (content.html)
      request.Set("html", base::UTF16ToUTF8(*content.html));
    if (content.html_source_url)
      request.Set("htmlSourceUrl", *content.html_source_url);
    if (content.png)
      request.Set("png", base::Value(std::vector<uint8_t>(*content.png)));
    base::Value::Dict result;
    return broker(std::move(request), &result)
        && result.FindBool("value").value_or(false);
  }

  SkBitmap bitmap;
  if (content.png && !IsValidPng(*content.png, &bitmap))
    return false;

  {
    ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
    if (content.text)
      writer.WriteText(*content.text);
    if (content.html) {
      writer.WriteHTML(*content.html,
          content.html_source_url ? *content.html_source_url : std::string());
    }
    if (content.rtf)
      writer.WriteRTF(*content.rtf);
    if (content.bookmark_title && content.bookmark_url)
      writer.WriteBookmark(*content.bookmark_title, *content.bookmark_url);
    if (content.png)
      writer.WriteImage(bitmap);
  }
  return true;
}

bool WriteClipboardData(const std::string& format,
                        const std::vector<uint8_t>& data) {
  if (RendererBrokerRequired())
    return false;
  std::optional<ui::ClipboardFormatType> type = FormatType(format);
  if (!type)
    return false;
  mojo_base::BigBuffer copy(data.size());
  std::copy(data.begin(), data.end(), copy.data());
  ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
  writer.WriteData(base::UTF8ToUTF16(format), std::move(copy));
  return true;
}

void ClearClipboard() {
  if (RendererBrokerRequired())
    return;
  ui::Clipboard::GetForCurrentThread()->Clear(
      ui::ClipboardBuffer::kCopyPaste);
}

}  // namespace mini_electron
