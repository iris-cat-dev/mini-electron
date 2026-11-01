#import <AppKit/AppKit.h>
#include <cmath>
#include <node_api.h>
#include <string>

static CGPoint original_pointer;
static bool pointer_saved = false;

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

static void Number(napi_env env, napi_value object, const char* name, double number) {
  napi_value value;
  napi_create_double(env, number, &value);
  napi_set_named_property(env, object, name, value);
}

static napi_value Mouse(napi_env env, napi_callback_info info) {
  size_t count = 5;
  napi_value args[5];
  double x = 0, y = 0;
  uint32_t index = 0;
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 5 || napi_get_value_double(env, args[2], &x) != napi_ok ||
      napi_get_value_double(env, args[3], &y) != napi_ok ||
      napi_get_value_uint32(env, args[4], &index) != napi_ok ||
      !std::isfinite(x) || !std::isfinite(y)) {
    napi_throw_type_error(env, nullptr, "mouse(title, type, x, y, rendererIndex)");
    return nullptr;
  }
  NSString* type = Text(env, args[1]);
  NSView* view = View(env, args[0], index);
  if (!type || !view) return nullptr;
  NSEventType native_type;
  const bool position_only = [type isEqualToString:@"position"];
  if (position_only || [type isEqualToString:@"move"]) native_type = NSEventTypeMouseMoved;
  else if ([type isEqualToString:@"down"]) native_type = NSEventTypeLeftMouseDown;
  else if ([type isEqualToString:@"drag"]) native_type = NSEventTypeLeftMouseDragged;
  else if ([type isEqualToString:@"up"]) native_type = NSEventTypeLeftMouseUp;
  else if ([type isEqualToString:@"rightDown"]) native_type = NSEventTypeRightMouseDown;
  else if ([type isEqualToString:@"rightDrag"]) native_type = NSEventTypeRightMouseDragged;
  else if ([type isEqualToString:@"rightUp"]) native_type = NSEventTypeRightMouseUp;
  else {
    napi_throw_type_error(env, nullptr, "Unknown native mouse type");
    return nullptr;
  }
  const NSPoint local = NSMakePoint(x, view.isFlipped ? y : NSHeight(view.bounds) - y);
  const NSPoint point = [view convertPoint:local toView:nil];
  const NSPoint world = [view.window convertPointToScreen:point];
  if (!pointer_saved) {
    const NSPoint original = NSEvent.mouseLocation;
    original_pointer = CGPointMake(original.x, NSMaxY(NSScreen.screens.firstObject.frame) - original.y);
    pointer_saved = true;
  }
  [view.window orderFrontRegardless];
  const CGPoint desktop = CGPointMake(world.x, NSMaxY(NSScreen.screens.firstObject.frame) - world.y);
  const CGError warp_error = CGWarpMouseCursorPosition(desktop);
  NSEvent* event = nil;
  if (!position_only) {
    event = [NSEvent mouseEventWithType:native_type location:point modifierFlags:0
        timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:view.window.windowNumber
        context:nil eventNumber:0 clickCount:native_type == NSEventTypeMouseMoved ? 0 : 1
        pressure:native_type == NSEventTypeLeftMouseDown || native_type == NSEventTypeLeftMouseDragged ? 1 : 0];
    CGEventSetLocation(event.CGEvent, desktop);
    [view updateTrackingAreas];
    const NSWindowStyleMask style = view.window.styleMask;
    const NSRect frame = [view.window convertRectFromScreen:view.window.frame];
    const bool border = (style & NSWindowStyleMaskResizable) &&
        !(style & NSWindowStyleMaskFullScreen) &&
        (!(style & NSWindowStyleMaskTitled) || (style & NSWindowStyleMaskFullSizeContentView)) &&
        (point.x < NSMinX(frame) + 5 || point.x >= NSMaxX(frame) - 5 ||
         point.y < NSMinY(frame) + 5 || point.y >= NSMaxY(frame) - 5);
    if (border) {
      // Border events exercise the window's real resize precedence.
      [view.window sendEvent:event];
    } else {
      // Deliver AppKit events to the owned NSView, as in the coordinate fixture.
      switch (native_type) {
        case NSEventTypeMouseMoved: [view mouseMoved:event]; break;
        case NSEventTypeLeftMouseDown: [view mouseDown:event]; break;
        case NSEventTypeLeftMouseDragged: [view mouseDragged:event]; break;
        case NSEventTypeLeftMouseUp: [view mouseUp:event]; break;
        case NSEventTypeRightMouseDown: [view rightMouseDown:event]; break;
        case NSEventTypeRightMouseDragged: [view rightMouseDragged:event]; break;
        case NSEventTypeRightMouseUp: [view rightMouseUp:event]; break;
        default: break;
      }
    }
  }
  napi_value result;
  napi_create_object(env, &result);
  Number(env, result, "warpError", warp_error);
  Number(env, result, "active", NSApp.active);
  Number(env, result, "windowKey", view.window.keyWindow);
  Number(env, result, "windowNumber", view.window.windowNumber);
  Number(env, result, "pointerWindow", [NSWindow windowNumberAtPoint:NSEvent.mouseLocation
      belowWindowWithWindowNumber:0]);
  Number(env, result, "x", point.x);
  Number(env, result, "y", point.y);
  return result;
}

static napi_value Ready(napi_env env, napi_callback_info info) {
  size_t count = 4;
  napi_value args[4];
  double x = 0, y = 0;
  uint32_t index = 0;
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 4 || napi_get_value_double(env, args[1], &x) != napi_ok ||
      napi_get_value_double(env, args[2], &y) != napi_ok ||
      napi_get_value_uint32(env, args[3], &index) != napi_ok) {
    napi_throw_type_error(env, nullptr, "ready(title, x, y, rendererIndex)");
    return nullptr;
  }
  NSView* view = View(env, args[0], index);
  if (!view) return nullptr;
  const NSPoint local = NSMakePoint(x, view.isFlipped ? y : NSHeight(view.bounds) - y);
  const NSPoint expected = [view.window convertPointToScreen:[view convertPoint:local toView:nil]];
  const NSPoint pointer = NSEvent.mouseLocation;
  const NSInteger under_pointer = [NSWindow windowNumberAtPoint:pointer belowWindowWithWindowNumber:0];
  const NSPoint quartz_point = NSMakePoint(pointer.x, NSMaxY(NSScreen.screens.firstObject.frame) - pointer.y);
  napi_value result, ready;
  napi_create_object(env, &result);
  napi_get_boolean(env, view.window.visible && !view.window.miniaturized &&
      under_pointer == view.window.windowNumber &&
      std::abs(pointer.x - expected.x) <= 1 && std::abs(pointer.y - expected.y) <= 1, &ready);
  napi_set_named_property(env, result, "ready", ready);
  Number(env, result, "expectedX", expected.x);
  Number(env, result, "expectedY", expected.y);
  Number(env, result, "pointerX", pointer.x);
  Number(env, result, "pointerY", pointer.y);
  Number(env, result, "windowNumber", view.window.windowNumber);
  Number(env, result, "underPointer", under_pointer);
  Number(env, result, "underQuartzPointer", [NSWindow windowNumberAtPoint:quartz_point belowWindowWithWindowNumber:0]);
  Number(env, result, "windowLevel", view.window.level);
  Number(env, result, "ignoresMouseEvents", view.window.ignoresMouseEvents);
  Number(env, result, "visible", view.window.visible);
  Number(env, result, "active", NSApp.active);
  CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID);
  for (NSDictionary* entry in (__bridge NSArray*)windows) {
    const NSInteger number = [entry[(__bridge NSString*)kCGWindowNumber] integerValue];
    if (number != under_pointer && number != view.window.windowNumber) continue;
    const bool own = number == view.window.windowNumber;
    Number(env, result, own ? "ownOnScreen" : "underOnScreen", [entry[(__bridge NSString*)kCGWindowIsOnscreen] boolValue]);
    Number(env, result, own ? "ownPID" : "underPID", [entry[(__bridge NSString*)kCGWindowOwnerPID] doubleValue]);
    napi_value owner;
    const char* name = [entry[(__bridge NSString*)kCGWindowOwnerName] UTF8String] ?: "";
    napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &owner);
    napi_set_named_property(env, result, own ? "ownOwner" : "underOwner", owner);
  }
  if (windows) CFRelease(windows);
  return result;
}

static NSInteger CoreType(NSCursor* cursor) {
  if (![cursor respondsToSelector:NSSelectorFromString(@"_coreCursorType")]) return -1;
  return [[cursor valueForKey:@"_coreCursorType"] integerValue];
}

static const char* Kind(NSCursor* cursor) {
  // Inspect the actual AppKit cursor, never renderer CSS or internal runtime state.
  if (cursor == NSCursor.arrowCursor) return "default";
  if (cursor == NSCursor.IBeamCursor) return "text";
  if (cursor == NSCursor.pointingHandCursor) return "pointer";
  if (cursor == NSCursor.crosshairCursor) return "crosshair";
  if (cursor == NSCursor.openHandCursor) return "grab";
  if (cursor == NSCursor.closedHandCursor) return "grabbing";
  if (cursor == NSCursor.operationNotAllowedCursor) return "not-allowed";
  if (cursor == NSCursor.dragCopyCursor) return "copy";
  if (cursor == NSCursor.dragLinkCursor) return "alias";
  if (cursor == NSCursor.contextualMenuCursor) return "context-menu";
  if (cursor == NSCursor.IBeamCursorForVerticalLayout) return "vertical-text";
  if (@available(macOS 15.0, *)) {
    if (cursor == NSCursor.zoomInCursor) return "zoom-in";
    if (cursor == NSCursor.zoomOutCursor) return "zoom-out";
    if (cursor == NSCursor.columnResizeCursor) return "col-resize";
    if (cursor == NSCursor.rowResizeCursor) return "row-resize";
    const struct { NSCursorFrameResizePosition position; const char* name; } diagonals[] = {
      {NSCursorFrameResizePositionTopLeft, "nwse-resize"},
      {NSCursorFrameResizePositionTopRight, "nesw-resize"},
      {NSCursorFrameResizePositionLeft, "ew-resize"},
      {NSCursorFrameResizePositionTop, "ns-resize"},
    };
    for (const auto& item : diagonals) {
      NSCursor* expected = [NSCursor frameResizeCursorFromPosition:item.position
          inDirections:NSCursorFrameResizeDirectionsAll];
      if (CoreType(cursor) >= 0 && CoreType(cursor) == CoreType(expected)) return item.name;
    }
  }
  // CoreCursor types used by Safari/Chromium for shapes without public selectors.
  switch (CoreType(cursor)) {
    case 4: return "wait";
    case 39: return "move";
    case 40: return "help";
    case 41: return "cell";
  }
  if (NSEqualSizes(cursor.image.size, NSMakeSize(1, 1))) return "none";
  return "custom";
}

static napi_value Cursor(napi_env env, napi_callback_info) {
  NSCursor* cursor = NSCursor.currentCursor;
  napi_value result, kind;
  napi_create_object(env, &result);
  napi_create_string_utf8(env, Kind(cursor), NAPI_AUTO_LENGTH, &kind);
  napi_set_named_property(env, result, "kind", kind);
  Number(env, result, "hotspotX", cursor.hotSpot.x);
  Number(env, result, "hotspotY", cursor.hotSpot.y);
  Number(env, result, "width", cursor.image.size.width);
  Number(env, result, "height", cursor.image.size.height);
  if (cursor.image) {
    NSDictionary* hints = @{ NSImageHintCTM : [NSAffineTransform transform] };
    CGImageRef image = [cursor.image CGImageForProposedRect:nullptr context:nil hints:hints];
    if (image) {
      // Render the real NSCursor image into sRGB. NSColor's calibrated component
      // getters are not a pixel measurement and depend on the display profile.
      unsigned char pixel[4] = {};
      CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
      CGContextRef context = CGBitmapContextCreate(pixel, 1, 1, 8, 4, space,
          kCGBitmapByteOrder32Big | static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast));
      if (context) {
        const size_t width = CGImageGetWidth(image), height = CGImageGetHeight(image);
        CGContextDrawImage(context, CGRectMake(-static_cast<double>(width / 2),
            -static_cast<double>(height / 2), width, height), image);
        const double alpha = pixel[3];
        Number(env, result, "red", alpha ? pixel[0] / alpha : 0);
        Number(env, result, "green", alpha ? pixel[1] / alpha : 0);
        Number(env, result, "blue", alpha ? pixel[2] / alpha : 0);
        Number(env, result, "alpha", alpha / 255);
        CGContextRelease(context);
      }
      CGColorSpaceRelease(space);
    }
  }
  return result;
}

static napi_value ConsoleUnlocked(napi_env env, napi_callback_info) {
  CFDictionaryRef session = CGSessionCopyCurrentDictionary();
  CFTypeRef locked = session ? CFDictionaryGetValue(session, CFSTR("CGSSessionScreenIsLocked")) : nullptr;
  const bool unlocked = session && !(locked && CFGetTypeID(locked) == CFBooleanGetTypeID() &&
      CFBooleanGetValue(static_cast<CFBooleanRef>(locked)));
  if (session) CFRelease(session);
  napi_value result;
  napi_get_boolean(env, unlocked, &result);
  return result;
}

static napi_value Restore(napi_env env, napi_callback_info) {
  if (pointer_saved) {
    CGWarpMouseCursorPosition(original_pointer);
    pointer_saved = false;
  }
  [NSCursor.arrowCursor set];
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}

static napi_value Init(napi_env env, napi_value exports) {
  const napi_property_descriptor properties[] = {
    {"mouse", nullptr, Mouse, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"ready", nullptr, Ready, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"cursor", nullptr, Cursor, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"consoleUnlocked", nullptr, ConsoleUnlocked, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"restore", nullptr, Restore, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, sizeof(properties) / sizeof(properties[0]), properties);
  return exports;
}
NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
