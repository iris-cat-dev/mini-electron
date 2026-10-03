// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "runtime/electron/browser/api/notification.h"

#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "build/build_config.h"
#include "runtime/electron/common/api/native_image.h"
#include "runtime/electron/common/gin_helper/dictionary.h"
#include "runtime/electron/common/gin_helper/object_template_builder.h"
#include "runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "runtime/electron/common/gin_helper/public/wrapper_info.h"
#include "runtime/electron/common/gin_helper/wrappable.h"
#include "runtime/electron/common/node_register_help.h"
#include "runtime/electron/node_bindings.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/libnode/src/node_buffer.h"

#if BUILDFLAG(IS_WIN)
#include <shellapi.h>
#include <strsafe.h>
#include <windows.h>
#elif BUILDFLAG(IS_MAC)
#include "platform/macos/electron/desktop_api_mac.h"
#endif

namespace atom {
#if BUILDFLAG(IS_WIN)
PCWSTR GetAppUserModelID();
#endif

namespace {

#if BUILDFLAG(IS_WIN)
constexpr wchar_t kNotificationWindowClass[] =
    L"MiniElectronNativeNotificationWindow";
constexpr UINT kNotificationMessage = WM_APP + 0x3a1;


struct WindowsNotificationCallbacks {
    std::function<void()> shown;
    std::function<void()> clicked;
    std::function<void()> closed;
    std::function<void(const std::string&)> failed;
};

class WindowsNotificationHost {
public:
    static WindowsNotificationHost& Get()
    {
        static WindowsNotificationHost instance;
        return instance;
    }

    bool supported()
    {
        HMODULE shell = ::GetModuleHandleW(L"shell32.dll");
        return shell && ::GetProcAddress(shell, "Shell_NotifyIconW");
    }

    uint64_t Show(const std::u16string& title, const std::u16string& body,
        bool silent, HICON icon, WindowsNotificationCallbacks callbacks)
    {
        if (!EnsureWindow()) {
            if (callbacks.failed)
                callbacks.failed("Unable to create the notification host window");
            return 0;
        }
        const UINT identifier = next_identifier_++;
        NOTIFYICONDATAW data = {};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = identifier;
        data.uFlags = NIF_MESSAGE | NIF_TIP | NIF_INFO;
        data.uCallbackMessage = kNotificationMessage;
        data.dwInfoFlags = icon ? NIIF_USER : NIIF_INFO;
        if (silent)
            data.dwInfoFlags |= NIIF_NOSOUND;
        data.uTimeout = 10000;
        data.hIcon = icon ? icon : ::LoadIconW(nullptr, IDI_APPLICATION);
        data.hBalloonIcon = icon;
        const std::wstring title_w(title.begin(), title.end());
        const std::wstring body_w(body.begin(), body.end());
        ::StringCchCopyW(data.szInfoTitle, std::size(data.szInfoTitle),
            title_w.c_str());
        ::StringCchCopyW(data.szInfo, std::size(data.szInfo), body_w.c_str());
        ::StringCchCopyW(data.szTip, std::size(data.szTip),
            GetAppUserModelID());
        if (!::Shell_NotifyIconW(NIM_ADD, &data)) {
            if (callbacks.failed)
                callbacks.failed("Windows rejected the native notification");
            return 0;
        }
        data.uVersion = NOTIFYICON_VERSION_4;
        ::Shell_NotifyIconW(NIM_SETVERSION, &data);
        callbacks_.emplace(identifier, std::move(callbacks));
        return identifier;
    }

    void Close(uint64_t identifier)
    {
        const UINT id = static_cast<UINT>(identifier);
        DeleteIcon(id);
        callbacks_.erase(id);
    }

private:
    static LRESULT CALLBACK WindowProc(
        HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == kNotificationMessage) {
            Get().OnNotification(static_cast<UINT>(wparam),
                static_cast<UINT>(LOWORD(lparam)));
            return 0;
        }
        return ::DefWindowProcW(window, message, wparam, lparam);
    }

    bool EnsureWindow()
    {
        if (window_)
            return true;
        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        window_class.hInstance = ::GetModuleHandleW(nullptr);
        window_class.lpfnWndProc = WindowProc;
        window_class.lpszClassName = kNotificationWindowClass;
        if (!::RegisterClassExW(&window_class)
            && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        window_ = ::CreateWindowExW(0, kNotificationWindowClass, L"", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, window_class.hInstance, nullptr);
        return window_ != nullptr;
    }

    void DeleteIcon(UINT identifier)
    {
        if (!window_)
            return;
        NOTIFYICONDATAW data = {};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = identifier;
        ::Shell_NotifyIconW(NIM_DELETE, &data);
    }

    void OnNotification(UINT identifier, UINT event)
    {
        auto found = callbacks_.find(identifier);
        if (found == callbacks_.end())
            return;
        if (event != NIN_BALLOONUSERCLICK && event != NIN_BALLOONTIMEOUT
            && event != NIN_BALLOONHIDE) {
            return;
        }
        WindowsNotificationCallbacks callbacks = std::move(found->second);
        callbacks_.erase(found);
        DeleteIcon(identifier);
        if (event == NIN_BALLOONUSERCLICK && callbacks.clicked)
            callbacks.clicked();
        if (callbacks.closed)
            callbacks.closed();
    }

    HWND window_ = nullptr;
    UINT next_identifier_ = 1;
    std::map<UINT, WindowsNotificationCallbacks> callbacks_;
};
#endif

} // namespace

Notification::Notification(v8::Isolate* isolate,
    v8::Local<v8::Object> wrapper, const gin_helper::Dictionary& options)
{
    gin_helper::Wrappable<Notification>::InitWith(isolate, wrapper);
    options.Get("title", &title_);
    options.Get("body", &body_);
    options.Get("silent", &silent_);
    v8::Local<v8::Value> icon;
    if (options.Get("icon", &icon) && icon->IsObject()) {
        v8::Local<v8::Object> icon_object = icon.As<v8::Object>();
        icon_ = NativeImage::GetSelf(icon_object);
        if (icon_)
            icon_handle_.Reset(isolate, icon_object);
    }
}

Notification::~Notification()
{
    closePlatform(false);
    icon_handle_.Reset();
}

void Notification::init(v8::Isolate* isolate, v8::Local<v8::Object> target)
{
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Local<v8::FunctionTemplate> prototype =
        v8::FunctionTemplate::New(isolate, newFunction);
    prototype->SetClassName(
        v8::String::NewFromUtf8Literal(isolate, "Notification"));
    gin_helper::ObjectTemplateBuilder builder(
        isolate, prototype->InstanceTemplate());
    builder.SetMethod("show", &Notification::show);
    builder.SetMethod("close", &Notification::close);
    v8::Local<v8::Function> constructor =
        prototype->GetFunction(context).ToLocalChecked();
    gin_helper::Dictionary constructor_dictionary(isolate, constructor);
    constructor_dictionary.SetMethod("isSupported", +[](const v8::FunctionCallbackInfo<v8::Value>& info) {
        info.GetReturnValue().Set(Notification::isSupported());
    });
    target
        ->Set(context, v8::String::NewFromUtf8Literal(isolate, "Notification"),
            constructor)
        .Check();
}

bool Notification::isSupported()
{
#if BUILDFLAG(IS_WIN)
    return WindowsNotificationHost::Get().supported();
#elif BUILDFLAG(IS_MAC)
    return mini_electron::mac::NativeNotificationsSupported();
#else
    return false;
#endif
}

void Notification::show()
{
    if (shown_ && !closed_)
        return;
    closed_ = false;
#if BUILDFLAG(IS_WIN)
    platform_identifier_ = WindowsNotificationHost::Get().Show(title_, body_,
        silent_, icon_ ? icon_->getIcon() : nullptr,
        { [this] { didShow(); }, [this] { didClick(); },
            [this] { didClose(); },
            [this](const std::string& error) { didFail(error); } });
    if (platform_identifier_)
        didShow();
#elif BUILDFLAG(IS_MAC)
    std::vector<uint8_t> png;
    if (icon_ && !icon_->isEmpty()) {
        v8::Local<v8::Object> encoded = icon_->toPNG();
        const auto* bytes = reinterpret_cast<const uint8_t*>(
            node::Buffer::Data(encoded));
        png.assign(bytes, bytes + node::Buffer::Length(encoded));
    }
    platform_identifier_ = mini_electron::mac::ShowNativeNotification(
        title_, body_, silent_, png,
        { [this] { didShow(); }, [this] { didClick(); },
            [this] { didClose(); },
            [this](const std::string& error) { didFail(error); } });
#else
    didFail("Native notifications are unavailable on this platform");
#endif
}

void Notification::close()
{
    closePlatform(true);
}

void Notification::closePlatform(bool emit_event)
{
    if (!platform_identifier_)
        return;
#if BUILDFLAG(IS_WIN)
    WindowsNotificationHost::Get().Close(platform_identifier_);
#elif BUILDFLAG(IS_MAC)
    mini_electron::mac::CloseNativeNotification(
        platform_identifier_, false);
#endif
    platform_identifier_ = 0;
    if (emit_event)
        didClose();
}

void Notification::didShow()
{
    shown_ = true;
    emit("show");
}

void Notification::didClick()
{
    platform_identifier_ = 0;
    emit("click");
}

void Notification::didClose()
{
    if (closed_)
        return;
    platform_identifier_ = 0;
    closed_ = true;
    emit("close");
}

void Notification::didFail(const std::string& error)
{
    platform_identifier_ = 0;
    emit("failed", error);
}

void Notification::newFunction(
    const v8::FunctionCallbackInfo<v8::Value>& args)
{
    if (!args.IsConstructCall())
        return;
    v8::Local<v8::Object> options = args.Length() > 0 && args[0]->IsObject()
        ? args[0].As<v8::Object>()
        : v8::Object::New(args.GetIsolate());
    gin_helper::Dictionary dictionary(args.GetIsolate(), options);
    new Notification(args.GetIsolate(), args.This(), dictionary);
    args.GetReturnValue().Set(args.This());
}

gin_helper::WrapperInfo Notification::kWrapperInfo = {
    gin_helper::GinEmbedder::kEmbedderNativeGin
};

void initializeNotificationApi(v8::Local<v8::Object> exports,
    v8::Local<v8::Value>, v8::Local<v8::Context> context, void*)
{
    Notification::init(context->GetIsolate(), exports);
}

} // namespace atom

static const char BrowserNotificationNative[] = "";
static NodeNative nativeBrowserNotification {
    "Notification", BrowserNotificationNative, 0
};

NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_MANUAL(
    electron_browser_notification, atom::initializeNotificationApi,
    &nativeBrowserNotification)
