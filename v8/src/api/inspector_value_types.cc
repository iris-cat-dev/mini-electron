// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "include/v8-inspector.h"

#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
#error "This file is only for the DevTools-free mini-electron target."
#endif

#include <charconv>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace v8_inspector {
namespace {

class OwnedStringBuffer final : public StringBuffer {
 public:
  explicit OwnedStringBuffer(StringView source) : is_8_bit_(source.is8Bit()) {
    if (source.length() == 0) {
      is_8_bit_ = true;
      return;
    }
    if (is_8_bit_) {
      characters8_.assign(source.characters8(),
                          source.characters8() + source.length());
    } else {
      characters16_.assign(source.characters16(),
                           source.characters16() + source.length());
    }
  }

  explicit OwnedStringBuffer(std::vector<uint8_t> characters)
      : is_8_bit_(true), characters8_(std::move(characters)) {}

  StringView string() const override {
    if (is_8_bit_)
      return StringView(characters8_.data(), characters8_.size());
    return StringView(characters16_.data(), characters16_.size());
  }

 private:
  bool is_8_bit_;
  std::vector<uint8_t> characters8_;
  std::vector<uint16_t> characters16_;
};

}  // namespace

std::unique_ptr<StringBuffer> StringBuffer::create(StringView source) {
  return std::make_unique<OwnedStringBuffer>(source);
}

std::unique_ptr<StringBuffer> V8DebuggerId::toString() const {
  char characters[48];
  auto first =
      std::to_chars(characters, characters + sizeof(characters), m_first);
  *first.ptr++ = '.';
  auto second =
      std::to_chars(first.ptr, characters + sizeof(characters), m_second);
  return std::make_unique<OwnedStringBuffer>(std::vector<uint8_t>(
      reinterpret_cast<uint8_t*>(characters),
      reinterpret_cast<uint8_t*>(second.ptr)));
}

bool V8DebuggerId::isValid() const {
  return m_first != 0 || m_second != 0;
}

std::pair<int64_t, int64_t> V8DebuggerId::pair() const {
  return {m_first, m_second};
}

V8StackTraceId::V8StackTraceId() : id(0), debugger_id({0, 0}) {}

V8StackTraceId::V8StackTraceId(
    uintptr_t id,
    const std::pair<int64_t, int64_t> debugger_id)
    : id(id), debugger_id(debugger_id) {}

V8StackTraceId::V8StackTraceId(
    uintptr_t id,
    const std::pair<int64_t, int64_t> debugger_id,
    bool should_pause)
    : id(id), debugger_id(debugger_id), should_pause(should_pause) {}

bool V8StackTraceId::IsInvalid() const {
  return id == 0;
}

}  // namespace v8_inspector
