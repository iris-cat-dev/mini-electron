// Copyright 2026 mini-electron contributors.
#ifndef MINI_ELECTRON_RUNTIME_ENGINE_RENDERER_WEBGPU_CONTEXT_PROVIDER_H_
#define MINI_ELECTRON_RUNTIME_ENGINE_RENDERER_WEBGPU_CONTEXT_PROVIDER_H_

#include <memory>

#include "third_party/blink/public/platform/web_graphics_context_3d_provider.h"

namespace content {

// Discovers the native backend and warms its device before renderer sandboxing.
// Providers acquire their Blink sequence only in BindToCurrentSequence().
bool InitializeWebGPU();
std::unique_ptr<blink::WebGraphicsContext3DProvider> CreateWebGPUContextProvider();

} // namespace content
#endif
