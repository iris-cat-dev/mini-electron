#import "macos-owned-window-capture.h"
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <cmath>
#include <dlfcn.h>
#include <unistd.h>

namespace mini_electron::test {

void CaptureOwnedWindow(NSWindow* window,
    void (^complete)(CGImageRef, CGRect, NSString*)) {
  if (!NSThread.isMainThread) {
    dispatch_async(dispatch_get_main_queue(), ^{ CaptureOwnedWindow(window, complete); });
    return;
  }
  if (!window || ![NSApp.windows containsObject:window] || window.windowNumber <= 0 ||
      !window.visible || window.miniaturized || !window.screen) {
    complete(nullptr, CGRectZero,
        @"INCOMPLETE: capture requires a visible owned window on an attached display.");
    return;
  }
  const CGWindowID window_id = static_cast<CGWindowID>(window.windowNumber);
  if (@available(macOS 14.0, *)) {
    void (^capture)(SCShareableContent*, NSError*) = ^(SCShareableContent* content, NSError* error) {
      SCWindow* target = nil;
      for (SCWindow* candidate in content.windows) {
        if (candidate.windowID == window_id && candidate.owningApplication.processID == getpid()) {
          target = candidate;
          break;
        }
      }
      if (error || !target || !target.onScreen) {
        complete(nullptr, CGRectZero, [NSString stringWithFormat:
            @"INCOMPLETE: owned WindowServer window unavailable: %@", error ?: @"No matching visible owned window"]);
        return;
      }
      const CGRect rect = target.frame;
      SCContentFilter* filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:target];
      const double scale = filter.pointPixelScale;
      if (CGRectIsEmpty(rect) || !std::isfinite(scale) || scale <= 0) {
        complete(nullptr, CGRectZero, @"INCOMPLETE: invalid native capture geometry.");
        return;
      }
      SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
      config.width = static_cast<size_t>(std::llround(rect.size.width * scale));
      config.height = static_cast<size_t>(std::llround(rect.size.height * scale));
      config.colorSpaceName = kCGColorSpaceSRGB;
      config.ignoreShadowsSingleWindow = YES;
      config.includeChildWindows = NO;
      config.showsCursor = NO;
      config.capturesAudio = NO;
      [SCScreenshotManager captureImageWithFilter:filter configuration:config
          completionHandler:^(CGImageRef image, NSError* capture_error) {
        if (image && !capture_error) {
          complete(image, rect, nil);
          return;
        }
        complete(nullptr, rect, [NSString stringWithFormat:
            @"INCOMPLETE: WindowServer screenshot %@ (%ld): %@; globalConsent=%d. If recording access was just granted, restart its launcher before rerunning.",
            capture_error.domain, static_cast<long>(capture_error.code), capture_error.localizedDescription,
            CGPreflightScreenCaptureAccess()]);
      }];
    };
    if (CGPreflightScreenCaptureAccess()) {
      // Remote renderer layers belong to a different process. Enumerate with
      // recording authorization, then still filter the exact owned window.
      [SCShareableContent getShareableContentExcludingDesktopWindows:YES
          onScreenWindowsOnly:YES completionHandler:capture];
    } else if (@available(macOS 14.4, *)) {
      [SCShareableContent getCurrentProcessShareableContentWithCompletionHandler:capture];
    } else {
      complete(nullptr, CGRectZero, @"INCOMPLETE: grant Screen Recording and restart the launcher.");
    }
    return;
  }
  // Only older systems use the supported legacy own-window API. A modern
  // capture error must not become a successful but transparent/redacted image.
  using Capture = CGImageRef (*)(CGRect, CGWindowListOption, CGWindowID, CGWindowImageOption);
  auto capture = reinterpret_cast<Capture>(dlsym(RTLD_DEFAULT, "CGWindowListCreateImage"));
  CGImageRef image = capture ? capture(CGRectNull, kCGWindowListOptionIncludingWindow, window_id,
      kCGWindowImageBoundsIgnoreFraming | kCGWindowImageBestResolution) : nullptr;
  CGRect rect = window.frame;
  rect.origin.y = NSMaxY(NSScreen.screens.firstObject.frame) - CGRectGetMaxY(rect);
  complete(image, rect, image ? nil : @"INCOMPLETE: legacy owned-window capture unavailable.");
  if (image) CGImageRelease(image);
}

}  // namespace mini_electron::test
