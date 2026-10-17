#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#include <node_api.h>
#include <string>

static NSString* Text(napi_env env, napi_value value) {
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
    napi_throw_type_error(env, nullptr, "Expected a string"); return nil;
  }
  std::string text(length + 1, '\0');
  napi_get_value_string_utf8(env, value, text.data(), text.size(), &length);
  return [[NSString alloc] initWithBytes:text.data() length:length encoding:NSUTF8StringEncoding];
}
static NSWindow* Window(napi_env env, napi_value value) {
  NSString* title = Text(env, value);
  NSWindow* result = nil;
  for (NSWindow* window in [NSApplication sharedApplication].windows) {
    if (![window.title isEqualToString:title]) continue;
    if (result) { napi_throw_error(env, nullptr, "Window title is not unique"); return nil; }
    result = window;
  }
  if (!result) napi_throw_error(env, nullptr, "Owned window not found");
  return result;
}
static napi_value Key(napi_env env, napi_callback_info info) {
  size_t count = 3; napi_value args[3];
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  uint32_t code = 0, modifiers = 0;
  if (count != 3 || napi_get_value_uint32(env, args[1], &code) != napi_ok ||
      napi_get_value_uint32(env, args[2], &modifiers) != napi_ok) {
    napi_throw_type_error(env, nullptr, "key(title, hardwareKeycode, modifiers)"); return nullptr;
  }
  NSWindow* window = Window(env, args[0]); if (!window) return nullptr;
  // Translate the actual current keyboard layout, rather than pretending the
  // hardware code is an ASCII or Windows virtual key.
  TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
  CFDataRef data = source ? (CFDataRef)TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) : nullptr;
  if (!data) { if (source) CFRelease(source); napi_throw_error(env, nullptr, "Keyboard layout unavailable"); return nullptr; }
  UInt32 dead = 0; UniChar chars[8]; UniCharCount length = 0;
  UInt32 shift = (modifiers & NSEventModifierFlagShift) ? shiftKey >> 8 : 0;
  UCKeyTranslate((const UCKeyboardLayout*)CFDataGetBytePtr(data), code, kUCKeyActionDown,
      shift, LMGetKbdType(), kUCKeyTranslateNoDeadKeysBit, &dead, 8, &length, chars);
  NSString* text = [NSString stringWithCharacters:chars length:length];
  CFRelease(source);
  for (NSEventType type : {NSEventTypeKeyDown, NSEventTypeKeyUp}) {
    NSEvent* event = [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:modifiers
        timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:window.windowNumber
        context:nil characters:text charactersIgnoringModifiers:text isARepeat:NO keyCode:code];
    [[NSApplication sharedApplication] sendEvent:event];
  }
  napi_value result; napi_get_undefined(env, &result); return result;
}
static NSMenuItem* FindItem(NSMenu* menu, NSString* title) {
  for (NSMenuItem* item in menu.itemArray) {
    if ([item.title isEqualToString:title]) return item;
    if (NSMenuItem* child = FindItem(item.submenu, title)) return child;
  }
  return nil;
}
static napi_value Click(napi_env env, napi_callback_info info) {
  size_t count = 1; napi_value arg;
  napi_get_cb_info(env, info, &count, &arg, nullptr, nullptr);
  NSString* title = count == 1 ? Text(env, arg) : nil;
  NSMenuItem* item = FindItem([NSApplication sharedApplication].mainMenu, title);
  if (!item) { napi_throw_error(env, nullptr, "Native menu item not found"); return nullptr; }
  [item.menu update];
  if (item.enabled && !item.hidden)
    [[NSApplication sharedApplication] sendAction:item.action to:item.target from:item];
  napi_value result; napi_get_undefined(env, &result); return result;
}
static NSArray<NSPasteboardItem*>* saved;
static napi_value Clipboard(napi_env env, napi_callback_info info) {
  size_t count = 1; napi_value arg;
  napi_get_cb_info(env, info, &count, &arg, nullptr, nullptr);
  bool restore = false; if (count) napi_get_value_bool(env, arg, &restore);
  NSPasteboard* board = NSPasteboard.generalPasteboard;
  if (restore) { [board clearContents]; if (saved.count) [board writeObjects:saved]; saved = nil; }
  else {
    NSMutableArray* copies = [NSMutableArray array];
    for (NSPasteboardItem* item in board.pasteboardItems) {
      NSPasteboardItem* copy = [[NSPasteboardItem alloc] init];
      for (NSString* type in item.types) { NSData* data = [item dataForType:type]; if (data) [copy setData:data forType:type]; }
      [copies addObject:copy];
    }
    saved = copies;
  }
  napi_value result; napi_get_undefined(env, &result); return result;
}
static napi_value ImageClipboard(napi_env env, napi_callback_info info) {
  size_t count = 1; napi_value arg;
  napi_get_cb_info(env, info, &count, &arg, nullptr, nullptr);
  NSString* format = count == 1 ? Text(env, arg) : nil;
  if (![format isEqualToString:@"png"] && ![format isEqualToString:@"tiff"]
      && ![format isEqualToString:@"malformed-png"]
      && ![format isEqualToString:@"oversized-png"]) {
    napi_throw_type_error(env, nullptr,
                          "imageClipboard expects a supported image fixture");
    return nullptr;
  }
  // Fixed 1x1 PNG and TIFF fixtures avoid encoder availability differences.
  NSData* png = [[NSData alloc] initWithBase64EncodedString:
      @"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScL9WQAAAABJRU5ErkJggg=="
      options:0];
  NSData* data = png;
  NSPasteboardType type = NSPasteboardTypePNG;
  if ([format isEqualToString:@"tiff"]) {
    data = [[NSData alloc] initWithBase64EncodedString:
        @"SUkqAAgAAAALAAABBAABAAAAAQAAAAEBBAABAAAAAQAAAAIBAwAEAAAAkgAAAAMBAwABAAAAAQAAAAYBAwABAAAAAgAAABEBBAABAAAAmgAAABUBAwABAAAABAAAABYBBAABAAAAAQAAABcBBAABAAAABAAAABwBAwABAAAAAQAAAFIBAwABAAAAAgAAAAAAAAAIAAgACAAIAP8AAP8="
        options:0];
    type = NSPasteboardTypeTIFF;
  } else if ([format isEqualToString:@"malformed-png"]) {
    data = [@"not a PNG" dataUsingEncoding:NSUTF8StringEncoding];
  } else if ([format isEqualToString:@"oversized-png"]) {
    data = [NSMutableData dataWithLength:32 * 1024 * 1024 + 1];
  }
  if (!data) { napi_throw_error(env, nullptr, "Failed to create clipboard image"); return nullptr; }
  NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
  [item setData:data forType:type];
  NSPasteboard* board = NSPasteboard.generalPasteboard;
  [board clearContents];
  if (![board writeObjects:@[ item ]]) {
    napi_throw_error(env, nullptr, "Failed to write clipboard image"); return nullptr;
  }
  napi_value result; napi_get_undefined(env, &result); return result;
}
NAPI_MODULE_INIT() {
  napi_property_descriptor methods[] = {
    {"key", nullptr, Key, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"click", nullptr, Click, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"clipboard", nullptr, Clipboard, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"imageClipboard", nullptr, ImageClipboard, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, 4, methods); return exports;
}
