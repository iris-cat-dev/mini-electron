// Copyright 2026 The miniblink132 Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef PLATFORM_MACOS_CLIPBOARD_MAC_H_
#define PLATFORM_MACOS_CLIPBOARD_MAC_H_

#include <cstdint>
#include <string>

namespace miniblink::mac {

uint64_t ClipboardSequenceNumber();
bool ClipboardHasText();
bool ClipboardHasHtml();
std::u16string ReadClipboardText();
std::u16string ReadClipboardHtml();

}  // namespace miniblink::mac

#endif  // PLATFORM_MACOS_CLIPBOARD_MAC_H_
