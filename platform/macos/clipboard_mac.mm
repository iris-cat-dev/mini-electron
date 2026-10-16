// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#import <AppKit/AppKit.h>

#include "platform/macos/clipboard_mac.h"
#include <algorithm>
#include <utility>

namespace mini_electron::mac {
namespace {

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

uint64_t ClipboardSequenceNumber() {
  @autoreleasepool {
    return static_cast<uint64_t>([[NSPasteboard generalPasteboard] changeCount]);
  }
}

bool ClipboardHasText() {
  @autoreleasepool {
    return [[NSPasteboard generalPasteboard]
        canReadItemWithDataConformingToTypes:@[ NSPasteboardTypeString ]];
  }
}

bool ClipboardHasHtml() {
  @autoreleasepool {
    return [[NSPasteboard generalPasteboard]
        canReadItemWithDataConformingToTypes:@[ NSPasteboardTypeHTML ]];
  }
}

std::u16string ReadClipboardText() {
  @autoreleasepool {
    return ToUTF16([[NSPasteboard generalPasteboard]
        stringForType:NSPasteboardTypeString]);
  }
}

std::u16string ReadClipboardHtml() {
  @autoreleasepool {
    return ToUTF16([[NSPasteboard generalPasteboard]
        stringForType:NSPasteboardTypeHTML]);
  }
}


std::vector<uint8_t> ReadClipboardPng() {
  @autoreleasepool {
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    NSData* data = [pasteboard dataForType:NSPasteboardTypePNG];
    if (!data) {
      NSData* tiff = [pasteboard dataForType:NSPasteboardTypeTIFF];
      NSBitmapImageRep* bitmap =
          tiff ? [NSBitmapImageRep imageRepWithData:tiff] : nil;
      data = [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                  properties:@{}];
    }
    if (!data)
      return {};
    const auto* bytes = static_cast<const uint8_t*>(data.bytes);
    return std::vector<uint8_t>(bytes, bytes + data.length);
  }
}

std::vector<std::u16string> AvailableClipboardFormats() {
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
  @autoreleasepool {
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    NSString* address = [pasteboard stringForType:NSPasteboardTypeURL];
    *title = ToUTF16([pasteboard stringForType:@"public.url-name"]);
    *url = address.UTF8String ?: "";
  }
}

bool WriteClipboard(const ClipboardContent& content) {
  @autoreleasepool {
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
    if (content.png && !content.png->empty()) {
      NSData* image = [NSData dataWithBytes:content.png->data()
                                     length:content.png->size()];
      if (![item setData:image forType:NSPasteboardTypePNG])
        return false;
    }
    return item.types.count == 0 || [pasteboard writeObjects:@[ item ]];
  }
}

bool WriteClipboardData(const std::string& format,
                        const std::vector<uint8_t>& data) {
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
  @autoreleasepool {
    [[NSPasteboard generalPasteboard] clearContents];
  }
}
}  // namespace mini_electron::mac
