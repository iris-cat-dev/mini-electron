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

static NSView* Renderer(NSView* view, uint32_t* index) {
  if ([NSStringFromClass(view.class) isEqualToString:@"MiniElectronRendererView"]) {
    if (!*index) return view;
    --*index;
  }
  for (NSView* child in view.subviews)
    if (NSView* found = Renderer(child, index)) return found;
  return nil;
}

static NSView* View(napi_env env, napi_value title, uint32_t index) {
  NSString* expected = Text(env, title);
  if (!expected) return nil;
  NSWindow* found = nil;
  for (NSWindow* window in NSApp.windows) {
    if (![window.title isEqualToString:expected]) continue;
    if (found) {
      napi_throw_error(env, nullptr, "Owned window title is not unique");
      return nil;
    }
    found = window;
  }
  NSView* renderer = found ? Renderer(found.contentView, &index) : nil;
  if (!renderer) napi_throw_error(env, nullptr, "Owned renderer view not found");
  return renderer;
}

static CGFloat DesktopTop() {
  return NSMaxY(NSScreen.screens.firstObject.frame);
}

static void Number(napi_env env, napi_value object, const char* name, double number) {
  napi_value value;
  napi_create_double(env, number, &value);
  napi_set_named_property(env, object, name, value);
}

static napi_value Geometry(napi_env env, napi_callback_info info) {
  size_t count = 2;
  napi_value args[2];
  uint32_t index = 0;
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 2 || napi_get_value_uint32(env, args[1], &index) != napi_ok) {
    napi_throw_type_error(env, nullptr, "geometry(title, rendererIndex)");
    return nullptr;
  }
  NSView* view = View(env, args[0], index);
  if (!view) return nullptr;
  const NSRect widget = [view.window convertRectToScreen:[view convertRect:view.bounds toView:nil]];
  const NSRect window = view.window.frame;
  napi_value result;
  napi_create_object(env, &result);
  Number(env, result, "widgetX", NSMinX(widget));
  Number(env, result, "widgetY", DesktopTop() - NSMaxY(widget));
  Number(env, result, "windowX", NSMinX(window));
  Number(env, result, "windowY", DesktopTop() - NSMaxY(window));
  Number(env, result, "width", NSWidth(widget));
  Number(env, result, "height", NSHeight(widget));
  Number(env, result, "scale", view.window.backingScaleFactor);
  return result;
}

static napi_value Mouse(napi_env env, napi_callback_info info) {
  size_t count = 6;
  napi_value args[6];
  double x = 0, y = 0;
  uint32_t index = 0;
  bool screen = false;
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 6 || napi_get_value_double(env, args[2], &x) != napi_ok ||
      napi_get_value_double(env, args[3], &y) != napi_ok ||
      napi_get_value_uint32(env, args[4], &index) != napi_ok ||
      napi_get_value_bool(env, args[5], &screen) != napi_ok ||
      !std::isfinite(x) || !std::isfinite(y)) {
    napi_throw_type_error(env, nullptr, "mouse(title, type, x, y, rendererIndex, screenCoordinates)");
    return nullptr;
  }
  NSString* type = Text(env, args[1]);
  NSView* view = View(env, args[0], index);
  if (!type || !view) return nullptr;
  NSEventType native_type;
  if ([type isEqualToString:@"move"]) native_type = NSEventTypeMouseMoved;
  else if ([type isEqualToString:@"down"]) native_type = NSEventTypeLeftMouseDown;
  else if ([type isEqualToString:@"drag"]) native_type = NSEventTypeLeftMouseDragged;
  else if ([type isEqualToString:@"up"]) native_type = NSEventTypeLeftMouseUp;
  else {
    napi_throw_type_error(env, nullptr, "Unknown native mouse type");
    return nullptr;
  }
  const NSPoint local = NSMakePoint(NSMinX(view.bounds) + x,
      view.isFlipped ? NSMinY(view.bounds) + y : NSMaxY(view.bounds) - y);
  const NSPoint point = screen ? [view.window convertPointFromScreen:NSMakePoint(x, DesktopTop() - y)]
                              : [view convertPoint:local toView:nil];
  const NSPoint world = [view.window convertPointToScreen:point];
  NSEvent* event = [NSEvent mouseEventWithType:native_type location:point modifierFlags:0
      timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:view.window.windowNumber
      context:nil eventNumber:0 clickCount:native_type == NSEventTypeMouseMoved ? 0 : 1
      pressure:native_type == NSEventTypeLeftMouseDown || native_type == NSEventTypeLeftMouseDragged ? 1 : 0];
  CGEventSetLocation(event.CGEvent, CGPointMake(world.x, DesktopTop() - world.y));
  switch (native_type) {
    case NSEventTypeMouseMoved: [view mouseMoved:event]; break;
    case NSEventTypeLeftMouseDown: [view mouseDown:event]; break;
    case NSEventTypeLeftMouseDragged: [view mouseDragged:event]; break;
    case NSEventTypeLeftMouseUp: [view mouseUp:event]; break;
    default: break;
  }
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}

static napi_value Scroll(napi_env env, napi_callback_info info) {
  size_t count = 7;
  napi_value args[7];
  double x = 0, y = 0, delta_x = 0, delta_y = 0;
  bool precise = false;
  uint32_t index = 0;
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 7 || napi_get_value_double(env, args[1], &x) != napi_ok ||
      napi_get_value_double(env, args[2], &y) != napi_ok ||
      napi_get_value_double(env, args[3], &delta_x) != napi_ok ||
      napi_get_value_double(env, args[4], &delta_y) != napi_ok ||
      napi_get_value_bool(env, args[5], &precise) != napi_ok ||
      napi_get_value_uint32(env, args[6], &index) != napi_ok ||
      !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(delta_x) || !std::isfinite(delta_y)) {
    napi_throw_type_error(env, nullptr, "scroll(title, x, y, deltaX, deltaY, precise, rendererIndex)");
    return nullptr;
  }
  NSView* view = View(env, args[0], index);
  if (!view) return nullptr;
  const NSPoint local = NSMakePoint(NSMinX(view.bounds) + x,
      view.isFlipped ? NSMinY(view.bounds) + y : NSMaxY(view.bounds) - y);
  const NSPoint point = [view convertPoint:local toView:nil];
  const NSPoint world = [view.window convertPointToScreen:point];
  CGEventRef scroll = CGEventCreateScrollWheelEvent(nullptr,
      precise ? kCGScrollEventUnitPixel : kCGScrollEventUnitLine, 2,
      static_cast<int32_t>(delta_y), static_cast<int32_t>(delta_x));
  CGEventSetLocation(scroll, CGPointMake(point.x, DesktopTop() - point.y));
  CGEventSetIntegerValueField(scroll, kCGMouseEventWindowUnderMousePointer, view.window.windowNumber);
  CGEventSetIntegerValueField(scroll, kCGMouseEventWindowUnderMousePointerThatCanHandleThisEvent, view.window.windowNumber);
  NSEvent* event = [NSEvent eventWithCGEvent:scroll];
  [event setValue:view.window forKey:@"_window"];
  CGEventSetLocation(event.CGEvent, CGPointMake(world.x, DesktopTop() - world.y));
  [view scrollWheel:event];
  CFRelease(scroll);
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}

static napi_value Init(napi_env env, napi_value exports) {
  const napi_property_descriptor properties[] = {
    {"geometry", nullptr, Geometry, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"mouse", nullptr, Mouse, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"scroll", nullptr, Scroll, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, sizeof(properties) / sizeof(properties[0]), properties);
  return exports;
}
NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
