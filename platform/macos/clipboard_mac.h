// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef PLATFORM_MACOS_CLIPBOARD_MAC_H_
#define PLATFORM_MACOS_CLIPBOARD_MAC_H_

#include <cstdint>
#include <string>
#include <vector>

namespace mini_electron::mac {

// Representations written together as one pasteboard item. Null members are
// omitted; a bookmark requires both its title and URL.
struct ClipboardContent {
  const std::u16string* text = nullptr;
  const std::u16string* html = nullptr;
  const std::string* rtf = nullptr;
  const std::u16string* bookmark_title = nullptr;
  const std::string* bookmark_url = nullptr;
  const std::vector<uint8_t>* png = nullptr;
};

uint64_t ClipboardSequenceNumber();
bool ClipboardHasText();
bool ClipboardHasHtml();
std::u16string ReadClipboardText();
std::u16string ReadClipboardHtml();
std::string ReadClipboardRtf();
void ReadClipboardBookmark(std::u16string* title, std::string* url);
std::vector<uint8_t> ReadClipboardPng();
std::vector<std::u16string> AvailableClipboardFormats();

// |format| is a MIME type (text/plain, text/html, text/rtf, image/png,
// text/uri-list) or a raw pasteboard type such as public.file-url.
bool ClipboardHasFormat(const std::string& format);
std::string ReadClipboardData(const std::string& format);

// Each write replaces the pasteboard contents.
bool WriteClipboard(const ClipboardContent& content);
bool WriteClipboardData(const std::string& format,
                        const std::vector<uint8_t>& data);
void ClearClipboard();

}  // namespace mini_electron::mac

#endif  // PLATFORM_MACOS_CLIPBOARD_MAC_H_
