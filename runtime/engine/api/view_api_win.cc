
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#include "runtime/engine/public/engine_api.h"

#include "runtime/engine/download/simple_download.h"
#include "runtime/engine/bindings/js_value.h"
#include "runtime/engine/api/internal_api.h"
#include "runtime/engine/browser/web_view_host.h"
#include "runtime/engine/browser/shared_timer_win.h"
#include "runtime/engine/common/live_id_detect.h"
#include "runtime/engine/common/thread_call.h"
#include "runtime/engine/common/utf16.h"
#include "runtime/engine/renderer/render_thread_impl.h"
#include "runtime/engine/renderer/renderer_blink_platform_impl.h"
#include "runtime/network/loader/web_url_loader_internal.h"
#include "runtime/network/cookies/web_cookie_jar_curl_impl.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/renderer/platform/wtf/allocator/partitions.h"
#include "services/network/public/cpp/resource_request.h"
#include "base/command_line.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_executor.h"
#include "base/at_exit.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#if defined(OS_WIN)
#include "base/win/scoped_com_initializer.h"
#else
#include <stdio.h>
#include <stdlib.h>
#include <utility>
#include <sys/types.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms-compat.h>
#endif // OS_WIN
#include "v8.h"
#include "third_party/skia/include/core/SkTraceMemoryDump.h"
#include "third_party/skia/include/core/SkGraphics.h"
#include "ui/display/win/screen_win.h"

#if OS_LINUX
extern base::RunLoop* g_mainThreadRunLoop;
#endif

namespace blink {
LocalFrame* FromFrameTokenHash(const size_t& frame_token_hash);
}

namespace content {
intptr_t getFrameIdByWebLocalFrame(blink::WebLocalFrame* frame);
}

DWORD g_uiThreadId = 0;

static bool checkThreadCallIsValidImpl(const char* funcName, bool isBlinkThread)
{
    return true;
    //     std::u16string textMsg;
    //     if (!g_mbIsInit) {
    //         textMsg = u16("禁止未初始化调用此接口：");
    //         textMsg += common::utf8ToUtf16(funcName);
    //         ::MessageBoxW(nullptr, textMsg.c_str(), u16("警告"), MB_OK);
    //         ::TerminateProcess((HANDLE)-1, 5);
    //         return false;
    //     }
    //
    //     if (isBlinkThread) {
    //         if (content::ThreadCall::isBlinkThread())
    //             return true;
    //     } else {
    //         if (content::ThreadCall::isUiThread())
    //             return true;
    //     }
    //
    // #if defined(OS_WIN)
    //     textMsg = u16("禁止跨线程调用此接口：");
    //     textMsg += common::utf8ToUtf16(funcName);
    //     textMsg += u16("，");
    //
    //     WCHAR* temp = (WCHAR*)malloc(0x200);
    //     wsprintf(temp, u16("当前线程:%d，主线程：%d"), ::GetCurrentThreadId(), common::ThreadCall::getUiThreadId());
    //     textMsg += temp;
    //     free(temp);
    //
    //     ::MessageBoxW(nullptr, textMsg.c_str(), u16("警告"), MB_OK);
    // #else
    //     printf("function: %s, current thread:%u, main thread:%u", funcName, ::GetCurrentThreadId(), common::ThreadCall::getUiThreadId());
    // #endif
    //     ::TerminateProcess((HANDLE)-1, 5);
    //     return false;
}

bool checkThreadCallIsValid(const char* funcName)
{
    return checkThreadCallIsValidImpl(funcName, false);
}

mini_electron_settings* MINI_ELECTRON_CALL_TYPE mini_electron_create_init_settings()
{
    mini_electron_settings* settings = new mini_electron_settings();
    memset(settings, 0, sizeof(mini_electron_settings));
    settings->version = kMiniElectronVersion;
    return settings;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_init_settings(mini_electron_settings* settings, const char* name, const char* value)
{
    if (0 == strcmp(name, "DisableCC"))
        settings->mask = MINI_ELECTRON_ENABLE_DISABLE_CC;
}

mini_electron_web_view MINI_ELECTRON_CALL_TYPE mini_electron_create_web_view()
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* result = new content::WebViewHost(false);

    // 之所以要嵌套一层，是为了让isTransparent获取到外部接下来可能设置mini_electron_set_transparent的值
    content::ThreadCall::callUiThreadAsync(FROM_HERE, [result] {
        content::ThreadCall::callBlinkThreadAsync(
            FROM_HERE, [result] { result->createWebWindowOrViewInBlinkThread(nullptr, nullptr, false, result->isTransparent()); });
    });

    return (int)result->getId();
}

static bool s_gtkActivate = false;

static void createWebWindowInUiThread(mini_electron_window_type type, HWND parent, int x, int y, int width, int height, content::WebViewHost* newWebview)
{
    newWebview->createWebWindowInUiThread(type, parent, x, y, width, height);

    content::ThreadCall::callBlinkThreadAsync(
        FROM_HERE, [newWebview, type] { newWebview->createWebWindowOrViewInBlinkThread(nullptr, nullptr, true, type == MINI_ELECTRON_WINDOW_TYPE_TRANSPARENT); });
}

static void createCustemWindowInUiThread(HWND parent, DWORD style, DWORD styleEx, int x, int y, int width, int height, content::WebViewHost* newWebview)
{
    newWebview->createWebWindowImplInUiThread(parent, style, styleEx, x, y, width, height);

    content::ThreadCall::callBlinkThreadAsync(FROM_HERE,
        [newWebview, styleEx] { newWebview->createWebWindowOrViewInBlinkThread(nullptr, nullptr, true, WS_EX_LAYERED == (WS_EX_LAYERED & styleEx)); });
}

mini_electron_web_view MINI_ELECTRON_CALL_TYPE mini_electron_create_web_window(mini_electron_window_type type, HWND parent, int x, int y, int width, int height)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* result = new content::WebViewHost(false);

#ifndef OS_WIN
    if (type == MINI_ELECTRON_WINDOW_TYPE_TRANSPARENT)
        type = MINI_ELECTRON_WINDOW_TYPE_POPUP; // linux下没有透明窗口
#endif // OS_WIN

    if (!s_gtkActivate) {
        content::ThreadCall::callUiThreadAsync(
            FROM_HERE, [type, parent, x, y, width, height, result] { createWebWindowInUiThread(type, parent, x, y, width, height, result); });
    } else
        createWebWindowInUiThread(type, parent, x, y, width, height, result);

    return (int)result->getId();
}

mini_electron_web_view MINI_ELECTRON_CALL_TYPE mini_electron_create_web_window_ex(mini_electron_window_type type, HWND parent, int x, int y, int width, int height, const mini_electron_view_settings* settings)
{
    mini_electron_web_view webview = mini_electron_create_web_window(type, parent, x, y, width, height);
    if (settings)
        mini_electron_set_view_settings(webview, settings);
    return webview;
}

mini_electron_web_view MINI_ELECTRON_CALL_TYPE mini_electron_create_web_custom_window(HWND parent, DWORD style, DWORD styleEx, int x, int y, int width, int height)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* result = new content::WebViewHost(false);

    if (!s_gtkActivate) {
        content::ThreadCall::callUiThreadAsync(FROM_HERE,
            [parent, style, styleEx, x, y, width, height, result] { createCustemWindowInUiThread(parent, style, styleEx, x, y, width, height, result); });
    } else
        createCustemWindowInUiThread(parent, style, styleEx, x, y, width, height, result);

    return (int)result->getId();
}

mini_electron_web_view MINI_ELECTRON_CALL_TYPE mini_electron_create_web_view_bind_gtk_window(void* rootWindow, void* drawingArea, const char* type, DWORD style, DWORD styleEx, int width, int height)
{
#if !defined(WIN32)
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* result = new content::WebViewHost(false);

    std::string typeStr(type);
    result->bindGtkWindow(rootWindow, drawingArea, typeStr == "glArea", style, styleEx, width, height);
    content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [result] { result->createWebWindowOrViewInBlinkThread(nullptr, nullptr, true, false); });

    return (int)result->getId();
#else
    return NULL_WEBVIEW;
#endif
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_handle(mini_electron_web_view webviewHandle, HWND wnd)
{
    checkThreadCallIsValid(__FUNCTION__);

    content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [wnd](content::WebViewHost* webview) {
        webview->setHostWnd(wnd); // 必须在这设置，不能放闭包外。那样会导致提前设置的mbSetDragDropEnable无效，从而导致RegisterDragDrop被设置
    });
}

void* MINI_ELECTRON_CALL_TYPE mini_electron_get_platform_window_handle(mini_electron_web_view webviewHandle)
{
    void* handle = (void*)mini_electron_get_host_hwnd(webviewHandle);
#ifdef OS_LINUX
    return HwndToGtkWindow(handle);
#else
    return handle;
#endif // OS_LINUX
}

HWND MINI_ELECTRON_CALL_TYPE mini_electron_get_host_hwnd(mini_electron_web_view webviewHandle)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return NULL;
    return webview->getHostWnd();
}


void gtkMessageBox(const char* txt)
{
#if !defined(OS_WIN)
    GtkDialogFlags flags = (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT);
    GtkWidget* dialog = gtk_message_dialog_new(NULL, flags, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "txt");

    char output[100] = { 0 };
    sprintf(output, "gtkMessageBox: %p\n", dialog);
    OutputDebugStringA(output);

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
#endif
}

#if !defined(OS_WIN)
static void onGtkActivate(GtkApplication* app, gpointer user_data)
{
    s_gtkActivate = true;
}
#endif

FN_OutputDebugString g_outputDebugString = nullptr;
void __cdecl defaultOutputDebugString(const char* str)
{
    OutputDebugStringA(str);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_init(const mini_electron_settings* settings)
{
    if (content::ThreadCall::isInitUiThread())
        return;

    g_outputDebugString = defaultOutputDebugString;

    base::AtExitManager* atExitManager = new base::AtExitManager();
#if defined(OS_WIN)
    base::win::ScopedCOMInitializer* comInitializer = new base::win::ScopedCOMInitializer();
    s_gtkActivate = true;
#else
    gtk_init(nullptr, nullptr);

    GtkApplication* app = gtk_application_new("org.gtk.example", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(app, "activate", G_CALLBACK(onGtkActivate), NULL);

    printf("mini_electron_init: %x\n", ::GetCurrentThreadId());
#endif
    base::CommandLine::Init(0, nullptr);
    content::RenderThreadImpl::get()->initializeWebKit();

#if defined(OS_WIN)
    content::setSharedTimerFireInterval(16);
#endif
}

void MINI_ELECTRON_CALL_TYPE mini_electron_uninit()
{
#if defined(OS_WIN)
    _CrtDumpMemoryLeaks();
#endif
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_did_create_script_context(mini_electron_web_view webviewHandle, mini_electron_did_create_script_context_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setDidCreateScriptContextCallback(callback, param);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_will_release_script_context(mini_electron_web_view webviewHandle, mini_electron_will_release_script_context_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setWillReleaseScriptContextCallback(callback, param);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_navigation(mini_electron_web_view webviewHandle, mini_electron_navigation_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setNavigationCallback(callback, param);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_zoom_factor(mini_electron_web_view webviewHandle, float factor)
{
    content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [webviewHandle, factor] {
        content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
        if (webview)
            webview->setZoomFactor(factor);
    });
}

float MINI_ELECTRON_CALL_TYPE mini_electron_get_zoom_factor(mini_electron_web_view webviewHandle)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return 1.0f;
    return webview->getZoomFactor();
}

mini_electron_js_exec_state MINI_ELECTRON_CALL_TYPE mini_electron_get_global_exec_by_frame(mini_electron_web_view webviewHandle, mini_electron_web_frame_handle frameId)
{
     
    //     if (!webview)
    //         return nullptr;

    return nullptr;
}

int MINI_ELECTRON_CALL_TYPE mini_electron_get_cursor_info_type(mini_electron_web_view webviewHandle)
{
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return 0;
    return webview->getCursorInfoType();
}

int MINI_ELECTRON_CALL_TYPE mini_electron_get_content_width(mini_electron_web_view webviewHandle)
{
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return 1;

    SIZE size = webview->getClientSizeLocked();
    return size.cx;
}

int MINI_ELECTRON_CALL_TYPE mini_electron_get_content_height(mini_electron_web_view webviewHandle)
{
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return 1;

    SIZE size = webview->getClientSizeLocked();
    return size.cy;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_get_caret_rect(mini_electron_web_view webviewHandle, mini_electron_rect* r)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return;
    gfx::Point pt = webview->getCaretPos();
    r->x = pt.x();
    r->y = pt.y();
    r->w = 3;
    r->h = 10;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_editor_un_select(mini_electron_web_view webviewHandle)
{
}

void MINI_ELECTRON_CALL_TYPE mini_electron_editor_copy(mini_electron_web_view webviewHandle)
{
}

void MINI_ELECTRON_CALL_TYPE mini_electron_editor_cut(mini_electron_web_view webviewHandle)
{
}

void MINI_ELECTRON_CALL_TYPE mini_electron_editor_paste(mini_electron_web_view webviewHandle)
{
}

void MINI_ELECTRON_CALL_TYPE mini_electron_editor_delete(mini_electron_web_view webviewHandle)
{
}

void MINI_ELECTRON_CALL_TYPE mini_electron_editor_redo(mini_electron_web_view webviewHandle)
{
}

void MINI_ELECTRON_CALL_TYPE mini_electron_editor_undo(mini_electron_web_view webviewHandle)
{
}

void MINI_ELECTRON_CALL_TYPE mini_electron_run_message_loop()
{
    content::ThreadCall::runUiThreadMessageLoop(nullptr, nullptr, nullptr);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_exit_message_loop()
{
    content::ThreadCall::exitUiThreadMessageLoop();
}

void MINI_ELECTRON_CALL_TYPE mini_electron_move_to_center(mini_electron_web_view webviewHandle)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;

    int width = 0;
    int height = 0;

    HWND hWnd = webview->getHostWnd();

    RECT rect = { 0 };
    ::GetWindowRect(hWnd, &rect);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;

    int parentWidth = 0;
    int parentHeight = 0;
    if (WS_CHILD == ::GetWindowLongW(hWnd, GWL_STYLE)) {
        HWND parent = ::GetParent(hWnd);
        RECT rect = { 0 };
        ::GetClientRect(parent, &rect);
        parentWidth = rect.right - rect.left;
        parentHeight = rect.bottom - rect.top;
    } else {
        parentWidth = ::GetSystemMetrics(SM_CXSCREEN);
        parentHeight = ::GetSystemMetrics(SM_CYSCREEN);
    }

    int x = (parentWidth - width) / 2;
    int y = (parentHeight - height) / 2;

    ::MoveWindow(hWnd, x, y, width, height, FALSE);
}

void mini_electron_show_window_impl(mini_electron_web_view webviewHandle, int nCmdShow)
{
    if (nCmdShow == -1)
        nCmdShow = SW_SHOW;
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return;

    if (content::ThreadCall::isUiThread() && webview->getHostWnd()) {
        webview->setShow(nCmdShow /*, true*/);
        return;
    }

    content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [webviewHandle, nCmdShow]() {
        content::ThreadCall::callUiThreadAsync(FROM_HERE, [webviewHandle, nCmdShow] {
            content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
            if (!webview)
                return;
            webview->setShow(nCmdShow /*, true*/);
        });
    });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_show_window(mini_electron_web_view webviewHandle, int nCmdShow)
{
    mini_electron_show_window_impl(webviewHandle, nCmdShow);
}

BOOL mini_electron_get_window_rect_impl(mini_electron_web_view webviewHandle, mini_electron_rect* rc)
{
    HWND hwnd = mini_electron_get_host_hwnd(webviewHandle);
    if (!hwnd)
        return FALSE;
    RECT windowRect;
    if (!::GetWindowRect(hwnd, &windowRect))
        return FALSE;

    rc->x = windowRect.top;
    rc->y = windowRect.left;
    rc->w = windowRect.bottom - windowRect.top;
    rc->h = windowRect.right - windowRect.left;
    return TRUE;
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_get_window_rect(mini_electron_web_view webviewHandle, mini_electron_rect* rc)
{
    if (content::ThreadCall::isUiThread())
        return mini_electron_get_window_rect_impl(webviewHandle, rc);

    BOOL ret = FALSE;
    content::ThreadCall::callUiThreadSync(FROM_HERE, [webviewHandle, rc, &ret] { ret = mini_electron_get_window_rect_impl(webviewHandle, rc); });
    return ret;
}


void MINI_ELECTRON_CALL_TYPE mini_electron_load_url(mini_electron_web_view webviewHandle, const utf8* url)
{
    std::string* urlString = new std::string(url);

    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return;
    webview->resetState();

    content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [webviewHandle, urlString] {
        content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
        if (webview)
            webview->loadUrl(urlString->c_str());
        delete urlString;
    });

}

void mini_electron_load_html_with_base_url_impl(mini_electron_web_view webviewHandle, const std::string* htmlString, const std::string* baseUrlString, int count)
{
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview) {
        delete htmlString;
        delete baseUrlString;
        return;
    }
    webview->resetState();

    content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [webviewHandle, htmlString, baseUrlString, count] {
        content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
        bool needFree = true;

        if (count > 50)
            Sleep(100);

        if (webview && !webview->loadHTMLString(*htmlString, *baseUrlString) && count < 500) {
            mini_electron_load_html_with_base_url_impl(webviewHandle, htmlString, baseUrlString, count + 1);
            needFree = false;
        }
        if (needFree) {
            delete htmlString;
            delete baseUrlString;
        }
    });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_load_html_with_base_url(mini_electron_web_view webviewHandle, const utf8* html, const utf8* baseUrl)
{
    std::string* htmlString = new std::string(html);
    std::string* baseUrlString = new std::string(baseUrl);
    mini_electron_load_html_with_base_url_impl(webviewHandle, htmlString, baseUrlString, 0);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_auto_draw_to_hwnd(mini_electron_web_view webviewHandle, BOOL b)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->setAutoDrawToHwnd(!!b);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_paint_updated(mini_electron_web_view webviewHandle, mini_electron_paint_updated_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setPaintUpdatedCallback(callback, param);
}


void MINI_ELECTRON_CALL_TYPE mini_electron_on_load_url_fail(mini_electron_web_view webviewHandle, mini_electron_load_url_fail_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setLoadUrlFailCallback(callback, param);
}


BOOL mini_electron_fire_key_up_event_impl(mini_electron_web_view webviewHandle, unsigned int virtualKeyCode, unsigned int flags, BOOL isSystemKey)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle,
        [virtualKeyCode, flags, isSystemKey](content::WebViewHost* webview) { webview->onKeyUp(virtualKeyCode, flags, isSystemKey); });

    return true;
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_fire_key_up_event(mini_electron_web_view webviewHandle, unsigned int virtualKeyCode, unsigned int flags, BOOL isSystemKey)
{
    return mini_electron_fire_key_up_event_impl(webviewHandle, virtualKeyCode, flags, isSystemKey);
}

BOOL mini_electron_fire_key_down_event_impl(mini_electron_web_view webviewHandle, unsigned int virtualKeyCode, unsigned int flags, BOOL isSystemKey)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [virtualKeyCode, flags, isSystemKey](content::WebViewHost* webview) {
        webview->onKeyDown(virtualKeyCode, flags, isSystemKey);

    });

    return true;
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_fire_key_down_event(mini_electron_web_view webviewHandle, unsigned int virtualKeyCode, unsigned int flags, BOOL isSystemKey)
{
    return mini_electron_fire_key_down_event_impl(webviewHandle, virtualKeyCode, flags, isSystemKey);
}

BOOL mini_electron_fire_key_press_event_impl(mini_electron_web_view webviewHandle, unsigned int charCode, unsigned int flags, BOOL isSystemKey)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::ThreadCall::callBlinkThreadAsyncWithValid(
        FROM_HERE, webviewHandle, [charCode, flags, isSystemKey](content::WebViewHost* webview) { webview->onKeyPress(charCode, flags, isSystemKey); });

    return true;
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_fire_key_press_event(mini_electron_web_view webviewHandle, unsigned int charCode, unsigned int flags, BOOL isSystemKey)
{
    return mini_electron_fire_key_press_event_impl(webviewHandle, charCode, flags, isSystemKey);
}

BOOL mini_electron_fire_windows_message_impl(mini_electron_web_view webviewHandle, HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result)
{
    checkThreadCallIsValid(__FUNCTION__);

    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return TRUE;

    if (WM_SETCURSOR == message) {
        if (webview->setCursorInfoTypeByCache()) {
            if (result)
                *result = 1;
            return TRUE;
        }
    } else if (WM_IME_STARTCOMPOSITION == message) {
        webview->onImeComposition(content::WebViewHost::kImeCompositioTypeStart, (WCHAR)(0));

        content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [webviewHandle, hWnd](content::WebViewHost* webview) {
            gfx::Point caret = webview->getCaretPos();

            POINT offset = webview->getOffset();
            int x = caret.x() + offset.x;
            int y = caret.y() + offset.y;

            content::ThreadCall::callUiThreadAsync(FROM_HERE, [hWnd, webviewHandle, x, y] {
                content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
                if (!webview)
                    return;

                COMPOSITIONFORM compositionForm;
                compositionForm.dwStyle = CFS_POINT | CFS_FORCE_POSITION;
                compositionForm.ptCurrentPos.x = x;
                compositionForm.ptCurrentPos.y = y;

                HIMC hIMC = ::ImmGetContext(hWnd);
                ::ImmSetCompositionWindow(hIMC, &compositionForm);
                ::ImmReleaseContext(hWnd, hIMC);
            });
        });
        return false;
    } else if (WM_IME_COMPOSITION == message) {
        WCHAR c = (WCHAR)wParam;
        webview->onImeComposition(content::WebViewHost::kImeCompositioTypeCom, c);

        if (lParam & GCS_RESULTSTR) {
            std::vector<WCHAR> buffer;
            HIMC hIMC = ::ImmGetContext(hWnd);
            int stringSize = ImmGetCompositionStringW(hIMC, GCS_COMPSTR, NULL, 0);
            buffer.resize(stringSize + 2);
            memset(&buffer[0], 0, buffer.size());
            ImmGetCompositionStringW(hIMC, GCS_COMPSTR, &buffer[0], buffer.size() - 2);
            ImmReleaseContext(hWnd, hIMC);
        }
    } else if (WM_IME_ENDCOMPOSITION == message) {
        webview->onImeComposition(content::WebViewHost::kImeCompositioTypeEnd, (WCHAR)(0));
    } else if (WM_IME_CHAR == message) {
        WCHAR c = (WCHAR)wParam;
        webview->onImeComposition(content::WebViewHost::kImeCompositioTypeChar, c);
    } else {
    }
    return false;
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_fire_windows_message(mini_electron_web_view webviewHandle, HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result)
{
    return mini_electron_fire_windows_message_impl(webviewHandle, hWnd, message, wParam, lParam, result);
}

BOOL mini_electron_fire_mouse_event_impl(mini_electron_web_view webviewHandle, unsigned int message, int x, int y, unsigned int flags)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (webview)
        webview->onMouseMessage(message, x, y, flags);

    return TRUE;
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_fire_context_menu_event(mini_electron_web_view webView, int x, int y, unsigned int flags)
{
    return false;
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_fire_mouse_event(mini_electron_web_view webviewHandle, unsigned int message, int x, int y, unsigned int flags)
{
    return mini_electron_fire_mouse_event_impl(webviewHandle, message, x, y, flags);
}

BOOL mini_electron_fire_mouse_wheel_event_impl(mini_electron_web_view webviewHandle, int x, int y, int wheelDelta, unsigned int flags)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return FALSE;

    WPARAM wParam = 0;

    HWND hWnd = webview->getHostWnd();

    POINT screenPoint = { x, y };
    ::ClientToScreen(hWnd, &screenPoint);
    LPARAM lParam = MAKELPARAM(screenPoint.x, screenPoint.y);
    if (flags & MINI_ELECTRON_CONTROL)
        wParam |= MK_CONTROL;
    if (flags & MINI_ELECTRON_SHIFT)
        wParam |= MK_SHIFT;

    if (flags & MINI_ELECTRON_LBUTTON)
        wParam |= MK_LBUTTON;
    if (flags & MINI_ELECTRON_MBUTTON)
        wParam |= MK_MBUTTON;
    if (flags & MINI_ELECTRON_RBUTTON)
        wParam |= MK_RBUTTON;

    wParam = MAKEWPARAM(wParam, wheelDelta);

    webview->fireWheelEventOnUiThread(wParam, lParam);


    return TRUE;
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_fire_mouse_wheel_event(mini_electron_web_view webviewHandle, int x, int y, int wheelDelta, unsigned int flags)
{
    return mini_electron_fire_mouse_wheel_event_impl(webviewHandle, x, y, wheelDelta, flags);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_navigation_to_new_window_enable(mini_electron_web_view webviewHandle, BOOL b)
{
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_load_url_begin(mini_electron_web_view webviewHandle, mini_electron_load_url_begin_callback callback, void* callbackParam)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setLoadUrlBeginCallback(callback, callbackParam);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_hook_request(mini_electron_net_job jobPtr)
{
    checkThreadCallIsValid(__FUNCTION__);
    mini_electron::WebURLLoaderInternal* job = (mini_electron::WebURLLoaderInternal*)jobPtr;
    job->m_hasResponseOverrideData = false;
    if (job->m_responseOverrideData)
        delete (job->m_responseOverrideData);
    job->m_responseOverrideData = nullptr;
    job->m_isHoldJobToAsynCommit = false;
    job->m_isHookRequest = true;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_url_changed(mini_electron_web_view webviewHandle, mini_electron_url_changed_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setURLChangedCallback(callback, param);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_title_changed(mini_electron_web_view webviewHandle, mini_electron_title_changed_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setTitleChangedCallback(callback, param);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_create_view(mini_electron_web_view webviewHandle, mini_electron_create_view_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setCreateViewCallback(callback, param);
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_on_close(mini_electron_web_view webviewHandle, mini_electron_close_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return FALSE;
    webview->getClosure().setCloseCallback(callback, param);

    return TRUE;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_download_in_blink_thread(mini_electron_web_view webviewHandle, mini_electron_download_in_blink_thread_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setDownloadInBlinkThreadCallback(callback, param);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_reload(mini_electron_web_view webviewHandle)
{
    content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [](content::WebViewHost* webview) { webview->reload(FALSE); });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_go_back(mini_electron_web_view webviewHandle)
{
    content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [](content::WebViewHost* webview) { webview->navigateBackForwardSoon(-1); });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_go_forward(mini_electron_web_view webviewHandle)
{
    content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [](content::WebViewHost* webview) { webview->navigateBackForwardSoon(1); });
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_can_go_back_or_forward(mini_electron_web_view webviewHandle, BOOL isGoBack)
{
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return FALSE;

    if (isGoBack)
        return webview->historyBackListCount() > 0;
    return webview->historyForwardListCount() > 0;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_user_agent(mini_electron_web_view webviewHandle, const utf8* userAgent)
{
    checkThreadCallIsValid(__FUNCTION__);
    if (!userAgent)
        return;

    std::string* userAgentString = new std::string(userAgent);

    content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [webviewHandle, userAgentString] {
        content::RendererBlinkPlatformImpl* platform = (content::RendererBlinkPlatformImpl*)blink::Platform::Current();
        platform->setUserAgent(*userAgentString);
        delete userAgentString;
    });
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_is_main_frame(mini_electron_web_view webviewHandle, mini_electron_web_frame_handle frameId)
{
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return FALSE;

    if (frameId == (mini_electron_web_frame_handle)-2)
        return TRUE;
    //     return webview->getMainFrameId() == frameId;

    blink::LocalFrame* blinkFrame = blink::FromFrameTokenHash((size_t)(frameId));
    if (!blinkFrame)
        return FALSE;
    blink::WebLocalFrameImpl* mainFrame = blink::WebLocalFrameImpl::FromFrame(blinkFrame);
    if (!mainFrame)
        return FALSE;
    return mainFrame->View()->MainFrame() == mainFrame;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_resize(mini_electron_web_view webviewHandle, int w, int h)
{
    content::ThreadCall::callBlinkThreadAsyncWithValid(
        FROM_HERE, webviewHandle, [w, h](content::WebViewHost* webview) { webview->onResize(w, h, webview->isWebWindowMode()); });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_wake(mini_electron_web_view webviewHandle)
{
    content::ThreadCall::wake();
}

// 本函数有可能在blink线程（例如js里用close调用）
void mini_electron_destroy_web_view_impl(mini_electron_web_view webviewHandle)
{
    checkThreadCallIsValid(__FUNCTION__);

    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;

    BOOL canContinue = TRUE;
    mini_electron_close_callback closingCallback = webview->getClosure().m_ClosingCallback;
    webview->getClosure().m_ClosingCallback = nullptr;
    if (webview->getClosure().m_ClosingCallback) {
        if (!content::ThreadCall::isUiThread()) {
            content::ThreadCall::callUiThreadSync(FROM_HERE, [&canContinue, webview, webviewHandle, closingCallback] {
                canContinue = (closingCallback((mini_electron_web_view)webviewHandle, webview->getClosure().m_ClosingParam, nullptr));
            });
        } else {
            canContinue = (closingCallback((mini_electron_web_view)webviewHandle, webview->getClosure().m_ClosingParam, nullptr));
        }
    }
    if (!canContinue)
        return;

    if (!webview->preDestroyOnUiThread())
        return;
    webview->getClosure().m_ClosingCallback = nullptr;

    //     if (webview->m_destroyCallback)
    //         webview->m_destroyCallback(webviewHandle, webview->m_destroyCallbackParam, nullptr);

    ::PostMessageW(webview->getHostWnd(), WM_CLOSE, 0, 0);

    content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [webview] { webview->preDestroyOnBlinkThread(); });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_destroy_web_view(mini_electron_web_view webviewHandle)
{
    mini_electron_destroy_web_view_impl(webviewHandle);
}

static mini_electron_download_opt mini_electron_simple_download(mini_electron_web_view mini_electron_webview, const WCHAR* path, const mini_electron_dialog_options* dialogOpt, const mini_electron_download_options* downloadOptions,
    size_t expectedContentLength, const char* url, const char* mime, const char* disposition, mini_electron_net_job job, mini_electron_net_job_data_bind* dataBind,
    mini_electron_download_bind* callbackBind)
{
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(mini_electron_webview);
    if (!webview)
        return kMiniElectronDownloadOptCancel;

    webview->setIsMouseKeyMessageEnable(false);
    download::SimpleDownload* downloader = download::SimpleDownload::create(
        mini_electron_webview, path, dialogOpt, downloadOptions, expectedContentLength, url, mime, disposition, job, dataBind, callbackBind);
    if (downloader)
        return kMiniElectronDownloadOptCacheData;

    webview->setIsMouseKeyMessageEnable(true);
    return kMiniElectronDownloadOptCancel;
}

mini_electron_download_opt MINI_ELECTRON_CALL_TYPE mini_electron_popup_dialog_and_download(mini_electron_web_view webviewHandle, const mini_electron_dialog_options* dialogOpt, size_t expectedContentLength, const char* url,
    const char* mime, const char* disposition, mini_electron_net_job job, mini_electron_net_job_data_bind* dataBind, mini_electron_download_bind* callbackBind)
{
    // #if ENABLE_IN_MB_MAIN
    //     return DownloadMgr::simpleDownload(webviewHandle, nullptr, expectedContentLength, url, mime, disposition, job, dataBind, callbackBind);
    // #endif
    return mini_electron_simple_download(webviewHandle, nullptr, dialogOpt, nullptr, expectedContentLength, url, mime, disposition, job, dataBind, callbackBind);
    return kMiniElectronDownloadOptCancel;
}

mini_electron_download_opt MINI_ELECTRON_CALL_TYPE mini_electron_download_by_path(mini_electron_web_view webviewHandle, const mini_electron_download_options* downloadOptions, const WCHAR* path, size_t expectedContentLength,
    const char* url, const char* mime, const char* disposition, mini_electron_net_job job, mini_electron_net_job_data_bind* dataBind, mini_electron_download_bind* callbackBind)
{
    return mini_electron_simple_download(webviewHandle, path, nullptr, downloadOptions, expectedContentLength, url, mime, disposition, job, dataBind, callbackBind);
}

mini_electron_download_opt MINI_ELECTRON_CALL_TYPE mini_electron_download_by_utf8_path(mini_electron_web_view webviewHandle, const mini_electron_download_options* downloadOptions, const char* path,
    size_t expectedContentLength, const char* url, const char* mime, const char* disposition, mini_electron_net_job job, mini_electron_net_job_data_bind* dataBind,
    mini_electron_download_bind* callbackBind)
{
    if (!path)
        return kMiniElectronDownloadOptCancel;
    std::u16string pathW = base::UTF8ToUTF16(path);
    return mini_electron_simple_download(
        webviewHandle, (const WCHAR*)pathW.c_str(), nullptr, downloadOptions, expectedContentLength, url, mime, disposition, job, dataBind, callbackBind);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_cancel_request(mini_electron_net_job jobPtr)
{
    mini_electron::WebURLLoaderInternal* job = (mini_electron::WebURLLoaderInternal*)jobPtr;
    if (content::ThreadCall::isBlinkThread()) {
        job->m_isEmbedderCanceled = true;
    } else {
        content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [job] { job->m_isEmbedderCanceled = true; });
    }
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_load_url_end(mini_electron_web_view webviewHandle, mini_electron_load_url_end_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setLoadUrlEndCallback(callback, param);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_image_buffer_to_data_url(mini_electron_web_view webviewHandle, mini_electron_image_buffer_to_data_url_callback callback, void* param)
{
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_document_ready(mini_electron_web_view webviewHandle, mini_electron_document_ready_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setDocumentReadyCallback(callback, param);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_document_ready_in_blink_thread(mini_electron_web_view webviewHandle, mini_electron_document_ready_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setDocumentReadyInBlinkCallback(callback, param);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_download(mini_electron_web_view webviewHandle, mini_electron_download_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setDownloadCallback(callback, param);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_loading_finish(mini_electron_web_view webviewHandle, mini_electron_loading_finish_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setLoadingFinishCallback(callback, param);
}

HDC MINI_ELECTRON_CALL_TYPE mini_electron_get_locked_view_dc(mini_electron_web_view webviewHandle)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return NULL;
    return webview->getViewDC();
}

void MINI_ELECTRON_CALL_TYPE mini_electron_unlock_view_dc(mini_electron_web_view webviewHandle)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return;
    webview->unlockViewDC();
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_focus(mini_electron_web_view webviewHandle)
{
    content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [](content::WebViewHost* webview) { webview->setFocus(); });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_kill_focus(mini_electron_web_view webviewHandle)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [](content::WebViewHost* webview) { webview->killFocus(); });
}

class SkTraceMemoryDumpImpl : public SkTraceMemoryDump {
    void dumpNumericValue(const char* dumpName, const char* valueName, const char* units, uint64_t value) override
    {
        char* output = (char*)malloc(400);
        sprintf(output, "SkTraceMemoryDumpImpl: %s %s %d\n", valueName, units, (int)value);
        OutputDebugStringA(output);
        free(output);
    }

    void setMemoryBacking(const char* dumpName, const char* backingType, const char* backingObjectId) override
    {
    }

    void setDiscardableMemoryBacking(const char* dumpName, const SkDiscardableMemory& discardableMemoryObject) override
    {
    }

    SkTraceMemoryDump::LevelOfDetail getRequestedDetails() const override
    {
        return SkTraceMemoryDump::kLight_LevelOfDetail;
    }
};

void MINI_ELECTRON_CALL_TYPE mini_electron_set_resource_gc(mini_electron_web_view webView, int intervalSec)
{
    content::RenderThreadImpl::get()->garbageCollectionDelay(100);
}

std::vector<std::vector<char>*>* s_sharedStringBuffers = nullptr;

const char* createTempCharString(const char* str, size_t length)
{
    if (!str || 0 == length)
        return "";
    std::vector<char>* stringBuffer = new std::vector<char>(length);
    memcpy(&stringBuffer->at(0), str, length * sizeof(char));
    stringBuffer->push_back('\0');

    if (!s_sharedStringBuffers)
        s_sharedStringBuffers = new std::vector<std::vector<char>*>();
    s_sharedStringBuffers->push_back(stringBuffer);
    return &stringBuffer->at(0);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_window_title(mini_electron_web_view webviewHandle, const utf8* title)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->setWindowTitle(title);
}

const utf8* MINI_ELECTRON_CALL_TYPE mini_electron_get_title(mini_electron_web_view webviewHandle)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return "";
    const std::string& title = webview->getWindowTitle();

    return createTempCharString(title.c_str(), title.length());
}

const utf8* MINI_ELECTRON_CALL_TYPE mini_electron_get_url(mini_electron_web_view webviewHandle)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return "";
    const std::string& url = webview->getUrl();

    return createTempCharString(url.c_str(), url.length());
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_transparent(mini_electron_web_view webviewHandle, BOOL transparent)
{
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->setIsTransparent(transparent);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_editable(mini_electron_web_view webviewHandle, bool editable)
{
    OutputDebugStringA("mini_electron_set_transparent not impl\n");
}

mini_electron_js_value MINI_ELECTRON_CALL_TYPE mini_electron_run_js_sync(mini_electron_web_view webviewHandle, mini_electron_web_frame_handle frameId, const utf8* script, BOOL isInClosure)
{
    OutputDebugStringA("mini_electron_set_transparent not impl\n");
    return 0;
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_register_embedder_custom_element(mini_electron_web_view webviewHandle, mini_electron_web_frame_handle frameId, const char* name, void* options, void* outResult)
{
    OutputDebugStringA("mini_electron_register_embedder_custom_element not impl\n");
    return FALSE;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_go_to_offset(mini_electron_web_view webviewHandle, int offset)
{
    content::ThreadCall::callBlinkThreadAsyncWithValid(
        FROM_HERE, webviewHandle, [offset](content::WebViewHost* webview) { webview->navigateBackForwardSoon(offset); });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_go_to_index(mini_electron_web_view webviewHandle, int index)
{
    content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [index](content::WebViewHost* webview) { webview->navigateToIndex(index); });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_navigate_at_index(mini_electron_web_view webviewHandle, int index)
{
    mini_electron_go_to_index(webviewHandle, index);
}

int MINI_ELECTRON_CALL_TYPE mini_electron_get_navigate_index(mini_electron_web_view webviewHandle)
{
    OutputDebugStringA("mini_electron_get_navigate_index not impl\n");
    return 0;
 
//     if (!webview)
//         return 0;
//     return webview->m_navigateIndex();
}

void MINI_ELECTRON_CALL_TYPE mini_electron_post_url(mini_electron_web_view webviewHandle, const utf8* url, const char* postData, int postLen)
{
    OutputDebugStringA("mini_electron_post_url not impl\n");
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_node_js_enable(mini_electron_web_view webviewHandle, BOOL b)
{
    OutputDebugStringA("mini_electron_set_node_js_enable not impl\n");
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_device_parameter(mini_electron_web_view webviewHandle, const char* device, const char* paramStr, int paramInt, float paramFloat)
{
    OutputDebugStringA("mini_electron_set_device_parameter not impl\n");
}

void MINI_ELECTRON_CALL_TYPE mini_electron_get_content_as_markup(mini_electron_web_view webviewHandle, mini_electron_get_content_as_markup_callback calback, void* param, mini_electron_web_frame_handle frameId)
{
    OutputDebugStringA("mini_electron_get_content_as_markup not impl\n");
    *(int*)1 = 1;
}


BOOL MINI_ELECTRON_CALL_TYPE mini_electron_util_print(mini_electron_web_view webviewHandle, mini_electron_web_frame_handle frameId, const mini_electron_print_settings* settings)
{
    OutputDebugStringA("mini_electron_util_print not impl\n");
    return FALSE;
}

const utf8* MINI_ELECTRON_CALL_TYPE mini_electron_util_decode_url_escape(const utf8* str)
{
    OutputDebugStringA("mini_electron_util_decode_url_escape not impl\n");
    *(int*)1 = 1;
    return nullptr;
}

const utf8* MINI_ELECTRON_CALL_TYPE mini_electron_util_encode_url_escape(const utf8* str)
{
    OutputDebugStringA("mini_electron_util_encode_url_escape not impl\n");
    *(int*)1 = 1;
    return nullptr;
}

const mini_electron_mem_buf* MINI_ELECTRON_CALL_TYPE mini_electron_util_create_v8_snapshot(const utf8* str)
{
    OutputDebugStringA("mini_electron_util_create_v8_snapshot not impl\n");
    *(int*)1 = 1;
    return nullptr;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_util_print_to_pdf(mini_electron_web_view webviewHandle, mini_electron_web_frame_handle frameId, const mini_electron_print_settings* settings, mini_electron_print_pdf_data_callback callback, void* param)
{
    OutputDebugStringA("mini_electron_util_print_to_pdf not impl\n");
    *(int*)1 = 1;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_util_print_to_bitmap(mini_electron_web_view webviewHandle, mini_electron_web_frame_handle frameId, const mini_electron_screenshot_settings* settings, mini_electron_print_bitmap_callback callback, void* param)
{
    OutputDebugStringA("mini_electron_util_print_to_bitmap not impl\n");
    *(int*)1 = 1;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_util_screenshot(mini_electron_web_view webviewHandle, const mini_electron_screenshot_settings* settings, mini_electron_on_screenshot callback, void* param)
{
    OutputDebugStringA("mini_electron_util_screenshot not impl\n");
    *(int*)1 = 1;
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_utils_silent_print(mini_electron_web_view webView, const char* settings)
{
    return FALSE;
}


void MINI_ELECTRON_CALL_TYPE mini_electron_get_pdf_page_data(mini_electron_web_view webviewHandle, mini_electron_on_get_pdf_page_data_callback callback, void* param)
{
    OutputDebugStringA("mini_electron_get_pdf_page_data not impl\n");
    *(int*)1 = 1;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_plugin_list_builder_add_plugin(void* builder, const utf8* name, const utf8* description, const utf8* fileName)
{
    OutputDebugStringA("mini_electron_plugin_list_builder_add_plugin not impl\n");
    *(int*)1 = 1;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_plugin_list_builder_add_media_type_to_last_plugin(void* builder, const utf8* name, const utf8* description)
{
    OutputDebugStringA("mini_electron_plugin_list_builder_add_media_type_to_last_plugin not impl\n");
    *(int*)1 = 1;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_plugin_list_builder_add_file_extension_to_last_media_type(void* builder, const utf8* fileExtension)
{
    OutputDebugStringA("mini_electron_plugin_list_builder_add_file_extension_to_last_media_type not impl\n");
    *(int*)1 = 1;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_load_url_finish(mini_electron_web_view webView, mini_electron_load_url_finish_callback callback, void* callbackParam)
{

}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_load_url_headers_received(mini_electron_web_view webView, mini_electron_load_url_headers_received_callback callback, void* callbackParam)
{

}

void MINI_ELECTRON_CALL_TYPE mini_electron_util_set_default_printer_settings(mini_electron_web_view webView, const mini_electron_default_printer_settings* setting)
{

}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_node_create_process(mini_electron_web_view webviewHandle, mini_electron_node_on_create_process_callback callback, void* param)
{
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_thread_idle(mini_electron_thread_callback callback, void* param1, void* param2)
{
    content::ThreadCall::setThreadIdle(callback, param1, param2);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_blink_thread_init(mini_electron_thread_callback callback, void* param1, void* param2)
{
    content::ThreadCall::setBlinkThreadInited(callback, param1, param2);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_call_ui_thread_sync(mini_electron_thread_callback callback, void* param1, void* param2)
{
    content::ThreadCall::callUiThreadSync(FROM_HERE, [callback, param1, param2]() {
        callback(param1, param2);
    });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_call_ui_thread_async(mini_electron_thread_callback callback, void* param1, void* param2)
{
    content::ThreadCall::callUiThreadAsync(FROM_HERE, [callback, param1, param2]() {
        callback(param1, param2);
    });
}

v8Isolate MINI_ELECTRON_CALL_TYPE mini_electron_get_blink_main_thread_isolate()
{
    OutputDebugStringA("mini_electron_get_blink_main_thread_isolate not impl\n");
    *(int*)1 = 1;
    return nullptr;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_web_frame_get_main_world_script_context(mini_electron_web_view webviewHandle, mini_electron_web_frame_handle frameId, v8ContextPtr contextOut)
{
    OutputDebugStringA("mini_electron_web_frame_get_main_world_script_context not impl\n");
    *(int*)1 = 1;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_insert_css_by_frame(mini_electron_web_view webviewHandle, mini_electron_web_frame_handle frameId, const utf8* cssText)
{
    OutputDebugStringA("mini_electron_insert_css_by_frame not impl\n");
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_is_loading(mini_electron_web_view webView)
{
    OutputDebugStringA("mini_electron_is_loading not impl\n");
    //     WKE_CHECK_WEBVIEW_AND_THREAD_IS_VALID(webView, false);
    //     return webView->isLoading();
    DebugBreak();
    return FALSE;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_stop_loading(mini_electron_web_view webView)
{
    OutputDebugStringA("mini_electron_stop_loading not impl\n");
    //     WKE_CHECK_WEBVIEW_AND_THREAD_IS_VALID(webView, (void)0);
    //     webView->stopLoading();
    //DebugBreak();
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_set_http_header_field(mini_electron_net_job jobPtr, const WCHAR* key, const WCHAR* value, BOOL response)
{
    std::string keyUtf8 = base::UTF16ToUTF8(std::u16string_view((const char16_t*)key));
    std::string valueUtf8 = base::UTF16ToUTF8(std::u16string_view((const char16_t*)value));
    mini_electron_net_set_http_header_field_utf8(jobPtr, keyUtf8.c_str(), valueUtf8.c_str(), response);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_editor_select_all(mini_electron_web_view webviewHandle)
{
    OutputDebugStringA("mini_electron_editor_select_all not impl\n");
    DebugBreak();
}

void* MINI_ELECTRON_CALL_TYPE mini_electron_js_to_v8_value(mini_electron_js_exec_state es, mini_electron_js_value v)
{
    mini_electron::engine::JsValueBridge* mini_electron_val = (mini_electron::engine::JsValueBridge*)v;
    if (mini_electron_val->getType() != kMiniElectronJsTypeV8Value)
        return nullptr;
    DebugBreak();
    return nullptr;
}

const char* MINI_ELECTRON_CALL_TYPE mini_electron_net_get_referrer(mini_electron_net_job jobPtr)
{
    return "";
}

void gtkSimpleWin()
{
#if !defined(OS_WIN)
    GtkWidget* window;
    GtkWidget* label;
    GtkWidget* grid;

    // 创建一个新窗口，并设置其标题和默认大小
    //window = gtk_application_window_new(app);
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Simple GTK3 Window");
    gtk_window_set_default_size(GTK_WINDOW(window), 300, 200);

    // 创建一个网格容器
    grid = gtk_grid_new();
    gtk_container_add(GTK_CONTAINER(window), grid);

    // 创建一个标签，并设置要显示的文字
    label = gtk_label_new("Hello, this is a simple GTK3 window!");
    gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);

    // 显示所有的部件
    gtk_widget_show_all(window);
#endif
}

#if OS_LINUX
static gboolean onTimerHeartbeat(gpointer arg)
{
    g_mainThreadRunLoop->RunUntilIdle();
    return TRUE;
}
#endif

bool g_isDownloadVersion2 = false;
extern "C" bool g_disableCookieFlushToFile = false;

void MINI_ELECTRON_CALL_TYPE mini_electron_set_debug_config(mini_electron_web_view webviewHandle, const char* debugString, const char* param)
{
    std::string dbgStr(debugString);
    if (dbgStr == "showDomNode") {
        content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [](content::WebViewHost* webview) { webview->debugShowDomNode(); });
    } else if (dbgStr == "ncHittestPaddingWidth") {
#ifndef _WIN32
        int ncHittestPadding = 0;
        printf("ncHittestPadding\n");
        if (base::StringToInt(base::StringPiece(param), &ncHittestPadding)) {
            printf("ncHittestPadding: %d\n", ncHittestPadding);
            content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
            if (webview)
                webview->setNcHittestPadding(ncHittestPadding);
        }
#endif
    } else if (dbgStr == "setDebugString") {
        g_outputDebugString = (FN_OutputDebugString)param;
    } else if (dbgStr == "setDownloadVersion2") {
        g_isDownloadVersion2 = true;
    } else if (dbgStr == "disableCookieFlushToFile") {
        g_disableCookieFlushToFile = true;
    } else if (dbgStr == "gtkMessageBox") {
        gtkMessageBox(param);
    } else if (dbgStr == "gtkSimpleWin") {
        gtkSimpleWin();
    } else if (dbgStr == "setTimerHeartbeat") {
#if OS_LINUX
        g_timeout_add(16, onTimerHeartbeat, NULL);
#endif
    } else if (dbgStr == "showTotalSizeOfCommittedPages") {
        content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [](content::WebViewHost* webview) {
            size_t size = WTF::Partitions::TotalSizeOfCommittedPages();

            v8::HeapStatistics v8HeapStats;
            v8::Isolate::GetCurrent()->GetHeapStatistics(&v8HeapStats);
            size_t totalMemory = v8HeapStats.total_heap_size();
            size_t totalMemoryExec = v8HeapStats.total_heap_size_executable();
            size_t totalPhysicalSize = v8HeapStats.total_physical_size();

            SkTraceMemoryDumpImpl dump;
            SkGraphics::DumpMemoryStatistics(&dump);

            char output[100] = { 0 };
            sprintf(output, "TotalSizeOfCommittedPages: %d %d\n", (int)size, (int)totalMemory);
            OutputDebugStringA(output);
        });
    }
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_cookie_enabled(mini_electron_web_view webView, BOOL enable)
{
}

double MINI_ELECTRON_CALL_TYPE mini_electron_js_to_double(mini_electron_js_exec_state es, mini_electron_js_value v)
{
    mini_electron::engine::JsValueBridge* jsV = (mini_electron::engine::JsValueBridge*)common::LiveIdDetect::get()->getPtrLocked(v);
    if (!jsV)
        return 0;

    double result = 0;
    if (kMiniElectronJsTypeNumber == jsV->getType())
        result = jsV->getDoubleVal();
    common::LiveIdDetect::get()->unlock(v, jsV);

    return result;
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_js_to_boolean(mini_electron_js_exec_state es, mini_electron_js_value v)
{
    mini_electron::engine::JsValueBridge* jsV = (mini_electron::engine::JsValueBridge*)common::LiveIdDetect::get()->getPtrLocked(v);
    if (!jsV)
        return false;

    BOOL result = FALSE;
    if (kMiniElectronJsTypeBool == jsV->getType())
        result = jsV->getBoolVal();
    common::LiveIdDetect::get()->unlock(v, jsV);

    return result;
}

const utf8* MINI_ELECTRON_CALL_TYPE mini_electron_js_to_string(mini_electron_js_exec_state es, mini_electron_js_value v)
{
    mini_electron::engine::JsValueBridge* jsV = (mini_electron::engine::JsValueBridge*)common::LiveIdDetect::get()->getPtrLocked(v);
    if (!jsV)
        return "";

    std::string result;
    if (kMiniElectronJsTypeString == jsV->getType())
        result = jsV->getStrVal();
    common::LiveIdDetect::get()->unlock(v, jsV);

    if (0 == result.size())
        return "";

    return createTempCharString(result.c_str(), result.size());
}

mini_electron_web_frame_handle MINI_ELECTRON_CALL_TYPE mini_electron_js_to_web_frame_handle(mini_electron_js_exec_state es, mini_electron_js_value v)
{
    mini_electron::engine::JsValueBridge* jsV = (mini_electron::engine::JsValueBridge*)common::LiveIdDetect::get()->getPtrLocked(v);
    if (!jsV)
        return nullptr;

    if (kMiniElectronJsTypeFrame == jsV->getType()) {
        mini_electron_web_frame_handle ret = jsV->getWebFrameHandle();
        common::LiveIdDetect::get()->unlock(v, jsV);
        return ret;
    }

    common::LiveIdDetect::get()->unlock(v, jsV);
    return nullptr;
}

mini_electron_web_frame_handle MINI_ELECTRON_CALL_TYPE mini_electron_get_parent_web_frame_handle(mini_electron_web_view webView, mini_electron_web_frame_handle frame)
{
    if (content::ThreadCall::isBlinkThread())
        return nullptr;

    blink::LocalFrame* blinkFrame = blink::FromFrameTokenHash((size_t)(frame));
    if (!blinkFrame)
        return nullptr;
    blink::WebLocalFrameImpl* mainFrame = blink::WebLocalFrameImpl::FromFrame(blinkFrame);
    blink::WebFrame* parent = mainFrame->Parent();
    if (!parent)
        return nullptr;

    return (mini_electron_web_frame_handle)content::getFrameIdByWebLocalFrame(parent->ToWebLocalFrame());
}

template <class T> static void freeShareds(std::vector<T*>* s_shared)
{
    if (!s_shared)
        return;

    for (size_t i = 0; i < s_shared->size(); ++i) {
        delete s_shared->at(i);
    }
    s_shared->clear();
}

namespace content {

void freeTempCharStrings()
{
    freeShareds(s_sharedStringBuffers);
}

}

std::vector<mini_electron_js_value>* s_jsValues;

mini_electron_js_type MINI_ELECTRON_CALL_TYPE mini_electron_get_js_value_type(mini_electron_js_exec_state es, mini_electron_js_value v)
{
    mini_electron::engine::JsValueBridge* jsV = (mini_electron::engine::JsValueBridge*)common::LiveIdDetect::get()->getPtrLocked(v);
    if (!jsV)
        return kMiniElectronJsTypeUndefined;

    mini_electron_js_type type = jsV->getType();
    common::LiveIdDetect::get()->unlock(v, jsV);
    return type;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_js_value_add_ref(mini_electron_js_exec_state es, mini_electron_js_value v)
{
    mini_electron::engine::JsValueBridge* jsV = (mini_electron::engine::JsValueBridge*)common::LiveIdDetect::get()->getPtrLocked(v);
    if (!jsV)
        return;

    jsV->ref();
    common::LiveIdDetect::get()->unlock(v, jsV);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_js_value_deref(mini_electron_js_exec_state es, mini_electron_js_value v)
{
    mini_electron::engine::JsValueBridge* jsV = (mini_electron::engine::JsValueBridge*)common::LiveIdDetect::get()->getPtrLocked(v);
    if (!jsV)
        return;

    jsV->deref();
    common::LiveIdDetect::get()->unlock(v, jsV);
}

namespace content {
void runJsOnBlinkThread(
    mini_electron_web_view webviewHandle, mini_electron_web_frame_handle frameId, int worldId, std::string* scriptString, BOOL isInClosure, mini_electron_run_js_callback callback, void* param);
}

mini_electron_web_frame_handle MINI_ELECTRON_CALL_TYPE mini_electron_web_frame_get_main_frame(mini_electron_web_view webviewHandle)
{

    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return nullptr;
    blink::WebLocalFrame* frame = (blink::WebLocalFrame*)webview->getMainFrame();
    return (mini_electron_web_frame_handle)(content::getFrameIdByWebLocalFrame(frame));
}

void MINI_ELECTRON_CALL_TYPE mini_electron_run_js(
    mini_electron_web_view webviewHandle, mini_electron_web_frame_handle frameId, const utf8* script, BOOL isInClosure, mini_electron_run_js_callback callback, void* param, void* unuse)
{
    std::string* scriptString = new std::string(script);
    if (content::ThreadCall::isBlinkThread()) {
        content::runJsOnBlinkThread(webviewHandle, frameId, (int)unuse, scriptString, isInClosure, callback, param);
    } else {
        content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [webviewHandle, frameId, unuse, scriptString, isInClosure, callback, param] {
            content::runJsOnBlinkThread(webviewHandle, frameId, (int)unuse, scriptString, isInClosure, callback, param);
        });
    }
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_js_query(mini_electron_web_view webviewHandle, mini_electron_js_query_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);

    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return;

    std::function<void(mini_electron_js_exec_state es, int64_t queryId, int customMsg, const utf8* request)>* closure = nullptr;
    closure = new std::function<void(mini_electron_js_exec_state es, int64_t queryId, int customMsg, const utf8* request)>(
        /*std::move*/ (FROM_HERE, [webviewHandle, callback, param](mini_electron_js_exec_state es, int64_t queryId, int customMsg, const utf8* request) {
            return callback(webviewHandle, param, es, queryId, customMsg, request);
        }));
    webview->getClosure().setJsQueryClosure(closure);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_js_query_ex(mini_electron_web_view webviewHandle, mini_electron_js_query_ex_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);

    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return;

    std::function<void(mini_electron_js_exec_state es, const mini_electron_js_value* val, int count)>* closure = nullptr;
    closure = new std::function<void(mini_electron_js_exec_state es, const mini_electron_js_value* val, int count)>(
        /*std::move*/ (FROM_HERE,
            [webviewHandle, callback, param](mini_electron_js_exec_state es, const mini_electron_js_value* val, int count) { return callback(webviewHandle, param, es, val, count); }));
    webview->getClosure().setJsQuery2Closure(closure);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_response_query(mini_electron_web_view webviewHandle, int64_t queryId, int customMsg, const utf8* response)
{
    std::string* requestString = new std::string(response ? response : "");
    content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [webviewHandle, queryId, customMsg, requestString] {
        std::pair<mini_electron_web_frame_handle, int>* idInfo = (std::pair<mini_electron_web_frame_handle, int>*)queryId;
        content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
        do {
            if (!webview)
                break;
            blink::LocalFrame* blinkFrame = blink::FromFrameTokenHash((size_t)(idInfo->first));
            if (!blinkFrame)
                break;

            v8::Isolate* isolate = v8::Isolate::GetCurrent();
            v8::HandleScope handleScope(isolate);
            blink::WebLocalFrame* mainFrame = blink::WebLocalFrameImpl::FromFrame(blinkFrame);
            if (!mainFrame)
                break;
            v8::Local<v8::Context> context = mainFrame->MainWorldScriptContext();
            v8::MicrotasksScope microtasksScope(context, v8::MicrotasksScope::kDoNotRunMicrotasks);

            context->Enter();

            v8::Local<v8::Object> global = context->Global();
            v8::Local<v8::Value> windowVal
                = global->Get(context, v8::String::NewFromUtf8(isolate, "window", v8::NewStringType::kNormal, -1).ToLocalChecked()).ToLocalChecked();

            v8::Local<v8::Object> windowObj = windowVal->ToObject(context).ToLocalChecked();

            v8::Local<v8::String> onEngineQueryStr = v8::String::NewFromUtf8(isolate, "__onMbQuery__", v8::NewStringType::kNormal, -1).ToLocalChecked();
            v8::Local<v8::Value> onEngineQueryValue = windowObj->Get(context, onEngineQueryStr).ToLocalChecked();
            v8::Function* onEngineQueryFunc = v8::Function::Cast(*onEngineQueryValue);

            v8::Local<v8::Integer> arg0 = v8::Integer::New(isolate, idInfo->second);
            v8::Local<v8::Integer> arg1 = v8::Integer::New(isolate, customMsg);
            v8::Local<v8::String> arg2
                = v8::String::NewFromUtf8(isolate, requestString->c_str(), v8::NewStringType::kNormal, requestString->size()).ToLocalChecked();
            v8::Local<v8::Value> argv[3] = { arg0, arg1, arg2 };

            onEngineQueryFunc->Call(context, v8::Undefined(isolate), 3, argv);
            context->Exit();
        } while (false);

        delete idInfo;
        delete requestString;
    });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_console(mini_electron_web_view webviewHandle, mini_electron_console_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setConsoleCallback(callback, param);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_post_to_ui_thread(mini_electron_on_call_ui_thread callback, void* param)
{
    content::ThreadCall::callUiThreadAsync(FROM_HERE, [callback, param]() { callback(NULL_WEBVIEW, param); });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_post_to_ui_thread_delay(mini_electron_on_call_ui_thread callback, void* param, size_t millisecond)
{
    content::ThreadCall::callUiThreadDelayed(
        FROM_HERE, [callback, param]() { callback(NULL_WEBVIEW, param); }, millisecond);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_get_size(mini_electron_web_view webviewHandle, mini_electron_rect* rc)
{
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return;

    rc->x = 0;
    rc->y = 0;
    rc->w = 0;
    rc->h = 0;

    SIZE size = webview->getClientSizeLocked();
    rc->w = size.cx;
    rc->h = size.cy;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_user_key_value(mini_electron_web_view webviewHandle, const char* key, void* value)
{
    //checkThreadCallIsValid(__FUNCTION__);
    if (!key)
        return;

    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return;
    webview->setUserKeyValue(key, value);
}

void* MINI_ELECTRON_CALL_TYPE mini_electron_get_user_key_value(mini_electron_web_view webviewHandle, const char* key)
{
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return nullptr;
    return webview->getUserKeyValue(key);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_alert_box(mini_electron_web_view webviewHandle, mini_electron_alert_box_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setAlertBoxCallback(callback, param);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_confirm_box(mini_electron_web_view webviewHandle, mini_electron_confirm_box_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setConfirmBoxCallback(callback, param);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_prompt_box(mini_electron_web_view webviewHandle, mini_electron_prompt_box_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setPromptBoxCallback(callback, param);
}

int MINI_ELECTRON_CALL_TYPE mini_electron_query_state(mini_electron_web_view webviewHandle, const char* type)
{
    std::string typeStr(type);
    if ("dispatchWillCommitProvisionalLoad" == typeStr) {
        content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
        if (!webview)
            return -1;
        return webview->hasDispatchWillCommitProvisionalLoad();
    }

    return -1;
}

void getSourceOrMHTML(mini_electron_web_view webviewHandle, mini_electron_get_source_callback calback, void* param, bool isSource);

void MINI_ELECTRON_CALL_TYPE mini_electron_get_source(mini_electron_web_view webviewHandle, mini_electron_get_source_callback calback, void* param)
{
    getSourceOrMHTML(webviewHandle, calback, param, true);
}

mini_electron_string_ptr getSourceOrMhtmlSync(mini_electron_web_view webviewHandle, bool isSource);

mini_electron_string_ptr MINI_ELECTRON_CALL_TYPE mini_electron_get_source_sync(mini_electron_web_view webviewHandle)
{
    return getSourceOrMhtmlSync(webviewHandle, true);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_call_blink_thread_async(mini_electron_thread_callback callback, void* param1, void* param2)
{
    content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [callback, param1, param2] { callback(param1, param2); });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_call_blink_thread_sync(mini_electron_thread_callback callback, void* param1, void* param2)
{
    content::ThreadCall::callBlinkThreadSync(FROM_HERE, [callback, param1, param2]() { callback(param1, param2); });
}

void mini_electron_set_proxy(mini_electron_web_view webviewHandle, const mini_electron_proxy* proxy)
{
    checkThreadCallIsValid(__FUNCTION__);

    mini_electron_proxy* proxyCopy = new mini_electron_proxy();
    memcpy(proxyCopy, proxy, sizeof(mini_electron_proxy));

    content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [webviewHandle, proxyCopy] {
        content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
        if (webview)
            webview->setProxy(proxyCopy);
    });
}

void setFullPath(mini_electron_web_view webviewHandle, const WCHAR* path, bool isCookiePath);

// 这两api如果在blink线程被调用，必须立刻执行。否则会产生老cookie\storage路径文件
void MINI_ELECTRON_CALL_TYPE mini_electron_set_local_storage_full_path(mini_electron_web_view webviewHandle, const WCHAR* path)
{
    setFullPath(webviewHandle, path, false);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_cookie_jar_full_path(mini_electron_web_view webviewHandle, const WCHAR* path)
{
    setFullPath(webviewHandle, path, true);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_context_menu_enabled(mini_electron_web_view webviewHandle, BOOL b)
{
    content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [b](content::WebViewHost* webview) { webview->setContextMenuEnable(!!b); });
}

namespace content {
extern uint32_t g_contextMenuItemMask;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_context_menu_item_show(mini_electron_web_view webviewHandle, mini_electron_menu_item_id item, BOOL isShow)
{
    if (isShow)
        content::g_contextMenuItemMask |= item;
    else
        content::g_contextMenuItemMask &= (~item);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_cookie_jar_path(mini_electron_web_view webviewHandle, const WCHAR* path)
{
    if (!path)
        return;

    std::u16string pathString((const char16_t*)path);
    if (0 == pathString.size())
        return;

    if (MINI_ELECTRON_U16('\\') != pathString[pathString.size() - 1])
        pathString += (char16_t)MINI_ELECTRON_U16('\\');
    pathString += (const char16_t*)MINI_ELECTRON_U16("cookies.dat");

    mini_electron_set_cookie_jar_full_path(webviewHandle, (const WCHAR*)pathString.c_str());
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_view_settings(mini_electron_web_view webviewHandle, const mini_electron_view_settings* settings)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return;


    webview->setBackgroundColor(settings->bgColor);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_window_title_w(mini_electron_web_view webviewHandle, const WCHAR* title)
{
    HWND hwnd = mini_electron_get_host_hwnd(webviewHandle);
    if (!hwnd)
        return;
    ::SetWindowTextW(hwnd, title);
}


static void canGoForwardOrBack(mini_electron_web_view webviewHandle, mini_electron_can_go_back_forward_callback callback, void* param, BOOL isGoForward)
{
    if (!callback)
        return;

    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview) {
        callback(NULL_WEBVIEW, param, MINI_ELECTRON_ASYNC_REQUEST_FAIL, false);
        return;
    }

    content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [webviewHandle, callback, param, isGoForward] {
        BOOL b = false;
        content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
        if (webview)
            b = isGoForward ? (webview->canGoForward()) : (webview->canGoBack());

        content::ThreadCall::callUiThreadAsync(FROM_HERE, [webviewHandle, callback, param, b] {
            content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
            if (!webview) {
                callback(NULL_WEBVIEW, param, MINI_ELECTRON_ASYNC_REQUEST_FAIL, false);
                return;
            }
            callback(webviewHandle, param, MINI_ELECTRON_ASYNC_REQUEST_OK, b);
        });
    });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_can_go_forward(mini_electron_web_view webviewHandle, mini_electron_can_go_back_forward_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    canGoForwardOrBack(webviewHandle, callback, param, TRUE);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_can_go_back(mini_electron_web_view webviewHandle, mini_electron_can_go_back_forward_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    canGoForwardOrBack(webviewHandle, callback, param, FALSE);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_paint_bit_updated(mini_electron_web_view webviewHandle, mini_electron_paint_bit_updated_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setPaintBitUpdatedCallback(callback, param);
}
