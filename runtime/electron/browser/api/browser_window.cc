
#include "runtime/electron/browser/api/web_contents.h"
#include "runtime/electron/browser/api/window_list.h"
#include "runtime/electron/browser/api/app.h"
#include "runtime/electron/browser/api/window_interface.h"
#include "runtime/electron/browser/api/protocol_interface.h"
#include "runtime/electron/browser/api/menu_event_notif.h"
#include "runtime/electron/browser/api/browser_view.h"
#include "runtime/electron/browser/api/session.h"
#include "runtime/electron/common/options_switches.h"
#include "runtime/electron/common/node_register_help.h"
#include "runtime/electron/common/embedded_resources.h"
#include "runtime/electron/common/string_util.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "runtime/electron/common/id_live_detect.h"
#include "runtime/electron/common/win_user_msg.h"
//#include "common/DragAction.h"
#include "runtime/electron/common/asar/asar_util.h"
#include "runtime/electron/common/platform_util.h"
#include "runtime/electron/common/gin_helper/per_isolate_data.h"
#include "runtime/electron/common/gin_helper/object_template_builder.h"
#include "runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/libnode/src/node_buffer.h"
#include "third_party/libuv/include/uv.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "runtime/engine/common/thread_call.h"
#include "ui/gfx/icon_util.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/display/win/screen_win.h"
#include "ui/display/screen.h"
#include "base/no_destructor.h"
#include "base/files/file_path.h"
#include "base/strings/utf_string_conversions.h"
#include "base/win/windows_version.h"
#include "platform/windows/resources/resource.h"
#include <shellapi.h>
#include <ole2.h>
#include <dwmapi.h>
#include <algorithm>
#include <cstdlib>
#include <vector>

#pragma comment(lib, "dwmapi.lib")

#pragma clang optimize off
namespace content {
void printCallstack();
}

extern "C" void PrintPath(const WCHAR * path)
{
    std::wstring temp = L"PrintPath";
    temp += path;
    temp += L"\n";
    OutputDebugStringW(temp.c_str());
}

namespace atom {

const int RESIZE_BORDER = 3; // 窗口尺寸和调整边缘的阈值
#define GET_X_LPARAM(lp)   ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp)   ((int)(short)HIWORD(lp))

const wchar_t WindowInterface::kElectronClassName[] = L"mb_electron_window";

// Converts binary data to Buffer.
v8::Local<v8::Value> toBuffer(v8::Isolate* isolate, void* val, int size)
{
    auto buffer = node::Buffer::Copy(isolate, static_cast<char*>(val), size);
    if (buffer.IsEmpty())
        return v8::Null(isolate);
    else
        return buffer.ToLocalChecked();
}

class BrowserWindow : public mate::EventEmitter<BrowserWindow>, public WindowInterface, public WebContentsObserver {
public:
    explicit BrowserWindow(v8::Isolate* isolate, v8::Local<v8::Object> wrapper)
    {
        gin_helper::Wrappable<BrowserWindow>::InitWith(isolate, wrapper);
        if (!display::Screen::GetScreen()) {
            static base::NoDestructor<display::win::ScreenWin> screen;
            display::Screen::SetScreenInstance(screen.get(), FROM_HERE);
        }
        m_webContents = nullptr;
        m_createWindowParam = nullptr;
        m_state = WindowUninited;
        m_hWnd = nullptr;
        m_hIMC = nullptr;
        m_cursorInfoType = 0;
        m_isCursorInfoTypeAsynGetting = false;
        m_memoryBMP = nullptr;
        m_memoryDC = nullptr;
        m_captionDC = nullptr;
        m_captionBitmap = nullptr;
        m_captionBitmapSize = { 0, 0 };
        m_isDestroyApiBeCalled = false;
        m_isMaximized = false;
        m_isFullScreen = false;
        m_isDocumentEdited = false;
        m_isIgnoreMouseEvents = false;
        m_isMouseDown = false;
        m_titleBarOverlayEnabled = false;
        m_titleBarOverlayHeight = 30;
        m_titleBarOverlayColor = RGB(0x18, 0x1b, 0x1a);
        m_titleBarOverlaySymbolColor = RGB(0xe4, 0xe4, 0xe7);
        m_captionHoverButton = CaptionButtonNone;
        m_captionPressedButton = CaptionButtonNone;
        m_trackingMouseLeave = false;
        m_nativeMenu = nullptr;
        m_autoHideMenuBar = false;
        m_menuBarVisible = true;
        m_menuBarAltVisible = false;
        m_lastAcceleratorKey = 0;
        m_fullScreenStyle = 0;
        m_fullScreenExStyle = 0;
        ::ZeroMemory(&m_windowPlacement, sizeof(m_windowPlacement));
        m_windowPlacement.length = sizeof(m_windowPlacement);

        m_clientRect.left = 0;
        m_clientRect.top = 0;
        m_clientRect.right = 0;
        m_clientRect.bottom = 0;
        m_memoryBmpSize.cx = 0;
        m_memoryBmpSize.cy = 0;
        m_contentsSize.cx = 0;
        m_contentsSize.cy = 0;

        ::InitializeCriticalSection(&m_browserViewsLock);
        ::InitializeCriticalSection(&m_memoryCanvasLock);
        ::InitializeCriticalSection(&m_mouseMsgQueueLock);

        m_draggableRegion = ::CreateRectRgn(0, 0, 0, 0);
        //m_dragAction = nullptr; // TODO
        m_foucsBrowserView = nullptr;

        m_id = IdLiveDetect::get()->constructed(this);
    }

    ~BrowserWindow()
    {
        char* output = (char*)malloc(0x100);
        sprintf(output, "~BrowserWindow %p\n", this);
        OutputDebugStringA(output);
        free(output);

        delete m_createWindowParam;

        ::DeleteObject(m_draggableRegion);

        if (m_memoryDC)
            ::DeleteDC(m_memoryDC);
        if (m_memoryBMP)
            ::DeleteObject(m_memoryBMP);
        m_captionSurface.reset();
        if (m_captionBitmap)
            ::DeleteObject(m_captionBitmap);
        if (m_captionDC)
            ::DeleteDC(m_captionDC);

        //ThreadCall::callUiThreadSync([this] {
        //delete data->m_webContents;
        //::SendMessage(this->m_hWnd, WM_CLOSE, 0, 0);
        //});
        //WindowList::getInstance()->removeWindow(this);

        IdLiveDetect::get()->deconstructed(m_id);

        ::DeleteCriticalSection(&m_memoryCanvasLock);
    }

    static const int WM_COPYGLOBALDATA = 0x0049;
    static const int MSG_FLT_ADD = 1;
    typedef WINUSERAPI BOOL WINAPI CHANGEWINDOWMESSAGEFILTER(UINT message, DWORD dwFlag);
    static void changeMessageProi()
    {
        HINSTANCE hDllInst = LoadLibraryW(L"user32.dll");
        if (hDllInst) {
            CHANGEWINDOWMESSAGEFILTER* pAddMessageFilterFunc = (CHANGEWINDOWMESSAGEFILTER*)GetProcAddress(hDllInst, "ChangeWindowMessageFilter");
            if (pAddMessageFilterFunc) {
                pAddMessageFilterFunc(WM_DROPFILES, MSG_FLT_ADD);
                pAddMessageFilterFunc(WM_COPYDATA, MSG_FLT_ADD);
                pAddMessageFilterFunc(WM_COPYGLOBALDATA, MSG_FLT_ADD);
            }
            FreeLibrary(hDllInst);
        }
    }

    static void init(v8::Local<v8::Object> target, node::Environment* env)
    {
        changeMessageProi();

        v8::Isolate* isolate = nodeEnvironmentGetV8Isolate(env);
        v8::Local<v8::Context> context = isolate->GetCurrentContext();
        //gin_helper::PerIsolateData* perIsolateData = new gin_helper::PerIsolateData(isolate, nullptr);

        v8::Local<v8::FunctionTemplate> prototype = v8::FunctionTemplate::New(isolate, newFunction);

        prototype->SetClassName(v8::String::NewFromUtf8(isolate, "BrowserWindow").ToLocalChecked());
        gin_helper::ObjectTemplateBuilder builder(isolate, prototype->InstanceTemplate());
        builder.SetMethod("_getWebContents", &BrowserWindow::_getWebContentsApi);
        builder.SetMethod("close", &BrowserWindow::closeApi);
        builder.SetMethod("focus", &BrowserWindow::focusApi);
        builder.SetMethod("blur", &BrowserWindow::blurApi);
        builder.SetMethod("isFocused", &BrowserWindow::isFocusedApi);
        builder.SetMethod("show", &BrowserWindow::showApi);
        builder.SetMethod("showInactive", &BrowserWindow::showInactiveApi);
        builder.SetMethod("hide", &BrowserWindow::hideApi);
        builder.SetMethod("destroy", &BrowserWindow::destroyApi);
        builder.SetMethod("isVisible", &BrowserWindow::isVisibleApi);
        builder.SetMethod("isEnabled", &BrowserWindow::isEnabledApi);
        builder.SetMethod("maximize", &BrowserWindow::maximizeApi);
        builder.SetMethod("unmaximize", &BrowserWindow::unmaximizeApi);
        builder.SetMethod("isMaximized", &BrowserWindow::isMaximizedApi);
        builder.SetMethod("minimize", &BrowserWindow::minimizeApi);
        builder.SetMethod("restore", &BrowserWindow::restoreApi);
        builder.SetMethod("isMinimized", &BrowserWindow::isMinimizedApi);
        builder.SetMethod("setFullScreen", &BrowserWindow::setFullScreenApi);
        builder.SetMethod("isFullScreen", &BrowserWindow::isFullScreenApi);
        builder.SetMethod("setAspectRatio", &BrowserWindow::nullFunction);
        builder.SetMethod("previewFile", &BrowserWindow::nullFunction);
        builder.SetMethod("closeFilePreview", &BrowserWindow::nullFunction);
        builder.SetMethod("setBrowserView", &BrowserWindow::setBrowserViewApi);
        builder.SetMethod("addBrowserView", &BrowserWindow::setBrowserViewApi);
        builder.SetMethod("removeBrowserView", &BrowserWindow::removeBrowserViewApi);
        builder.SetMethod("setParentWindow", &BrowserWindow::nullFunction);
        builder.SetMethod("getParentWindow", &BrowserWindow::nullFunction);
        builder.SetMethod("getChildWindows", &BrowserWindow::nullFunction);
        builder.SetMethod("setTitleBarOverlay", &BrowserWindow::setTitleBarOverlayApi);
        builder.SetMethod("isSimpleFullScreen", &BrowserWindow::isSimpleFullScreenApi);
        builder.SetMethod("isModal", &BrowserWindow::isModalApi);
        builder.SetMethod("setEnable", &BrowserWindow::setEnableApi);
        builder.SetMethod("getNativeWindowHandle", &BrowserWindow::getNativeWindowHandleApi);
        builder.SetMethod("getBounds", &BrowserWindow::getBoundsApi);
        builder.SetMethod("getNormalBounds", &BrowserWindow::getNormalBoundsApi);
        builder.SetMethod("setBounds", &BrowserWindow::setBoundsApi);
        builder.SetMethod("getSize", &BrowserWindow::getSizeApi);
        builder.SetMethod("setSize", &BrowserWindow::setSizeApi);
        builder.SetMethod("getContentBounds", &BrowserWindow::getContentBoundsApi);
        builder.SetMethod("setContentBounds", &BrowserWindow::setContentBoundsApi);
        builder.SetMethod("getContentSize", &BrowserWindow::getContentSizeApi);
        builder.SetMethod("setContentSize", &BrowserWindow::setContentSizeApi);
        builder.SetMethod("setMinimumSize", &BrowserWindow::setMinimumSizeApi);
        builder.SetMethod("getMinimumSize", &BrowserWindow::getMinimumSizeApi);
        builder.SetMethod("setMaximumSize", &BrowserWindow::setMaximumSizeApi);
        builder.SetMethod("getMaximumSize", &BrowserWindow::getMaximumSizeApi);
        builder.SetMethod("setSheetOffset", &BrowserWindow::nullFunction);
        builder.SetMethod("setResizable", &BrowserWindow::setResizableApi);
        builder.SetMethod("isResizable", &BrowserWindow::isResizableApi);
        builder.SetMethod("setMovable", &BrowserWindow::setMovableApi);
        builder.SetMethod("isMovable", &BrowserWindow::isMovableApi);
        builder.SetMethod("setMinimizable", &BrowserWindow::setMinimizableApi);
        builder.SetMethod("isMinimizable", &BrowserWindow::isMinimizableApi);
        builder.SetMethod("isMaximizable", &BrowserWindow::isMaximizableApi);
        builder.SetMethod("setMaximizable", &BrowserWindow::setMaximizableApi);
        builder.SetMethod("setFullScreenable", &BrowserWindow::setFullScreenableApi);
        builder.SetMethod("isFullScreenable", &BrowserWindow::isFullScreenableApi);
        builder.SetMethod("setClosable", &BrowserWindow::setClosableApi);
        builder.SetMethod("isClosable", &BrowserWindow::isClosableApi);
        builder.SetMethod("setAlwaysOnTop", &BrowserWindow::setAlwaysOnTopApi);
        builder.SetMethod("isAlwaysOnTop", &BrowserWindow::isAlwaysOnTopApi);
        builder.SetMethod("setOpacity", &BrowserWindow::setOpacityApi);
        builder.SetMethod("center", &BrowserWindow::centerApi);
        builder.SetMethod("setWindowButtonPosition", &BrowserWindow::setWindowButtonPositionApi);
        builder.SetMethod("setPosition", &BrowserWindow::setPositionApi);
        builder.SetMethod("getPosition", &BrowserWindow::getPositionApi);
        builder.SetMethod("_setTitle", &BrowserWindow::setTitleApi);
        builder.SetMethod("getTitle", &BrowserWindow::getTitleApi);
        builder.SetMethod("flashFrame", &BrowserWindow::flashFrameApi);
        builder.SetMethod("setSkipTaskbar", &BrowserWindow::setSkipTaskbarApi);
        builder.SetMethod("setKiosk", &BrowserWindow::nullFunction);
        builder.SetMethod("isKiosk", &BrowserWindow::nullFunction);
        builder.SetMethod("setBackgroundColor", &BrowserWindow::setBackgroundColorApi);
        builder.SetMethod("setHasShadow", &BrowserWindow::nullFunction);
        builder.SetMethod("hasShadow", &BrowserWindow::nullFunction);
        builder.SetMethod("setRepresentedFilename", &BrowserWindow::nullFunction);
        builder.SetMethod("getRepresentedFilename", &BrowserWindow::nullFunction);
        builder.SetMethod("setDocumentEdited", &BrowserWindow::setDocumentEditedApi);
        builder.SetMethod("isDocumentEdited", &BrowserWindow::isDocumentEditedApi);
        builder.SetMethod("setIgnoreMouseEvents", &BrowserWindow::setIgnoreMouseEventsApi);
        builder.SetMethod("setContentProtection", &BrowserWindow::setContentProtectionApi);
        builder.SetMethod("setFocusable", &BrowserWindow::setFocusableApi);
        builder.SetMethod("focusOnWebView", &BrowserWindow::focusOnWebViewApi);
        builder.SetMethod("blurWebView", &BrowserWindow::blurApi);
        builder.SetMethod("isWebViewFocused", &BrowserWindow::isWebViewFocusedApi);
        builder.SetMethod("setOverlayIcon", &BrowserWindow::setOverlayIconApi);
        builder.SetMethod("setThumbarButtons", &BrowserWindow::setThumbarButtonsApi);
        builder.SetMethod("setMenu", &BrowserWindow::setMenuApi);
        builder.SetMethod("setAutoHideMenuBar", &BrowserWindow::setAutoHideMenuBarApi);
        builder.SetMethod("isMenuBarAutoHide", &BrowserWindow::isMenuBarAutoHideApi);
        builder.SetMethod("setMenuBarVisibility", &BrowserWindow::setMenuBarVisibilityApi);
        builder.SetMethod("isMenuBarVisible", &BrowserWindow::isMenuBarVisibleApi);
        builder.SetMethod("setVisibleOnAllWorkspaces", &BrowserWindow::setVisibleOnAllWorkspacesApi);
        builder.SetMethod("isVisibleOnAllWorkspaces", &BrowserWindow::isVisibleOnAllWorkspacesApi);
        builder.SetMethod("setVibrancy", &BrowserWindow::nullFunction);
        builder.SetMethod("hookWindowMessage", &BrowserWindow::hookWindowMessageApi);
        builder.SetMethod("isWindowMessageHooked", &BrowserWindow::isWindowMessageHookedApi);
        builder.SetMethod("unhookWindowMessage", &BrowserWindow::unhookWindowMessageApi);
        builder.SetMethod("unhookAllWindowMessages", &BrowserWindow::unhookAllWindowMessagesApi);
        builder.SetMethod("setThumbnailClip", &BrowserWindow::setThumbnailClipApi);
        builder.SetMethod("setThumbnailToolTip", &BrowserWindow::setThumbnailToolTipApi);
        builder.SetMethod("setAppDetails", &BrowserWindow::setAppDetailsApi);
        builder.SetMethod("setIcon", &BrowserWindow::setIconApi);
        builder.SetMethod("setProgressBar", &BrowserWindow::setProgressBarApi);
        builder.SetMethod("isDestroyed", &BrowserWindow::isDestroyedApi);
        builder.SetMethod("moveTop", &BrowserWindow::moveTopApi);

        builder.SetProperty("id", &BrowserWindow::getIdApi);

        //NODE_SET_PROTOTYPE_METHOD(prototype, &BrowserWindow::"id", &BrowserWindow::nullFunction);

        gin_helper::Dictionary browserWindowClass(isolate, prototype->GetFunction(context).ToLocalChecked());
        browserWindowClass.SetMethod("getFocusedWindow", &BrowserWindow::getFocusedWindowApi);
        browserWindowClass.SetMethod("fromId", &BrowserWindow::fromIdApi);
        browserWindowClass.SetMethod("getAllWindows", &BrowserWindow::getAllWindowsApi);
        browserWindowClass.SetMethod("fromWebContents", &BrowserWindow::fromWebContentsApi);

        constructor.Reset(isolate, prototype->GetFunction(context).ToLocalChecked());
        target->Set(context, v8::String::NewFromUtf8(isolate, "BrowserWindow").ToLocalChecked(), prototype->GetFunction(context).ToLocalChecked());
    }

    // WindowInterface impl
    virtual bool isClosed() override
    {
        return m_state == WindowDestroyed;
    }

    virtual void close() override
    {
        ::PostMessage(m_hWnd, WM_CLOSE, 0, 0);
    }

    virtual void destroy() override
    {
        m_isDestroyApiBeCalled = true;
        ::DestroyWindow(m_hWnd);
    }

    virtual v8::Local<v8::Object> getWrapper() override
    {
        return GetWrapper(isolate());
    }

    virtual int getId() const override
    {
        return m_id;
    }

    virtual WebContents* getWebContents() const override
    {
        return m_webContents;
    }

    virtual HWND getHWND() const override
    {
        return m_hWnd;
    }

    void onWebContentsPaint(WebContents*) override
    {
        if (::IsWindow(m_hWnd))
            ::InvalidateRect(m_hWnd, nullptr, FALSE);
    }

    void onWebContentsDraggableRegions(
        WebContents*, const base::Value::List& regions) override
    {
        m_draggableRegions.clear();
        m_draggableRegions.reserve(regions.size());
        for (const base::Value& value : regions) {
            const base::Value::Dict* region = value.GetIfDict();
            if (!region)
                continue;
            const auto x = region->FindInt("x");
            const auto y = region->FindInt("y");
            const auto width = region->FindInt("width");
            const auto height = region->FindInt("height");
            const auto draggable = region->FindBool("draggable");
            if (!x || !y || !width || !height || !draggable
                || *width <= 0 || *height <= 0)
                continue;
            m_draggableRegions.push_back({
                gfx::Rect(*x, *y, *width, *height), *draggable });
        }
        rebuildDraggableRegion();
    }

    void rebuildDraggableRegion()
    {
        if (!m_hWnd)
            return;
        HRGN draggable = ::CreateRectRgn(0, 0, 0, 0);
        HRGN no_drag = ::CreateRectRgn(0, 0, 0, 0);
        HRGN item = ::CreateRectRgn(0, 0, 0, 0);
        const float scale = display::win::ScreenWin::GetScaleFactorForHWND(m_hWnd);
        for (const DraggableRegion& region : m_draggableRegions) {
            const gfx::Rect bounds = gfx::ScaleToEnclosingRect(region.bounds, scale);
            ::SetRectRgn(item, bounds.x(), bounds.y(), bounds.right(), bounds.bottom());
            HRGN target = region.draggable ? draggable : no_drag;
            ::CombineRgn(target, target, item, RGN_OR);
        }
        ::CombineRgn(draggable, draggable, no_drag, RGN_DIFF);
        ::DeleteObject(item);
        ::DeleteObject(no_drag);
        ::DeleteObject(m_draggableRegion);
        m_draggableRegion = draggable;
    }

    virtual void setNativeMenu(HMENU menu) override
    {
        m_nativeMenu = menu;
        updateNativeMenu();
    }

    enum CaptionButton {
        CaptionButtonNone = -1,
        CaptionButtonMinimize = 0,
        CaptionButtonMaximize = 1,
        CaptionButtonClose = 2,
    };

    static bool parseOverlayColor(const std::string& input, COLORREF* result)
    {
        if (!result || input.empty() || input[0] != '#')
            return false;

        const char* digits = input.c_str() + 1;
        size_t length = input.size() - 1;
        char expanded[7] = { 0 };
        if (length == 3 || length == 4) {
            for (size_t i = 0; i < 3; ++i) {
                expanded[i * 2] = digits[i];
                expanded[i * 2 + 1] = digits[i];
            }
            digits = expanded;
            length = 6;
        }
        if (length != 6 && length != 8)
            return false;

        for (size_t i = 0; i < length; ++i) {
            const char c = digits[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return false;
        }
        std::string rgbDigits(digits, 6);
        char* end = nullptr;
        unsigned long rgb = std::strtoul(rgbDigits.c_str(), &end, 16);
        if (!end || *end != '\0')
            return false;
        *result = RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
        return true;
    }

    UINT windowDpi() const
    {
        typedef UINT(WINAPI* GetDpiForWindowFn)(HWND);
        static GetDpiForWindowFn getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
            ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        UINT dpi = getDpiForWindow && m_hWnd ? getDpiForWindow(m_hWnd) : 96;
        return dpi ? dpi : 96;
    }

    int dipToPixel(int dip) const
    {
        return ::MulDiv(dip, windowDpi(), 96);
    }

    bool isCustomTitleBar() const
    {
        return m_titleBarOverlayEnabled && m_createWindowParam && !m_createWindowParam->isFrame && !m_isFullScreen;
    }

    void updateWindowFrame(HWND window) const
    {
        if (m_createWindowParam->isFrame || m_createWindowParam->transparent)
            return;
        const MARGINS margins = { 1, 1, 1, 1 };
        ::DwmExtendFrameIntoClientArea(window, &margins);
    }

    RECT captionButtonRect(CaptionButton button) const
    {
        RECT client = { 0 };
        ::GetClientRect(m_hWnd, &client);
        const int buttonWidth = dipToPixel(46);
        const bool restored = !::IsZoomed(m_hWnd);
        const int topInset = restored ? dipToPixel(1) : 0;
        const int overlayHeight = std::max(1, dipToPixel(m_titleBarOverlayHeight - (restored ? 1 : 0)));
        const int indexFromRight = CaptionButtonClose - button;
        RECT result = {
            std::max(client.left, client.right - buttonWidth * (indexFromRight + 1)),
            client.top + topInset,
            std::max(client.left, client.right - buttonWidth * indexFromRight),
            std::min(client.bottom, client.top + topInset + overlayHeight)
        };
        return result;
    }

    RECT captionButtonsRect() const
    {
        RECT result = captionButtonRect(CaptionButtonMinimize);
        RECT close = captionButtonRect(CaptionButtonClose);
        result.top = 0;
        result.right = close.right;
        return result;
    }

    CaptionButton captionButtonFromPoint(POINT point) const
    {
        if (!isCustomTitleBar())
            return CaptionButtonNone;
        for (int value = CaptionButtonMinimize; value <= CaptionButtonClose; ++value) {
            CaptionButton button = static_cast<CaptionButton>(value);
            RECT rect = captionButtonRect(button);
            if (::PtInRect(&rect, point))
                return button;
        }
        return CaptionButtonNone;
    }

    bool isCaptionButtonEnabled(CaptionButton button) const
    {
        if (!m_createWindowParam)
            return false;
        if (button == CaptionButtonMinimize)
            return m_createWindowParam->isMinimizable;
        if (button == CaptionButtonMaximize)
            return m_createWindowParam->isMaximizable || !!::IsZoomed(m_hWnd);
        if (button == CaptionButtonClose)
            return m_createWindowParam->isClosable;
        return false;
    }

    static COLORREF blendColor(COLORREF from, COLORREF to, int toPercent)
    {
        const int fromPercent = 100 - toPercent;
        return RGB(
            (GetRValue(from) * fromPercent + GetRValue(to) * toPercent) / 100,
            (GetGValue(from) * fromPercent + GetGValue(to) * toPercent) / 100,
            (GetBValue(from) * fromPercent + GetBValue(to) * toPercent) / 100);
    }

    void invalidateCaptionButtons()
    {
        if (!m_hWnd || !isCustomTitleBar())
            return;
        RECT rect = captionButtonsRect();
        ::InvalidateRect(m_hWnd, &rect, FALSE);
    }

    bool ensureCaptionSurface(HDC dc, int width, int height)
    {
        if (m_captionSurface && m_captionBitmapSize.cx == width && m_captionBitmapSize.cy == height)
            return true;
        if (!m_captionDC)
            m_captionDC = ::CreateCompatibleDC(dc);
        if (!m_captionDC)
            return false;
        m_captionSurface.reset();
        if (m_captionBitmap)
            ::DeleteObject(m_captionBitmap);
        BITMAPINFO info = { 0 };
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        m_captionBitmap = ::CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!m_captionBitmap)
            return false;
        m_captionBitmapSize = { width, height };
        m_captionSurface = SkSurfaces::WrapPixels(SkImageInfo::MakeN32Premul(width, height), pixels, width * 4);
        return !!m_captionSurface;
    }

    void paintTitleBarOverlay(HDC dc, const RECT& invalidRect)
    {
        if (!isCustomTitleBar())
            return;
        RECT buttons = captionButtonsRect();
        RECT intersection;
        if (!::IntersectRect(&intersection, &buttons, &invalidRect))
            return;
        const int width = buttons.right - buttons.left;
        const int height = buttons.bottom - buttons.top;
        if (!ensureCaptionSurface(dc, width, height))
            return;

        ::GdiFlush();
        SkCanvas* canvas = m_captionSurface->getCanvas();
        canvas->save();
        canvas->translate(-buttons.left, -buttons.top);
        SkPaint paint;
        const int symbolSize = std::max(1, dipToPixel(10));
        const int strokeWidth = std::max(1, dipToPixel(1));
        for (int value = CaptionButtonMinimize; value <= CaptionButtonClose; ++value) {
            CaptionButton button = static_cast<CaptionButton>(value);
            RECT rect = captionButtonRect(button);
            COLORREF background = m_titleBarOverlayColor;
            if (button == m_captionPressedButton && button == m_captionHoverButton) {
                background = button == CaptionButtonClose
                    ? RGB(0xb4, 0x26, 0x35)
                    : blendColor(background, RGB(0xff, 0xff, 0xff), 20);
            } else if (button == m_captionHoverButton) {
                background = button == CaptionButtonClose
                    ? RGB(0xc4, 0x2b, 0x3a)
                    : blendColor(background, RGB(0xff, 0xff, 0xff), 12);
            }
            const SkColor backgroundColor = SkColorSetRGB(GetRValue(background), GetGValue(background), GetBValue(background));
            paint.setStyle(SkPaint::kFill_Style);
            paint.setAntiAlias(false);
            paint.setColor(backgroundColor);
            canvas->drawRect(SkRect::MakeLTRB(rect.left, buttons.top, rect.right, rect.bottom), paint);

            COLORREF symbol = isCaptionButtonEnabled(button)
                ? m_titleBarOverlaySymbolColor
                : blendColor(m_titleBarOverlayColor, m_titleBarOverlaySymbolColor, 40);
            paint.setColor(SkColorSetRGB(GetRValue(symbol), GetGValue(symbol), GetBValue(symbol)));
            paint.setStyle(SkPaint::kStroke_Style);
            paint.setStrokeWidth(strokeWidth);
            const int left = (rect.left + rect.right - symbolSize) / 2;
            const int top = (rect.top + rect.bottom - symbolSize) / 2;
            const float inset = strokeWidth / 2.0f;
            SkRect glyph = SkRect::MakeLTRB(left + inset, top + inset,
                left + symbolSize - inset, top + symbolSize - inset);
            if (button == CaptionButtonMinimize) {
                const int centerY = top + symbolSize / 2;
                canvas->drawLine(left, centerY, left + symbolSize, centerY, paint);
            } else if (button == CaptionButtonMaximize) {
                if (::IsZoomed(m_hWnd)) {
                    const int offset = dipToPixel(2);
                    SkRect back = SkRect::MakeLTRB(glyph.left() + offset, glyph.top(), glyph.right(), glyph.bottom() - offset);
                    SkRect front = SkRect::MakeLTRB(glyph.left(), glyph.top() + offset, glyph.right() - offset, glyph.bottom());
                    canvas->drawRect(back, paint);
                    const SkColor symbolColor = paint.getColor();
                    paint.setStyle(SkPaint::kFill_Style);
                    paint.setColor(backgroundColor);
                    canvas->drawRect(front, paint);
                    paint.setStyle(SkPaint::kStroke_Style);
                    paint.setColor(symbolColor);
                    canvas->drawRect(front, paint);
                } else {
                    canvas->drawRect(glyph, paint);
                }
            } else {
                paint.setAntiAlias(true);
                paint.setStrokeWidth(strokeWidth * 1.05f);
                paint.setStrokeCap(SkPaint::kSquare_Cap);
                canvas->drawLine(glyph.left(), glyph.top(), glyph.right(), glyph.bottom(), paint);
                canvas->drawLine(glyph.right(), glyph.top(), glyph.left(), glyph.bottom(), paint);
            }
        }
        canvas->restore();

        // GDI drawing clears alpha on DWM glass. Composite the cached opaque
        // raster surface instead, keeping both the caption edge and shadow.
        HGDIOBJ oldBitmap = ::SelectObject(m_captionDC, m_captionBitmap);
        const BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        ::GdiAlphaBlend(dc, intersection.left, intersection.top,
            intersection.right - intersection.left, intersection.bottom - intersection.top,
            m_captionDC, intersection.left - buttons.left, intersection.top - buttons.top,
            intersection.right - intersection.left, intersection.bottom - intersection.top, blend);
        ::SelectObject(m_captionDC, oldBitmap);
    }

    void runCaptionButton(CaptionButton button)
    {
        if (!isCaptionButtonEnabled(button))
            return;
        if (button == CaptionButtonMinimize) {
            ::PostMessage(m_hWnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        } else if (button == CaptionButtonMaximize) {
            ::PostMessage(m_hWnd, WM_SYSCOMMAND, ::IsZoomed(m_hWnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
        } else if (button == CaptionButtonClose) {
            ::PostMessage(m_hWnd, WM_CLOSE, 0, 0);
        }
    }

    bool handleCaptionMouseMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (!isCustomTitleBar())
            return false;

        POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        CaptionButton button = captionButtonFromPoint(point);
        if (message == WM_MOUSEMOVE) {
            if (!m_trackingMouseLeave) {
                TRACKMOUSEEVENT tracking = { sizeof(tracking), TME_LEAVE, m_hWnd, 0 };
                m_trackingMouseLeave = !!::TrackMouseEvent(&tracking);
            }
            if (button != m_captionHoverButton) {
                m_captionHoverButton = button;
                invalidateCaptionButtons();
            }
            return button != CaptionButtonNone || m_captionPressedButton != CaptionButtonNone;
        }
        if (message == WM_MOUSELEAVE) {
            m_trackingMouseLeave = false;
            if (m_captionHoverButton != CaptionButtonNone) {
                m_captionHoverButton = CaptionButtonNone;
                invalidateCaptionButtons();
            }
            return m_captionPressedButton != CaptionButtonNone;
        }
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
            if (button == CaptionButtonNone)
                return false;
            m_captionPressedButton = button;
            ::SetCapture(m_hWnd);
            invalidateCaptionButtons();
            return true;
        }
        if (message == WM_LBUTTONUP && m_captionPressedButton != CaptionButtonNone) {
            CaptionButton pressed = m_captionPressedButton;
            m_captionPressedButton = CaptionButtonNone;
            if (::GetCapture() == m_hWnd)
                ::ReleaseCapture();
            invalidateCaptionButtons();
            if (pressed == button)
                runCaptionButton(pressed);
            return true;
        }
        if (message == WM_CAPTURECHANGED && m_captionPressedButton != CaptionButtonNone) {
            m_captionPressedButton = CaptionButtonNone;
            invalidateCaptionButtons();
            return true;
        }
        return button != CaptionButtonNone;
    }

    void updateNativeMenu()
    {
        if (!m_hWnd)
            return;
        HMENU attached = nullptr;
        if (m_nativeMenu && m_menuBarVisible && !m_isFullScreen
            && (!m_autoHideMenuBar || m_menuBarAltVisible))
            attached = m_nativeMenu;
        if (::GetMenu(m_hWnd) != attached) {
            ::SetMenu(m_hWnd, attached);
            ::DrawMenuBar(m_hWnd);
        }
    }

    void hideAutoMenuBar()
    {
        if (!m_autoHideMenuBar || !m_menuBarAltVisible)
            return;
        m_menuBarAltVisible = false;
        updateNativeMenu();
    }


    bool ensureLayeredSurface(int width, int height)
    {
        if (width <= 0 || height <= 0)
            return false;
        if (m_memoryDC && m_memoryBMP
            && m_memoryBmpSize.cx == width && m_memoryBmpSize.cy == height) {
            return true;
        }
        if (!m_memoryDC)
            m_memoryDC = ::CreateCompatibleDC(nullptr);
        if (!m_memoryDC)
            return false;

        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        HBITMAP bitmap = ::CreateDIBSection(
            nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap || !pixels) {
            if (bitmap)
                ::DeleteObject(bitmap);
            return false;
        }
        ::ZeroMemory(pixels,
            static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        HGDIOBJ oldBitmap = ::SelectObject(m_memoryDC, bitmap);
        if (!oldBitmap || oldBitmap == HGDI_ERROR) {
            ::DeleteObject(bitmap);
            return false;
        }
        if (m_memoryBMP)
            ::DeleteObject(oldBitmap);
        m_memoryBMP = bitmap;
        m_memoryBmpSize = { width, height };
        return true;
    }

    bool updateLayeredWindow()
    {
        if (!m_memoryDC || !m_memoryBMP)
            return false;
        POINT source = { 0, 0 };
        SIZE size = m_memoryBmpSize;
        BLENDFUNCTION blend = {
            AC_SRC_OVER, 0, 255, AC_SRC_ALPHA
        };
        HDC screen = ::GetDC(nullptr);
        const BOOL updated = ::UpdateLayeredWindow(
            m_hWnd, screen, nullptr, &size, m_memoryDC, &source,
            0, &blend, ULW_ALPHA);
        if (screen)
            ::ReleaseDC(nullptr, screen);
        return !!updated;
    }


    void onPaintMessage(HWND hWnd)
    {
        PAINTSTRUCT ps = { 0 };
        HDC paintDC = ::BeginPaint(hWnd, &ps);

        RECT rcClip = ps.rcPaint;
        RECT rcClient;
        ::GetClientRect(hWnd, &rcClient);

        const bool transparent = m_createWindowParam->transparent;
        RECT rcInvalid = rcClient;
        if (!transparent && rcClip.right != rcClip.left
            && rcClip.bottom != rcClip.top) {
            ::IntersectRect(&rcInvalid, &rcClip, &rcClient);
        }

        const int srcX = rcInvalid.left - rcClient.left;
        const int srcY = rcInvalid.top - rcClient.top;
        const int width = rcInvalid.right - rcInvalid.left;
        const int height = rcInvalid.bottom - rcInvalid.top;
        HDC targetDC = paintDC;
        if (transparent) {
            const int clientWidth = rcClient.right - rcClient.left;
            const int clientHeight = rcClient.bottom - rcClient.top;
            targetDC = ensureLayeredSurface(clientWidth, clientHeight)
                ? m_memoryDC
                : nullptr;
        }

        if (targetDC && width > 0 && height > 0) {
            if (m_webContents) {
                m_webContents->paintFrame(targetDC, rcInvalid.left,
                    rcInvalid.top, srcX, srcY, width, height);
            }
            paintBrowserViews(targetDC, rcInvalid);
            paintTitleBarOverlay(targetDC, rcInvalid);
        }
        if (transparent && targetDC)
            updateLayeredWindow();

        ::EndPaint(hWnd, &ps);
    }

    void paintBrowserViews(HDC hdc, const RECT& parentPaintRect)
    {
        for (BrowserView* view : m_browserViews) {
            if (view && view->getWebContents())
                view->onPaintInUiThread(hdc, parentPaintRect);
        }
    }



    bool mouseMsgToBrowserViews(unsigned int message, int x, int y, unsigned int flags)
    {
        if (m_foucsBrowserView && !m_foucsBrowserView->getWebContents())
            m_foucsBrowserView = nullptr;
        bool isHandled = false;
        BrowserView* hittestView = findHittestBrowserview(x, y);
        // 如果是WM_LBUTTONDOWN消息，就先找到焦点view，发送过去
        // 如果不是WM_LBUTTONDOWN，就看是否是mouse down状态。是的话，还是发给焦点view。否则发给hittest的view
        if (WM_LBUTTONDOWN == message) {
            m_isMouseDown = true;

            if (hittestView) {
                isHandled = true;
                m_foucsBrowserView = hittestView;
                m_foucsBrowserView->handleMouseMsgInUiThread(message, x, y, flags);
            } else
                m_foucsBrowserView = nullptr;
        } else {
            if (WM_LBUTTONUP == message) {
                m_isMouseDown = false;
            }

            if (m_isMouseDown) {
                if (m_foucsBrowserView) {
                    isHandled = true;
                    m_foucsBrowserView->handleMouseMsgInUiThread(message, x, y, flags);
                }
            } else {
                if (hittestView) {
                    isHandled = true;
                    hittestView->handleMouseMsgInUiThread(message, x, y, flags);
                }
            }
        }
        return isHandled;
    }


    void onMouseMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (m_isIgnoreMouseEvents)
            return;

        if (message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_RBUTTONDOWN) {
            ::SetFocus(hWnd);
            ::SetCapture(hWnd);
        } else if (message == WM_LBUTTONUP || message == WM_MBUTTONUP || message == WM_RBUTTONUP) {
            ::ReleaseCapture();
        }

        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);

        unsigned int flags = 0;

        if (wParam & MK_CONTROL)
            flags |= MINI_ELECTRON_CONTROL;
        if (wParam & MK_SHIFT)
            flags |= MINI_ELECTRON_SHIFT;

        if (wParam & MK_LBUTTON)
            flags |= MINI_ELECTRON_LBUTTON;
        if (wParam & MK_MBUTTON)
            flags |= MINI_ELECTRON_MBUTTON;
        if (wParam & MK_RBUTTON)
            flags |= MINI_ELECTRON_RBUTTON;

        //         ::EnterCriticalSection(&m_mouseMsgQueueLock);
        //         m_mouseMsgQueue.push_back(new MouseMsg(message, x, y, flags));
        //         ::LeaveCriticalSection(&m_mouseMsgQueueLock);
        //
        //         BrowserWindow* self = this;
        //         ThreadCall::callBlinkThreadAsync([self, id, webview] {
        //             if (!IdLiveDetect::get()->isLive(id))
        //                 return;
        //             self->delayDoMouseMsg(webview);
        //         });
        if (!mouseMsgToBrowserViews(message, x, y, flags)) {
            // Blink applies the widget DSF to native device-pixel input.
            m_webContents->sendWindowsMouseEvent(message, x, y, flags);
        }

    }

    BrowserView* findHittestBrowserview(int x, int y)
    {
        POINT pt = { x, y };
        for (int i = static_cast<int>(m_browserViews.size()) - 1; i >= 0; --i) {
            BrowserView* view = m_browserViews[i];
            if (!view || !view->getWebContents())
                continue;
            RECT r = view->getClientRect();
            if (::PtInRect(&r, pt))
                return view;
        }
        return nullptr;
    }

    WebContents* focusedWebContents()
    {
        if (m_foucsBrowserView) {
            WebContents* contents = m_foucsBrowserView->getWebContents();
            if (contents)
                return contents;
            m_foucsBrowserView = nullptr;
        }
        return m_webContents;
    }

    void onCursorChange()
    {
        // Cursor shape changes arrive from the remote renderer. Until a new
        // shape is received, retain the standard arrow rather than querying an
        // in-process Blink view.
        m_cursorInfoType = 0;
    }

    bool setCursorInfoTypeByCache(LPARAM lParam)
    {
        RECT rc;
        ::GetClientRect(m_hWnd, &rc);

        POINT pt;
        ::GetCursorPos(&pt);
        ::ScreenToClient(m_hWnd, &pt);
        if (!::PtInRect(&rc, pt))
            return false;

        HCURSOR hCur = NULL;
        if (!m_createWindowParam->isFrame && m_createWindowParam->isResizable
            && !m_isMaximized && !::IsZoomed(m_hWnd) && !m_isFullScreen) {
            const int border = std::max(RESIZE_BORDER, dipToPixel(RESIZE_BORDER));
            int x = pt.x;
            int y = pt.y;
            int w = rc.right;
            int h = rc.bottom;

            bool left = x < border;
            bool right = x >= w - border;
            bool top = y < border;
            bool bottom = y >= h - border;

            if (left && top)
                hCur = LoadCursor(NULL, IDC_SIZENWSE);
            else if (right && top)
                hCur = LoadCursor(NULL, IDC_SIZENESW);
            else if (left && bottom)
                hCur = LoadCursor(NULL, IDC_SIZENESW);
            else if (right && bottom)
                hCur = LoadCursor(NULL, IDC_SIZENWSE);
            else if (left || right)
                hCur = LoadCursor(NULL, IDC_SIZEWE);
            else if (top || bottom)
                hCur = LoadCursor(NULL, IDC_SIZENS);

            if (hCur) {
                ::SetCursor(hCur);
                return true;
            }
        }

        switch (m_cursorInfoType) {
        case kMiniElectronCursorInfoPointer:
            hCur = ::LoadCursor(NULL, IDC_ARROW);
            break;
        case kMiniElectronCursorInfoIBeam:
            hCur = ::LoadCursor(NULL, IDC_IBEAM);
            break;
        case kMiniElectronCursorInfoHand:
            hCur = ::LoadCursor(NULL, IDC_HAND);
            break;
        case kMiniElectronCursorInfoWait:
            hCur = ::LoadCursor(NULL, IDC_WAIT);
            break;
        case kMiniElectronCursorInfoHelp:
            hCur = ::LoadCursor(NULL, IDC_HELP);
            break;
        case kMiniElectronCursorInfoNorthResize:
            hCur = ::LoadCursor(NULL, IDC_SIZENS);
            break;
        case kMiniElectronCursorInfoSouthWestResize:
        case kMiniElectronCursorInfoNorthEastResize:
            hCur = ::LoadCursor(NULL, IDC_SIZENESW);
            break;
        case kMiniElectronCursorInfoSouthResize:
        case kMiniElectronCursorInfoNorthSouthResize:
            hCur = ::LoadCursor(NULL, IDC_SIZENS);
            break;
        case kMiniElectronCursorInfoNorthWestResize:
        case kMiniElectronCursorInfoSouthEastResize:
            hCur = ::LoadCursor(NULL, IDC_SIZENWSE);
            break;
        case kMiniElectronCursorInfoWestResize:
        case kMiniElectronCursorInfoEastWestResize:
        case kMiniElectronCursorInfoColumnResize:
        case kMiniElectronCursorInfoEastResize:
            hCur = ::LoadCursor(NULL, IDC_SIZEWE);
            break;
        case kMiniElectronCursorInfoNorthEastSouthWestResize:
        case kMiniElectronCursorInfoNorthWestSouthEastResize:
            hCur = ::LoadCursor(NULL, IDC_SIZEALL);
            break;
        default:
            hCur = ::LoadCursor(NULL, IDC_ARROW);
            break;
        }

        if (hCur) {
            ::SetCursor(hCur);
            return true;
        }

        return false;
    }


    LRESULT windowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        BrowserWindow* self = this;
        int id = m_id;

        if ((message == WM_MOUSEMOVE || message == WM_MOUSELEAVE
                || message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK || message == WM_LBUTTONUP
                || message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK || message == WM_MBUTTONUP
                || message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK || message == WM_RBUTTONUP
                || message == WM_CAPTURECHANGED)
            && handleCaptionMouseMessage(message, wParam, lParam)) {
            return 0;
        }
        if (isCustomTitleBar() && (message == WM_CONTEXTMENU || message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)) {
            POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (point.x != -1 || point.y != -1) {
                ::ScreenToClient(hWnd, &point);
                if (captionButtonFromPoint(point) != CaptionButtonNone)
                    return 0;
            }
        }

        switch (message) {
        case WM_NCCALCSIZE:
            if (m_createWindowParam->isFrame)
                break;
            if (::IsZoomed(hWnd) && !m_isFullScreen) {
                RECT* client = wParam
                    ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0]
                    : reinterpret_cast<RECT*>(lParam);
                const LONG top = client->top;
                const LONG left = client->left;
                ::DefWindowProcW(hWnd, message, wParam, lParam);
                client->top = top + client->left - left;
            }
            return 0;
        case WM_DWMCOMPOSITIONCHANGED:
            updateWindowFrame(hWnd);
            return 0;
        case WM_CLOSE: {
            WindowState state = m_state;
            m_state = WindowDestroying;
            if (WindowDestroying != state && !m_isDestroyApiBeCalled) {
                bool isPreventDefault = mate::EventEmitter<BrowserWindow>::emit("close");
                if (isPreventDefault) {
                    if (m_state == WindowDestroying)
                        m_state = state;
                    App::getInstance()->onWindowCloseCancelled();
                    return 0;
                }
            }
            ::ShowWindow(hWnd, SW_HIDE);
        } break;

        case WM_NCDESTROY:
            mate::EventEmitter<BrowserWindow>::emit("closed");

            ::KillTimer(hWnd, (UINT_PTR)this);
            ::RemovePropW(hWnd, kPropW);
            ::RevokeDragDrop(m_hWnd);
            if (m_webContents)
                m_webContents->closeRenderer();

            for (size_t i = 0; i < m_browserViews.size(); ++i) {
                BrowserView* view = m_browserViews[i];
                view->destroyed();
            }

            m_webContents->destroyed();
            m_webContents = nullptr;

            WindowList::getInstance()->removeWindow(self);
            if (WindowList::getInstance()->empty())
                App::getInstance()->onWindowAllClosed();

            m_state = WindowDestroyed;
            m_webContents = nullptr;

            IdLiveDetect::get()->deconstructed(m_id);
            break;

        case WM_TIMER:
            return 0;

        case WM_COMMAND:
            MenuEventNotif::onMenuCommon(message, wParam, lParam);
            hideAutoMenuBar();
            break;
        case WM_EXITMENULOOP:
            hideAutoMenuBar();
            break;
        case WM_SYSKEYDOWN:
            if (wParam != VK_MENU && ::GetForegroundWindow() == hWnd
                && MenuEventNotif::onAccelerator(m_nativeMenu, static_cast<UINT>(wParam))) {
                m_lastAcceleratorKey = static_cast<UINT>(wParam);
                return 0;
            }
            if (wParam == VK_MENU && m_autoHideMenuBar && m_menuBarVisible && m_nativeMenu) {
                m_menuBarAltVisible = !m_menuBarAltVisible;
                updateNativeMenu();
                if (!m_menuBarAltVisible)
                    return 0;
            }
            break;
        case WM_SYSKEYUP:
            if (m_lastAcceleratorKey == static_cast<UINT>(wParam)) {
                m_lastAcceleratorKey = 0;
                return 0;
            }
            break;

        case WM_PAINT:
            onPaintMessage(hWnd);
            break;

        case WM_SHOWWINDOW:
            if (TRUE == wParam)
                mate::EventEmitter<BrowserWindow>::emit("show");
            else
                mate::EventEmitter<BrowserWindow>::emit("hide");
            break;

        case WM_ERASEBKGND:
            return TRUE;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* minmaxInfo = (MINMAXINFO*)lParam;
            const gfx::Size minimum = display::win::ScreenWin::DIPToScreenSize(
                hWnd, gfx::Size(m_createWindowParam->minWidth, m_createWindowParam->minHeight));
            const gfx::Size maximum = display::win::ScreenWin::DIPToScreenSize(
                hWnd, gfx::Size(m_createWindowParam->maxWidth, m_createWindowParam->maxHeight));
            minmaxInfo->ptMinTrackSize.x = minimum.width();
            minmaxInfo->ptMinTrackSize.y = minimum.height();
            minmaxInfo->ptMaxTrackSize.x = maximum.width();
            minmaxInfo->ptMaxTrackSize.y = maximum.height();
            if (!m_createWindowParam->isFrame && !m_isFullScreen) {
                HMONITOR monitor = ::MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO info = { sizeof(info) };
                if (::GetMonitorInfoW(monitor, &info)) {
                    minmaxInfo->ptMaxPosition.x = info.rcWork.left - info.rcMonitor.left;
                    minmaxInfo->ptMaxPosition.y = info.rcWork.top - info.rcMonitor.top;
                    minmaxInfo->ptMaxSize.x = info.rcWork.right - info.rcWork.left;
                    minmaxInfo->ptMaxSize.y = info.rcWork.bottom - info.rcWork.top;
                }
            }
        } break;

        case WM_MOVE:
            mate::EventEmitter<BrowserWindow>::emit("move");
            break;

        case WM_SYSCOMMAND: {
            const WPARAM command = wParam & 0xfff0;
            if (m_isFullScreen && (command == SC_MOVE || command == SC_SIZE || command == SC_MAXIMIZE))
                return 0;
            if (command == SC_MAXIMIZE && !m_createWindowParam->isMaximizable)
                return 0;
            if (command == SC_MINIMIZE && !m_createWindowParam->isMinimizable)
                return 0;
            if (command == SC_CLOSE && !m_createWindowParam->isClosable)
                return 0;
            if (command == SC_MOVE && !m_createWindowParam->isMovable)
                return 0;
        } break;

        case WM_SIZE: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            const gfx::Size viewportSize = display::win::ScreenWin::ScreenToDIPSize(m_hWnd, gfx::Size(x, y));
            m_webContents->resize(viewportSize.width(), viewportSize.height(),
                display::win::ScreenWin::GetScaleFactorForHWND(hWnd));

            if (WindowInited == m_state)
                mate::EventEmitter<BrowserWindow>::emit("resize");

            if (SIZE_MAXIMIZED == wParam) {
                m_isMaximized = true;
                mate::EventEmitter<BrowserWindow>::emit("maximize");
            }
            if (SIZE_MINIMIZED == wParam)
                mate::EventEmitter<BrowserWindow>::emit("minimize");
            if (SIZE_RESTORED == wParam) {
                if (m_isMaximized)
                    mate::EventEmitter<BrowserWindow>::emit("unmaximize");
                m_isMaximized = false;
            }

            m_contentsSize.cx = x;
            m_contentsSize.cy = y;

            invalidateCaptionButtons();
        }
            return 0;
//         case WM_SETICON:
//             return 0;
        case WM_KEYDOWN: {
            if (::GetForegroundWindow() == hWnd
                && MenuEventNotif::onAccelerator(m_nativeMenu, static_cast<UINT>(wParam))) {
                m_lastAcceleratorKey = static_cast<UINT>(wParam);
                return 0;
            }

            setIcon(hWnd); // !!

            if (m_hIMC) {
                ::ImmAssociateContext(hWnd, m_hIMC);
                m_hIMC = nullptr;
            }

            unsigned int virtualKeyCode = wParam;
            unsigned int flags = 0;
            if (HIWORD(lParam) & KF_REPEAT)
                flags |= MINI_ELECTRON_REPEAT;
            if (HIWORD(lParam) & KF_EXTENDED)
                flags |= MINI_ELECTRON_EXTENDED;

            if (WebContents* target = focusedWebContents())
                target->sendWindowsKeyEvent("keyDown", virtualKeyCode, flags);

            return 0;
            break;
        }
        case WM_KEYUP: {
            if (m_lastAcceleratorKey == static_cast<UINT>(wParam)) {
                m_lastAcceleratorKey = 0;
                return 0;
            }

            unsigned int virtualKeyCode = wParam;
            unsigned int flags = 0;
            if (HIWORD(lParam) & KF_REPEAT)
                flags |= MINI_ELECTRON_REPEAT;
            if (HIWORD(lParam) & KF_EXTENDED)
                flags |= MINI_ELECTRON_EXTENDED;

            if (WebContents* target = focusedWebContents())
                target->sendWindowsKeyEvent("keyUp", virtualKeyCode, flags);

            return 0;
            break;
        }
        case WM_CHAR: {
            unsigned int charCode = wParam;
            unsigned int flags = 0;
            if (HIWORD(lParam) & KF_REPEAT)
                flags |= MINI_ELECTRON_REPEAT;
            if (HIWORD(lParam) & KF_EXTENDED)
                flags |= MINI_ELECTRON_EXTENDED;

            if (WebContents* target = focusedWebContents())
                target->sendWindowsKeyEvent("char", charCode, flags);
            return 0;
            break;
        }
        case WM_IME_COMPOSITION:
            break;
        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_MBUTTONDBLCLK:
        case WM_RBUTTONDBLCLK:
        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MOUSEMOVE:
            onCursorChange();
            onMouseMessage(hWnd, message, wParam, lParam);
            break;
        case WM_CONTEXTMENU: {
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);

            if (pt.x != -1 && pt.y != -1)
                ::ScreenToClient(hWnd, &pt);

            unsigned int flags = 0;

            if (wParam & MK_CONTROL)
                flags |= MINI_ELECTRON_CONTROL;
            if (wParam & MK_SHIFT)
                flags |= MINI_ELECTRON_SHIFT;

            if (wParam & MK_LBUTTON)
                flags |= MINI_ELECTRON_LBUTTON;
            if (wParam & MK_MBUTTON)
                flags |= MINI_ELECTRON_MBUTTON;
            if (wParam & MK_RBUTTON)
                flags |= MINI_ELECTRON_RBUTTON;

            m_webContents->sendWindowsMouseEvent(WM_CONTEXTMENU, pt.x, pt.y, flags);
            break;
        }
        case WM_MOUSEWHEEL: {
            if (m_isIgnoreMouseEvents)
                break;
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            ::ScreenToClient(hWnd, &pt);

            int delta = GET_WHEEL_DELTA_WPARAM(wParam);

            unsigned int flags = 0;

            if (wParam & MK_CONTROL)
                flags |= MINI_ELECTRON_CONTROL;
            if (wParam & MK_SHIFT)
                flags |= MINI_ELECTRON_SHIFT;

            if (wParam & MK_LBUTTON)
                flags |= MINI_ELECTRON_LBUTTON;
            if (wParam & MK_MBUTTON)
                flags |= MINI_ELECTRON_MBUTTON;
            if (wParam & MK_RBUTTON)
                flags |= MINI_ELECTRON_RBUTTON;

            BrowserView* hittestView = findHittestBrowserview(pt.x, pt.y);
            WebContents* target = m_webContents;
            int targetX = pt.x;
            int targetY = pt.y;
            if (hittestView) {
                target = hittestView->getWebContents();
                const RECT bounds = hittestView->getClientRect();
                targetX -= bounds.left;
                targetY -= bounds.top;
            }
            if (target) {
                // Input stays in physical pixels; the renderer applies its DSF.
                target->sendWindowsMouseEvent(
                    WM_MOUSEWHEEL, targetX, targetY, flags, delta);
            }
            break;
        }
        case WM_SETFOCUS:
            mate::EventEmitter<BrowserWindow>::emit("focus");
            m_webContents->setFocus(true);
            return 0;

        case WM_KILLFOCUS:
            m_lastAcceleratorKey = 0;
            mate::EventEmitter<BrowserWindow>::emit("blur");
            hideAutoMenuBar();
            m_webContents->setFocus(false);
            return 0;

        case WM_SETCURSOR: {
            POINT point;
            ::GetCursorPos(&point);
            ::ScreenToClient(hWnd, &point);
            if (captionButtonFromPoint(point) != CaptionButtonNone) {
                ::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
                return TRUE;
            }
            if (setCursorInfoTypeByCache(lParam))
                return 0;
            break;
        }
        case WM_IME_STARTCOMPOSITION: {
            RECT* caret = new RECT { 0, 0, 0, 1 };
            ::PostMessage(hWnd, WM_IME_STARTCOMPOSITION_ASYN, (WPARAM)caret, 0);
            return 0;
        }
        case WM_IME_STARTCOMPOSITION_ASYN: {
            RECT* caret = reinterpret_cast<RECT*>(wParam);
            COMPOSITIONFORM compositionForm;
            compositionForm.dwStyle = CFS_POINT | CFS_FORCE_POSITION;
            compositionForm.ptCurrentPos.x = caret->left;
            compositionForm.ptCurrentPos.y = caret->top;
            HIMC hIMC = ::ImmGetContext(hWnd);
            ::ImmSetCompositionWindow(hIMC, &compositionForm);
            ::ImmReleaseContext(hWnd, hIMC);
            delete caret;
            break;
        }
        case WM_DROPFILES:
            //onDragFiles((HDROP)wParam);
            break; //         if (message != WM_TIMER) {
            //             char* output = (char*)malloc(0x100);
            //             sprintf_s(output, 0x99, "staticWindowProc: %x, %p\n", message, self);
            //             OutputDebugStringA(output);
            //             free(output);
            //         }
        case WM_NCLBUTTONDOWN: {
            if (HTCAPTION == wParam && !m_createWindowParam->isMovable) {
                return 0;
            }
        }
            break;

        case WM_ACTIVATE:
            updateWindowFrame(hWnd);
            if (LOWORD(wParam) == WA_INACTIVE)
                hideAutoMenuBar();
            break;
        case WM_DPICHANGED: {
            display::win::ScreenWin::UpdateDisplayInfos();
            if (!m_isFullScreen) {
                RECT* suggested = reinterpret_cast<RECT*>(lParam);
                ::SetWindowPos(hWnd, nullptr, suggested->left, suggested->top,
                    suggested->right - suggested->left, suggested->bottom - suggested->top,
                    SWP_NOACTIVATE | SWP_NOZORDER);
            }
            const float scale = LOWORD(wParam) / 96.f;
            RECT client;
            ::GetClientRect(hWnd, &client);
            const gfx::Size viewport = gfx::ScaleToCeiledSize(
                gfx::Size(client.right - client.left, client.bottom - client.top), 1.f / scale);
            m_webContents->resize(viewport.width(), viewport.height(), scale);
            for (BrowserView* view : m_browserViews) {
                if (view)
                    view->onParentScaleFactorChanged();
            }
            rebuildDraggableRegion();
            invalidateCaptionButtons();
            return 0;
        }
        case WM_NCHITTEST: {
            POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ::ScreenToClient(hWnd, &point);
            if (captionButtonFromPoint(point) != CaptionButtonNone)
                return HTCLIENT;

            if (m_createWindowParam->isFrame) {
                const LRESULT native_hit = ::DefWindowProcW(hWnd, message, wParam, lParam);
                if (native_hit != HTCLIENT)
                    return native_hit;
            } else if (m_createWindowParam->isResizable
                && !m_isMaximized && !::IsZoomed(hWnd) && !m_isFullScreen) {
                RECT client;
                ::GetClientRect(hWnd, &client);
                const int border = std::max(RESIZE_BORDER, dipToPixel(RESIZE_BORDER));
                const bool left = point.x < border;
                const bool right = point.x >= client.right - border;
                const bool top = point.y < border;
                const bool bottom = point.y >= client.bottom - border;
                if (left && top)
                    return HTTOPLEFT;
                if (right && top)
                    return HTTOPRIGHT;
                if (left && bottom)
                    return HTBOTTOMLEFT;
                if (right && bottom)
                    return HTBOTTOMRIGHT;
                if (left)
                    return HTLEFT;
                if (right)
                    return HTRIGHT;
                if (top)
                    return HTTOP;
                if (bottom)
                    return HTBOTTOM;
            }
            if (!m_isFullScreen && m_createWindowParam->isMovable
                && !findHittestBrowserview(point.x, point.y)
                && m_draggableRegion && ::PtInRegion(m_draggableRegion, point.x, point.y))
                return HTCAPTION;
            return HTCLIENT;
        }
        }

        return ::DefWindowProcW(hWnd, message, wParam, lParam);
    }

    void setIcon(HWND hWnd)
    {
        static bool m_hadSetIcon = false;
        if (m_hadSetIcon)
            return;

        std::string contents; 
        HICON hIcon = nullptr;
        if (asar::readFileToString(base::FilePath::FromUTF8Unsafe(
            //"W:\\WeGameApps\\downloading\\icon.png"
            m_createWindowParam->m_iconPath
            //"W:\\mycode\\mb132\\third_party\\skia\\tools\\skiaserve\\favicon.ico"
        ), &contents)) {
            void* picture = platform_util::loadIconFromMemory((const uint8_t*)contents.data(), contents.size(), &hIcon);
            if (picture) {
                ::SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
                ::SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
                platform_util::loadIconFromMemoryFree(picture);
                m_hadSetIcon = true;
            } else {
                //sk_sp<SkData> data = SkData::MakeWithoutCopy(contents.data(), contents.size());
                //std::unique_ptr<SkImageGenerator> xx = SkImageGenerators::MakeFromEncoded(data);
            }
        }

        if (!m_hadSetIcon) {
            hIcon = LoadIcon(::GetModuleHandleW(NULL), MAKEINTRESOURCE(IDI_ICON_ELECTRON32x32));
            if (NULL != hIcon) {
                ::SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
                ::SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            }
        }
    }

    static LRESULT CALLBACK staticWindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        int id = -1;
        BrowserWindow* self = (BrowserWindow*)::GetPropW(hWnd, kPropW);
        if (!self && message == WM_CREATE) {
            LPCREATESTRUCTW cs = (LPCREATESTRUCTW)lParam;
            self = (BrowserWindow*)cs->lpCreateParams;
            id = self->m_id;
            ::SetPropW(hWnd, kPropW, (HANDLE)self);
            ::SetTimer(hWnd, (UINT_PTR)self, 70, NULL);

            self->setIcon(hWnd);
            return 0;
        }
        if (!self)
            return ::DefWindowProcW(hWnd, message, wParam, lParam);

        id = self->m_id;

        return self->windowProc(hWnd, message, wParam, lParam);
    }

private:
    void closeApi()
    {
        ::PostMessage(m_hWnd, WM_CLOSE, 0, 0);
    }

    void destroyApi()
    {
        destroy();
    }

    void focusApi()
    {
        ::SetFocus(m_hWnd);
    }

    void blurApi()
    {
        ::SetFocus(NULL);
    }

    bool isFocusedApi()
    {
        return ::GetFocus() == m_hWnd;
    }

    void showApi()
    {
        ::ShowWindow(m_hWnd, SW_SHOW);
        ::SetFocus(m_hWnd);
    }

    void showInactiveApi()
    {
        ::ShowWindow(m_hWnd, SW_SHOWMINNOACTIVE);
    }

    void hideApi()
    {
        ::ShowWindow(m_hWnd, SW_HIDE);
    }

    bool isVisibleApi()
    {
        return !!::IsWindowVisible(m_hWnd);
    }

    bool isEnabledApi()
    {
        return !!::IsWindowEnabled(m_hWnd);
    }

    void maximizeApi()
    {
        if (m_createWindowParam->isMaximizable)
            ::ShowWindow(m_hWnd, SW_MAXIMIZE);
    }

    void unmaximizeApi()
    {
        ::ShowWindow(m_hWnd, SW_RESTORE);
    }

    bool isMaximizedApi()
    {
        return !!::IsZoomed(m_hWnd);
    }

    void minimizeApi()
    {
        if (m_createWindowParam->isMinimizable)
            ::ShowWindow(m_hWnd, SW_MINIMIZE);
    }

    //restore
    void restoreApi()
    {
        ::ShowWindow(m_hWnd, SW_RESTORE);
    }

    //isMinimized
    bool isMinimizedApi()
    {
        return !!IsIconic(m_hWnd);
    }

    void setFullScreenApi(bool fullScreen)
    {
        if (fullScreen == m_isFullScreen)
            return;

        if (fullScreen) {
            m_windowPlacement.length = sizeof(m_windowPlacement);
            ::GetWindowPlacement(m_hWnd, &m_windowPlacement);
            m_fullScreenStyle = ::GetWindowLong(m_hWnd, GWL_STYLE);
            m_fullScreenExStyle = ::GetWindowLong(m_hWnd, GWL_EXSTYLE);
            HMONITOR monitor = ::MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info = { sizeof(info) };
            if (!::GetMonitorInfoW(monitor, &info))
                return;
            m_captionHoverButton = CaptionButtonNone;
            m_captionPressedButton = CaptionButtonNone;
            if (::GetCapture() == m_hWnd)
                ::ReleaseCapture();
            if (m_autoHideMenuBar)
                m_menuBarAltVisible = false;

            m_isFullScreen = true;
            ::SetWindowLong(m_hWnd, GWL_STYLE, m_fullScreenStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZE | WS_MINIMIZE));
            ::SetWindowPos(m_hWnd, m_createWindowParam->isAlwaysOnTop ? HWND_TOPMOST : HWND_TOP,
                info.rcMonitor.left, info.rcMonitor.top,
                info.rcMonitor.right - info.rcMonitor.left,
                info.rcMonitor.bottom - info.rcMonitor.top,
                SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        } else {
            ::SetWindowLong(m_hWnd, GWL_STYLE, m_fullScreenStyle & ~(WS_MAXIMIZE | WS_MINIMIZE));
            ::SetWindowLong(m_hWnd, GWL_EXSTYLE, m_fullScreenExStyle);
            m_isFullScreen = false;
            ::SetWindowPlacement(m_hWnd, &m_windowPlacement);
            ::SetWindowPos(m_hWnd, nullptr, 0, 0, 0, 0,
                SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        updateNativeMenu();
        ::InvalidateRect(m_hWnd, nullptr, FALSE);
    }

    BrowserView* getBrowserView(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        if (1 != info.Length())
            return nullptr;
        v8::Local<v8::Value> arg0 = info[0];
        if (!arg0->IsObject())
            return nullptr;

        v8::Local<v8::Object> browserViewV8 = arg0->ToObject(info.GetIsolate()->GetCurrentContext()).ToLocalChecked();
        WrappableBase* ptr = GetNativePtr(browserViewV8, &BrowserView::kWrapperInfo);
        if (!ptr)
            return nullptr;

        BrowserView* browserView = (BrowserView*)ptr;
        return browserView;
    }

    void setBrowserViewApi(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        BrowserView* browserView = getBrowserView(info);
        if (!browserView || !browserView->getWebContents())
            return;
        if (std::find(m_browserViews.begin(), m_browserViews.end(), browserView)
            == m_browserViews.end()) {
            m_browserViews.push_back(browserView);
        }
        browserView->attachBrowserWindow(m_hWnd);
    }

    void removeBrowserViewApi(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        BrowserView* browserView = getBrowserView(info);
        if (!browserView)
            return;
        for (std::vector<BrowserView*>::iterator it = m_browserViews.begin(); it != m_browserViews.end(); ++it) {
            BrowserView* view = *it;
            if (view != browserView)
                continue;
            if (m_foucsBrowserView == browserView)
                m_foucsBrowserView = nullptr;
            m_browserViews.erase(it);
            browserView->detachBrowserWindow();
            ::InvalidateRect(m_hWnd, nullptr, FALSE);
            break;
        }
    }

    bool isFullScreenApi()
    {
        return m_isFullScreen;
    }

    void setParentWindowApi()
    {
        OutputDebugStringA("setParentWindowApi\n");
        DebugBreak();
    }

    void getParentWindowApi()
    {
        OutputDebugStringA("getParentWindowApi\n");
        DebugBreak();
    }

    void getChildWindowsApi()
    {
        OutputDebugStringA("getChildWindowsApi\n");
        DebugBreak();
    }

    bool isModalApi()
    {
        OutputDebugStringA("isModalApi\n");
        DebugBreak();
        return false;
    }

    void getNativeWindowHandleApi(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        info.GetReturnValue().Set(toBuffer(info.GetIsolate(), (void*)(&m_hWnd), sizeof(HWND)));
        //return toBuffer(isolate(), &m_hWnd, sizeof(UINT_PTR));
        //return (UINT_PTR)m_hWnd;
    }

    void setWindowButtonPositionApi(v8::Local<v8::Object> bounds)
    {

    }

    v8::Local<v8::Object> getBoundsApi()
    {
        RECT windowRect;
        ::GetWindowRect(m_hWnd, &windowRect);
        const gfx::Rect dipBounds = display::win::ScreenWin::ScreenToDIPRect(m_hWnd, gfx::Rect(windowRect));

        v8::Local<v8::Context> context = isolate()->GetCurrentContext();

        v8::Local<v8::Integer> x = v8::Integer::New(isolate(), dipBounds.x());
        v8::Local<v8::Integer> y = v8::Integer::New(isolate(), dipBounds.y());
        v8::Local<v8::Integer> width = v8::Integer::New(isolate(), dipBounds.width());
        v8::Local<v8::Integer> height = v8::Integer::New(isolate(), dipBounds.height());
        v8::Local<v8::Object> bounds = v8::Object::New(isolate());
        bounds->Set(context, v8::String::NewFromUtf8(isolate(), "x").ToLocalChecked(), x);
        bounds->Set(context, v8::String::NewFromUtf8(isolate(), "y").ToLocalChecked(), y);
        bounds->Set(context, v8::String::NewFromUtf8(isolate(), "width").ToLocalChecked(), width);
        bounds->Set(context, v8::String::NewFromUtf8(isolate(), "height").ToLocalChecked(), height);
        return bounds;
    }

    v8::Local<v8::Object> getNormalBoundsApi()
    {
        if (!m_isFullScreen && !::IsZoomed(m_hWnd) && !::IsIconic(m_hWnd))
            return getBoundsApi();

        WINDOWPLACEMENT placement = m_windowPlacement;
        if (!m_isFullScreen) {
            placement.length = sizeof(placement);
            ::GetWindowPlacement(m_hWnd, &placement);
        }
        RECT normalRect = placement.rcNormalPosition;
        const DWORD exStyle = m_isFullScreen
            ? m_fullScreenExStyle
            : ::GetWindowLong(m_hWnd, GWL_EXSTYLE);
        if (!(exStyle & WS_EX_TOOLWINDOW)) {
            MONITORINFO monitor = { sizeof(monitor) };
            if (::GetMonitorInfoW(::MonitorFromRect(&normalRect,
                    MONITOR_DEFAULTTONEAREST), &monitor)) {
                ::OffsetRect(&normalRect,
                    monitor.rcWork.left - monitor.rcMonitor.left,
                    monitor.rcWork.top - monitor.rcMonitor.top);
            }
        }
        const gfx::Rect dipBounds = display::win::ScreenWin::ScreenToDIPRect(
            nullptr, gfx::Rect(normalRect));
        v8::Local<v8::Object> bounds = v8::Object::New(isolate());
        gin_helper::Dictionary dictionary(isolate(), bounds);
        dictionary.Set("x", dipBounds.x());
        dictionary.Set("y", dipBounds.y());
        dictionary.Set("width", dipBounds.width());
        dictionary.Set("height", dipBounds.height());
        return bounds;
    }

    void setBoundsApi(v8::Local<v8::Object> bounds)
    {
        v8::Local<v8::Context> context = isolate()->GetCurrentContext();

        gin_helper::Dictionary boundsDict(isolate(), bounds);
        int x, y, width, height;
        if (!boundsDict.Get("x", &x))
            return;
        if (!boundsDict.Get("y", &y))
            return;
        if (!boundsDict.Get("width", &width))
            return;
        if (!boundsDict.Get("height", &height))
            return;

        char output[100] = { 0 };
        sprintf_s(output, 99, "BrowserWindow::setBoundsApi: %d %d, %d %d\n", x, y, width, height);
        OutputDebugStringA(output);
        const gfx::Rect pixelBounds = display::win::ScreenWin::DIPToScreenRect(nullptr, gfx::Rect(x, y, width, height));
        ::MoveWindow(m_hWnd, pixelBounds.x(), pixelBounds.y(), pixelBounds.width(), pixelBounds.height(), TRUE);
    }

    v8::Local<v8::Object> getSizeApi()
    {
        RECT windowRect;
        ::GetWindowRect(m_hWnd, &windowRect);
        const gfx::Size dipSize = display::win::ScreenWin::ScreenToDIPSize(
            m_hWnd, gfx::Size(windowRect.right - windowRect.left, windowRect.bottom - windowRect.top));
        v8::Local<v8::Context> context = isolate()->GetCurrentContext();

        v8::Local<v8::Integer> width = v8::Integer::New(isolate(), dipSize.width());
        v8::Local<v8::Integer> height = v8::Integer::New(isolate(), dipSize.height());
        v8::Local<v8::Array> size = v8::Array::New(isolate(), 2);
        size->Set(context, 0, width);
        size->Set(context, 1, height);
        return size;
    }

    void setSizeApi(int32_t width, int32_t height)
    {
        char output[100] = { 0 };
        sprintf_s(output, 99, "BrowserWindow::setSizeApi: %d %d\n", width, height);
        OutputDebugStringA(output);

        const gfx::Size pixelSize = display::win::ScreenWin::DIPToScreenSize(
            m_hWnd, gfx::Size(width, height));
        ::SetWindowPos(m_hWnd, nullptr, 0, 0, pixelSize.width(), pixelSize.height(),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void getContentBoundsApi()
    {
        ::OutputDebugStringA("getContentBoundsApi\n");
        ::DebugBreak();
    }

    void setContentBoundsApi()
    {
        ::OutputDebugStringA("setContentBoundsApi\n");
        ::DebugBreak();
    }

    std::vector<int> getContentSizeApi()
    {
        BrowserWindow* self = this;

        SIZE contentsSize;
        ::EnterCriticalSection(&m_memoryCanvasLock);
        contentsSize = m_contentsSize;
        ::LeaveCriticalSection(&m_memoryCanvasLock);

        const gfx::Size dipSize = display::win::ScreenWin::ScreenToDIPSize(
            m_hWnd, gfx::Size(contentsSize.cx, contentsSize.cy));
        std::vector<int> size = { dipSize.width(), dipSize.height() };
        return size;
    }

    void setContentSizeApi(int width, int height)
    {
        const gfx::Size pixelSize = display::win::ScreenWin::DIPToScreenSize(
            m_hWnd, gfx::Size(width, height));
        RECT windowRect;
        RECT clientRect;
        ::GetWindowRect(m_hWnd, &windowRect);
        ::GetClientRect(m_hWnd, &clientRect);
        const int frameWidth = windowRect.right - windowRect.left
            - (clientRect.right - clientRect.left);
        const int frameHeight = windowRect.bottom - windowRect.top
            - (clientRect.bottom - clientRect.top);
        ::SetWindowPos(m_hWnd, nullptr, 0, 0, pixelSize.width() + frameWidth,
            pixelSize.height() + frameHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void setMinimumSizeApi(int width, int height)
    {
        m_createWindowParam->minWidth = width;
        m_createWindowParam->minHeight = height;
    }

    std::vector<int> getMinimumSizeApi()
    {
        std::vector<int> size = { m_createWindowParam->minWidth, m_createWindowParam->minHeight };
        return size;
    }

    void setMaximumSizeApi(int width, int height)
    {
        m_createWindowParam->maxWidth = width;
        m_createWindowParam->maxHeight = height;
    }

    std::vector<int> getMaximumSizeApi()
    {
        std::vector<int> size = { m_createWindowParam->maxWidth, m_createWindowParam->maxHeight };
        return size;
    }

    void setResizableApi(bool resizable)
    {
        m_createWindowParam->isResizable = resizable;
        if (m_createWindowParam->isFrame || !m_createWindowParam->transparent) {
            DWORD style = ::GetWindowLong(m_hWnd, GWL_STYLE);
            if (resizable)
                style |= WS_THICKFRAME;
            else
                style &= ~WS_THICKFRAME;
            ::SetWindowLong(m_hWnd, GWL_STYLE, style);
            m_createWindowParam->styles = style;
            ::SetWindowPos(m_hWnd, nullptr, 0, 0, 0, 0,
                SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    bool isResizableApi()
    {
        return m_createWindowParam->isResizable;
    }

    void setMovableApi(bool isMovable)
    {
        m_createWindowParam->isMovable = isMovable;
    }

    bool isMovableApi()
    {
        return m_createWindowParam->isMovable;
    }

    void setMinimizableApi(bool isMinimizable)
    {
        m_createWindowParam->isMinimizable = isMinimizable;
        DWORD style = ::GetWindowLong(m_hWnd, GWL_STYLE);
        style = isMinimizable ? style | WS_MINIMIZEBOX : style & ~WS_MINIMIZEBOX;
        ::SetWindowLong(m_hWnd, GWL_STYLE, style);
        m_createWindowParam->styles = style;
        invalidateCaptionButtons();
    }

    bool isMinimizableApi()
    {
        return m_createWindowParam->isMinimizable;
    }

    bool isMaximizableApi()
    {
        return m_createWindowParam->isMaximizable;
    }

    void setMaximizableApi(bool isMaximizable)
    {
        m_createWindowParam->isMaximizable = isMaximizable;
        DWORD style = ::GetWindowLong(m_hWnd, GWL_STYLE);
        style = isMaximizable ? style | WS_MAXIMIZEBOX : style & ~WS_MAXIMIZEBOX;
        ::SetWindowLong(m_hWnd, GWL_STYLE, style);
        m_createWindowParam->styles = style;
        invalidateCaptionButtons();
    }

    void setOpacityApi(float f)
    {

    }

    void setEnableApi(bool b)
    {
        ::EnableWindow(m_hWnd, b);
    }

    void setFullScreenableApi(bool isFullScreenable)
    {
    }

    bool isFullScreenableApi()
    {
        return false;
    }

    void setClosableApi(bool isClosable)
    {
        m_createWindowParam->isClosable = isClosable;
        ::EnableMenuItem(::GetSystemMenu(m_hWnd, FALSE), SC_CLOSE,
            MF_BYCOMMAND | (isClosable ? MF_ENABLED : MF_GRAYED));
        invalidateCaptionButtons();
    }

    bool isClosableApi()
    {
        return m_createWindowParam->isClosable;
    }

    void setAlwaysOnTopApi(bool b)
    {
        m_createWindowParam->isAlwaysOnTop = b;
        ::SetWindowPos(m_hWnd, b ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }

    bool isAlwaysOnTopApi()
    {
        return 0 != (::GetWindowLong(m_hWnd, GWL_EXSTYLE) & WS_EX_TOPMOST);
    }

    void centerApi()
    {
        int screenX, screenY;
        screenX = ::GetSystemMetrics(SM_CXSCREEN); //取得屏幕的宽度
        screenY = ::GetSystemMetrics(SM_CYSCREEN); //取得屏幕的高度

        RECT rect;
        ::GetWindowRect(m_hWnd, &rect);
        rect.left = (screenX - rect.right) / 2;
        rect.top = (screenY - rect.bottom) / 2;

        //设置窗体位置
        ::SetWindowPos(m_hWnd, NULL, rect.left, rect.top, rect.right, rect.bottom, SWP_NOSIZE);
    }

    void setPositionApi(int x, int y)
    {
        ::SetWindowPos(m_hWnd, NULL, x, y, 0, 0, SWP_NOSIZE);
    }

    std::vector<int> getPositionApi()
    {
        RECT rect = { 0 };
        ::GetWindowRect(m_hWnd, &rect);
        std::vector<int> pos = { rect.left, rect.top };
        return pos;
    }

    void setTitleApi(const std::string& title)
    {
        std::wstring titleW;
        titleW = StringUtil::UTF8ToUTF16(title);
        ::SetWindowText(m_hWnd, titleW.c_str());
    }

    std::string getTitleApi()
    {
        const int length = ::GetWindowTextLengthW(m_hWnd);
        std::wstring title(length + 1, L'\0');
        const int copied = ::GetWindowTextW(m_hWnd, title.data(), static_cast<int>(title.size()));
        title.resize(copied);
        return base::WideToUTF8(title);
    }

    void flashFrameApi()
    {
    }

    void setSkipTaskbarApi(bool b)
    {
        DWORD style = ::GetWindowLong(m_hWnd, GWL_STYLE);
        if (b) {
            style |= WS_EX_TOOLWINDOW;
            style &= ~WS_EX_APPWINDOW;
        } else { //todo 如果窗口原来的style没有WS_EX_APPWINDOW，就可能有问题
            style &= ~WS_EX_TOOLWINDOW;
            style |= WS_EX_APPWINDOW;
        }
        ::SetWindowLong(m_hWnd, GWL_EXSTYLE, style);
    }

    void setBackgroundColorApi()
    {
    }

    void setDocumentEditedApi(bool edited)
    {
        m_isDocumentEdited = edited;
    }

    bool isDocumentEditedApi()
    {
        return m_isDocumentEdited;
    }

    void setIgnoreMouseEventsApi(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        if (0 == info.Length())
            return;
        if (info[0]->IsBoolean())
            m_isIgnoreMouseEvents = info[0]->ToBoolean(info.GetIsolate())->Value();
    }

    void setContentProtectionApi()
    {
    }

    void setFocusableApi()
    {
    }

    void focusOnWebViewApi()
    {
        m_webContents->setFocus(true);
    }

    void isWebViewFocusedApi()
    {
    }

    void setOverlayIconApi()
    {
    }

    void setThumbarButtonsApi()
    {
    }

    void setMenuApi(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        if (info.Length() != 1)
            return;
        HMENU menu = nullptr;
        if (MenuEventNotif::getNativeMenu(info[0], &menu))
            setNativeMenu(menu);
    }

    void setAutoHideMenuBarApi(bool autoHide)
    {
        m_autoHideMenuBar = autoHide;
        m_menuBarAltVisible = false;
        updateNativeMenu();
    }

    bool isMenuBarAutoHideApi() const
    {
        return m_autoHideMenuBar;
    }

    void setMenuBarVisibilityApi(bool visible)
    {
        m_menuBarVisible = visible;
        m_menuBarAltVisible = visible && m_autoHideMenuBar;
        updateNativeMenu();
    }

    bool isMenuBarVisibleApi() const
    {
        return m_hWnd && ::GetMenu(m_hWnd) != nullptr;
    }

    void setVisibleOnAllWorkspacesApi()
    {
    }

    void isVisibleOnAllWorkspacesApi()
    {
    }

    void hookWindowMessageApi()
    {
    }

    void isWindowMessageHookedApi()
    {
    }

    void unhookWindowMessageApi()
    {
    }

    void unhookAllWindowMessagesApi()
    {
    }

    void setThumbnailClipApi()
    {
    }

    void setThumbnailToolTipApi()
    {
    }

    void setAppDetailsApi()
    {
    }

    void setIconApi()
    {
    }

    void setProgressBarApi(double progress)
    {
    }

    bool isDestroyedApi() const
    {
        return false;
    }

    void moveTopApi() const
    {
#if _WIN32
        ::SetWindowPos(m_hWnd, HWND_TOP, 0, 0, 1, 1, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
#elif defined(USE_OZONE_PLATFORM_X11)
        if (IsX11())
            electron::MoveWindowToForeground(static_cast<x11::Window>(GetAcceleratedWidget()));
#endif
    }

    static void getFocusedWindowApi(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        v8::Local<v8::Value> result = WindowInterface::getFocusedWindow(info.GetIsolate());
        info.GetReturnValue().Set(result);
    }

    static void fromIdApi(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        OutputDebugStringA("fromIdApi\n");
        if (1 != info.Length())
            return;
        v8::Local<v8::Value> arg0 = info[0];
        if (!arg0->IsInt32())
            return;

        int32_t id = arg0->Int32Value(info.GetIsolate()->GetCurrentContext()).ToChecked();

        v8::Isolate* isolate = v8::Isolate::GetCurrent();
        BrowserWindow* self = (BrowserWindow*)WindowList::getInstance()->find(id);
        if (!self) {
            info.GetReturnValue().Set(v8::Null(isolate));
            return;
        }

        v8::Local<v8::Value> result = v8::Local<v8::Value>::New(isolate, self->GetWrapper(isolate));
        info.GetReturnValue().Set(result);
    }

    static void getAllWindowsApi(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        size_t size = WindowList::getInstance()->size();
        v8::Local<v8::Array> result = v8::Array::New(info.GetIsolate(), size);
        v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();

        for (size_t i = 0; i < size; ++i) {
            BrowserWindow* self = (BrowserWindow*)WindowList::getInstance()->get(i);
            result->Set(context, i, self->GetWrapper(info.GetIsolate()));
        }

        info.GetReturnValue().Set(result);
    }

    static void fromWebContentsApi(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        if (1 != info.Length())
            return;
        v8::Local<v8::Value> arg0 = info[0];
        if (!arg0->IsObject())
            return;

        v8::Local<v8::Object> webContents = arg0->ToObject(info.GetIsolate()->GetCurrentContext()).ToLocalChecked();

        WrappableBase* webContentsPtr = GetNativePtr(webContents, &WebContents::kWrapperInfo);
        if (!webContentsPtr)
            return;

        WebContents* contents = (WebContents*)webContentsPtr;
        WindowInterface* win = contents->getOwner();
        if (!win)
            return;
        BrowserWindow* self = (BrowserWindow*)win;
        v8::Local<v8::Object> winObject = self->GetWrapperImpl(info.GetIsolate(), &BrowserWindow::kWrapperInfo);
        info.GetReturnValue().Set(winObject);
    }

    int getIdApi() const
    {
        return m_id;
    }

    v8::Local<v8::Value> _getWebContentsApi()
    {
        if (!m_webContents)
            return v8::Null(isolate());

        return v8::Local<v8::Value>::New(isolate(), m_webContents->getWrapper());
    }

    // 空实现
    void nullFunction()
    {
        OutputDebugStringA("nullFunction\n");
        DebugBreak();
    }

    void applyTitleBarOverlay(v8::Local<v8::Value> value)
    {
        RECT oldButtons = { 0 };
        bool hadButtons = m_hWnd && isCustomTitleBar();
        if (hadButtons)
            oldButtons = captionButtonsRect();

        if (value->IsBoolean()) {
            m_titleBarOverlayEnabled = value->BooleanValue(isolate());
        } else if (value->IsObject()) {
            gin_helper::Dictionary options(isolate(), value->ToObject(isolate()->GetCurrentContext()).ToLocalChecked());
            std::string color;
            if (options.Get("color", &color))
                parseOverlayColor(color, &m_titleBarOverlayColor);
            if (options.Get("symbolColor", &color))
                parseOverlayColor(color, &m_titleBarOverlaySymbolColor);
            int height = 0;
            if (options.Get("height", &height) && height > 0)
                m_titleBarOverlayHeight = height;
            m_titleBarOverlayEnabled = true;
        } else {
            return;
        }

        if (!m_titleBarOverlayEnabled) {
            m_captionHoverButton = CaptionButtonNone;
            m_captionPressedButton = CaptionButtonNone;
            if (m_hWnd && ::GetCapture() == m_hWnd)
                ::ReleaseCapture();
        }

        if (m_hWnd) {
            if (hadButtons)
                ::InvalidateRect(m_hWnd, &oldButtons, FALSE);
            invalidateCaptionButtons();
            ::UpdateWindow(m_hWnd);
        }
    }

    void setTitleBarOverlayApi(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        if (info.Length() != 1 || (!info[0]->IsObject() && !info[0]->IsBoolean())) {
            info.GetIsolate()->ThrowException(v8::Exception::TypeError(
                v8::String::NewFromUtf8(info.GetIsolate(), "setTitleBarOverlay requires an options object or boolean").ToLocalChecked()));
            return;
        }
        applyTitleBarOverlay(info[0]);
    }

    bool isSimpleFullScreenApi()
    {
        return false;
    }


    static BrowserWindow* newWindow(gin_helper::Dictionary* options, v8::Local<v8::Object> wrapper)
    {
        BrowserWindow* self = new BrowserWindow(options->isolate(), wrapper);

        WebContents::BrowserWindowConstructorOptions* createWindowParam = new WebContents::BrowserWindowConstructorOptions();
        createWindowParam->styles = 0;
        createWindowParam->styleEx = 0;
        createWindowParam->transparent = false;

        WebContents* webContents = nullptr;
        v8::Handle<v8::Object> webContentsV8;
        // If no WebContents was passed to the constructor, create it from options.
        if (options->Get("webContents", &webContentsV8))
            DebugBreak();

        // Use options.webPreferences to create WebContents.
        gin_helper::Dictionary webPreferences = gin_helper::Dictionary::CreateEmpty(options->isolate());
        options->Get(options::kWebPreferences, &webPreferences);

        // Copy the backgroundColor to webContents.
        v8::Local<v8::Value> value;
        if (options->Get(options::kBackgroundColor, &value))
            webPreferences.Set(options::kBackgroundColor, value);

        v8::Local<v8::Value> transparent;
        if (options->Get("transparent", &transparent))
            webPreferences.Set("transparent", transparent);

        // Offscreen windows are always created frameless.
        bool offscreen;
        if (webPreferences.Get("offscreen", &offscreen) && offscreen)
            options->Set(options::kFrame, false);

        webPreferences.GetBydefaultVal("nodeIntegration", false, &createWindowParam->m_isNodeIntegration);
        webPreferences.GetBydefaultVal("contextIsolation", true, &createWindowParam->m_isContextIsolation);
        webPreferences.Get("additionalArguments", &createWindowParam->m_customArgs);

        options->GetBydefaultVal("minWidth", 400, &createWindowParam->minWidth);
        options->GetBydefaultVal("minHeight", 400, &createWindowParam->minHeight);
        options->GetBydefaultVal("maxWidth", ::GetSystemMetrics(SM_CXSCREEN), &createWindowParam->maxWidth);
        options->GetBydefaultVal("maxHeight", ::GetSystemMetrics(SM_CYSCREEN), &createWindowParam->maxHeight);
        options->GetBydefaultVal("transparent", false, &createWindowParam->transparent);
        options->GetBydefaultVal("center", false, &createWindowParam->isCenter);
        options->GetBydefaultVal("resizable", true, &createWindowParam->isResizable);
        options->GetBydefaultVal("show", true, &createWindowParam->isShow);
        options->GetBydefaultVal("minimizable", true, &createWindowParam->isMinimizable);
        options->GetBydefaultVal("maximizable", true, &createWindowParam->isMaximizable);
        options->GetBydefaultVal("movable", true, &createWindowParam->isMovable);
        options->GetBydefaultVal("frame", true, &createWindowParam->isFrame);
        options->GetBydefaultVal("icon", "", &createWindowParam->m_iconPath);

        options->GetBydefaultVal("useContentSize", false, &createWindowParam->isUseContentSize);
        options->GetBydefaultVal("alwaysOnTop", false, &createWindowParam->isAlwaysOnTop);
        options->GetBydefaultVal("closable", true, &createWindowParam->isClosable);
        options->GetBydefaultVal("autoHideMenuBar", false, &self->m_autoHideMenuBar);

        std::string titleBarStyle;
        options->GetBydefaultVal("titleBarStyle", "", &titleBarStyle);
        if (titleBarStyle == "hidden" || titleBarStyle == "hiddenInset")
            createWindowParam->isFrame = false;

        v8::Local<v8::Value> titleBarOverlay;
        if (options->Get("titleBarOverlay", &titleBarOverlay)
            && (titleBarOverlay->IsBoolean() || titleBarOverlay->IsObject())) {
            self->applyTitleBarOverlay(titleBarOverlay);
        }

        options->GetBydefaultVal("width", 1, &createWindowParam->width);
        options->GetBydefaultVal("height", 1, &createWindowParam->height);

        const gfx::Size primarySize = display::win::ScreenWin::ScreenToDIPSize(
            nullptr, gfx::Size(::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)));
        int kNotSetXFlag = (primarySize.width() - createWindowParam->width) / 2;
        int kNotSetYFlag = (primarySize.height() - createWindowParam->height) / 2;

        options->GetBydefaultVal("x", kNotSetXFlag, &createWindowParam->x);
        options->GetBydefaultVal("y", kNotSetYFlag, &createWindowParam->y);

        if (createWindowParam->width < createWindowParam->minWidth)
            createWindowParam->width = createWindowParam->minWidth;

        if (createWindowParam->height < createWindowParam->minHeight)
            createWindowParam->height = createWindowParam->minHeight;
        const gfx::Rect pixelBounds = display::win::ScreenWin::DIPToScreenRect(
            nullptr, gfx::Rect(createWindowParam->x, createWindowParam->y,
                createWindowParam->width, createWindowParam->height));
        createWindowParam->x = pixelBounds.x();
        createWindowParam->y = pixelBounds.y();
        createWindowParam->width = pixelBounds.width();
        createWindowParam->height = pixelBounds.height();
        std::string title;
        options->GetBydefaultVal("title", "Electron", &title);
        createWindowParam->title = StringUtil::UTF8ToUTF16(title);

        if (createWindowParam->transparent) {
            createWindowParam->styles = WS_POPUP;
            createWindowParam->styleEx = WS_EX_LAYERED;
        } else {
            createWindowParam->styles = WS_POPUP | WS_OVERLAPPED | WS_SYSMENU | WS_CAPTION | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
            createWindowParam->styleEx = 0;

            if (!createWindowParam->isFrame)
                createWindowParam->styles = WS_CAPTION | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        }

        if (createWindowParam->isMinimizable)
            createWindowParam->styles |= WS_MINIMIZEBOX;
        if (createWindowParam->isMaximizable)
            createWindowParam->styles |= WS_MAXIMIZEBOX;

        if (createWindowParam->isResizable && (createWindowParam->isFrame || !createWindowParam->transparent))
            createWindowParam->styles |= WS_THICKFRAME;
        createWindowParam->styleEx |= WS_EX_ACCEPTFILES;

        if (createWindowParam->isShow)
            createWindowParam->styles |= WS_VISIBLE;

        webContents = WebContents::create(options->isolate(), webPreferences, self);
        self->m_webContents = webContents;
        self->m_webContents->addObserver(self);
        self->newWindowTaskInUiThread(createWindowParam);

        return self;
    }


    void newWindowTaskInUiThread(WebContents::BrowserWindowConstructorOptions* createWindowParam)
    {
        m_createWindowParam = createWindowParam;
        m_webContents->setCreateWindowParam(createWindowParam);

        m_hWnd = ::CreateWindowEx(createWindowParam->styleEx, kElectronClassName, createWindowParam->title.c_str(), createWindowParam->styles,
            createWindowParam->x, createWindowParam->y, createWindowParam->width, createWindowParam->height, NULL, NULL, ::GetModuleHandleW(NULL), this);

        if (!::IsWindow(m_hWnd))
            return;

        HWND dwFlag = HWND_NOTOPMOST;
        if (createWindowParam->isAlwaysOnTop)
            dwFlag = HWND_TOPMOST;
        ::SetWindowPos(m_hWnd, dwFlag, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOREPOSITION | SWP_FRAMECHANGED);
        updateWindowFrame(m_hWnd);


        if (!createWindowParam->isClosable)
            ::EnableMenuItem(::GetSystemMenu(m_hWnd, false), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);

        ::GetClientRect(m_hWnd, &m_clientRect);

        if (createWindowParam->isCenter)
            platform_util::moveToCenter(m_hWnd);

        int width = m_clientRect.right - m_clientRect.left;
        int height = m_clientRect.bottom - m_clientRect.top;

        HWND hWnd = m_hWnd;
        const gfx::Size viewportSize = display::win::ScreenWin::ScreenToDIPSize(
            hWnd, gfx::Size(width, height));
        m_webContents->resize(viewportSize.width(), viewportSize.height(),
            display::win::ScreenWin::GetScaleFactorForHWND(hWnd));
        rebuildDraggableRegion();
        m_webContents->setFocus(true);
        //setIcon(m_hWnd);

        MenuEventNotif::onWindowDidCreated(this);

        ::ShowWindow(m_hWnd, createWindowParam->isShow ? SW_SHOWNORMAL : SW_HIDE);
        ::UpdateWindow(m_hWnd);

        m_state = WindowInited;
    }

    virtual void onWebContentsCreated(WebContents* contents) override
    {
    }

    virtual void onWebContentsDeleted(WebContents* contents) override
    {
    }
    virtual void onWebContentsReadyToShow(WebContents* contents) override
    {
        mate::EventEmitter<BrowserWindow>::emit("ready-to-show");
    }

    static void newFunction(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        v8::Isolate* isolate = args.GetIsolate();

        if (args.IsConstructCall()) {
            int conut = args.Length();
            if (conut > 1)
                return;

            gin_helper::Dictionary options(isolate, (conut != 0 ? 
                args[0]->ToObject(args.GetIsolate()->GetCurrentContext()).ToLocalChecked() : 
                v8::Object::New(isolate)));
            BrowserWindow* self = newWindow(&options, args.This());
            WindowList::getInstance()->addWindow(self);
            if (self && self->m_webContents && self->m_state == WindowInited
                && ::IsWindow(self->m_hWnd) && App::getInstance()) {
                App::getInstance()->emit("browser-window-created", args.This());
            }

            args.GetReturnValue().Set(args.This());
        }
    }

    static v8::Persistent<v8::Function> constructor;

public:
    static gin_helper::WrapperInfo kWrapperInfo;
    static const WCHAR* kPropW;

private:
    friend class WindowInterface;

    WindowState m_state;
    WebContents* m_webContents;

    v8::Persistent<v8::Object> m_live;

    bool m_isDestroyApiBeCalled;

    HWND m_hWnd;
    HIMC m_hIMC;

    int m_cursorInfoType;
    bool m_isCursorInfoTypeAsynGetting;
    CRITICAL_SECTION m_memoryCanvasLock;
    HBITMAP m_memoryBMP;
    HDC m_memoryDC;
    RECT m_clientRect;
    SIZE m_memoryBmpSize;
    HDC m_captionDC;
    HBITMAP m_captionBitmap;
    SIZE m_captionBitmapSize;
    sk_sp<SkSurface> m_captionSurface;

    HRGN m_draggableRegion;
    struct DraggableRegion {
        gfx::Rect bounds;
        bool draggable;
    };
    std::vector<DraggableRegion> m_draggableRegions;

    bool m_isMaximized;
    bool m_isFullScreen;
    bool m_isDocumentEdited;
    bool m_isIgnoreMouseEvents;
    bool m_isMouseDown;

    bool m_titleBarOverlayEnabled;
    int m_titleBarOverlayHeight;
    COLORREF m_titleBarOverlayColor;
    COLORREF m_titleBarOverlaySymbolColor;
    CaptionButton m_captionHoverButton;
    CaptionButton m_captionPressedButton;
    bool m_trackingMouseLeave;

    HMENU m_nativeMenu;
    UINT m_lastAcceleratorKey;
    bool m_autoHideMenuBar;
    bool m_menuBarVisible;
    bool m_menuBarAltVisible;

    DWORD m_fullScreenStyle;
    DWORD m_fullScreenExStyle;
    WINDOWPLACEMENT m_windowPlacement;

    SIZE m_contentsSize;

    WebContents::BrowserWindowConstructorOptions* m_createWindowParam;
    //DragAction* m_dragAction;

    CRITICAL_SECTION m_mouseMsgQueueLock;
    struct MouseMsg {
        MouseMsg(const MouseMsg& other)
        {
            init(message, x, y, flags);
        }

        MouseMsg(unsigned int message, int x, int y, unsigned int flags)
        {
            init(message, x, y, flags);
        }

        void init(unsigned int message, int x, int y, unsigned int flags)
        {
            this->message = message;
            this->x = x;
            this->y = y;
            this->flags = flags;
        }
        unsigned int message;
        int x;
        int y;
        unsigned int flags;
    };
    std::list<MouseMsg*> m_mouseMsgQueue;
    mutable CRITICAL_SECTION m_browserViewsLock;
    std::vector<BrowserView*> m_browserViews;
    BrowserView* m_foucsBrowserView;

    int m_id;
};

void WindowInterface::setNativeMenu(HMENU menu)
{
    HWND window = getHWND();
    if (!window)
        return;
    ::SetMenu(window, menu);
    ::DrawMenuBar(window);
}

v8::Local<v8::Value> WindowInterface::getFocusedWindow(v8::Isolate* isolate)
{
    v8::Local<v8::Value> result;
    HWND focusWnd = ::GetFocus();
    BrowserWindow* self = (BrowserWindow*)::GetPropW(focusWnd, BrowserWindow::kPropW);
    if (!self)
        result = v8::Null(isolate);
    else
        result = self->GetWrapper(isolate);
    return result;
}

v8::Local<v8::Value> WindowInterface::getFocusedContents(v8::Isolate* isolate)
{
    v8::Local<v8::Value> result;
    HWND focusWnd = ::GetFocus();
    BrowserWindow* self = (BrowserWindow*)::GetPropW(focusWnd, BrowserWindow::kPropW);
    if (!self)
        return v8::Null(isolate);

    WebContents* content = self->getWebContents();
    if (!content)
        return v8::Null(isolate);
    result = content->GetWrapper(isolate);
    return result;
}

WebContents* WindowInterface::onCreateNewWebview(v8::Local<v8::Object> newGuestWindow)
{
    BrowserWindow* self = (BrowserWindow*)gin_helper::WrappableBase::GetNativePtr(newGuestWindow, &BrowserWindow::kWrapperInfo);
    if (!self)
        return nullptr;
    return self->getWebContents();
}

const WCHAR* BrowserWindow::kPropW = L"ElectronWindow";
v8::Persistent<v8::Function> BrowserWindow::constructor;
gin_helper::WrapperInfo BrowserWindow::kWrapperInfo = { gin_helper::GinEmbedder::kEmbedderNativeGin };

static void initializeWindowApi(v8::Local<v8::Object> target, v8::Local<v8::Value> unused, v8::Local<v8::Context> context, const NodeNative* native)
{
    node::Environment* env = nodeEnvironmentGetByV8Context(context);
    BrowserWindow::init(target, env);
    WNDCLASSEXW wndClass = { 0 };

    static bool isInitClass = false;

    if (!isInitClass) {
        isInitClass = true;

        wndClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;

        HMODULE hMod = ::GetModuleHandleW(NULL);
        wndClass.cbSize = sizeof(WNDCLASSEX);
        wndClass.lpfnWndProc = &BrowserWindow::staticWindowProc;
        wndClass.cbClsExtra = 0;
        wndClass.cbWndExtra = 0;
        wndClass.hInstance = hMod;
        wndClass.hIcon = nullptr;// LoadIcon(hMod, MAKEINTRESOURCE(IDC_SMALL));
        wndClass.hCursor = LoadCursor(hMod, IDC_ARROW);
        wndClass.hbrBackground = NULL;
        wndClass.lpszMenuName = NULL;
        wndClass.lpszClassName = WindowInterface::kElectronClassName;
        wndClass.hIconSm = LoadIcon(hMod, MAKEINTRESOURCE(IDC_SMALL));
        ::RegisterClassExW(&wndClass);
    }
}

#pragma warning(push)
#pragma warning(disable : 4309)
#pragma warning(disable : 4838)
static const char BrowserWindowNative[] = "//const {EventEmitter} = require('events');"
                                          "//const {BrowserWindow} = process.binding('atom_browser_window');"
                                          "//Object.setPrototypeOf(BrowserWindow.prototype, EventEmitter.prototype);"
                                          "//module.exports = BrowserWindow;";
#pragma warning(pop)

static NodeNative nativeBrowserWindowNative { "BrowserWindow", BrowserWindowNative, sizeof(BrowserWindowNative) - 1 };

NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_MANUAL(electron_browser_browserwindow, initializeWindowApi, &nativeBrowserWindowNative)

} // atom﻿
