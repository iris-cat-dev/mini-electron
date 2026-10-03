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

bool WriteClipboard(const std::u16string* text,
                    const std::vector<uint8_t>* png) {
  @autoreleasepool {
    if (!text && (!png || png->empty()))
      return false;
    NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
    if (text && ![item setString:FromUTF16(*text)
                        forType:NSPasteboardTypeString]) {
      return false;
    }
    if (png && !png->empty()) {
      NSData* image = [NSData dataWithBytes:png->data() length:png->size()];
      if (![item setData:image forType:NSPasteboardTypePNG])
        return false;
    }
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    return [pasteboard writeObjects:@[ item ]];
  }
}

void ClearClipboard() {
  @autoreleasepool {
    [[NSPasteboard generalPasteboard] clearContents];
  }
}
}  // namespace mini_electron::mac
