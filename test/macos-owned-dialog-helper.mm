// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <node_api.h>
#include <string>
#include <unistd.h>

namespace {
NSString* Text(napi_env env, napi_value value) {
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
    napi_throw_type_error(env, nullptr, "Expected a string");
    return nil;
  }
  std::string bytes(length + 1, '\0');
  napi_get_value_string_utf8(env, value, bytes.data(), bytes.size(), &length);
  return [[NSString alloc] initWithBytes:bytes.data() length:length encoding:NSUTF8StringEncoding];
}
NSWindow* Owner(NSString* title) {
  if (title.length == 0) return nil;
  for (NSWindow* window in NSApp.windows)
    if ([window.title isEqualToString:title] && ![window isKindOfClass:NSSavePanel.class]) return window;
  return nil;
}
NSSavePanel* Panel(NSString* title) {
  for (NSWindow* window in NSApp.windows)
    if ([window isKindOfClass:NSSavePanel.class] && [window.title isEqualToString:title] && window.visible)
      return (NSSavePanel*)window;
  return nil;
}
NSDictionary* State(NSString* owner_title, NSString* panel_title) {
  NSWindow* owner = Owner(owner_title);
  NSSavePanel* panel = Panel(panel_title);
  NSMutableDictionary* state = [@{
    @"visible": @(panel != nil), @"ownerVisible": @(owner.visible),
    @"ownerNumber": @(owner ? owner.windowNumber : 0),
    @"panelNumber": @(panel ? panel.windowNumber : 0),
    @"sheetParentNumber": @(panel.sheetParent ? panel.sheetParent.windowNumber : 0),
    @"sheetParentTitle": panel.sheetParent.title ?: @"",
    @"ownerAttachedSheetNumber": @(owner.attachedSheet ? owner.attachedSheet.windowNumber : 0),
    @"modalWindowNumber": @(NSApp.modalWindow ? NSApp.modalWindow.windowNumber : 0),
    @"keyWindowNumber": @(NSApp.keyWindow ? NSApp.keyWindow.windowNumber : 0),
    @"active": @(NSApp.active)
  } mutableCopy];
  if (panel) {
    state[@"title"] = panel.title;
    state[@"prompt"] = panel.prompt;
    state[@"message"] = panel.message;
    state[@"directory"] = panel.directoryURL.path ?: @"";
    state[@"name"] = panel.nameFieldStringValue;
    state[@"allowedFileTypes"] = panel.allowedFileTypes ?: @[];
    state[@"allowsOtherFileTypes"] = @(panel.allowsOtherFileTypes);
    state[@"firstResponderClass"] = NSStringFromClass(panel.firstResponder.class) ?: @"";
    state[@"defaultButtonTitle"] = panel.defaultButtonCell.title ?: @"";
    if ([panel isKindOfClass:NSOpenPanel.class]) {
      NSOpenPanel* open = (NSOpenPanel*)panel;
      state[@"canChooseFiles"] = @(open.canChooseFiles);
      state[@"canChooseDirectories"] = @(open.canChooseDirectories);
      state[@"allowsMultipleSelection"] = @(open.allowsMultipleSelection);
    }
  }
  return state;
}
napi_value JSON(napi_env env, id value) {
  NSData* data = [NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
  napi_value result;
  napi_create_string_utf8(env, static_cast<const char*>(data.bytes), data.length, &result);
  return result;
}
NSTimer* watchdog = nil;
NSMutableArray* snapshots = nil;
NSString* armed_owner = nil;
NSString* armed_panel = nil;
void Reset() {
  [watchdog invalidate];
  watchdog = nil;
}
napi_value Observe(napi_env env, napi_callback_info info) {
  size_t count = 2;
  napi_value args[2];
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  if (count != 2) { napi_throw_type_error(env, nullptr, "observe(ownerTitle, panelTitle)"); return nullptr; }
  NSString* owner = Text(env, args[0]);
  NSString* panel = Text(env, args[1]);
  if (!owner || !panel) return nullptr;
  Reset();
  armed_owner = owner;
  armed_panel = panel;
  snapshots = [NSMutableArray array];
  napi_value result;
  napi_get_undefined(env, &result);
  return result;
}
napi_value Arm(napi_env env, napi_callback_info info) {
  napi_value result = Observe(env, info);
  if (!result) return nullptr;
  NSString* owner = armed_owner;
  NSString* panel = armed_panel;
  // Runs even inside the old blocking runModal, so fail-before never needs a
  // fake panel result or a stuck JS timer to dismiss the actual native panel.
  const CFTimeInterval start = NSProcessInfo.processInfo.systemUptime;
  watchdog = [NSTimer timerWithTimeInterval:0.05 repeats:YES block:^(NSTimer*) {
    NSMutableDictionary* state = [State(owner, panel) mutableCopy];
    state[@"elapsedMs"] = @((NSProcessInfo.processInfo.systemUptime - start) * 1000);
    [snapshots addObject:state];
    if (NSProcessInfo.processInfo.systemUptime - start >= 2.5) {
      Reset();
      if (NSSavePanel* found = Panel(panel)) {
        state[@"watchdogCancellation"] = @YES;
        [found cancel:nil];
      }
    }
  }];
  [NSRunLoop.mainRunLoop addTimer:watchdog forMode:NSRunLoopCommonModes];
  [NSRunLoop.mainRunLoop addTimer:watchdog forMode:NSModalPanelRunLoopMode];
  return result;
}
napi_value Snapshot(napi_env env, napi_callback_info info) {
  return JSON(env, State(armed_owner, armed_panel));
}
napi_value History(napi_env env, napi_callback_info info) {
  return JSON(env, snapshots ?: @[]);
}
napi_value Act(napi_env env, napi_callback_info info) {
  size_t count = 1;
  napi_value argument;
  napi_get_cb_info(env, info, &count, &argument, nullptr, nullptr);
  NSString* action = count == 1 ? Text(env, argument) : nil;
  NSSavePanel* panel = Panel(armed_panel);
  if (!action || !panel) { napi_throw_error(env, nullptr, "Owned visible file panel not found"); return nullptr; }
  NSMutableDictionary* before = [State(armed_owner, armed_panel) mutableCopy];
  if ([action isEqualToString:@"cancel"]) {
    // Sends Escape through AppKit; the regression separately rejects watchdog
    // cleanup as proof that this input reached the remote file-picker service.
    NSEvent* escape = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
      modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime
      windowNumber:panel.windowNumber context:nil characters:@"\033"
      charactersIgnoringModifiers:@"\033" isARepeat:NO keyCode:53];
    [NSApp sendEvent:escape];
  } else if ([action isEqualToString:@"accept"]) {
    // The picker is an NSRemoteView in AppKit's XPC service. Synthetic NSWindow
    // sendEvent: input never reached it; send real WindowServer keys only to
    // this process, with the exact owned panel already holding native key focus.
    if (!NSApp.active || NSApp.keyWindow != panel) {
      napi_throw_error(env, nullptr, "Owned remote panel does not hold active key focus");
      return nullptr;
    }
    const bool posting_allowed = CGPreflightPostEventAccess();
    before[@"postEventAccess"] = @(posting_allowed);
    before[@"accessibilityTrusted"] = @(AXIsProcessTrusted());
    before[@"inputTransport"] = @"WindowServer-pid";
    if (!posting_allowed) {
      napi_throw_error(env, nullptr, "WindowServer event posting denied; no TCC prompt or fallback is permitted");
      return nullptr;
    }
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    if (!source) {
      napi_throw_error(env, nullptr, "Cannot create native keyboard event source");
      return nullptr;
    }
    for (bool down : {true, false}) {
      CGEventRef event = CGEventCreateKeyboardEvent(source, 36, down);
      if (!event) {
        CFRelease(source);
        napi_throw_error(env, nullptr, "Cannot create native keyboard event");
        return nullptr;
      }
      const UniChar character = '\r';
      CGEventKeyboardSetUnicodeString(event, 1, &character);
      CGEventPostToPid(getpid(), event);
      CFRelease(event);
    }
    CFRelease(source);
  } else {
    napi_throw_type_error(env, nullptr, "Expected cancel or accept");
    return nullptr;
  }
  return JSON(env, before);
}
napi_value Stop(napi_env env, napi_callback_info info) {
  Reset();
  return JSON(env, snapshots ?: @[]);
}
void Cleanup(void*) { Reset(); }
}  // namespace
NAPI_MODULE_INIT() {
  napi_property_descriptor methods[] = {
    {"arm", nullptr, Arm, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"observe", nullptr, Observe, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"snapshot", nullptr, Snapshot, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"history", nullptr, History, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"act", nullptr, Act, nullptr, nullptr, nullptr, napi_default, nullptr},
    {"stop", nullptr, Stop, nullptr, nullptr, nullptr, napi_default, nullptr}
  };
  napi_define_properties(env, exports, sizeof(methods) / sizeof(methods[0]), methods);
  napi_value provenance;
  napi_get_boolean(env, true, &provenance);
  napi_set_named_property(env, exports, "watchdogProvenance", provenance);
  napi_add_env_cleanup_hook(env, Cleanup, nullptr);
  return exports;
}
