#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <node_api.h>
#include <cmath>
#include <string>
#include <dlfcn.h>

static napi_value Fail(napi_env env, const char* message) {
  napi_throw_error(env, nullptr, message);
  return nullptr;
}
static void Number(napi_env env, napi_value object, const char* key, double number) {
  napi_value value;
  napi_create_double(env, number, &value);
  napi_set_named_property(env, object, key, value);
}
static void SetBoolean(napi_env env, napi_value object, const char* key, bool boolean) {
  napi_value value;
  napi_get_boolean(env, boolean, &value);
  napi_set_named_property(env, object, key, value);
}
static napi_value RectValue(napi_env env, CGRect rect) {
  napi_value result;
  napi_create_object(env, &result);
  Number(env, result, "x", rect.origin.x); Number(env, result, "y", rect.origin.y);
  Number(env, result, "width", rect.size.width); Number(env, result, "height", rect.size.height);
  return result;
}
static NSView* Renderer(NSView* view) {
  if ([NSStringFromClass(view.class) isEqualToString:@"MiniElectronRendererView"]) return view;
  for (NSView* child in view.subviews) {
    NSView* found = Renderer(child);
    if (found) return found;
  }
  return nil;
}
static NSWindow* Window(napi_env env, napi_callback_info info) {
  size_t count = 1, size = 0;
  napi_value arg;
  napi_get_cb_info(env, info, &count, &arg, nullptr, nullptr);
  if (count != 1 || napi_get_value_string_utf8(env, arg, nullptr, 0, &size) != napi_ok) {
    Fail(env, "Expected unique window title"); return nil;
  }
  std::string text(size + 1, '\0');
  napi_get_value_string_utf8(env, arg, text.data(), text.size(), &size);
  NSString* title = [[NSString alloc] initWithBytes:text.data() length:size encoding:NSUTF8StringEncoding];
  NSWindow* found = nil;
  for (NSWindow* window in NSApplication.sharedApplication.windows) {
    if (![window.title isEqualToString:title]) continue;
    if (found) { Fail(env, "Ambiguous window title"); return nil; }
    found = window;
  }
  if (!found) Fail(env, "Owned window not found");
  return found;
}
static napi_value Metadata(napi_env env, NSWindow* window) {
  napi_value result, rgba;
  napi_create_object(env, &result);
  SetBoolean(env, result, "visible", window.visible);
  SetBoolean(env, result, "appActive", NSApplication.sharedApplication.active);
  SetBoolean(env, result, "keyWindow", window.keyWindow);
  SetBoolean(env, result, "occlusionVisible", (window.occlusionState & NSWindowOcclusionStateVisible) != 0);
  Number(env, result, "windowNumber", window.windowNumber);
  SetBoolean(env, result, "opaque", window.opaque);
  SetBoolean(env, result, "canBecomeKey", window.canBecomeKeyWindow);
  SetBoolean(env, result, "titled", (window.styleMask & NSWindowStyleMaskTitled) != 0);
  SetBoolean(env, result, "closeButtonVisible", [window standardWindowButton:NSWindowCloseButton] != nil && ![window standardWindowButton:NSWindowCloseButton].hidden);
  Number(env, result, "level", window.level);
  NSColor* color = [window.backgroundColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
  napi_create_array_with_length(env, 4, &rgba);
  const CGFloat channels[] = {color.redComponent, color.greenComponent, color.blueComponent, color.alphaComponent};
  for (uint32_t i = 0; i < 4; ++i) {
    napi_value value; napi_create_double(env, channels[i] * 255, &value); napi_set_element(env, rgba, i, value);
  }
  napi_set_named_property(env, result, "backgroundRGBA", rgba);
  bool on_screen = false;
  CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, (CGWindowID)window.windowNumber);
  if (list) {
    for (NSDictionary* entry in (__bridge NSArray*)list)
      if ([entry[(__bridge NSString*)kCGWindowNumber] integerValue] == window.windowNumber)
        on_screen = [entry[(__bridge NSString*)kCGWindowIsOnscreen] boolValue];
    CFRelease(list);
  }
  SetBoolean(env, result, "onScreen", on_screen);
  return result;
}
static napi_value Inspect(napi_env env, napi_callback_info info) {
  NSWindow* window = Window(env, info);
  return window ? Metadata(env, window) : nullptr;
}
static void Complete(napi_env env, napi_deferred deferred, napi_value result,
                     CGImageRef image, CGRect renderer, CGRect display, NSString* error) {
  napi_handle_scope scope;
  napi_open_handle_scope(env, &scope);
  if (!image || error) {
    napi_value text, exception;
    NSString* message = [@"Actual composed display capture failed. Grant Screen Recording to the mini-electron executable in System Settings > Privacy & Security > Screen Recording, restart it, and run on an unlocked graphical macOS session. " stringByAppendingString:error ?: @"No screen image returned."];
    napi_create_string_utf8(env, message.UTF8String, NAPI_AUTO_LENGTH, &text);
    napi_create_error(env, nullptr, text, &exception);
    napi_reject_deferred(env, deferred, exception);
    napi_close_handle_scope(env, scope);
    return;
  }
  napi_value samples;
  napi_create_object(env, &samples);
  // Read each actual display pixel without resizing or averaging the screenshot.
  const char* names[] = {"topLeft", "top", "topRight", "left", "center", "right", "bottomLeft", "bottom", "bottomRight"};
  const double fractions[] = {0.1, 0.5, 0.9};
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  for (uint32_t row = 0; row < 3; ++row) for (uint32_t col = 0; col < 3; ++col) {
    double x = renderer.origin.x + renderer.size.width * fractions[col];
    double y = renderer.origin.y + renderer.size.height * fractions[row];
    size_t px = std::floor((x - display.origin.x) * CGImageGetWidth(image) / display.size.width);
    size_t py = std::floor((y - display.origin.y) * CGImageGetHeight(image) / display.size.height);
    CGImageRef pixel_image = CGImageCreateWithImageInRect(image, CGRectMake(px, py, 1, 1));
    uint8_t bytes[4] = {};
    CGContextRef context = CGBitmapContextCreate(bytes, 1, 1, 8, 4, space,
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
    if (!pixel_image || !context) {
      if (pixel_image) CGImageRelease(pixel_image);
      if (context) CGContextRelease(context);
      CGColorSpaceRelease(space);
      Complete(env, deferred, result, nullptr, renderer, display, @"Could not decode display pixels.");
      napi_close_handle_scope(env, scope); return;
    }
    CGContextDrawImage(context, CGRectMake(0, 0, 1, 1), pixel_image);
    CGContextRelease(context); CGImageRelease(pixel_image);
    napi_value pixel; napi_create_array_with_length(env, 4, &pixel);
    for (uint32_t channel = 0; channel < 4; ++channel) {
      napi_value value; napi_create_uint32(env, bytes[channel], &value); napi_set_element(env, pixel, channel, value);
    }
    napi_set_named_property(env, samples, names[row * 3 + col], pixel);
  }
  CGColorSpaceRelease(space);
  napi_set_named_property(env, result, "samples", samples);
  napi_set_named_property(env, result, "rendererScreenRect", RectValue(env, renderer));
  napi_resolve_deferred(env, deferred, result);
  napi_close_handle_scope(env, scope);
}
static napi_value Snapshot(napi_env env, napi_callback_info info) {
  NSWindow* window = Window(env, info);
  if (!window) return nullptr;
  if (!CGPreflightScreenCaptureAccess()) return Fail(env,
      "Screen Recording permission is REQUIRED: grant the mini-electron executable access in System Settings > Privacy & Security > Screen Recording, restart it, and rerun. Acceptance cannot be skipped.");
  NSView* view = Renderer(window.contentView);
  if (!view || !window.screen) return Fail(env, "Visible renderer and attached display required");
  CGRect renderer = [window convertRectToScreen:[view convertRect:view.bounds toView:nil]];
  renderer.origin.y = NSMaxY(NSScreen.screens.firstObject.frame) - CGRectGetMaxY(renderer);
  CGDirectDisplayID display_id = [window.screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
  CGRect display = CGDisplayBounds(display_id);
  if (!CGRectContainsRect(display, renderer)) return Fail(env, "Renderer must be fully inside one display; move windows to an unobstructed display");
  napi_value promise, result = Metadata(env, window);
  napi_deferred deferred;
  napi_create_promise(env, &deferred, &promise);
  napi_ref reference;
  napi_create_reference(env, result, 1, &reference);
  void (^deliver)(CGImageRef, NSString*) = ^(CGImageRef image, NSString* error) {
    if (image) CGImageRetain(image);
    dispatch_async(dispatch_get_main_queue(), ^{
      napi_handle_scope scope; napi_open_handle_scope(env, &scope);
      napi_value retained; napi_get_reference_value(env, reference, &retained);
      Complete(env, deferred, retained, image, renderer, display, error);
      napi_delete_reference(env, reference);
      if (image) CGImageRelease(image);
      napi_close_handle_scope(env, scope);
    });
  };
  if (@available(macOS 14.0, *)) {
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:YES
        completionHandler:^(SCShareableContent* content, NSError* error) {
      SCDisplay* target = nil;
      for (SCDisplay* candidate in content.displays) if (candidate.displayID == display_id) target = candidate;
      if (error || !target) { deliver(nullptr, error.description ?: @"Display not available to ScreenCaptureKit."); return; }
      // Display filter includes every window and the desktop: transparent pixels
      // must contain the underlay, unlike a desktop-independent window snapshot.
      SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:target excludingWindows:@[]];
      SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
      config.width = CGDisplayPixelsWide(display_id); config.height = CGDisplayPixelsHigh(display_id);
      config.showsCursor = NO;
      [SCScreenshotManager captureImageWithFilter:filter configuration:config
          completionHandler:^(CGImageRef image, NSError* capture_error) { deliver(image, capture_error.description); }];
    }];
  } else {
    // New SDKs mark CGWindowListCreateImage unavailable. Resolve the legacy
    // symbol only on the pre-ScreenshotManager runtime branch instead of
    // referencing an unavailable declaration when building with those SDKs.
    using LegacyCapture = CGImageRef (*)(CGRect, CGWindowListOption, CGWindowID, CGWindowImageOption);
    auto capture = reinterpret_cast<LegacyCapture>(dlsym(RTLD_DEFAULT, "CGWindowListCreateImage"));
    CGImageRef image = capture ? capture(display, kCGWindowListOptionOnScreenOnly,
        kCGNullWindowID, kCGWindowImageDefault) : nullptr;
    deliver(image, nil);
    if (image) CGImageRelease(image);
  }
  return promise;
}
static napi_value Init(napi_env env, napi_value exports) {
  const napi_property_descriptor properties[] = {
      {"inspect", nullptr, Inspect, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"snapshot", nullptr, Snapshot, nullptr, nullptr, nullptr, napi_default, nullptr}};
  napi_define_properties(env, exports, 2, properties);
  return exports;
}
NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
