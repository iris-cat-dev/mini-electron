// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef PLATFORM_WINDOWS_DCOMP_PRESENTER_H_
#define PLATFORM_WINDOWS_DCOMP_PRESENTER_H_

#include <windows.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "src/platform/windows/d3d_compositor.h"

struct ID3D11Device;
struct ID3D11Texture2D;

namespace mini_electron::win {

// Returns whether the browser can create the hardware D3D11 device, shared
// fences, and DirectComposition device needed by native presentation. Call on
// the browser UI thread; the result is cached for the shared device's lifetime.
bool CanUseD3DPresentation();

// Imports renderer-owned D3D12 frames into a browser-owned retained D3D11
// texture. All methods are browser-UI-thread-affine.
class D3DFrameConsumer {
 public:
  D3DFrameConsumer();
  ~D3DFrameConsumer();

  D3DFrameConsumer(const D3DFrameConsumer&) = delete;
  D3DFrameConsumer& operator=(const D3DFrameConsumer&) = delete;

  // |renderer_process| must be a real process handle with DUP_HANDLE access.
  // Self-import is permitted only for the explicitly selected in-process mode.
  // On success the copy and the odd release-fence signal have both been queued.
  bool Import(HANDLE renderer_process, const D3DFrame& frame, bool in_process);

  uint64_t source_id() const;
  uint64_t frame_id() const;
  uint64_t imported_frames() const;
  int width() const;
  int height() const;

  // Native access is for DCompPresenter only. The returned objects remain owned
  // by this consumer and must only be used on the browser UI thread.
  ID3D11Texture2D* texture() const;
  ID3D11Device* device() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Bounds and clip use client-area pixel coordinates. An empty clip means the
// complete layer bounds. Layers are ordered bottom-to-top.
struct DCompLayer {
  int id = 0;
  std::shared_ptr<D3DFrameConsumer> frame;
  RECT bounds{};
  RECT clip{};
  // Optional premultiplied, top-down CPU fallback. It is used only when frame
  // is null, and is uploaded only when generation or dimensions change.
  const void* rgba = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;
  uint64_t generation = 0;
};

// Premultiplied, top-down BGRA8 caption pixels. The pixel memory is only read
// when generation or dimensions change.
struct DCompOverlay {
  const void* bgra = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;
  int x = 0;
  int y = 0;
  uint64_t generation = 0;
};

class DCompPresenter {
 public:
  static std::unique_ptr<DCompPresenter> Create(HWND window);
  ~DCompPresenter();

  DCompPresenter(const DCompPresenter&) = delete;
  DCompPresenter& operator=(const DCompPresenter&) = delete;

  // Updates and commits the native visual tree. Transient occlusion and a full
  // flip queue preserve existing content and are retried by a later call.
  bool Present(const std::vector<DCompLayer>& layers,
               const DCompOverlay& overlay);

  uint64_t presented_frame_id(int id) const;

 private:
  struct Impl;
  explicit DCompPresenter(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace mini_electron::win

#endif  // PLATFORM_WINDOWS_DCOMP_PRESENTER_H_
