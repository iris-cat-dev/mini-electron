
#ifndef MiniElectronInternalApi_h
#define MiniElectronInternalApi_h

#include "src/runtime/engine/public/engine_api.h"

#if defined(__APPLE__) || defined(_WIN32)
// Configure the first Blink page before its asynchronous creation is queued.
mini_electron_web_view mini_electron_create_web_view_with_background(
    unsigned background_color, bool transparent, bool initially_visible);
#endif

#if defined(__APPLE__)

// AppKit precise deltas are logical pixels, not 120-unit mouse-wheel ticks.
extern "C" BOOL mini_electron_fire_precise_wheel_event(mini_electron_web_view webview,
    int x, int y, float delta_x, float delta_y, unsigned int flags,
    double screen_x, double screen_y);

// Native screen identity is independent of view size and device scale factor.
extern "C" void mini_electron_mac_set_vsync_display_id(
    mini_electron_web_view webview, int64_t display_id);
#endif

// 不知道为啥，如果在mb内部直接调用mbXXX api，在某些linux下会报错，导出表被改了
void mini_electron_destroy_web_view_impl(mini_electron_web_view webviewHandle);
void mini_electron_show_window_impl(mini_electron_web_view webviewHandle, BOOL b);
BOOL mini_electron_fire_mouse_event_impl(mini_electron_web_view webviewHandle, unsigned int message, int x, int y, unsigned int flags);
BOOL mini_electron_fire_key_press_event_impl(mini_electron_web_view webviewHandle, unsigned int charCode, unsigned int flags, BOOL isSystemKey);
BOOL mini_electron_fire_key_up_event_impl(mini_electron_web_view webviewHandle, unsigned int virtualKeyCode, unsigned int flags, BOOL isSystemKey);
BOOL mini_electron_fire_key_down_event_impl(mini_electron_web_view webviewHandle, unsigned int virtualKeyCode, unsigned int flags, BOOL isSystemKey);
BOOL mini_electron_fire_mouse_wheel_event_impl(mini_electron_web_view webviewHandle, int x, int y, int deltaX, int deltaY, unsigned int flags);
BOOL mini_electron_fire_windows_message_impl(mini_electron_web_view webviewHandle, HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result);

using FN_OutputDebugString = void(__cdecl*)(const char* str);
extern FN_OutputDebugString g_outputDebugString;
extern bool g_isDownloadVersion2;

#endif  