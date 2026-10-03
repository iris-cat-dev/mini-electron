// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>

#include "platform/macos/electron/desktop_api_mac.h"

#include <map>
#include <mutex>
#include <utility>

#include "third_party/libnode/src/node.h"
#include "v8.h"

extern "C" void _register_electron_common_nativeImage();
extern "C" void _register_electron_common_clipboard();
extern "C" void _register_electron_browser_notification();
extern "C" void _register_electron_browser_native_theme();

namespace mini_electron::mac {
namespace {

NSString* FromUTF16(const std::u16string& value) {
  return [[NSString alloc]
      initWithCharacters:reinterpret_cast<const unichar*>(value.data())
                  length:value.size()];
}

std::string ToUTF8(NSString* value) {
  if (!value)
    return {};
  const char* bytes = value.UTF8String;
  return bytes ? std::string(bytes) : std::string();
}

std::mutex g_notification_lock;
std::map<uint64_t, NotificationCallbacks> g_notifications;
uint64_t g_next_notification_id = 1;

NotificationCallbacks TakeNotification(uint64_t identifier) {
  std::lock_guard<std::mutex> lock(g_notification_lock);
  auto found = g_notifications.find(identifier);
  if (found == g_notifications.end())
    return {};
  NotificationCallbacks callbacks = std::move(found->second);
  g_notifications.erase(found);
  return callbacks;
}

uint64_t ParseIdentifier(NSString* identifier) {
  NSScanner* scanner = [NSScanner scannerWithString:identifier ?: @""];
  unsigned long long value = 0;
  return [scanner scanUnsignedLongLong:&value] && scanner.isAtEnd
             ? static_cast<uint64_t>(value)
             : 0;
}
NSString* const kNotificationCategory = @"mini-electron.notification";


@interface MiniElectronNotificationDelegate
    : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation MiniElectronNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    willPresentNotification:(UNNotification*)notification
      withCompletionHandler:(void (^)(UNNotificationPresentationOptions))handler {
  handler(UNNotificationPresentationOptionBanner |
          UNNotificationPresentationOptionList |
          UNNotificationPresentationOptionSound);
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))handler {
  const uint64_t identifier =
      ParseIdentifier(response.notification.request.identifier);
  NotificationCallbacks callbacks = TakeNotification(identifier);
  const bool dismissed =
      [response.actionIdentifier
          isEqualToString:UNNotificationDismissActionIdentifier];
  dispatch_async(dispatch_get_main_queue(), ^{
    if (!dismissed && callbacks.clicked)
      callbacks.clicked();
    if (callbacks.closed)
      callbacks.closed();
  });
  handler();
}
@end

MiniElectronNotificationDelegate* NotificationDelegate() {
  static MiniElectronNotificationDelegate* delegate;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    delegate = [[MiniElectronNotificationDelegate alloc] init];
    UNUserNotificationCenter* center =
        [UNUserNotificationCenter currentNotificationCenter];
    center.delegate = delegate;
    UNNotificationCategory* category = [UNNotificationCategory
        categoryWithIdentifier:kNotificationCategory
                       actions:@[]
             intentIdentifiers:@[]
                       options:UNNotificationCategoryOptionCustomDismissAction];
    [center setNotificationCategories:[NSSet setWithObject:category]];
  });
  return delegate;
}

std::mutex g_theme_lock;
std::map<uint64_t, std::pair<id, std::function<void()>>> g_theme_observers;
uint64_t g_next_theme_observer_id = 1;

void InitializeDesktopApis(v8::Local<v8::Object> exports,
                           v8::Local<v8::Value>,
                           v8::Local<v8::Context> context,
                           void*) {
  exports
      ->Set(context, v8::String::NewFromUtf8Literal(context->GetIsolate(),
                                                    "platform"),
            v8::String::NewFromUtf8Literal(context->GetIsolate(), "darwin"))
      .Check();
}

node::node_module g_desktop_api_module = {
    NODE_MODULE_VERSION,
    NM_F_LINKED,
    nullptr,
    __FILE__,
    nullptr,
    reinterpret_cast<node::addon_context_register_func>(InitializeDesktopApis),
    "electron_common_desktop_apis",
    nullptr,
    nullptr,
};

}  // namespace

bool NativeNotificationsSupported() {
  if (@available(macOS 10.14, *))
    return NSBundle.mainBundle.bundleIdentifier.length > 0;
  return false;
}

uint64_t ShowNativeNotification(const std::u16string& title,
                                const std::u16string& body,
                                bool silent,
                                const std::vector<uint8_t>&,
                                NotificationCallbacks callbacks) {
  if (!NativeNotificationsSupported()) {
    if (callbacks.failed)
      callbacks.failed("Native notifications require a bundled macOS application");
    return 0;
  }

  NotificationDelegate();
  uint64_t identifier;
  {
    std::lock_guard<std::mutex> lock(g_notification_lock);
    identifier = g_next_notification_id++;
    g_notifications.emplace(identifier, std::move(callbacks));
  }

  UNMutableNotificationContent* content =
      [[UNMutableNotificationContent alloc] init];
  content.title = FromUTF16(title);
  content.body = FromUTF16(body);
  if (!silent)
    content.sound = UNNotificationSound.defaultSound;
  content.categoryIdentifier = kNotificationCategory;
  NSString* string_identifier =
      [NSString stringWithFormat:@"%llu", identifier];
  UNNotificationRequest* request =
      [UNNotificationRequest requestWithIdentifier:string_identifier
                                           content:content
                                           trigger:nil];
  UNUserNotificationCenter* center =
      [UNUserNotificationCenter currentNotificationCenter];
  [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert |
                                            UNAuthorizationOptionSound)
                        completionHandler:^(BOOL granted, NSError* error) {
    if (!granted || error) {
      NotificationCallbacks failed = TakeNotification(identifier);
      const std::string message = error
          ? ToUTF8(error.localizedDescription)
          : "Notification permission was denied";
      dispatch_async(dispatch_get_main_queue(), ^{
        if (failed.failed)
          failed.failed(message);
      });
      return;
    }
    [center addNotificationRequest:request
             withCompletionHandler:^(NSError* add_error) {
      if (add_error) {
        NotificationCallbacks failed = TakeNotification(identifier);
        const std::string message = ToUTF8(add_error.localizedDescription);
        dispatch_async(dispatch_get_main_queue(), ^{
          if (failed.failed)
            failed.failed(message);
        });
        return;
      }
      dispatch_async(dispatch_get_main_queue(), ^{
        std::function<void()> shown;
        {
          std::lock_guard<std::mutex> lock(g_notification_lock);
          auto found = g_notifications.find(identifier);
          if (found != g_notifications.end())
            shown = found->second.shown;
        }
        if (shown)
          shown();
      });
    }];
  }];
  return identifier;
}

void CloseNativeNotification(uint64_t identifier, bool notify_closed) {
  if (!identifier)
    return;
  NSString* string_identifier =
      [NSString stringWithFormat:@"%llu", identifier];
  UNUserNotificationCenter* center =
      [UNUserNotificationCenter currentNotificationCenter];
  [center removePendingNotificationRequestsWithIdentifiers:
              @[ string_identifier ]];
  [center removeDeliveredNotificationsWithIdentifiers:@[ string_identifier ]];
  NotificationCallbacks callbacks = TakeNotification(identifier);
  if (notify_closed && callbacks.closed)
    callbacks.closed();
}

bool ShouldUseDarkColors() {
  @autoreleasepool {
    NSAppearance* appearance = NSApp.effectiveAppearance ?: NSAppearance.currentAppearance;
    NSString* match = [appearance bestMatchFromAppearancesWithNames:@[
      NSAppearanceNameAqua, NSAppearanceNameDarkAqua
    ]];
    return [match isEqualToString:NSAppearanceNameDarkAqua];
  }
}

uint64_t ObserveNativeTheme(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(g_theme_lock);
  const uint64_t identifier = g_next_theme_observer_id++;
  id token = [[NSDistributedNotificationCenter defaultCenter]
      addObserverForName:@"AppleInterfaceThemeChangedNotification"
                  object:nil
                   queue:[NSOperationQueue mainQueue]
              usingBlock:^(NSNotification*) {
                std::function<void()> current;
                {
                  std::lock_guard<std::mutex> callback_lock(g_theme_lock);
                  auto found = g_theme_observers.find(identifier);
                  if (found != g_theme_observers.end())
                    current = found->second.second;
                }
                if (current)
                  current();
              }];
  g_theme_observers.emplace(identifier,
                            std::make_pair(token, std::move(callback)));
  return identifier;
}

void RemoveNativeThemeObserver(uint64_t identifier) {
  std::lock_guard<std::mutex> lock(g_theme_lock);
  auto found = g_theme_observers.find(identifier);
  if (found == g_theme_observers.end())
    return;
  [[NSDistributedNotificationCenter defaultCenter]
      removeObserver:found->second.first];
  g_theme_observers.erase(found);
}

void RegisterMacDesktopApiModule() {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    _register_electron_common_nativeImage();
    _register_electron_common_clipboard();
    _register_electron_browser_notification();
    _register_electron_browser_native_theme();
    node_module_register(&g_desktop_api_module);
  });
}

}  // namespace mini_electron::mac
