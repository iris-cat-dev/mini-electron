// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "runtime/engine/renderer/renderer_permission_broker.h"

#include <mutex>
#include <utility>

#include "third_party/blink/public/mojom/permissions/permission.mojom-blink.h"

namespace content {
namespace {

std::mutex g_lock;
RendererPermissionBroker g_broker;

} // namespace

void SetRendererPermissionBroker(RendererPermissionBroker broker)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_broker = std::move(broker);
}

void RequestRendererPermission(const std::string& permission,
    bool user_gesture, bool is_request, RendererPermissionReply reply)
{
    RendererPermissionBroker broker;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        broker = g_broker;
    }
    if (!broker) {
        reply(false);
        return;
    }
    broker(permission, user_gesture, is_request, std::move(reply));
}

std::string RendererPermissionName(int permission_name)
{
    using PermissionName = blink::mojom::blink::PermissionName;
    switch (static_cast<PermissionName>(permission_name)) {
    case PermissionName::GEOLOCATION:
        return "geolocation";
    case PermissionName::NOTIFICATIONS:
        return "notifications";
    case PermissionName::MIDI:
        return "midi";
    case PermissionName::PROTECTED_MEDIA_IDENTIFIER:
        return "mediaKeySystem";
    case PermissionName::DURABLE_STORAGE:
        return "persistent-storage";
    case PermissionName::AUDIO_CAPTURE:
    case PermissionName::VIDEO_CAPTURE:
        return "media";
    case PermissionName::BACKGROUND_SYNC:
        return "background-sync";
    case PermissionName::SENSORS:
        return "sensors";
    case PermissionName::CLIPBOARD_READ:
        return "clipboard-read";
    case PermissionName::CLIPBOARD_WRITE:
        return "clipboard-sanitized-write";
    case PermissionName::PAYMENT_HANDLER:
        return "payment-handler";
    case PermissionName::BACKGROUND_FETCH:
        return "background-fetch";
    case PermissionName::IDLE_DETECTION:
        return "idle-detection";
    case PermissionName::PERIODIC_BACKGROUND_SYNC:
        return "periodic-background-sync";
    case PermissionName::SCREEN_WAKE_LOCK:
        return "screen-wake-lock";
    case PermissionName::SYSTEM_WAKE_LOCK:
        return "system-wake-lock";
    case PermissionName::NFC:
        return "nfc";
    case PermissionName::STORAGE_ACCESS:
        return "storage-access";
    case PermissionName::WINDOW_MANAGEMENT:
        return "window-management";
    case PermissionName::LOCAL_FONTS:
        return "local-fonts";
    case PermissionName::DISPLAY_CAPTURE:
        return "display-capture";
    case PermissionName::TOP_LEVEL_STORAGE_ACCESS:
        return "top-level-storage-access";
    case PermissionName::CAPTURED_SURFACE_CONTROL:
        return "captured-surface-control";
    case PermissionName::SPEAKER_SELECTION:
        return "speaker-selection";
    case PermissionName::KEYBOARD_LOCK:
        return "keyboardLock";
    case PermissionName::POINTER_LOCK:
        return "pointerLock";
    case PermissionName::FULLSCREEN:
        return "fullscreen";
    case PermissionName::WEB_APP_INSTALLATION:
        return "web-app-installation";
    }
    return "unknown";
}

} // namespace content
