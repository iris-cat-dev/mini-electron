// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef PLATFORM_MACOS_CLIPBOARD_MAC_H_
#define PLATFORM_MACOS_CLIPBOARD_MAC_H_

#include <cstdint>
#include <string>
#include <vector>

namespace mini_electron::mac {

uint64_t ClipboardSequenceNumber();
bool ClipboardHasText();
bool ClipboardHasHtml();
std::u16string ReadClipboardText();
std::u16string ReadClipboardHtml();
std::vector<uint8_t> ReadClipboardPng();
std::vector<std::u16string> AvailableClipboardFormats();

// Writes one pasteboard item so text and image remain available together.
bool WriteClipboard(const std::u16string* text,
                    const std::vector<uint8_t>* png);
void ClearClipboard();

}  // namespace mini_electron::mac

#endif  // PLATFORM_MACOS_CLIPBOARD_MAC_H_
