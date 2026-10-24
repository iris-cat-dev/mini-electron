// Copyright 2026 mini-electron contributors.
#ifndef MINI_ELECTRON_RUNTIME_ENGINE_RENDERER_WEBGL_CONTEXT_PROVIDER_H_
#define MINI_ELECTRON_RUNTIME_ENGINE_RENDERER_WEBGL_CONTEXT_PROVIDER_H_

#include <memory>

#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/web_graphics_context_3d_provider.h"

namespace content {

// Initializes the process-lifetime ANGLE display/device before sandbox entry.
// Independent contexts may subsequently be created and bound on worker threads.
bool InitializeWebGL();
std::unique_ptr<blink::WebGraphicsContext3DProvider> CreateWebGLContextProvider(
    const blink::Platform::ContextAttributes& attributes,
    blink::Platform::GraphicsInfo* graphics_info);

}  // namespace content
#endif
