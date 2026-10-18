// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#import <AppKit/AppKit.h>

#include "base/check.h"
#include "base/no_destructor.h"
#include "runtime/engine/renderer/renderer_blink_platform_impl.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/blink/public/platform/mac/web_sandbox_support.h"

namespace {

class MacSandboxSupport final : public blink::WebSandboxSupport {
public:
    SkColor GetSystemColor(blink::MacSystemColorID color_id,
        blink::mojom::ColorScheme color_scheme) override
    {
        CHECK(color_id != blink::MacSystemColorID::kCount);
        __block SkColor result = SK_ColorTRANSPARENT;
        NSAppearance* appearance = [NSAppearance appearanceNamed:
            color_scheme == blink::mojom::ColorScheme::kDark
                ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
        // Resolve dynamic AppKit colors for the document's color scheme, not
        // the embedding application's appearance. This API scopes the change
        // to the calling thread and restores its previous drawing appearance.
        [appearance performAsCurrentDrawingAppearance:^{
            NSColor* color = nil;
            switch (color_id) {
            case blink::MacSystemColorID::kControlAccentBlueColor:
                color = NSColor.systemBlueColor;
                break;
            case blink::MacSystemColorID::kControlAccentColor:
                color = NSColor.controlAccentColor;
                break;
            case blink::MacSystemColorID::kKeyboardFocusIndicator:
                color = NSColor.keyboardFocusIndicatorColor;
                break;
            case blink::MacSystemColorID::kSecondarySelectedControl:
                color = NSColor.secondarySelectedControlColor;
                break;
            case blink::MacSystemColorID::kSelectedTextBackground:
                color = NSColor.selectedTextBackgroundColor;
                break;
            case blink::MacSystemColorID::kCount:
                break;
            }
            NSColor* rgb = [color colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];
            CHECK(rgb);
            CGFloat red, green, blue, alpha;
            [rgb getRed:&red green:&green blue:&blue alpha:&alpha];
            result = SkColor4f { static_cast<float>(red), static_cast<float>(green),
                static_cast<float>(blue), static_cast<float>(alpha) }.toSkColor();
        }];
        return result;
    }
};

} // namespace

namespace content {

blink::WebSandboxSupport* RendererBlinkPlatformImpl::GetSandboxSupport()
{
    // The renderer already uses AppKit native themes. Keep the platform
    // boundary intact instead of making Blink bypass its sandbox interface.
    static base::NoDestructor<MacSandboxSupport> support;
    return support.get();
}

} // namespace content
