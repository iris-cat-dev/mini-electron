// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef PLATFORM_WINDOWS_COMPOSITOR_DIAGNOSTICS_H_
#define PLATFORM_WINDOWS_COMPOSITOR_DIAGNOSTICS_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "base/values.h"

namespace mini_electron::win {

// Process-lifetime generations, not addresses or HANDLEs. Pair a producer
// identity with rendererPid; browser identities belong to the browser process.
inline uint64_t NextCompositorDiagnosticId() {
  static std::atomic<uint64_t> next{0};
  return next.fetch_add(1, std::memory_order_relaxed) + 1;
}

// Numeric records only: no allocation, resource ownership, or I/O on append.
// Callers provide sequence affinity/locking. Strings must be static literals.
template <size_t Capacity = 64>
class CompositorDiagnosticEvents {
 public:
  void Add(uint64_t owner, const char* type, uint64_t identity = 0,
           uint64_t source = 0, uint64_t frame = 0, int layer = 0,
           int width = 0, int height = 0, int32_t result = 0) {
    entries_[next_++ % Capacity] = {
        owner, ++sequence_, base::TimeTicks::Now().since_origin().InMicroseconds(),
        type, identity, source, frame, layer, width, height, result};
  }

  void Erase(uint64_t owner) {
    for (auto& entry : entries_) {
      if (entry.owner == owner)
        entry.type = nullptr;
    }
  }

  void AppendTo(base::Value::Dict& value, uint64_t owner) const {
    // Bounds describe this ring before owner filtering. A sequence gap can
    // also belong to another window/view, not necessarily a lost owner event.
    value.Set("eventHistoryCapacity", static_cast<int>(Capacity));
    value.Set("eventHistoryFirstSeq", base::NumberToString(
        sequence_ > Capacity ? sequence_ - Capacity + 1 : sequence_ ? 1 : 0));
    value.Set("eventHistoryLastSeq", base::NumberToString(sequence_));
    value.Set("eventHistoryOverwrites", base::NumberToString(
        sequence_ > Capacity ? sequence_ - Capacity : 0));
    value.Set("events", ToValue(owner));
  }

 private:

  base::Value::List ToValue(uint64_t owner) const {
    base::Value::List values;
    const size_t count = next_ < Capacity ? next_ : Capacity;
    const size_t start = next_ < Capacity ? 0 : next_ % Capacity;
    for (size_t index = 0; index < count; ++index) {
      const auto& entry = entries_[(start + index) % Capacity];
      if (!entry.type || entry.owner != owner)
        continue;
      base::Value::Dict value;
      value.Set("seq", base::NumberToString(entry.sequence));
      value.Set("timeUs", base::NumberToString(entry.time_us));
      value.Set("type", entry.type);
      value.Set("identity", base::NumberToString(entry.identity));
      value.Set("sourceId", base::NumberToString(entry.source));
      value.Set("frameId", base::NumberToString(entry.frame));
      value.Set("layerId", entry.layer);
      value.Set("width", entry.width);
      value.Set("height", entry.height);
      value.Set("result", entry.result);
      values.Append(std::move(value));
    }
    return values;
  }

  struct Entry {
    uint64_t owner = 0;
    uint64_t sequence = 0;
    int64_t time_us = 0;
    const char* type = nullptr;
    uint64_t identity = 0;
    uint64_t source = 0;
    uint64_t frame = 0;
    int layer = 0;
    int width = 0;
    int height = 0;
    int32_t result = 0;
  };
  std::array<Entry, Capacity> entries_{};
  size_t next_ = 0;
  uint64_t sequence_ = 0;
};

}  // namespace mini_electron::win

#endif  // PLATFORM_WINDOWS_COMPOSITOR_DIAGNOSTICS_H_
