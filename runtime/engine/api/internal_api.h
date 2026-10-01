
#ifndef MiniElectronInternalApi_h
#define MiniElectronInternalApi_h

#include "runtime/engine/public/engine_api.h"

// 不知道为啥，如果在mb内部直接调用mbXXX api，在某些linux下会报错，导出表被改了
void mini_electron_destroy_web_view_impl(mini_electron_web_view webviewHandle);
void mini_electron_show_window_impl(mini_electron_web_view webviewHandle, BOOL b);
BOOL mini_electron_fire_mouse_event_impl(mini_electron_web_view webviewHandle, unsigned int message, int x, int y, unsigned int flags);
BOOL mini_electron_fire_key_press_event_impl(mini_electron_web_view webviewHandle, unsigned int charCode, unsigned int flags, BOOL isSystemKey);
BOOL mini_electron_fire_key_up_event_impl(mini_electron_web_view webviewHandle, unsigned int virtualKeyCode, unsigned int flags, BOOL isSystemKey);
BOOL mini_electron_fire_key_down_event_impl(mini_electron_web_view webviewHandle, unsigned int virtualKeyCode, unsigned int flags, BOOL isSystemKey);
BOOL mini_electron_fire_mouse_wheel_event_impl(mini_electron_web_view webviewHandle, int x, int y, int wheelDelta, unsigned int flags);
BOOL mini_electron_fire_windows_message_impl(mini_electron_web_view webviewHandle, HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result);

using FN_OutputDebugString = void(__cdecl*)(const char* str);
extern FN_OutputDebugString g_outputDebugString;
extern bool g_isDownloadVersion2;

#endif  