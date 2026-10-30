// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef MINI_ELECTRON_RENDERER_MEMORY_MAC_H_
#define MINI_ELECTRON_RENDERER_MEMORY_MAC_H_

#include <cstdint>
#include <algorithm>

namespace atom {

enum class MacMemoryPressureLevel { kUnknown, kNormal, kWarning, kCritical };

struct MacRendererMemoryStatus {
    uint64_t physical_bytes = 0;
    uint64_t available_bytes = 0;
    uint64_t footprint_bytes = 0;
    MacMemoryPressureLevel pressure = MacMemoryPressureLevel::kUnknown;
};

// Raw Mach free_count includes speculative; staggered counters can report more
// speculative pages than free pages. Match base::SysInfo's saturating sum.
inline uint64_t MacAvailableMemoryBytes(uint64_t free_pages,
    uint64_t speculative_pages, uint64_t page_size)
{
    return std::max(free_pages, speculative_pages) * page_size;
}

// Reads a fresh snapshot without caching failed samples. Pressure is optional;
// true requires valid physical, available, and process footprint measurements.
bool ReadMacRendererMemoryStatus(MacRendererMemoryStatus* output);

} // namespace atom

#endif // MINI_ELECTRON_RENDERER_MEMORY_MAC_H_
