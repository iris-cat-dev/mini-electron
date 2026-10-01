// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#import <AppKit/AppKit.h>

#include "platform/macos/clipboard_mac.h"

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

}  // namespace mini_electron::mac
