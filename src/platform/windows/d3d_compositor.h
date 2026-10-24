// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef PLATFORM_WINDOWS_D3D_COMPOSITOR_H_
#define PLATFORM_WINDOWS_D3D_COMPOSITOR_H_

#include <cstdint>
#include <memory>

#include "base/functional/callback.h"
#include "third_party/skia/include/core/SkBitmap.h"

namespace viz {
class SoftwareOutputDevice;
}

namespace mini_electron::win {

// Publication occurs only after ready_value has completed on the producer
// fence; consumers still validate that fact before accepting the frame.
// Handles are renderer-process HANDLE values. The browser duplicates and
// validates them before use. They remain valid until this frame is acknowledged.
struct D3DFrame {
  uint64_t texture_handle = 0;
  uint64_t fence_handle = 0;
  uint64_t ready_value = 0;
  uint64_t frame_id = 0;
  // Monotonic within the renderer process and changed whenever the producer
  // texture is recreated. Raw HANDLE values alone are not resource identities.
  uint64_t source_id = 0;
  uint32_t adapter_luid_low = 0;
  int32_t adapter_luid_high = 0;
  int width = 0;
  int height = 0;
  float scale_factor = 1.f;
  int error = 0;
};

struct D3DCompositorInfo {
  bool gpu_active = false;
  uint64_t frames = 0;
  uint64_t readbacks = 0;
};

std::unique_ptr<viz::SoftwareOutputDevice> CreateD3DOutputDevice(
    int64_t view_id,
    bool transparent);

// Register/unregister on the engine UI sequence. Callbacks run there as well.
void SetD3DFrameCallback(
    int64_t view_id,
    base::RepeatingCallback<void(const D3DFrame&)> callback);

// Release a published frame after the browser has queued its consumer signal.
// Both identity components are required so a delayed acknowledgement cannot
// release a frame from a newly recreated producer texture.
void AcknowledgeD3DFrame(int64_t view_id,
                         uint64_t frame_id,
                         uint64_t source_id);

// Explicit capture boundary. Ordinary native presentation never reads pixels.
// The callback is posted back to the calling sequence after the
// device-sequence readback finishes. Its SkBitmap owns its pixels, and the
// scale factor is snapshotted on the device sequence with that readback. An
// empty bitmap reports an unavailable or failed capture.
using D3DFrameCopyCallback =
    base::OnceCallback<void(SkBitmap bitmap, float scale_factor)>;
void CopyD3DFrameAsync(int64_t view_id, D3DFrameCopyCallback callback);

D3DCompositorInfo GetD3DCompositorInfo(int64_t view_id);

}  // namespace mini_electron::win

#endif  // PLATFORM_WINDOWS_D3D_COMPOSITOR_H_
