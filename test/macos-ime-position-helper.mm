#import <AppKit/AppKit.h>

#include <node_api.h>
#include <string>

// Only public AppKit input-client APIs are used here. Page geometry is an
// independent oracle: it is never installed in the renderer's native state.
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

static NSWindow* Window(napi_env env, napi_value value) {
  NSString* title = Text(env, value);
  if (!title) return nil;
  NSWindow* found = nil;
  for (NSWindow* window in NSApp.windows) {
    if (![window.title isEqualToString:title]) continue;
    if (found) {
      napi_throw_error(env, nullptr, "Window title is not unique");
      return nil;
    }
    found = window;
  }
  if (!found) napi_throw_error(env, nullptr, "Owned window not found");
  return found;
}

static NSView* Renderer(NSView* view) {
  if ([NSStringFromClass(view.class) isEqualToString:@"MiniElectronRendererView"])
    return view;
  for (NSView* child in view.subviews)
    if (NSView* found = Renderer(child)) return found;
  return nil;
}

static NSView<NSTextInputClient>* Client(napi_env env, NSWindow* window) {
  NSView* view = Renderer(window.contentView);
  if (!view || ![view conformsToProtocol:@protocol(NSTextInputClient)]) {
    napi_throw_error(env, nullptr, "Owned renderer NSTextInputClient not found");
    return nil;
  }
  return (NSView<NSTextInputClient>*)view;
}

static napi_value Number(napi_env env, double number) {
  napi_value result;
  napi_create_double(env, number, &result);
  return result;
}

static napi_value BoolValue(napi_env env, bool value) {
  napi_value result;
  napi_get_boolean(env, value, &result);
  return result;
}

static napi_value Rectangle(napi_env env, NSRect rect) {
  napi_value result;
  napi_create_array_with_length(env, 4, &result);
  const CGFloat components[] = {rect.origin.x, rect.origin.y, rect.size.width, rect.size.height};
  for (uint32_t i = 0; i < 4; ++i)
    napi_set_element(env, result, i, Number(env, components[i]));
  return result;
}

static napi_value Range(napi_env env, NSRange range) {
  napi_value result, location;
  napi_create_object(env, &result);
  if (range.location == NSNotFound) napi_get_null(env, &location);
  else location = Number(env, range.location);
  napi_set_named_property(env, result, "location", location);
  napi_set_named_property(env, result, "length", Number(env, range.length));
  return result;
}

static NSPoint PagePoint(NSView* view, double x, double y) {
  return NSMakePoint(NSMinX(view.bounds) + x,
      view.isFlipped ? NSMinY(view.bounds) + y : NSMaxY(view.bounds) - y);
}

static napi_value Click(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3];
  double x = 0, y = 0;
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc != 3 || napi_get_value_double(env, args[1], &x) != napi_ok ||
      napi_get_value_double(env, args[2], &y) != napi_ok) {
    napi_throw_type_error(env, nullptr, "click(title, pageX, pageY)");
    return nullptr;
  }
  NSWindow* window = Window(env, args[0]);
  if (!window) return nullptr;
  NSView* view = Client(env, window);
  if (!view) return nullptr;
  [NSApp activateIgnoringOtherApps:YES];
  [window makeKeyAndOrderFront:nil];
  const NSPoint point = [view convertPoint:PagePoint(view, x, y) toView:nil];
  // Dispatch through NSApplication/NSWindow hit testing, without calling
  // mouseDown: or makeFirstResponder: on the renderer ourselves.
  for (NSEventType type : {NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp}) {
    NSEvent* event = [NSEvent mouseEventWithType:type location:point modifierFlags:0
        timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:window.windowNumber
        context:nil eventNumber:0 clickCount:1 pressure:type == NSEventTypeLeftMouseDown ? 1 : 0];
    [NSApp sendEvent:event];
  }
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}

static napi_value State(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2], result;
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc != 2) {
    napi_throw_type_error(env, nullptr, "state(title, pageCaretRect)");
    return nullptr;
  }
  NSWindow* window = Window(env, args[0]);
  if (!window) return nullptr;
  NSView<NSTextInputClient>* view = Client(env, window);
  if (!view) return nullptr;
  double components[4];
  for (uint32_t i = 0; i < 4; ++i) {
    napi_value value;
    if (napi_get_element(env, args[1], i, &value) != napi_ok ||
        napi_get_value_double(env, value, &components[i]) != napi_ok) {
      napi_throw_type_error(env, nullptr, "Expected [x, y, width, height] page caret");
      return nullptr;
    }
  }
  NSPoint point = PagePoint(view, components[0], components[1]);
  if (!view.isFlipped) point.y -= components[3];
  const NSRect local = NSMakeRect(point.x, point.y, components[2], components[3]);
  const NSRect expected = [window convertRectToScreen:[view convertRect:local toView:nil]];
  const NSRange selected = [view selectedRange];
  NSRange actual = NSMakeRange(NSNotFound, 0);
  const NSRect rect = [view firstRectForCharacterRange:selected actualRange:&actual];
  napi_create_object(env, &result);
  napi_set_named_property(env, result, "rect", Rectangle(env, rect));
  napi_set_named_property(env, result, "expectedScreen", Rectangle(env, expected));
  napi_set_named_property(env, result, "expectedBacking", Rectangle(env, [view convertRectToBacking:local]));
  napi_set_named_property(env, result, "selected", Range(env, selected));
  napi_set_named_property(env, result, "actual", Range(env, actual));
  napi_set_named_property(env, result, "marked", Range(env, [view markedRange]));
  napi_set_named_property(env, result, "hasMarked", BoolValue(env, [view hasMarkedText]));
  napi_set_named_property(env, result, "firstResponder", BoolValue(env, window.firstResponder == view));
  napi_set_named_property(env, result, "keyWindow", BoolValue(env, window.isKeyWindow));
  napi_set_named_property(env, result, "scale", Number(env, window.backingScaleFactor));
  return result;
}

static napi_value Mark(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value args[4];
  uint32_t start = 0, length = 0;
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc != 4 || napi_get_value_uint32(env, args[2], &start) != napi_ok ||
      napi_get_value_uint32(env, args[3], &length) != napi_ok) {
    napi_throw_type_error(env, nullptr, "mark(title, text, selectionStart, selectionLength)");
    return nullptr;
  }
  NSWindow* window = Window(env, args[0]);
  if (!window) return nullptr;
  NSView<NSTextInputClient>* view = Client(env, window);
  if (!view) return nullptr;
  NSString* text = Text(env, args[1]);
  if (!text) return nullptr;
  if (!window.isKeyWindow || window.firstResponder != view || start > text.length ||
      length > text.length - start) {
    napi_throw_error(env, nullptr, "Marked text requires a focused native client and valid selection");
    return nullptr;
  }
  [view setMarkedText:[[NSAttributedString alloc] initWithString:text]
       selectedRange:NSMakeRange(start, length) replacementRange:NSMakeRange(NSNotFound, 0)];
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}

static napi_value Commit(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc != 2) {
    napi_throw_type_error(env, nullptr, "commit(title, text)");
    return nullptr;
  }
  NSWindow* window = Window(env, args[0]);
  if (!window) return nullptr;
  NSView<NSTextInputClient>* view = Client(env, window);
  if (!view) return nullptr;
  NSString* text = Text(env, args[1]);
  if (!text) return nullptr;
  if (!window.isKeyWindow || window.firstResponder != view) {
    napi_throw_error(env, nullptr, "Commit requires the focused native input client");
    return nullptr;
  }
  [view insertText:text replacementRange:NSMakeRange(NSNotFound, 0)];
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}

static napi_value Move(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3];
  double x = 0, y = 0;
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc != 3 || napi_get_value_double(env, args[1], &x) != napi_ok ||
      napi_get_value_double(env, args[2], &y) != napi_ok) {
    napi_throw_type_error(env, nullptr, "move(title, deltaX, deltaY)");
    return nullptr;
  }
  NSWindow* window = Window(env, args[0]);
  if (!window) return nullptr;
  const NSPoint before = window.frame.origin;
  [window setFrameOrigin:NSMakePoint(before.x + x, before.y + y)];
  return Rectangle(env, NSMakeRect(window.frame.origin.x - before.x,
      window.frame.origin.y - before.y, window.frame.size.width, window.frame.size.height));
}

NAPI_MODULE_INIT() {
  napi_property_descriptor methods[] = {
    {"click", nullptr, Click, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"state", nullptr, State, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"mark", nullptr, Mark, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"commit", nullptr, Commit, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"move", nullptr, Move, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, sizeof(methods) / sizeof(methods[0]), methods);
  return exports;
}
