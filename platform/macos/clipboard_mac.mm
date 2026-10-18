// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>

#include "platform/clipboard.h"
#include <algorithm>
#include <mutex>
#include <utility>
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"

namespace mini_electron {
namespace {

std::mutex clipboard_broker_lock;
ClipboardBroker clipboard_broker;
bool renderer_broker_required = false;


bool IsDecodableImage(NSData* data, CFStringRef expected_type) {
  if (!data || data.length == 0
      || data.length > kMaximumClipboardImageBytes) {
    return false;
  }
  CGImageSourceRef source = CGImageSourceCreateWithData(
      (__bridge CFDataRef)data, nullptr);
  CFStringRef actual_type = source ? CGImageSourceGetType(source) : nullptr;
  if (!source || CGImageSourceGetCount(source) != 1 || !actual_type
      || !CFEqual(actual_type, expected_type)) {
    if (source)
      CFRelease(source);
    return false;
  }
  CFDictionaryRef properties =
      CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr);
  int64_t width = 0;
  int64_t height = 0;
  bool dimensions_valid = false;
  if (properties) {
    CFNumberRef width_value = static_cast<CFNumberRef>(
        CFDictionaryGetValue(properties, kCGImagePropertyPixelWidth));
    CFNumberRef height_value = static_cast<CFNumberRef>(
        CFDictionaryGetValue(properties, kCGImagePropertyPixelHeight));
    dimensions_valid = width_value && height_value
        && CFGetTypeID(width_value) == CFNumberGetTypeID()
        && CFGetTypeID(height_value) == CFNumberGetTypeID()
        && CFNumberGetValue(width_value, kCFNumberSInt64Type, &width)
        && CFNumberGetValue(height_value, kCFNumberSInt64Type, &height)
        && width > 0 && height > 0
        && width <= static_cast<int64_t>(kMaximumClipboardImageDimension)
        && height <= static_cast<int64_t>(kMaximumClipboardImageDimension)
        && static_cast<uint64_t>(width)
            <= kMaximumClipboardImagePixels / static_cast<uint64_t>(height);
    CFRelease(properties);
  }
  CGImageRef image = dimensions_valid
      ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
  const bool valid = image
      && CGImageGetWidth(image) == static_cast<size_t>(width)
      && CGImageGetHeight(image) == static_cast<size_t>(height);
  if (image)
    CGImageRelease(image);
  CFRelease(source);
  return valid;
}

std::vector<uint8_t> CopyValidatedPng(NSData* data) {
  if (!IsDecodableImage(data, CFSTR("public.png")))
    return {};
  const auto* bytes = static_cast<const uint8_t*>(data.bytes);
  return std::vector<uint8_t>(bytes, bytes + data.length);
}

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

std::u16string ToUTF16(NSString* value) {
  if (!value)
    return {};
  std::u16string result([value length], u'\0');
  if (!result.empty()) {
    [value getCharacters:reinterpret_cast<unichar*>(result.data())
                   range:NSMakeRange(0, [value length])];
  }
  return result;
}

NSString* FromUTF16(const std::u16string& value) {
  if (value.empty())
    return @"";
  return [[NSString alloc]
      initWithCharacters:reinterpret_cast<const unichar*>(value.data())
                  length:value.size()];
}

std::u16string TypeName(NSString* value) {
  return ToUTF16(value);
}

void AppendUnique(std::vector<std::u16string>* formats,
                  std::u16string value) {
  if (std::find(formats->begin(), formats->end(), value) == formats->end())
    formats->push_back(std::move(value));
}

NSPasteboardType PasteboardType(const std::string& format) {
  if (format == "text/plain")
    return NSPasteboardTypeString;
  if (format == "text/html")
    return NSPasteboardTypeHTML;
  if (format == "text/rtf")
    return NSPasteboardTypeRTF;
  if (format == "image/png")
    return NSPasteboardTypePNG;
  if (format == "text/uri-list")
    return NSPasteboardTypeURL;
  return [NSString stringWithUTF8String:format.c_str()] ?: @"";
}

}  // namespace

void SetRendererClipboardBroker(ClipboardBroker broker) {
  std::lock_guard<std::mutex> lock(clipboard_broker_lock);
  if (broker)
    renderer_broker_required = true;
  clipboard_broker = std::move(broker);
}

ClipboardSequence GetClipboardSequence() {
  auto broker = BrokerRequest("sequence");
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
  @autoreleasepool {
    return {static_cast<uint64_t>(
                [[NSPasteboard generalPasteboard] changeCount]) + 1,
            0};
  }
}

bool ClipboardHasText() {
  return ClipboardHasFormat("text/plain");
}

bool ClipboardHasHtml() {
  return ClipboardHasFormat("text/html");
}

std::u16string ReadClipboardText() {
  auto broker = BrokerRequest("readText");
  if (broker.required) {
    const std::string* value = broker.succeeded
        ? broker.value.FindString("value") : nullptr;
    return value ? base::UTF8ToUTF16(*value) : std::u16string();
  }
  @autoreleasepool {
    return ToUTF16([[NSPasteboard generalPasteboard]
        stringForType:NSPasteboardTypeString]);
  }
}

ClipboardHtml ReadClipboardHtml() {
  auto broker = BrokerRequest("readHtml");
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
  @autoreleasepool {
    ClipboardHtml html;
    html.markup = ToUTF16([[NSPasteboard generalPasteboard]
        stringForType:NSPasteboardTypeHTML]);
    html.fragment_end = static_cast<uint32_t>(html.markup.size());
    return html;
  }
}


std::vector<uint8_t> ReadClipboardPng() {
  auto broker = BrokerRequest("readPng");
  if (broker.required) {
    base::Value::BlobStorage* data = broker.succeeded
        ? broker.value.FindBlob("data") : nullptr;
    if (!data)
      return {};
    @autoreleasepool {
      NSData* image =
          [NSData dataWithBytes:data->data() length:data->size()];
      return CopyValidatedPng(image);
    }
  }
  @autoreleasepool {
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    NSData* png = [pasteboard dataForType:NSPasteboardTypePNG];
    if (png)
      return CopyValidatedPng(png);
    NSData* tiff = [pasteboard dataForType:NSPasteboardTypeTIFF];
    if (!IsDecodableImage(tiff, CFSTR("public.tiff")))
      return {};
    NSBitmapImageRep* bitmap = [NSBitmapImageRep imageRepWithData:tiff];
    NSData* converted = [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                             properties:@{}];
    return CopyValidatedPng(converted);
  }
}

std::vector<std::u16string> AvailableClipboardFormats() {
  auto broker = BrokerRequest("availableFormats");
  if (broker.required) {
    std::vector<std::u16string> formats;
    if (!broker.succeeded)
      return formats;
    if (const auto* values = broker.value.FindList("formats")) {
      for (const auto& value : *values) {
        if (value.is_string())
          formats.push_back(base::UTF8ToUTF16(value.GetString()));
      }
    }
    return formats;
  }
  @autoreleasepool {
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    std::vector<std::u16string> formats;
    if ([pasteboard canReadItemWithDataConformingToTypes:
                        @[ NSPasteboardTypeString ]]) {
      AppendUnique(&formats, u"text/plain");
    }
    if ([pasteboard canReadItemWithDataConformingToTypes:
                        @[ NSPasteboardTypeHTML ]]) {
      AppendUnique(&formats, u"text/html");
    }
    if ([pasteboard canReadItemWithDataConformingToTypes:
                        @[ NSPasteboardTypeRTF ]]) {
      AppendUnique(&formats, u"text/rtf");
    }
    if ([pasteboard canReadItemWithDataConformingToTypes:
                        @[ NSPasteboardTypePNG, NSPasteboardTypeTIFF ]]) {
      AppendUnique(&formats, u"image/png");
    }
    for (NSPasteboardType type in pasteboard.types)
      AppendUnique(&formats, TypeName(type));
    return formats;
  }
}

bool ClipboardHasFormat(const std::string& format) {
  if (RendererBrokerRequired()) {
    const std::u16string requested = base::UTF8ToUTF16(format);
    const auto formats = AvailableClipboardFormats();
    return std::find(formats.begin(), formats.end(), requested) != formats.end();
  }
  @autoreleasepool {
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    NSPasteboardType type = PasteboardType(format);
    if (type.length == 0)
      return false;
    NSMutableArray<NSPasteboardType>* types = [NSMutableArray arrayWithObject:type];
    if ([type isEqualToString:NSPasteboardTypePNG])
      [types addObject:NSPasteboardTypeTIFF];
    if ([type isEqualToString:NSPasteboardTypeURL])
      [types addObject:NSPasteboardTypeFileURL];
    return [pasteboard availableTypeFromArray:types] != nil;
  }
}

std::string ReadClipboardData(const std::string& format) {
  if (RendererBrokerRequired())
    return {};
  @autoreleasepool {
    NSPasteboardType type = PasteboardType(format);
    if (type.length == 0)
      return {};
    NSData* data = [[NSPasteboard generalPasteboard] dataForType:type];
    if (!data)
      return {};
    return std::string(static_cast<const char*>(data.bytes), data.length);
  }
}

std::string ReadClipboardRtf() {
  return ReadClipboardData("text/rtf");
}

void ReadClipboardBookmark(std::u16string* title, std::string* url) {
  if (RendererBrokerRequired()) {
    title->clear();
    url->clear();
    return;
  }
  @autoreleasepool {
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    NSString* address = [pasteboard stringForType:NSPasteboardTypeURL];
    *title = ToUTF16([pasteboard stringForType:@"public.url-name"]);
    *url = address.UTF8String ?: "";
  }
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
  @autoreleasepool {
    NSData* image = nil;
    if (content.png) {
      if (content.png->empty() || content.png->size() > kMaximumClipboardImageBytes)
        return false;
      image = [NSData dataWithBytes:content.png->data()
                            length:content.png->size()];
      if (!IsDecodableImage(image, CFSTR("public.png")))
        return false;
    }
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
    if (content.text &&
        ![item setString:FromUTF16(*content.text)
                 forType:NSPasteboardTypeString]) {
      return false;
    }
    if (content.html &&
        ![item setString:FromUTF16(*content.html) forType:NSPasteboardTypeHTML]) {
      return false;
    }
    if (content.rtf) {
      NSData* rtf = [NSData dataWithBytes:content.rtf->data()
                                   length:content.rtf->size()];
      if (![item setData:rtf forType:NSPasteboardTypeRTF])
        return false;
    }
    if (content.bookmark_title && content.bookmark_url) {
      NSString* url =
          [NSString stringWithUTF8String:content.bookmark_url->c_str()] ?: @"";
      if (![item setString:url forType:NSPasteboardTypeURL] ||
          ![item setString:FromUTF16(*content.bookmark_title)
                   forType:@"public.url-name"]) {
        return false;
      }
      if (!content.text && ![item setString:url forType:NSPasteboardTypeString])
        return false;
    }
    if (image) {
      if (![item setData:image forType:NSPasteboardTypePNG])
        return false;
    }
    return item.types.count == 0 || [pasteboard writeObjects:@[ item ]];
  }
}

bool WriteClipboardData(const std::string& format,
                        const std::vector<uint8_t>& data) {
  if (RendererBrokerRequired())
    return false;
  @autoreleasepool {
    NSPasteboardType type = PasteboardType(format);
    if (type.length == 0)
      return false;
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    return [pasteboard setData:[NSData dataWithBytes:data.data()
                                              length:data.size()]
                       forType:type];
  }
}

void ClearClipboard() {
  if (RendererBrokerRequired())
    return;
  @autoreleasepool {
    [[NSPasteboard generalPasteboard] clearContents];
  }
}
}  // namespace mini_electron
