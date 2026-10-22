// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_HTML_CANVAS_CANVAS_CONTEXT_CREATION_ATTRIBUTES_CORE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_HTML_CANVAS_CANVAS_CONTEXT_CREATION_ATTRIBUTES_CORE_H_

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/graphics/graphics_types.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

class CORE_EXPORT CanvasContextCreationAttributesCore {
    DISALLOW_NEW();

public:
    CanvasContextCreationAttributesCore();
    CanvasContextCreationAttributesCore(blink::CanvasContextCreationAttributesCore const&);
    virtual ~CanvasContextCreationAttributesCore();

    enum class WillReadFrequently { kTrue, kFalse, kUndefined };
#if !defined(MINI_UI_QUICKJS)
    enum class PowerPreference { kDefault, kLowPower, kHighPerformance };
#endif

    bool alpha = true;
#if !defined(MINI_UI_QUICKJS)
    bool antialias = true;
#endif
    PredefinedColorSpace color_space = PredefinedColorSpace::kSRGB;
#if !defined(MINI_UI_QUICKJS)
    bool depth = true;
    bool fail_if_major_performance_caveat = false;
#endif
    bool desynchronized = false;
    CanvasPixelFormat pixel_format = CanvasPixelFormat::kUint8;
#if !defined(MINI_UI_QUICKJS)
    bool premultiplied_alpha = true;
    bool preserve_drawing_buffer = false;
    PowerPreference power_preference = PowerPreference::kDefault;
    bool stencil = false;
#endif
    // Help to determine whether to use GPU or CPU for the canvas.
    WillReadFrequently will_read_frequently = WillReadFrequently::kUndefined;
#if !defined(MINI_UI_QUICKJS)
    bool xr_compatible = false;
#endif
};

} // namespace blink

#endif // THIRD_PARTY_BLINK_RENDERER_CORE_HTML_CANVAS_CANVAS_CONTEXT_CREATION_ATTRIBUTES_CORE_H_
