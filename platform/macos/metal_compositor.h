// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef PLATFORM_MACOS_METAL_COMPOSITOR_H_
#define PLATFORM_MACOS_METAL_COMPOSITOR_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "base/functional/callback.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/geometry/rect_f.h"

namespace viz { class SoftwareOutputDevice; }

namespace mini_electron::mac {

// A completed GPU backing, not a physical display/scanout acknowledgment.
struct MetalFrame {
  uint32_t context_id = 0;
  int width = 0;
  int height = 0;
  float scale_factor = 1.f;
  uint64_t frame_number = 0;
  int error = 0;
};
struct MetalGuestLayer {
  int id = 0;
  uint32_t context_id = 0;
  int pixel_width = 0;
  int pixel_height = 0;
  float scale_factor = 1.f;
  gfx::RectF bounds;
  uint64_t generation = 0;
  SkBitmap cpu_bitmap;  // Only for a guest that selected the CPU fallback.
};

// Open the device/queue before Seatbelt lockdown. Shader compilation is allowed
// through its specifically named XPC service; no broad driver access is added.
bool PrepareMetalDevice();
std::unique_ptr<viz::SoftwareOutputDevice> CreateMetalOutputDevice(
    int64_t web_view_id, bool transparent);

// Register/unregister on the engine UI sequence; callbacks run there as well.
void SetMetalFrameCallback(int64_t web_view_id,
    base::RepeatingCallback<void(const MetalFrame&)> callback);

// Explicit capture boundary. Never used by ordinary native presentation.
SkBitmap CopyMetalFrame(int64_t web_view_id);
void UpdateMetalGuestLayers(int64_t web_view_id,
    std::vector<MetalGuestLayer> guests);

}  // namespace mini_electron::mac
#endif  // PLATFORM_MACOS_METAL_COMPOSITOR_H_
