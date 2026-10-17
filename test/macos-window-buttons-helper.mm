#import <AppKit/AppKit.h>

#include <node_api.h>

#include <string>

static std::string String(napi_env env, napi_value value) {
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok)
    return {};
  std::string result(length + 1, '\0');
  if (napi_get_value_string_utf8(env, value, result.data(), result.size(),
                                 &length) != napi_ok) {
    return {};
  }
  result.resize(length);
  return result;
}

static NSWindow* OwnedWindow(napi_env env, napi_value title_value) {
  const std::string title = String(env, title_value);
  NSString* expected = [[NSString alloc] initWithBytes:title.data()
                                               length:title.size()
                                             encoding:NSUTF8StringEncoding];
  NSWindow* found = nil;
  for (NSWindow* window in [NSApplication sharedApplication].windows) {
    if (![window.title isEqualToString:expected])
      continue;
    if (found) {
      napi_throw_error(env, nullptr, "Owned native window title is not unique");
      return nil;
    }
    found = window;
  }
  if (!found)
    napi_throw_error(env, nullptr, "Owned native window was not found");
  return found;
}

static void SetNumber(napi_env env,
                      napi_value object,
                      const char* name,
                      double number) {
  napi_value value;
  napi_create_double(env, number, &value);
  napi_set_named_property(env, object, name, value);
}

static void SetBoolean(napi_env env,
                       napi_value object,
                       const char* name,
                       bool boolean) {
  napi_value value;
  napi_get_boolean(env, boolean, &value);
  napi_set_named_property(env, object, name, value);
}

static bool HitTargetsButton(NSWindow* window, NSButton* button) {
  NSView* frame_view = window.contentView.superview;
  if (!frame_view)
    return false;
  const NSPoint center =
      [button convertPoint:NSMakePoint(NSMidX(button.bounds),
                                      NSMidY(button.bounds))
                   toView:nil];
  NSView* hit = [frame_view hitTest:[frame_view convertPoint:center
                                                        fromView:nil]];
  for (NSView* view = hit; view; view = view.superview) {
    if (view == button)
      return true;
  }
  return false;
}

static napi_value ButtonValue(napi_env env,
                              NSWindow* window,
                              NSWindowButton type) {
  NSButton* button = [window standardWindowButton:type];
  if (!button) {
    napi_throw_error(env, nullptr, "Standard window button was not found");
    return nullptr;
  }
  const NSRect frame = [button convertRect:button.bounds toView:nil];
  napi_value result;
  napi_create_object(env, &result);
  SetNumber(env, result, "x", NSMinX(frame));
  SetNumber(env, result, "y", NSMinY(frame));
  SetNumber(env, result, "width", NSWidth(frame));
  SetNumber(env, result, "height", NSHeight(frame));
  SetNumber(env, result, "top", NSHeight(window.frame) - NSMaxY(frame));
  SetBoolean(env, result, "hitTarget", HitTargetsButton(window, button));
  return result;
}

static napi_value Snapshot(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc != 1) {
    napi_throw_type_error(env, nullptr, "snapshot(title) requires one argument");
    return nullptr;
  }
  NSWindow* window = OwnedWindow(env, args[0]);
  if (!window)
    return nullptr;
  napi_value result;
  napi_create_object(env, &result);
  napi_set_named_property(env, result, "close",
                          ButtonValue(env, window, NSWindowCloseButton));
  napi_set_named_property(env, result, "minimize",
                          ButtonValue(env, window, NSWindowMiniaturizeButton));
  napi_set_named_property(env, result, "zoom",
                          ButtonValue(env, window, NSWindowZoomButton));
  return result;
}

static NSEvent* MouseEvent(NSWindow* window,
                           NSEventType type,
                           NSPoint point) {
  return [NSEvent mouseEventWithType:type
                           location:point
                      modifierFlags:0
                          timestamp:NSProcessInfo.processInfo.systemUptime
                       windowNumber:window.windowNumber
                            context:nil
                        eventNumber:74129
                         clickCount:1
                           pressure:type == NSEventTypeLeftMouseUp ? 0.0 : 1.0];
}

static napi_value Click(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc != 2) {
    napi_throw_type_error(env, nullptr,
                          "click(title, button) requires two arguments");
    return nullptr;
  }
  NSWindow* window = OwnedWindow(env, args[0]);
  if (!window)
    return nullptr;
  const std::string name = String(env, args[1]);
  NSWindowButton type;
  if (name == "close") {
    type = NSWindowCloseButton;
  } else if (name == "minimize") {
    type = NSWindowMiniaturizeButton;
  } else if (name == "zoom") {
    type = NSWindowZoomButton;
  } else {
    napi_throw_range_error(env, nullptr,
                           "button must be close, minimize, or zoom");
    return nullptr;
  }
  NSButton* button = [window standardWindowButton:type];
  if (!button) {
    napi_throw_error(env, nullptr, "Standard window button was not found");
    return nullptr;
  }

  const NSPoint point =
      [button convertPoint:NSMakePoint(NSMidX(button.bounds),
                                      NSMidY(button.bounds))
                   toView:nil];
  NSApplication* application = [NSApplication sharedApplication];
  // Return to the native event pump before dispatch. Direct sendEvent: from a
  // JavaScript callback masks native-to-JS callback and microtask ordering.
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
                 dispatch_get_main_queue(), ^{
    [application postEvent:MouseEvent(window, NSEventTypeLeftMouseUp, point)
                  atStart:NO];
  });
  [application postEvent:MouseEvent(window, NSEventTypeLeftMouseDown, point)
                atStart:YES];

  napi_value result;
  napi_get_boolean(env, true, &result);
  return result;
}

NAPI_MODULE_INIT() {
  const napi_property_descriptor methods[] = {
      {"snapshot", nullptr, Snapshot, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"click", nullptr, Click, nullptr, nullptr, nullptr, napi_default,
       nullptr},
  };
  napi_define_properties(env, exports, sizeof(methods) / sizeof(methods[0]),
                         methods);
  return exports;
}
