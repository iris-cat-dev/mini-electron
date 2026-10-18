// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef PLATFORM_CLIPBOARD_H_
#define PLATFORM_CLIPBOARD_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "base/values.h"

namespace mini_electron {

using ClipboardBroker =
    std::function<bool(base::Value::Dict, base::Value::Dict*)>;

// Renderer processes install an authenticated synchronous browser transport.
// Once installed, clearing it fails closed; direct platform clipboard access is
// never restored in that process.
void SetRendererClipboardBroker(ClipboardBroker broker);

inline constexpr size_t kMaximumClipboardImageBytes = 32 * 1024 * 1024;
inline constexpr int kMaximumClipboardImageDimension = 8192;
inline constexpr uint64_t kMaximumClipboardImagePixels = 40ULL * 1024 * 1024;

inline bool ClipboardImageDimensionsAreValid(int width, int height) {
  return width > 0 && height > 0
      && width <= kMaximumClipboardImageDimension
      && height <= kMaximumClipboardImageDimension
      && static_cast<uint64_t>(width)
          <= kMaximumClipboardImagePixels / static_cast<uint64_t>(height);
}

struct ClipboardSequence {
  uint64_t high = 0;
  uint64_t low = 0;
};

struct ClipboardHtml {
  std::u16string markup;
  std::string source_url;
  uint32_t fragment_start = 0;
  uint32_t fragment_end = 0;
};

// Representations written together in one native clipboard transaction. Null
// members are omitted; empty pointed-to strings remain real representations.
// A bookmark requires both its title and URL.
struct ClipboardContent {
  const std::u16string* text = nullptr;
  const std::u16string* html = nullptr;
  const std::string* html_source_url = nullptr;
  const std::string* rtf = nullptr;
  const std::u16string* bookmark_title = nullptr;
  const std::string* bookmark_url = nullptr;
  const std::vector<uint8_t>* png = nullptr;
};

ClipboardSequence GetClipboardSequence();
bool ClipboardHasText();
bool ClipboardHasHtml();
std::u16string ReadClipboardText();
ClipboardHtml ReadClipboardHtml();
std::string ReadClipboardRtf();
void ReadClipboardBookmark(std::u16string* title, std::string* url);
std::vector<uint8_t> ReadClipboardPng();
std::vector<std::u16string> AvailableClipboardFormats();

// |format| is a MIME type (text/plain, text/html, text/rtf, image/png,
// text/uri-list) or a raw native clipboard type.
bool ClipboardHasFormat(const std::string& format);
std::string ReadClipboardData(const std::string& format);

// Each write replaces the clipboard contents.
bool WriteClipboard(const ClipboardContent& content);
bool WriteClipboardData(const std::string& format,
                        const std::vector<uint8_t>& data);
void ClearClipboard();

}  // namespace mini_electron

#endif  // PLATFORM_CLIPBOARD_H_
