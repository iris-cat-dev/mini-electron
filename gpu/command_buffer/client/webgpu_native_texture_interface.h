// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef GPU_COMMAND_BUFFER_CLIENT_WEBGPU_NATIVE_TEXTURE_INTERFACE_H_
#define GPU_COMMAND_BUFFER_CLIENT_WEBGPU_NATIVE_TEXTURE_INTERFACE_H_

#include <cstddef>
#include "gpu/command_buffer/client/webgpu_interface.h"

namespace gpu::webgpu {

// Renderer-local Dawn textures, separate from Chromium SharedImage mailboxes.
// All calls run on the WebGPU channel's owning sequence. Returned wire handles
// own one reference; ReleaseCanvasTexture only releases the native bridge record.
class NativeTextureInterface {
public:
    virtual ~NativeTextureInterface() = default;
    virtual ReservedTexture CreateCanvasTexture(WGPUDevice device, const WGPUTextureDescriptor& descriptor) = 0;
    // Copy actual GPU pixels to caller-owned memory in the texture's native
    // format and top-left row order. row_bytes must fit a full unpadded row.
    // No mapped staging storage escapes this synchronous operation.
    virtual bool ReadCanvasTexture(const ReservedTexture& texture, void* pixels, size_t row_bytes) = 0;
    virtual void ReleaseCanvasTexture(const ReservedTexture& texture) = 0;
};

} // namespace gpu::webgpu

#endif // GPU_COMMAND_BUFFER_CLIENT_WEBGPU_NATIVE_TEXTURE_INTERFACE_H_
