#import <AppKit/AppKit.h>
#include <cmath>
#include <node_api.h>
#include <string>

static NSString* Text(napi_env env, napi_value value) {
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
    napi_throw_type_error(env, nullptr, "Expected a string");
    return nil;
  }
  std::string bytes(length + 1, '\0');
  napi_get_value_string_utf8(env, value, bytes.data(), bytes.size(), &length);
  return [[NSString alloc] initWithBytes:bytes.data() length:length encoding:NSUTF8StringEncoding];
}
static NSView* Renderer(NSView* view) {
  if ([NSStringFromClass(view.class) isEqualToString:@"MiniElectronRendererView"]) return view;
  for (NSView* child in view.subviews) {
    if (NSView* found = Renderer(child)) return found;
  }
  return nil;
}
static NSView* View(napi_env env, napi_value title) {
  NSString* expected = Text(env, title);
  if (!expected) return nil;
  NSView* found = nil;
  for (NSWindow* window in NSApp.windows) {
    if (![window.title isEqualToString:expected]) continue;
    if (found) {
      napi_throw_error(env, nullptr, "Window title is not unique");
      return nil;
    }
    found = Renderer(window.contentView);
  }
  if (!found) napi_throw_error(env, nullptr, "Owned renderer view not found");
  return found;
}
static napi_value Mouse(napi_env env, napi_callback_info info) {
  size_t count = 5;
  napi_value args[5];
  double x = 0, y = 0;
  uint32_t modifiers = 0;
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if ((count != 4 && count != 5) || napi_get_value_double(env, args[2], &x) != napi_ok ||
      napi_get_value_double(env, args[3], &y) != napi_ok || !std::isfinite(x) || !std::isfinite(y) ||
      (count == 5 && napi_get_value_uint32(env, args[4], &modifiers) != napi_ok)) {
    napi_throw_type_error(env, nullptr, "mouse(title, type, pageX, pageY, modifiers = 0)");
    return nullptr;
  }
  NSString* type = Text(env, args[1]);
  NSView* view = View(env, args[0]);
  if (!type || !view) return nullptr;
  NSEventType eventType;
  if ([type isEqualToString:@"move"]) eventType = NSEventTypeMouseMoved;
  else if ([type isEqualToString:@"down"]) eventType = NSEventTypeLeftMouseDown;
  else if ([type isEqualToString:@"drag"]) eventType = NSEventTypeLeftMouseDragged;
  else if ([type isEqualToString:@"up"]) eventType = NSEventTypeLeftMouseUp;
  else if ([type isEqualToString:@"rightDown"]) eventType = NSEventTypeRightMouseDown;
  else if ([type isEqualToString:@"rightUp"]) eventType = NSEventTypeRightMouseUp;
  else {
    napi_throw_type_error(env, nullptr, "Unknown native mouse event type");
    return nullptr;
  }
  const NSPoint local = NSMakePoint(NSMinX(view.bounds) + x,
      view.isFlipped ? NSMinY(view.bounds) + y : NSMaxY(view.bounds) - y);
  NSEvent* event = [NSEvent mouseEventWithType:eventType
      location:[view convertPoint:local toView:nil] modifierFlags:modifiers
      timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:view.window.windowNumber
      context:nil eventNumber:0 clickCount:eventType == NSEventTypeMouseMoved ? 0 : 1
      pressure:eventType == NSEventTypeLeftMouseDown || eventType == NSEventTypeLeftMouseDragged
          || eventType == NSEventTypeRightMouseDown ? 1 : 0];
  [NSApp sendEvent:event];
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}
static napi_value Scroll(napi_env env, napi_callback_info info) {
  size_t count = 6;
  napi_value args[6];
  double x = 0, y = 0, deltaX = 0, deltaY = 0;
  bool precise = false;
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 6 || napi_get_value_double(env, args[1], &x) != napi_ok ||
      napi_get_value_double(env, args[2], &y) != napi_ok ||
      napi_get_value_double(env, args[3], &deltaX) != napi_ok ||
      napi_get_value_double(env, args[4], &deltaY) != napi_ok ||
      napi_get_value_bool(env, args[5], &precise) != napi_ok ||
      !std::isfinite(x) || !std::isfinite(y) ||
      !std::isfinite(deltaX) || !std::isfinite(deltaY)) {
    napi_throw_type_error(env, nullptr, "scroll(title, pageX, pageY, deltaX, deltaY, precise)");
    return nullptr;
  }
  NSView* view = View(env, args[0]);
  if (!view) return nullptr;
  const NSPoint local = NSMakePoint(NSMinX(view.bounds) + x,
      view.isFlipped ? NSMinY(view.bounds) + y : NSMaxY(view.bounds) - y);
  const NSPoint point = [view convertPoint:local toView:nil];
  CGEventRef scroll = CGEventCreateScrollWheelEvent(nullptr,
      precise ? kCGScrollEventUnitPixel : kCGScrollEventUnitLine, 2,
      static_cast<int32_t>(deltaY), static_cast<int32_t>(deltaX));
  // NSEvent's CGEvent bridge flips using the primary screen, even for a window event.
  CGEventSetLocation(scroll, CGPointMake(point.x,
      NSHeight(NSScreen.screens.firstObject.frame) - point.y));
  CGEventSetFlags(scroll, 0);
  CGEventSetIntegerValueField(scroll, kCGMouseEventWindowUnderMousePointer,
      view.window.windowNumber);
  CGEventSetIntegerValueField(scroll, kCGMouseEventWindowUnderMousePointerThatCanHandleThisEvent,
      view.window.windowNumber);
  NSEvent* event = [NSEvent eventWithCGEvent:scroll];
  [event setValue:view.window forKey:@"_window"];
  [view scrollWheel:event];
  CFRelease(scroll);
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}
static napi_value Snapshot(napi_env env, napi_callback_info info) {
  size_t count = 2;
  napi_value args[2];
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 2) {
    napi_throw_type_error(env, nullptr, "snapshot(title, path)");
    return nullptr;
  }
  NSView* view = View(env, args[0]);
  NSString* path = Text(env, args[1]);
  if (!view || !path) return nullptr;
  NSBitmapImageRep* bitmap = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
  [view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap];
  if (!bitmap || ![[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
      writeToFile:path atomically:YES]) {
    napi_throw_error(env, nullptr, "Cannot save native pointer screenshot");
    return nullptr;
  }
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}
static id popupObserver = nil;
static napi_value SelectNextPopup(napi_env env, napi_callback_info info) {
  size_t count = 1;
  napi_value argument;
  napi_get_cb_info(env, info, &count, &argument, nullptr, nullptr);
  if (count != 1) {
    napi_throw_type_error(env, nullptr, "selectNextPopup(label)");
    return nullptr;
  }
  NSString* label = Text(env, argument);
  if (!label) return nullptr;
  if (popupObserver)
    [NSNotificationCenter.defaultCenter removeObserver:popupObserver];
  popupObserver = [NSNotificationCenter.defaultCenter
      addObserverForName:NSMenuDidBeginTrackingNotification object:nil queue:nil
      usingBlock:^(NSNotification* notification) {
        NSMenu* menu = notification.object;
        NSInteger index = [menu indexOfItemWithTitle:label];
        if (index < 0) return;
        [NSNotificationCenter.defaultCenter removeObserver:popupObserver];
        popupObserver = nil;
        NSTimer* timer = [NSTimer timerWithTimeInterval:0.05 repeats:NO
            block:^(NSTimer*) {
              [menu performActionForItemAtIndex:index];
              [menu cancelTracking];
            }];
        [NSRunLoop.mainRunLoop addTimer:timer forMode:NSEventTrackingRunLoopMode];
      }];
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}
NAPI_MODULE_INIT() {
  napi_property_descriptor methods[] = {
      {"mouse", nullptr, Mouse, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"scroll", nullptr, Scroll, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"snapshot", nullptr, Snapshot, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"selectNextPopup", nullptr, SelectNextPopup, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, 4, methods);
  return exports;
}
