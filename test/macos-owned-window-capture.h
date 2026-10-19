#pragma once

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

namespace mini_electron::test {

// Initiates capture on the main thread. Completion may run on any thread; image
// is borrowed and remains valid only for the callback. Retain it before dispatch.
// screen_rect is the full captured window in global top-left screen points,
// without a shadow, not the renderer's content rectangle or a physical display.
// macOS 14+ uses a shadow-free owned-window ScreenCaptureKit screenshot at
// native resolution. OS capture/privacy failures are INCOMPLETE, never a
// producer/AppKit bitmap fallback. Recording authorization may require restarting
// the launcher. Link the companion .mm with ScreenCaptureKit and AppKit/CoreGraphics.
void CaptureOwnedWindow(
    NSWindow* window,
    void (^complete)(CGImageRef image, CGRect screen_rect, NSString* error));

}  // namespace mini_electron::test
