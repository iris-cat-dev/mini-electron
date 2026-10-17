#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <node_api.h>
#include <cmath>
#include <string>

static CGPoint g_original_pointer;
static bool g_pointer_saved = false;

static NSWindow* OwnedWindow(napi_env env, napi_value value) {
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
    napi_throw_type_error(env, nullptr, "Expected unique native window title");
    return nil;
  }
  std::string title(length + 1, '\0');
  napi_get_value_string_utf8(env, value, title.data(), title.size(), &length);
  NSString* expected = [NSString stringWithUTF8String:title.c_str()];
  NSWindow* found = nil;
  for (NSWindow* window in NSApplication.sharedApplication.windows) {
    if (![window.title isEqualToString:expected]) continue;
    if (found) {
      napi_throw_error(env, nullptr, "Native window title is not unique");
      return nil;
    }
    found = window;
  }
  if (!found) napi_throw_error(env, nullptr, "Native window was not found");
  return found;
}
static void SetBoolean(napi_env env, napi_value object, const char* key, bool flag) {
  napi_value value;
  napi_get_boolean(env, flag, &value);
  napi_set_named_property(env, object, key, value);
}
static void Number(napi_env env, napi_value object, const char* key, double number) {
  napi_value value;
  napi_create_double(env, number, &value);
  napi_set_named_property(env, object, key, value);
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
  SetBoolean(env, result, "visible", window.visible);
  SetBoolean(env, result, "key", window.keyWindow);
  SetBoolean(env, result, "main", window.mainWindow);
  SetBoolean(env, result, "appActive", NSApplication.sharedApplication.active);
  SetBoolean(env, result, "appKeyMatches", NSApplication.sharedApplication.keyWindow == window);
  SetBoolean(env, result, "appMainMatches", NSApplication.sharedApplication.mainWindow == window);
  Number(env, result, "styleMask", window.styleMask);
  SetBoolean(env, result, "canBecomeKey", window.canBecomeKeyWindow);
  SetBoolean(env, result, "opaque", window.opaque);
  SetBoolean(env, result, "rendererOpaque", window.contentView.subviews.firstObject.opaque);
  SetBoolean(env, result, "hasShadow", window.hasShadow);
  SetBoolean(env, result, "ignoresMouseEvents", window.ignoresMouseEvents);
  SetBoolean(env, result, "alwaysOnTop", window.level > NSNormalWindowLevel);
  SetBoolean(env, result, "allWorkspaces", window.collectionBehavior & NSWindowCollectionBehaviorCanJoinAllSpaces);
  SetBoolean(env, result, "fullScreenAuxiliary", window.collectionBehavior & NSWindowCollectionBehaviorFullScreenAuxiliary);
  Number(env, result, "windowNumber", window.windowNumber);
  Number(env, result, "backgroundAlpha", window.backgroundColor.alphaComponent);
  Number(env, result, "frontmostPid", NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier);
  return result;
}

// Physical WindowServer events, not sendEvent: directly to the overlay. This
// allows the smoke to prove that ignored clicks reach the underlying window.
static napi_value Pointer(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value args[4];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  double x = NAN, y = NAN;
  bool click = false;
  if (argc != 4 || napi_get_value_double(env, args[1], &x) != napi_ok ||
      napi_get_value_double(env, args[2], &y) != napi_ok ||
      napi_get_value_bool(env, args[3], &click) != napi_ok ||
      !std::isfinite(x) || !std::isfinite(y)) {
    napi_throw_type_error(env, nullptr, "pointer(title, x, y, click) requires finite content coordinates");
    return nullptr;
  }
  NSWindow* window = OwnedWindow(env, args[0]);
  if (!window) return nullptr;
  if (!CGPreflightPostEventAccess()) {
    napi_throw_error(env, nullptr, "Physical mouse smoke requires macOS Accessibility permission");
    return nullptr;
  }
  if (!g_pointer_saved) {
    CGEventRef current = CGEventCreate(nullptr);
    g_original_pointer = CGEventGetLocation(current);
    CFRelease(current);
    g_pointer_saved = true;
  }
  NSPoint local = NSMakePoint(x, window.contentView.bounds.size.height - y);
  NSPoint screen = [window convertPointToScreen:[window.contentView convertPoint:local toView:nil]];
  CGPoint point = CGPointMake(screen.x, NSMaxY(NSScreen.screens.firstObject.frame) - screen.y);
  CGEventRef event = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, point, kCGMouseButtonLeft);
  CGEventPost(kCGHIDEventTap, event);
  CFRelease(event);
  if (click) {
    for (CGEventType type : {kCGEventLeftMouseDown, kCGEventLeftMouseUp}) {
      event = CGEventCreateMouseEvent(nullptr, type, point, kCGMouseButtonLeft);
      CGEventPost(kCGHIDEventTap, event);
      CFRelease(event);
    }
  }
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}
static napi_value AppKitPointer(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value args[4];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  double x = NAN, y = NAN;
  bool click = false;
  if (argc != 4 || napi_get_value_double(env, args[1], &x) != napi_ok ||
      napi_get_value_double(env, args[2], &y) != napi_ok ||
      napi_get_value_bool(env, args[3], &click) != napi_ok ||
      !std::isfinite(x) || !std::isfinite(y)) {
    napi_throw_type_error(env, nullptr, "appKitPointer(title,x,y,click) requires finite content coordinates");
    return nullptr;
  }
  NSWindow* window = OwnedWindow(env, args[0]);
  if (!window) return nullptr;
  const NSPoint local = [window.contentView convertPoint:
      NSMakePoint(x, window.contentView.bounds.size.height - y) toView:nil];
  auto event = [&](NSEventType type) {
    return [NSEvent mouseEventWithType:type location:local modifierFlags:0
        timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:window.windowNumber
        context:nil eventNumber:0 clickCount:1 pressure:0];
  };
  if (click) {
    [window sendEvent:event(NSEventTypeLeftMouseDown)];
    [window sendEvent:event(NSEventTypeLeftMouseUp)];
  } else {
    [window sendEvent:event(NSEventTypeMouseMoved)];
  }
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}
static napi_value RestorePointer(napi_env env, napi_callback_info) {
  if (g_pointer_saved) {
    CGEventRef event = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved,
        g_original_pointer, kCGMouseButtonLeft);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    g_pointer_saved = false;
  }
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}
static napi_value Init(napi_env env, napi_value exports) {
  const napi_property_descriptor methods[] = {
    {"snapshot", nullptr, Snapshot, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"pointer", nullptr, Pointer, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"appKitPointer", nullptr, AppKitPointer, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"restorePointer", nullptr, RestorePointer, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, 4, methods);
  return exports;
}
NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
