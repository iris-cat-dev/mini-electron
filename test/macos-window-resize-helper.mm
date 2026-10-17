#import <AppKit/AppKit.h>

#include <node_api.h>

#include <cmath>
#include <string>

static NSHashTable<NSWindow*>* g_full_screen_windows;
static id g_enter_full_screen;
static id g_exit_full_screen;

// Use +sharedApplication rather than the NSApp global: the executable need not
// export its NSApp reference for a dynamically loaded N-API bundle.
static NSWindow* OwnedWindow(napi_env env, napi_value title_value) {
  size_t length = 0;
  if (napi_get_value_string_utf8(env, title_value, nullptr, 0, &length) != napi_ok) {
    napi_throw_type_error(env, nullptr, "Expected the unique owned window title");
    return nil;
  }
  std::string title(length + 1, '\0');
  napi_get_value_string_utf8(env, title_value, title.data(), title.size(), &length);
  NSString* expected = [[NSString alloc] initWithBytes:title.data()
                                             length:length
                                           encoding:NSUTF8StringEncoding];
  NSWindow* found = nil;
  for (NSWindow* window in [NSApplication sharedApplication].windows) {
    if (![window.title isEqualToString:expected]) continue;
    if (found) {
      napi_throw_error(env, nullptr, "Owned native window title is not unique");
      return nil;
    }
    found = window;
  }
  if (!found) napi_throw_error(env, nullptr, "Owned native window was not found");
  return found;
}

static double Number(napi_env env, napi_value object, const char* name) {
  napi_value value;
  double number = NAN;
  if (napi_get_named_property(env, object, name, &value) == napi_ok)
    napi_get_value_double(env, value, &number);
  return number;
}

static void SetNumber(napi_env env, napi_value object, const char* name, double number) {
  napi_value value;
  napi_create_double(env, number, &value);
  napi_set_named_property(env, object, name, value);
}

static napi_value RectValue(napi_env env, NSRect rect) {
  napi_value result;
  napi_create_object(env, &result);
  SetNumber(env, result, "x", rect.origin.x);
  SetNumber(env, result, "y", rect.origin.y);
  SetNumber(env, result, "width", rect.size.width);
  SetNumber(env, result, "height", rect.size.height);
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
  if (!window) return nullptr;
  napi_value result;
  napi_create_object(env, &result);
  napi_value frontmost;
  const char* bundle = NSWorkspace.sharedWorkspace.frontmostApplication.bundleIdentifier.UTF8String;
  napi_create_string_utf8(env, bundle ? bundle : "", NAPI_AUTO_LENGTH, &frontmost);
  napi_set_named_property(env, result, "frontmostBundleIdentifier", frontmost);
  napi_set_named_property(env, result, "frame", RectValue(env, window.frame));
  napi_set_named_property(env, result, "content", RectValue(env, window.contentView.bounds));
  napi_value full_screen;
  napi_get_boolean(env, [g_full_screen_windows containsObject:window], &full_screen);
  napi_set_named_property(env, result, "fullScreenSettled", full_screen);
  return result;
}

static napi_value SetFrame(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc != 2) {
    napi_throw_type_error(env, nullptr, "setFrame(title, rect) requires two arguments");
    return nullptr;
  }
  NSWindow* window = OwnedWindow(env, args[0]);
  if (!window) return nullptr;
  const NSRect frame = NSMakeRect(Number(env, args[1], "x"), Number(env, args[1], "y"),
                                 Number(env, args[1], "width"), Number(env, args[1], "height"));
  if (!std::isfinite(frame.origin.x) || !std::isfinite(frame.origin.y) ||
      !std::isfinite(frame.size.width) || !std::isfinite(frame.size.height) ||
      frame.size.width <= 0 || frame.size.height <= 0) {
    napi_throw_type_error(env, nullptr, "Expected a finite positive frame");
    return nullptr;
  }
  [window setFrame:frame display:YES];
  return RectValue(env, window.frame);
}

static NSEvent* MouseEvent(NSWindow* window, NSEventType type, NSPoint screen_point) {
  // Convert afresh after every resize. Reusing the original window coordinates
  // would move the pointer when a left/bottom edge changes the window origin.
  return [NSEvent mouseEventWithType:type
                           location:[window convertPointFromScreen:screen_point]
                      modifierFlags:0
                          timestamp:NSProcessInfo.processInfo.systemUptime
                       windowNumber:window.windowNumber
                            context:nil
                        eventNumber:77231
                         clickCount:1
                           pressure:type == NSEventTypeLeftMouseUp ? 0.0 : 1.0];
}

static napi_value CursorAt(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc != 2) {
    napi_throw_type_error(env, nullptr, "cursorAt(title, point) requires two arguments");
    return nullptr;
  }
  NSWindow* window = OwnedWindow(env, args[0]);
  if (!window) return nullptr;
  const NSPoint point = NSMakePoint(Number(env, args[1], "x"),
                                   Number(env, args[1], "y"));
  [NSCursor.arrowCursor set];
  [window sendEvent:MouseEvent(window, NSEventTypeMouseMoved,
                               [window convertPointToScreen:point])];
  napi_value arrow;
  napi_get_boolean(env, [NSCursor.currentCursor isEqual:NSCursor.arrowCursor], &arrow);
  return arrow;
}

static napi_value Gesture(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc != 2) {
    napi_throw_type_error(env, nullptr, "gesture(title, options) requires two arguments");
    return nullptr;
  }
  NSWindow* window = OwnedWindow(env, args[0]);
  if (!window) return nullptr;
  const double x = Number(env, args[1], "x");
  const double y = Number(env, args[1], "y");
  const double dx = Number(env, args[1], "dx");
  const double dy = Number(env, args[1], "dy");
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(dx) || !std::isfinite(dy)) {
    napi_throw_type_error(env, nullptr, "Expected finite mouse coordinates and deltas");
    return nullptr;
  }
  napi_value result;
  napi_create_object(env, &result);
  const NSRect initial = window.frame;
  const NSPoint start = NSMakePoint(initial.origin.x + x, initial.origin.y + y);
  napi_set_named_property(env, result, "before", RectValue(env, initial));
  NSApplication* application = [NSApplication sharedApplication];
  NSEvent* escape = MouseEvent(window, NSEventTypeLeftMouseUp, start);
  // Old AppKit caption/native-border paths can enter a synchronous tracking
  // loop inside sendEvent:. Queue an up first so that loop always terminates,
  // even with a locked desktop. New nontracking interception leaves it queued;
  // remove that exact event without dispatching it before the genuine drags.
  [application postEvent:escape atStart:YES];
  @try {
    [window sendEvent:MouseEvent(window, NSEventTypeLeftMouseDown, start)];
  } @finally {
    NSEvent* queued = [application nextEventMatchingMask:NSEventMaskLeftMouseUp
                                             untilDate:NSDate.distantPast
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:NO];
    if (queued == escape) {
      [application nextEventMatchingMask:NSEventMaskLeftMouseUp
                              untilDate:NSDate.distantPast
                                 inMode:NSDefaultRunLoopMode
                                dequeue:YES];
    }
  }
  [window sendEvent:MouseEvent(window, NSEventTypeLeftMouseDragged,
                              NSMakePoint(start.x + dx / 2, start.y + dy / 2))];
  napi_set_named_property(env, result, "half", RectValue(env, window.frame));
  const NSPoint end = NSMakePoint(start.x + dx, start.y + dy);
  [window sendEvent:MouseEvent(window, NSEventTypeLeftMouseDragged, end)];
  napi_set_named_property(env, result, "dragged", RectValue(env, window.frame));
  [window sendEvent:MouseEvent(window, NSEventTypeLeftMouseUp, end)];
  napi_set_named_property(env, result, "released", RectValue(env, window.frame));
  [window sendEvent:MouseEvent(window, NSEventTypeLeftMouseDragged,
                              NSMakePoint(end.x + 19, end.y + 17))];
  napi_set_named_property(env, result, "afterUp", RectValue(env, window.frame));
  [window sendEvent:MouseEvent(window, NSEventTypeLeftMouseUp, end)];
  return result;
}

NAPI_MODULE_INIT() {
  g_full_screen_windows = [NSHashTable weakObjectsHashTable];
  NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
  g_enter_full_screen = [center addObserverForName:NSWindowDidEnterFullScreenNotification
      object:nil queue:nil usingBlock:^(NSNotification* notification) {
        [g_full_screen_windows addObject:notification.object];
      }];
  g_exit_full_screen = [center addObserverForName:NSWindowDidExitFullScreenNotification
      object:nil queue:nil usingBlock:^(NSNotification* notification) {
        [g_full_screen_windows removeObject:notification.object];
      }];
  const napi_property_descriptor methods[] = {
      {"snapshot", nullptr, Snapshot, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setFrame", nullptr, SetFrame, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"gesture", nullptr, Gesture, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"cursorAt", nullptr, CursorAt, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, sizeof(methods) / sizeof(methods[0]), methods);
  return exports;
}
