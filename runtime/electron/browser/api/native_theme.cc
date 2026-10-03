// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "runtime/electron/browser/api/native_theme.h"

#include "build/build_config.h"
#include "runtime/electron/common/gin_helper/object_template_builder.h"
#include "runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "runtime/electron/common/gin_helper/public/wrapper_info.h"
#include "runtime/electron/common/gin_helper/wrappable.h"
#include "runtime/electron/common/node_register_help.h"
#include "runtime/electron/node_bindings.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"

#if BUILDFLAG(IS_MAC)
#include "platform/macos/electron/desktop_api_mac.h"
#endif

namespace atom {
namespace {

#if BUILDFLAG(IS_WIN)
constexpr wchar_t kThemeWindowClass[] = L"MiniElectronNativeThemeWindow";

bool SystemUsesDarkColors()
{
    HIGHCONTRASTW contrast = {};
    contrast.cbSize = sizeof(contrast);
    if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast),
            &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
        return false;
    }
    DWORD light_theme = 1;
    DWORD size = sizeof(light_theme);
    const LSTATUS status = ::RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light_theme, &size);
    return status == ERROR_SUCCESS && light_theme == 0;
}
#endif

} // namespace

NativeTheme::NativeTheme(
    v8::Isolate* isolate, v8::Local<v8::Object> wrapper)
{
    gin_helper::Wrappable<NativeTheme>::InitWith(isolate, wrapper);
#if BUILDFLAG(IS_WIN)
    dark_ = SystemUsesDarkColors();
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = ::GetModuleHandleW(nullptr);
    window_class.lpfnWndProc = WindowProc;
    window_class.lpszClassName = kThemeWindowClass;
    if (::RegisterClassExW(&window_class)
        || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        window_ = ::CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kThemeWindowClass, L"",
            WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
            window_class.hInstance, this);
    }
#elif BUILDFLAG(IS_MAC)
    dark_ = mini_electron::mac::ShouldUseDarkColors();
    observer_identifier_ = mini_electron::mac::ObserveNativeTheme(
        [this] { systemThemeChanged(); });
#endif
}

NativeTheme::~NativeTheme()
{
#if BUILDFLAG(IS_WIN)
    if (window_)
        ::DestroyWindow(window_);
#elif BUILDFLAG(IS_MAC)
    mini_electron::mac::RemoveNativeThemeObserver(observer_identifier_);
#endif
}

void NativeTheme::init(v8::Isolate* isolate, v8::Local<v8::Object> target)
{
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Local<v8::FunctionTemplate> prototype =
        v8::FunctionTemplate::New(isolate, newFunction);
    prototype->SetClassName(
        v8::String::NewFromUtf8Literal(isolate, "NativeTheme"));
    gin_helper::ObjectTemplateBuilder builder(
        isolate, prototype->InstanceTemplate());
    builder.SetProperty(
        "shouldUseDarkColors", &NativeTheme::shouldUseDarkColors);
    target
        ->Set(context, v8::String::NewFromUtf8Literal(isolate, "NativeTheme"),
            prototype->GetFunction(context).ToLocalChecked())
        .Check();
}

bool NativeTheme::shouldUseDarkColors() const
{
    return dark_;
}

void NativeTheme::systemThemeChanged()
{
#if BUILDFLAG(IS_WIN)
    const bool dark = SystemUsesDarkColors();
#elif BUILDFLAG(IS_MAC)
    const bool dark = mini_electron::mac::ShouldUseDarkColors();
#else
    const bool dark = false;
#endif
    if (dark == dark_)
        return;
    dark_ = dark;
    emit("updated");
}

#if BUILDFLAG(IS_WIN)
LRESULT CALLBACK NativeTheme::WindowProc(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    NativeTheme* self = reinterpret_cast<NativeTheme*>(
        ::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<NativeTheme*>(create->lpCreateParams);
        ::SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else if (self
        && (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED
            || message == WM_SYSCOLORCHANGE)) {
        self->systemThemeChanged();
    } else if (message == WM_NCDESTROY) {
        ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return ::DefWindowProcW(window, message, wparam, lparam);
}
#endif

void NativeTheme::newFunction(
    const v8::FunctionCallbackInfo<v8::Value>& args)
{
    if (!args.IsConstructCall())
        return;
    new NativeTheme(args.GetIsolate(), args.This());
    args.GetReturnValue().Set(args.This());
}

gin_helper::WrapperInfo NativeTheme::kWrapperInfo = {
    gin_helper::GinEmbedder::kEmbedderNativeGin
};

void initializeNativeThemeApi(v8::Local<v8::Object> exports,
    v8::Local<v8::Value>, v8::Local<v8::Context> context, void*)
{
    NativeTheme::init(context->GetIsolate(), exports);
}

} // namespace atom

static const char BrowserNativeThemeNative[] = "";
static NodeNative nativeBrowserNativeTheme {
    "NativeTheme", BrowserNativeThemeNative, 0
};

NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_MANUAL(
    electron_browser_native_theme, atom::initializeNativeThemeApi,
    &nativeBrowserNativeTheme)
