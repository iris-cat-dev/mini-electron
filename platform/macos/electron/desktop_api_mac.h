// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef PLATFORM_MACOS_ELECTRON_DESKTOP_API_MAC_H_
#define PLATFORM_MACOS_ELECTRON_DESKTOP_API_MAC_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mini_electron::mac {

struct NotificationCallbacks {
  std::function<void()> shown;
  std::function<void()> clicked;
  std::function<void()> closed;
  std::function<void(const std::string&)> failed;
};

bool NativeNotificationsSupported();
uint64_t ShowNativeNotification(const std::u16string& title,
                                const std::u16string& body,
                                bool silent,
                                const std::vector<uint8_t>& png,
                                NotificationCallbacks callbacks);
void CloseNativeNotification(uint64_t identifier, bool notify_closed);

bool ShouldUseDarkColors();
uint64_t ObserveNativeTheme(std::function<void()> callback);
void RemoveNativeThemeObserver(uint64_t identifier);

// Called by the mac Node host before creating the main Environment. It
// registers the standard Electron desktop linked bindings and the aggregate
// electron_common_desktop_apis marker binding.
void RegisterMacDesktopApiModule();

}  // namespace mini_electron::mac

#endif  // PLATFORM_MACOS_ELECTRON_DESKTOP_API_MAC_H_
