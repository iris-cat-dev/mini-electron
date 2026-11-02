
#include "src/runtime/electron/browser/api/web_contents.h"
#include "src/runtime/electron/browser/api/window_list.h"
#include "src/runtime/electron/browser/api/app.h"
#include "src/runtime/electron/browser/api/window_interface.h"
#include "src/runtime/electron/browser/api/protocol_interface.h"
#include "src/runtime/electron/browser/api/menu_event_notif.h"
#include "src/runtime/electron/browser/api/browser_view.h"
#include "src/runtime/electron/browser/api/session.h"
#include "src/runtime/electron/common/options_switches.h"
#include "src/runtime/electron/common/node_register_help.h"
#include "src/runtime/electron/common/embedded_resources.h"
#include "src/runtime/electron/common/string_util.h"
#include "src/runtime/electron/common/api/event_emitter.h"
#include "src/runtime/electron/common/id_live_detect.h"
#include "src/runtime/electron/common/win_user_msg.h"
//#include "common/DragAction.h"
#include "src/runtime/electron/common/asar/asar_util.h"
#include "src/runtime/electron/common/platform_util.h"
#include "src/runtime/electron/common/gin_helper/per_isolate_data.h"
#include "src/runtime/electron/common/gin_helper/object_template_builder.h"
#include "src/runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/libnode/src/node_buffer.h"
#include "third_party/libuv/include/uv.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "src/platform/windows/dcomp_presenter.h"
#include "base/command_line.h"
#include "base/auto_reset.h"
#include "src/runtime/engine/common/thread_call.h"
#include "ui/gfx/icon_util.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/display/win/screen_win.h"
#include "ui/display/screen.h"
#include "base/no_destructor.h"
#include "base/files/file_path.h"
#include "base/strings/utf_string_conversions.h"
#include "base/strings/string_number_conversions.h"
#include "base/win/windows_version.h"
#include "src/platform/windows/resources/resource.h"
#include <shellapi.h>
#include <ole2.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
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

class BrowserWindow;
// Registered and invoked by OLE on the BrowserWindow UI STA. The owner is
// detached before revocation so a late COM release/callback cannot reach it.

class BrowserWindowDropTarget final : public IDropTarget {
public:
    explicit BrowserWindowDropTarget(BrowserWindow* owner);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE DragEnter(
        IDataObject* data_object, DWORD key_state, POINTL point, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(
        DWORD key_state, POINTL point, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(
        IDataObject* data_object, DWORD key_state, POINTL point, DWORD* effect) override;

    void cancelDrag();
    void browserViewWillDetach(BrowserView* view);
    void detachOwner();

private:
    ~BrowserWindowDropTarget() = default;

    static bool readPaths(IDataObject* data_object, std::vector<std::string>* paths);
    bool updateDestination(POINTL point, DWORD key_state);
    void leaveDestination(POINTL point, DWORD key_state);
    void resetDrag();
    WebContents* currentContents() const;

    BrowserWindow* owner_;
    volatile LONG references_;
    bool tracking_;
    bool destination_selected_;
    bool destination_active_;
    bool destination_is_main_;
    BrowserView* destination_view_;
    bool source_allows_copy_;
    POINTL last_point_;
    DWORD last_key_state_;
    std::vector<std::string> paths_;
};


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
        m_focusedWebContents = nullptr;
        m_textInputContents = nullptr;
        m_pendingImeContents = nullptr;
        m_imeTarget = nullptr;
        m_textInputCaret = {};
        m_textInputEditable = false;
        m_nativeImeActive = false;
        m_rendererCompositionActive = false;
        m_imeCleanup = false;
        m_ownsImeCaret = false;
        m_imeCaretHeight = 0;
        m_imeSerial = 0;
        m_pendingImeSerial = 0;
        m_memoryBMP = nullptr;
        m_memoryDC = nullptr;
        m_captionDC = nullptr;
        m_captionBitmap = nullptr;
        m_captionBitmapSize = { 0, 0 };
        m_isDestroyApiBeCalled = false;
        m_isMaximized = false;
        m_isMinimized = false;
        m_isFullScreen = false;
        m_isDocumentEdited = false;
        m_isIgnoreMouseEvents = false;
        m_isMouseDown = false;
        m_titleBarOverlayEnabled = false;
        m_titleBarOverlayHeight = 30;
        m_titleBarOverlayColor = SkColorSetRGB(0x18, 0x1b, 0x1a);
        m_titleBarOverlaySymbolColor = SkColorSetRGB(0xe4, 0xe4, 0xe7);
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
        m_dropTarget = nullptr;

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


        if (m_dropTarget) {
            m_dropTarget->detachOwner();
            m_dropTarget->Release();
            m_dropTarget = nullptr;
        }
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
        builder.SetMethod("setAspectRatio", &BrowserWindow::setAspectRatioApi);
        builder.SetMethod("previewFile", &BrowserWindow::previewFileApi);
        builder.SetMethod("closeFilePreview", &BrowserWindow::closeFilePreviewApi);
        builder.SetMethod("setBrowserView", &BrowserWindow::setBrowserViewApi);
        builder.SetMethod("addBrowserView", &BrowserWindow::setBrowserViewApi);
        builder.SetMethod("removeBrowserView", &BrowserWindow::removeBrowserViewApi);
        builder.SetMethod("setParentWindow", &BrowserWindow::setParentWindowApi);
        builder.SetMethod("getParentWindow", &BrowserWindow::getParentWindowApi);
        builder.SetMethod("getChildWindows", &BrowserWindow::getChildWindowsApi);
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
        builder.SetMethod("setSheetOffset", &BrowserWindow::setSheetOffsetApi);
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
        builder.SetMethod("setKiosk", &BrowserWindow::setKioskApi);
        builder.SetMethod("isKiosk", &BrowserWindow::isKioskApi);
        builder.SetMethod("setBackgroundColor", &BrowserWindow::setBackgroundColorApi);
        builder.SetMethod("setHasShadow", &BrowserWindow::setHasShadowApi);
        builder.SetMethod("hasShadow", &BrowserWindow::hasShadowApi);
        builder.SetMethod("setRepresentedFilename", &BrowserWindow::setRepresentedFilenameApi);
        builder.SetMethod("getRepresentedFilename", &BrowserWindow::getRepresentedFilenameApi);
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
        builder.SetMethod("setVibrancy", &BrowserWindow::setVibrancyApi);
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

    bool handleNativeWindowCommand(const std::string& command,
        const base::Value::Dict& params) override
    {
        if (isClosed() || !::IsWindow(m_hWnd))
            return false;
        if (command == "fullscreen") {
            const auto enabled = params.FindBool("enabled");
            if (!enabled)
                return false;
            if (*enabled == m_isFullScreen) {
                notifyRendererFullScreenState();
                return true;
            }
            setFullScreenApi(*enabled);
            return m_isFullScreen == *enabled;
        }
        if (command == "focus") {
            focusApi();
            return ::GetFocus() == m_hWnd;
        }
        if (command == "take-focus") {
            const bool reverse = params.FindBool("reverse").value_or(false);
            HWND target = ::GetNextDlgTabItem(::GetParent(m_hWnd), m_hWnd, reverse);
            if (!target || target == m_hWnd)
                return false;
            ::SetFocus(target);
            return ::GetFocus() == target;
        }
        if (command == "bounds") {
            const auto x = params.FindInt("x");
            const auto y = params.FindInt("y");
            const auto width = params.FindInt("width");
            const auto height = params.FindInt("height");
            if (!x || !y || !width || !height || *width < 0 || *height < 0)
                return false;
            const gfx::Rect pixels = display::win::ScreenWin::DIPToScreenRect(
                nullptr, gfx::Rect(*x, *y, *width, *height));
            return !!::SetWindowPos(m_hWnd, nullptr, pixels.x(), pixels.y(),
                pixels.width(), pixels.height(), SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (command == "show") {
            showApi();
            return isVisibleApi();
        }
        if (command == "maximize") {
            maximizeApi();
            return !!::IsZoomed(m_hWnd);
        }
        if (command == "minimize") {
            if (!m_createWindowParam->isMinimizable)
                return false;
            minimizeApi();
            return !!::IsIconic(m_hWnd);
        }
        if (command == "restore") {
            restoreApi();
            return !::IsIconic(m_hWnd) && !::IsZoomed(m_hWnd);
        }
        if (command == "resizable") {
            const auto enabled = params.FindBool("enabled");
            if (!enabled)
                return false;
            setResizableApi(*enabled);
            return isResizableApi() == *enabled;
        }
        if (command == "close")
            return !!::PostMessageW(m_hWnd, WM_CLOSE, 0, 0);
        return false;
    }

    void updateVSyncDisplay(bool force = false)
    {
        if (!m_webContents || !::IsWindow(m_hWnd))
            return;
        const HMONITOR monitor = ::MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
        if (!force && monitor == m_vsyncMonitor)
            return;
        m_vsyncMonitor = monitor;
        const int64_t id = static_cast<int64_t>(reinterpret_cast<intptr_t>(monitor));
        m_webContents->setVSyncDisplayId(id);
        for (BrowserView* view : m_browserViews) {
            if (view && view->getWebContents())
                view->getWebContents()->setVSyncDisplayId(id);
        }
    }
    void publishScreenRects()
    {
        if (!m_webContents || !::IsWindow(m_hWnd))
            return;
        RECT window;
        RECT client;
        if (!::GetWindowRect(m_hWnd, &window)
            || !::GetClientRect(m_hWnd, &client)) {
            return;
        }
        POINT client_origin = { client.left, client.top };
        if (!::ClientToScreen(m_hWnd, &client_origin))
            return;
        const gfx::Rect widget_pixels(client_origin.x, client_origin.y,
            client.right - client.left, client.bottom - client.top);
        m_webContents->setScreenRects(
            display::win::ScreenWin::ScreenToDIPRect(m_hWnd, widget_pixels),
            display::win::ScreenWin::ScreenToDIPRect(m_hWnd, gfx::Rect(window)));
        for (BrowserView* view : m_browserViews) {
            if (view)
                view->updateScreenRects();
        }
    }


    void setRendererVisible(bool visible)
    {
        if (m_webContents)
            m_webContents->setVisible(visible);
        for (BrowserView* view : m_browserViews) {
            if (view && view->getWebContents())
                view->getWebContents()->setVisible(visible);
        }
    }

    void onWebContentsPaint(WebContents* contents, const gfx::Rect& damage) override
    {
        if (contents != m_webContents || !::IsWindow(m_hWnd))
            return;
        RECT client;
        if (!::GetClientRect(m_hWnd, &client))
            return;
        gfx::Rect dirty(0, 0, client.right - client.left, client.bottom - client.top);
        if (!damage.IsEmpty())
            dirty.Intersect(damage);
        if (dirty.IsEmpty())
            return;
        const RECT rect = { dirty.x(), dirty.y(), dirty.right(), dirty.bottom() };
        ::InvalidateRect(m_hWnd, &rect, FALSE);
    }

    void onWebContentsDraggableRegions(
        WebContents* contents, const base::Value::List& regions) override
    {
        if (contents != m_webContents)
            return;
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

    static bool parseOverlayColor(const std::string& input, SkColor* result)
    {
        if (!result || input.empty() || input[0] != '#')
            return false;

        const size_t length = input.size() - 1;
        if (length != 3 && length != 4 && length != 6 && length != 8)
            return false;

        const bool shortForm = length <= 4;
        const size_t componentCount = (length == 4 || length == 8) ? 4 : 3;
        const auto hexValue = [](char digit) -> int {
            if (digit >= '0' && digit <= '9')
                return digit - '0';
            if (digit >= 'a' && digit <= 'f')
                return digit - 'a' + 10;
            if (digit >= 'A' && digit <= 'F')
                return digit - 'A' + 10;
            return -1;
        };
        uint8_t components[4] = { 0, 0, 0, 0xff };
        for (size_t component = 0; component < componentCount; ++component) {
            const size_t offset = 1 + component * (shortForm ? 1 : 2);
            const int high = hexValue(input[offset]);
            if (high < 0)
                return false;
            if (shortForm) {
                components[component] = static_cast<uint8_t>(high * 0x11);
                continue;
            }
            const int low = hexValue(input[offset + 1]);
            if (low < 0)
                return false;
            components[component] = static_cast<uint8_t>((high << 4) | low);
        }

        *result = SkColorSetARGB(components[3], components[0], components[1], components[2]);
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
        const int shadowMargin = m_hasShadow ? 1 : 0;
        const MARGINS margins = { shadowMargin, shadowMargin, shadowMargin, shadowMargin };
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

    static SkColor sourceOver(SkColor under, SkColor over)
    {
        const int overAlpha = SkColorGetA(over);
        const int underAlpha = SkColorGetA(under);
        const int inverseAlpha = 0xff - overAlpha;
        const int outputAlpha = overAlpha + (underAlpha * inverseAlpha + 0x7f) / 0xff;
        if (!outputAlpha)
            return SK_ColorTRANSPARENT;

        const auto compositeChannel = [&](int underChannel, int overChannel) {
            const int premultiplied = overChannel * overAlpha
                + (underChannel * underAlpha * inverseAlpha + 0x7f) / 0xff;
            return (premultiplied + outputAlpha / 2) / outputAlpha;
        };
        return SkColorSetARGB(outputAlpha,
            compositeChannel(SkColorGetR(under), SkColorGetR(over)),
            compositeChannel(SkColorGetG(under), SkColorGetG(over)),
            compositeChannel(SkColorGetB(under), SkColorGetB(over)));
    }

    void invalidateCaptionButtons()
    {
        if (!m_hWnd || !isCustomTitleBar())
            return;
        m_gpuCaptionDirty = true;
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
        canvas->clear(SK_ColorTRANSPARENT);
        canvas->save();
        canvas->translate(-buttons.left, -buttons.top);
        SkPaint paint;
        const int symbolSize = std::max(1, dipToPixel(10));
        const int strokeWidth = std::max(1, dipToPixel(1));
        for (int value = CaptionButtonMinimize; value <= CaptionButtonClose; ++value) {
            CaptionButton button = static_cast<CaptionButton>(value);
            RECT rect = captionButtonRect(button);
            SkColor backgroundColor = m_titleBarOverlayColor;
            if (button == m_captionHoverButton) {
                const bool pressed = button == m_captionPressedButton;
                if (button == CaptionButtonClose) {
                    backgroundColor = pressed
                        ? SkColorSetRGB(0xb4, 0x26, 0x35)
                        : SkColorSetRGB(0xc4, 0x2b, 0x3a);
                } else {
                    const int tintAlpha = pressed ? 0x33 : 0x1f;
                    const SkColor tint = SkColorSetARGB(tintAlpha,
                        SkColorGetR(m_titleBarOverlaySymbolColor),
                        SkColorGetG(m_titleBarOverlaySymbolColor),
                        SkColorGetB(m_titleBarOverlaySymbolColor));
                    backgroundColor = sourceOver(backgroundColor, tint);
                }
            }
            paint.setStyle(SkPaint::kFill_Style);
            paint.setAntiAlias(false);
            paint.setBlendMode(SkBlendMode::kSrcOver);
            paint.setColor(backgroundColor);
            canvas->drawRect(SkRect::MakeLTRB(rect.left, buttons.top, rect.right, rect.bottom), paint);

            SkColor symbolColor = m_titleBarOverlaySymbolColor;
            if (!isCaptionButtonEnabled(button)) {
                symbolColor = SkColorSetA(symbolColor,
                    (SkColorGetA(symbolColor) * 40 + 50) / 100);
            }
            paint.setColor(symbolColor);
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
                    paint.setBlendMode(SkBlendMode::kSrc);
                    paint.setColor(backgroundColor);
                    canvas->drawRect(front, paint);
                    paint.setStyle(SkPaint::kStroke_Style);
                    paint.setBlendMode(SkBlendMode::kSrcOver);
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

        if (!dc)
            return; // GPU presentation uploads only this cached caption surface.
        // GDI drawing clears alpha on DWM glass. Alpha-composite the cached
        // premultiplied raster surface over the browser's painted header.
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


    bool ensurePaintSurface(int width, int height)
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


    bool paintGpuScene()
    {
        if (!m_createWindowParam
            || !(m_createWindowParam->styleEx & WS_EX_NOREDIRECTIONBITMAP)) {
            return false;
        }
        if (m_gpuPresenting)
            return true;
        base::AutoReset<bool> presenting(&m_gpuPresenting, true);
        RECT client;
        if (!m_webContents || !::GetClientRect(m_hWnd, &client)
            || client.right <= 0 || client.bottom <= 0)
            return true;
        m_gpuLayers.clear();
        m_webContents->appendDCompLayers(m_gpuLayers, client, client);
        for (BrowserView* view : m_browserViews) {
            if (!view || !view->getWebContents())
                continue;
            const RECT bounds = view->getClientRect();
            RECT clip;
            if (::IntersectRect(&clip, &bounds, &client))
                view->getWebContents()->appendDCompLayers(m_gpuLayers, bounds, clip);
        }
        if (!m_gpuPresenter) {
            m_gpuPresenter = mini_electron::win::DCompPresenter::Create(m_hWnd);
            if (!m_gpuPresenter)
                return true;
        }
        mini_electron::win::DCompOverlay overlay;
        SkPixmap pixels;
        if (isCustomTitleBar()) {
            const RECT buttons = captionButtonsRect();
            if (m_gpuCaptionDirty || !m_captionSurface
                || m_captionBitmapSize.cx != buttons.right - buttons.left
                || m_captionBitmapSize.cy != buttons.bottom - buttons.top) {
                paintTitleBarOverlay(nullptr, client);
                ++m_gpuCaptionGeneration;
                m_gpuCaptionDirty = false;
            }
            if (m_captionSurface && m_captionSurface->peekPixels(&pixels)) {
                overlay.bgra = pixels.addr();
                overlay.width = pixels.width();
                overlay.height = pixels.height();
                overlay.stride = static_cast<int>(pixels.rowBytes());
                overlay.x = buttons.left;
                overlay.y = buttons.top;
                overlay.generation = m_gpuCaptionGeneration;
            }
        }
        auto presenter = m_gpuPresenter;
        if (!presenter->Present(m_gpuLayers, overlay))
            m_gpuPresenter.reset();
        return true;
    }

    void onPaintMessage(HWND hWnd)
    {
        PAINTSTRUCT ps = { 0 };
        HDC paintDC = ::BeginPaint(hWnd, &ps);
        if (paintGpuScene()) {
            ::EndPaint(hWnd, &ps);
            return;
        }

        RECT rcClip = ps.rcPaint;
        RECT rcClient;
        ::GetClientRect(hWnd, &rcClient);

        const bool transparent = m_createWindowParam->transparent;
        const bool buffered = transparent || isCustomTitleBar();
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
        if (buffered) {
            const int clientWidth = rcClient.right - rcClient.left;
            const int clientHeight = rcClient.bottom - rcClient.top;
            targetDC = ensurePaintSurface(clientWidth, clientHeight)
                ? m_memoryDC
                : nullptr;
        }

        if (targetDC && width > 0 && height > 0) {
            if (m_webContents) {
                m_webContents->paintFrame(targetDC, rcInvalid.left,
                    rcInvalid.top, srcX, srcY, width, height,
                    gfx::Size(rcClient.right - rcClient.left, rcClient.bottom - rcClient.top));
            }
            paintBrowserViews(targetDC, rcInvalid);
            paintTitleBarOverlay(targetDC, rcInvalid);
        }
        if (transparent && targetDC)
            updateLayeredWindow();
        else if (buffered && targetDC && width > 0 && height > 0) {
            // Present the webpage, BrowserViews and caption buttons together.
            // Reuse the backing DIB and copy only the dirty region.
            ::BitBlt(paintDC, rcInvalid.left, rcInvalid.top, width, height,
                targetDC, srcX, srcY, SRCCOPY);
        }

        ::EndPaint(hWnd, &ps);
    }

    void paintBrowserViews(HDC hdc, const RECT& parentPaintRect)
    {
        for (BrowserView* view : m_browserViews) {
            if (view && view->getWebContents())
                view->onPaintInUiThread(hdc, parentPaintRect);
        }
    }



    static bool parseTextInputRect(
        const base::Value::Dict& state, const char* name, RECT* result)
    {
        const base::Value::Dict* value = state.FindDict(name);
        if (!value)
            return false;
        const std::optional<int> x = value->FindInt("x");
        const std::optional<int> y = value->FindInt("y");
        const std::optional<int> width = value->FindInt("width");
        const std::optional<int> height = value->FindInt("height");
        if (!x || !y || !width || !height || *width < 0 || *height <= 0)
            return false;
        const int64_t right = static_cast<int64_t>(*x) + *width;
        const int64_t bottom = static_cast<int64_t>(*y) + *height;
        if (right > std::numeric_limits<LONG>::max()
            || right < std::numeric_limits<LONG>::min()
            || bottom > std::numeric_limits<LONG>::max()
            || bottom < std::numeric_limits<LONG>::min()) {
            return false;
        }
        result->left = *x;
        result->top = *y;
        result->right = static_cast<LONG>(right);
        result->bottom = static_cast<LONG>(bottom);
        return true;
    }

    void clearTextInputState()
    {
        if (m_textInputEditable && ::IsWindow(m_hWnd))
            ::ImmAssociateContextEx(m_hWnd, nullptr, 0);
        m_textInputContents = nullptr;
        m_textInputCaret = {};
        m_textInputEditable = false;
        m_pendingImeContents = nullptr;
        m_pendingImeSerial = 0;
    }

    bool sendImeInput(WebContents* contents, base::Value::Dict input)
    {
        if (!contents)
            return false;
        if (m_imeSerial == std::numeric_limits<uint64_t>::max())
            m_imeSerial = 1;
        else
            ++m_imeSerial;
        input.Set("imeSerial", base::NumberToString(m_imeSerial));
        if (!contents->sendInput(std::move(input)))
            return false;
        m_pendingImeContents = contents;
        m_pendingImeSerial = m_imeSerial;
        return true;
    }

    bool sendImeSetComposition(
        WebContents* contents, const std::string& text, int cursor)
    {
        base::Value::Dict input;
        input.Set("type", "imeSetComposition");
        input.Set("text", text);
        input.Set("selectionStart", cursor);
        input.Set("selectionEnd", cursor);
        return sendImeInput(contents, std::move(input));
    }

    bool sendImeCommitText(WebContents* contents, const std::string& text)
    {
        base::Value::Dict input;
        input.Set("type", "imeCommitText");
        input.Set("text", text);
        input.Set("relativeCursorPosition", 0);
        return sendImeInput(contents, std::move(input));
    }

    bool sendImeFinishComposition(WebContents* contents)
    {
        base::Value::Dict input;
        input.Set("type", "imeFinishComposition");
        return sendImeInput(contents, std::move(input));
    }

    static bool readImeString(
        HIMC context, DWORD index, std::wstring* result)
    {
        constexpr LONG kMaximumCompositionBytes = 1024 * 1024;
        const LONG bytes = ::ImmGetCompositionStringW(
            context, index, nullptr, 0);
        if (bytes < 0 || bytes > kMaximumCompositionBytes
            || bytes % sizeof(wchar_t) != 0) {
            return false;
        }
        result->resize(bytes / sizeof(wchar_t));
        if (bytes == 0)
            return true;
        return ::ImmGetCompositionStringW(
                   context, index, result->data(), bytes)
            == bytes;
    }

    void destroyImeCaret()
    {
        if (m_ownsImeCaret)
            ::DestroyCaret();
        m_ownsImeCaret = false;
        m_imeCaretHeight = 0;
    }

    bool currentTextInputCaret(RECT* result)
    {
        if (!m_textInputEditable || !m_textInputContents
            || m_textInputContents != m_focusedWebContents) {
            return false;
        }
        *result = m_textInputCaret;
        if (m_textInputContents == m_webContents)
            return true;
        BrowserView* owner = nullptr;
        for (BrowserView* view : m_browserViews) {
            if (view && view->getWebContents() == m_textInputContents) {
                owner = view;
                break;
            }
        }
        if (!owner)
            return false;
        const RECT bounds = owner->getClientRect();
        const int64_t left = static_cast<int64_t>(result->left) + bounds.left;
        const int64_t top = static_cast<int64_t>(result->top) + bounds.top;
        const int64_t right = static_cast<int64_t>(result->right) + bounds.left;
        const int64_t bottom = static_cast<int64_t>(result->bottom) + bounds.top;
        if (left > std::numeric_limits<LONG>::max()
            || left < std::numeric_limits<LONG>::min()
            || top > std::numeric_limits<LONG>::max()
            || top < std::numeric_limits<LONG>::min()
            || right > std::numeric_limits<LONG>::max()
            || right < std::numeric_limits<LONG>::min()
            || bottom > std::numeric_limits<LONG>::max()
            || bottom < std::numeric_limits<LONG>::min()) {
            return false;
        }
        result->left = static_cast<LONG>(left);
        result->top = static_cast<LONG>(top);
        result->right = static_cast<LONG>(right);
        result->bottom = static_cast<LONG>(bottom);
        return true;
    }

    void updateImePosition()
    {
        if (!m_nativeImeActive || m_imeTarget != m_focusedWebContents
            || ::GetFocus() != m_hWnd) {
            return;
        }
        RECT caret;
        if (!currentTextInputCaret(&caret))
            return;
        const int height = std::max<LONG>(1, caret.bottom - caret.top);
        if (!m_ownsImeCaret || m_imeCaretHeight != height) {
            destroyImeCaret();
            m_ownsImeCaret = !!::CreateCaret(m_hWnd, nullptr, 1, height);
            if (m_ownsImeCaret)
                m_imeCaretHeight = height;
        }
        if (m_ownsImeCaret)
            ::SetCaretPos(caret.left, caret.top);

        HIMC context = ::ImmGetContext(m_hWnd);
        if (!context)
            return;
        COMPOSITIONFORM composition = {};
        composition.dwStyle = CFS_FORCE_POSITION;
        composition.ptCurrentPos = { caret.left, caret.top };
        ::ImmSetCompositionWindow(context, &composition);

        CANDIDATEFORM candidate = {};
        candidate.dwIndex = 0;
        candidate.dwStyle = CFS_EXCLUDE;
        candidate.ptCurrentPos = { caret.left, caret.top };
        candidate.rcArea = caret;
        ::ImmSetCandidateWindow(context, &candidate);
        ::ImmReleaseContext(m_hWnd, context);
    }

    void resetImeState()
    {
        m_nativeImeActive = false;
        m_rendererCompositionActive = false;
        m_imeTarget = nullptr;
        destroyImeCaret();
    }

    void stopImeComposition(bool finish_renderer)
    {
        WebContents* target = m_imeTarget;
        const bool had_native_composition = m_nativeImeActive;
        const bool had_renderer_composition = m_rendererCompositionActive;
        resetImeState();
        if (had_native_composition && ::IsWindow(m_hWnd)) {
            HIMC context = ::ImmGetContext(m_hWnd);
            if (context) {
                m_imeCleanup = true;
                ::ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                m_imeCleanup = false;
                ::ImmReleaseContext(m_hWnd, context);
            }
        }
        if (finish_renderer && had_renderer_composition)
            sendImeFinishComposition(target);
    }

    bool beginImeComposition()
    {
        WebContents* target = focusedWebContents();
        if (!target || ::GetFocus() != m_hWnd)
            return false;
        if (m_nativeImeActive && m_imeTarget == target)
            return true;
        if (m_nativeImeActive || m_rendererCompositionActive)
            stopImeComposition(true);
        m_nativeImeActive = true;
        m_imeTarget = target;
        updateImePosition();
        return true;
    }

    LRESULT onImeComposition(LPARAM flags)
    {
        if (m_imeCleanup)
            return 0;
        if (flags == 0) {
            if (m_rendererCompositionActive)
                sendImeSetComposition(m_imeTarget, std::string(), 0);
            resetImeState();
            return 0;
        }
        if (!m_nativeImeActive && !beginImeComposition())
            return 0;
        WebContents* target = m_imeTarget;
        HIMC context = ::ImmGetContext(m_hWnd);
        if (!context)
            return 0;

        if (flags & GCS_RESULTSTR) {
            std::wstring result;
            if (readImeString(context, GCS_RESULTSTR, &result)
                && sendImeCommitText(target, base::WideToUTF8(result))) {
                m_rendererCompositionActive = false;
            }
        }
        if (flags & GCS_COMPSTR) {
            std::wstring composition;
            if (readImeString(context, GCS_COMPSTR, &composition)) {
                LONG cursor = ::ImmGetCompositionStringW(
                    context, GCS_CURSORPOS, nullptr, 0);
                if (cursor < 0)
                    cursor = static_cast<LONG>(composition.size());
                cursor = std::clamp<LONG>(
                    cursor, 0, static_cast<LONG>(composition.size()));
                if (flags & CS_NOMOVECARET)
                    cursor = 0;
                if (sendImeSetComposition(
                        target, base::WideToUTF8(composition), cursor)) {
                    m_rendererCompositionActive = !composition.empty();
                }
            }
        }
        ::ImmReleaseContext(m_hWnd, context);
        updateImePosition();
        return 0;
    }

    LRESULT onImeEndComposition()
    {
        if (m_imeCleanup)
            return 0;
        if (m_rendererCompositionActive)
            sendImeSetComposition(m_imeTarget, std::string(), 0);
        resetImeState();
        return 0;
    }

    void setFocusedBrowserView(BrowserView* view)
    {
        WebContents* next = view ? view->getWebContents() : m_webContents;
        WebContents* previous = focusedWebContents();
        if (view == m_foucsBrowserView && next == previous)
            return;
        stopImeComposition(true);
        if (previous)
            previous->setFocus(false);
        if (previous && previous != m_webContents)
            previous->removeObserver(this);
        clearTextInputState();
        m_foucsBrowserView = view;
        m_focusedWebContents = next;
        if (next && next != m_webContents)
            next->addObserver(this);
        if (next && ::GetFocus() == m_hWnd)
            next->setFocus(true);
    }

    bool mouseMsgToBrowserViews(unsigned int message, int x, int y,
        const gfx::PointF& screenPoint, unsigned int flags)
    {
        if (m_foucsBrowserView && !m_foucsBrowserView->getWebContents())
            setFocusedBrowserView(nullptr);
        bool isHandled = false;
        BrowserView* hittestView = m_isMouseDown ? nullptr : findHittestBrowserview(x, y);
        const bool buttonDown = message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN
            || message == WM_RBUTTONDOWN || message == WM_LBUTTONDBLCLK
            || message == WM_MBUTTONDBLCLK || message == WM_RBUTTONDBLCLK;
        const bool buttonUp = message == WM_LBUTTONUP || message == WM_MBUTTONUP
            || message == WM_RBUTTONUP;
        // Choose the captured view on the first press, then deliver every
        // chorded transition (including the final release) to that same view.
        if (buttonDown && !m_isMouseDown) {
            m_isMouseDown = true;
            setFocusedBrowserView(hittestView);
        }
        BrowserView* target = m_isMouseDown ? m_foucsBrowserView : hittestView;
        if (target) {
            isHandled = true;
            target->handleMouseMsgInUiThread(message, x, y,
                screenPoint.x(), screenPoint.y(), flags);
        }
        if (buttonUp) {
            m_isMouseDown = !!(flags & (MINI_ELECTRON_LBUTTON | MINI_ELECTRON_MBUTTON | MINI_ELECTRON_RBUTTON));
            if (!m_isMouseDown && ::IsWindow(m_hWnd))
                ::PostMessageW(m_hWnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(m_hWnd), MAKELPARAM(HTCLIENT, 0));
        }
        return isHandled;
    }


    void onMouseMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (m_isIgnoreMouseEvents)
            return;

        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        POINT nativeScreenPoint = { x, y };
        if (!::ClientToScreen(hWnd, &nativeScreenPoint))
            return;
        // Snapshot before focus/capture or application callbacks can move the
        // host. A renderer HWND is not the BrowserWindow's native window.
        const gfx::PointF screenPoint = display::win::ScreenWin::ScreenToDIPPoint(
            gfx::PointF(nativeScreenPoint.x, nativeScreenPoint.y));

        if (message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_RBUTTONDOWN
            || message == WM_LBUTTONDBLCLK || message == WM_MBUTTONDBLCLK || message == WM_RBUTTONDBLCLK) {
            if (m_createWindowParam && m_createWindowParam->isFocusable)
                ::SetFocus(hWnd);
            ::SetCapture(hWnd);
        }

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
        if (!mouseMsgToBrowserViews(message, x, y, screenPoint, flags)) {
            // Widget coordinates are native pixels; screen coordinates are DIP.
            m_webContents->sendWindowsMouseEvent(
                message, x, y, screenPoint.x(), screenPoint.y(), flags);
        }
        if ((message == WM_LBUTTONUP || message == WM_MBUTTONUP || message == WM_RBUTTONUP)
            && !(wParam & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON)) && ::GetCapture() == hWnd)
            ::ReleaseCapture();

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
            if (contents) {
                m_focusedWebContents = contents;
                return contents;
            }
            m_foucsBrowserView = nullptr;
        }
        m_focusedWebContents = m_webContents;
        return m_webContents;
    }

    WebContents* nativeDragContents(BrowserView* view) const
    {
        if (!view)
            return m_webContents;
        if (std::find(m_browserViews.begin(), m_browserViews.end(), view)
            == m_browserViews.end()) {
            return nullptr;
        }
        return view->getWebContents();
    }

    bool nativeDragDestinationAt(
        POINTL screen_point, BrowserView** view, WebContents** contents)
    {
        if (!view || !contents || !::IsWindow(m_hWnd))
            return false;
        POINT client_point = { screen_point.x, screen_point.y };
        if (!::ScreenToClient(m_hWnd, &client_point))
            return false;
        RECT client_rect = {};
        if (!::GetClientRect(m_hWnd, &client_rect)
            || !::PtInRect(&client_rect, client_point)) {
            return false;
        }
        *view = findHittestBrowserview(client_point.x, client_point.y);
        *contents = nativeDragContents(*view);
        return *contents != nullptr;
    }

    bool buildNativeDragEvent(const char* type, POINTL screen_point,
        DWORD key_state, BrowserView* view, base::Value::Dict* event) const
    {
        if (!type || !event || !::IsWindow(m_hWnd)
            || !nativeDragContents(view)) {
            return false;
        }
        POINT client_point = { screen_point.x, screen_point.y };
        if (!::ScreenToClient(m_hWnd, &client_point))
            return false;
        if (view) {
            const RECT bounds = view->getClientRect();
            client_point.x -= bounds.left;
            client_point.y -= bounds.top;
        }
        const gfx::Point client_dip =
            display::win::ScreenWin::ClientToDIPPoint(
                m_hWnd, gfx::Point(client_point.x, client_point.y));
        const gfx::PointF screen_dip =
            display::win::ScreenWin::ScreenToDIPPoint(
                gfx::PointF(screen_point.x, screen_point.y));
        int modifiers = 0;
        if (key_state & MK_SHIFT)
            modifiers |= MINI_ELECTRON_SHIFT;
        if (key_state & MK_CONTROL)
            modifiers |= MINI_ELECTRON_CONTROL;
        if (key_state & MK_LBUTTON)
            modifiers |= MINI_ELECTRON_LBUTTON;
        if (key_state & MK_MBUTTON)
            modifiers |= MINI_ELECTRON_MBUTTON;
        if (key_state & MK_RBUTTON)
            modifiers |= MINI_ELECTRON_RBUTTON;
        if (::GetKeyState(VK_MENU) & 0x8000)
            modifiers |= MINI_ELECTRON_ALT;
        if ((::GetKeyState(VK_LWIN) & 0x8000)
            || (::GetKeyState(VK_RWIN) & 0x8000)) {
            modifiers |= MINI_ELECTRON_META;
        }
        event->Set("type", type);
        event->Set("x", client_dip.x());
        event->Set("y", client_dip.y());
        event->Set("screenX", static_cast<int>(std::lround(screen_dip.x())));
        event->Set("screenY", static_cast<int>(std::lround(screen_dip.y())));
        event->Set("modifiers", modifiers);
        return true;
    }

    void registerNativeDropTarget()
    {
        if (m_dropTarget || !::IsWindow(m_hWnd))
            return;
        BrowserWindowDropTarget* target = new BrowserWindowDropTarget(this);
        if (SUCCEEDED(::RegisterDragDrop(m_hWnd, target))) {
            m_dropTarget = target;
            return;
        }
        target->detachOwner();
        target->Release();
    }

    void unregisterNativeDropTarget(HWND hWnd)
    {
        BrowserWindowDropTarget* target = m_dropTarget;
        if (!target)
            return;
        m_dropTarget = nullptr;
        target->cancelDrag();
        target->detachOwner();
        if (hWnd)
            ::RevokeDragDrop(hWnd);
        target->Release();
    }


    void onWebContentsCursorChanged(WebContents* contents, const ui::Cursor&) override
    {
        if (contents != m_webContents)
            return;
        if (::IsWindow(m_hWnd))
            ::PostMessageW(m_hWnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(m_hWnd), MAKELPARAM(HTCLIENT, 0));
    }

    bool applyContentsCursor(LPARAM)
    {
        RECT rc;
        ::GetClientRect(m_hWnd, &rc);

        POINT pt;
        ::GetCursorPos(&pt);
        ::ScreenToClient(m_hWnd, &pt);
        if (!::PtInRect(&rc, pt))
            return false;

        HCURSOR hCur = NULL;
        if (!m_isMouseDown && !m_createWindowParam->isFrame && m_createWindowParam->isResizable
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

        const UINT dpi = windowDpi();
        BrowserView* view = m_isMouseDown ? m_foucsBrowserView : findHittestBrowserview(pt.x, pt.y);
        if (view)
            view->applyCursor(dpi);
        else if (m_webContents)
            m_nativeCursor.apply(m_webContents->getCursor(), dpi);
        else
            return false;
        return true;
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
            m_gpuPresenter.reset();
            m_gpuLayers.clear();
            stopImeComposition(true);
            clearTextInputState();
            m_state = WindowDestroyed;
            mate::EventEmitter<BrowserWindow>::emit("closed");

            ::KillTimer(hWnd, (UINT_PTR)this);
            unregisterNativeDropTarget(hWnd);
            ::RemovePropW(hWnd, kPropW);
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
        case WM_SYSKEYDOWN: {
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
            unsigned int flags = 0;
            if (HIWORD(lParam) & KF_REPEAT)
                flags |= MINI_ELECTRON_REPEAT;
            if (HIWORD(lParam) & KF_EXTENDED)
                flags |= MINI_ELECTRON_EXTENDED;
            if (WebContents* target = focusedWebContents()) {
                target->sendWindowsKeyEvent(
                    "keyDown", static_cast<unsigned int>(wParam), flags, message, lParam);
            }
            return 0;
        }
        case WM_SYSKEYUP: {
            if (m_lastAcceleratorKey == static_cast<UINT>(wParam)) {
                m_lastAcceleratorKey = 0;
                return 0;
            }
            unsigned int flags = 0;
            if (HIWORD(lParam) & KF_REPEAT)
                flags |= MINI_ELECTRON_REPEAT;
            if (HIWORD(lParam) & KF_EXTENDED)
                flags |= MINI_ELECTRON_EXTENDED;
            if (WebContents* target = focusedWebContents()) {
                target->sendWindowsKeyEvent(
                    "keyUp", static_cast<unsigned int>(wParam), flags, message, lParam);
            }
            return 0;
        }

        case WM_PAINT:
            onPaintMessage(hWnd);
            break;

        case WM_SHOWWINDOW:
            // Keep the initial show:false painting contract for ready-to-show.
            // Subsequent native hide/show transitions control renderer work.
            if (m_state == WindowInited || wParam)
                setRendererVisible(wParam && !m_isMinimized);
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
            updateVSyncDisplay();
            publishScreenRects();
            updateImePosition();
            mate::EventEmitter<BrowserWindow>::emit("move");
            break;

        case WM_DISPLAYCHANGE:
            display::win::ScreenWin::UpdateDisplayInfos();
            updateVSyncDisplay(true);
            publishScreenRects();
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
            const bool wasMinimized = m_isMinimized;
            if (SIZE_MINIMIZED == wParam) {
                m_isMinimized = true;
                setRendererVisible(false);
                if (WindowInited == m_state) {
                    mate::EventEmitter<BrowserWindow>::emit("resize");
                    if (!wasMinimized)
                        mate::EventEmitter<BrowserWindow>::emit("minimize");
                }
                invalidateCaptionButtons();
                return 0;
            }

            m_isMinimized = false;
            const int x = LOWORD(lParam);
            const int y = HIWORD(lParam);
            const bool becameMaximized = SIZE_MAXIMIZED == wParam && !m_isMaximized;
            const bool becameUnmaximized = SIZE_RESTORED == wParam && m_isMaximized;
            if (SIZE_MAXIMIZED == wParam)
                m_isMaximized = true;
            else if (SIZE_RESTORED == wParam)
                m_isMaximized = false;

            const gfx::Size viewportSize = display::win::ScreenWin::ScreenToDIPSize(
                m_hWnd, gfx::Size(x, y));
            m_webContents->resize(viewportSize.width(), viewportSize.height(),
                display::win::ScreenWin::GetScaleFactorForHWND(hWnd));
            publishScreenRects();

            ::EnterCriticalSection(&m_memoryCanvasLock);
            m_contentsSize.cx = x;
            m_contentsSize.cy = y;
            ::LeaveCriticalSection(&m_memoryCanvasLock);

            if (m_state == WindowInited || wasMinimized)
                setRendererVisible(!!::IsWindowVisible(hWnd));
            invalidateCaptionButtons();
            if (wasMinimized && ::IsWindow(hWnd)) {
                // A minimized window has no drawable viewport. Present the
                // retained renderer snapshots synchronously when its real
                // client returns instead of exposing the DWM clear surface
                // until the renderer produces another frame.
                ::InvalidateRect(hWnd, nullptr, FALSE);
                ::UpdateWindow(hWnd);
            }

            if (WindowInited == m_state)
                mate::EventEmitter<BrowserWindow>::emit("resize");
            if (becameMaximized)
                mate::EventEmitter<BrowserWindow>::emit("maximize");
            if (becameUnmaximized)
                mate::EventEmitter<BrowserWindow>::emit("unmaximize");
            if (wasMinimized)
                mate::EventEmitter<BrowserWindow>::emit("restore");
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



            unsigned int virtualKeyCode = wParam;
            unsigned int flags = 0;
            if (HIWORD(lParam) & KF_REPEAT)
                flags |= MINI_ELECTRON_REPEAT;
            if (HIWORD(lParam) & KF_EXTENDED)
                flags |= MINI_ELECTRON_EXTENDED;

            if (WebContents* target = focusedWebContents()) {
                target->sendWindowsKeyEvent(
                    "keyDown", virtualKeyCode, flags, message, lParam);
            }

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

            if (WebContents* target = focusedWebContents()) {
                target->sendWindowsKeyEvent(
                    "keyUp", virtualKeyCode, flags, message, lParam);
            }

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

            if (WebContents* target = focusedWebContents()) {
                target->sendWindowsKeyEvent(
                    "char", charCode, flags, message, lParam);
            }
            return 0;
            break;
        }
        case WM_IME_COMPOSITION:
            return onImeComposition(lParam);
        case WM_IME_ENDCOMPOSITION:
            return onImeEndComposition();
        case WM_IME_CHAR:
            // GCS_RESULTSTR is committed directly above. Letting DefWindowProc
            // translate WM_IME_CHAR would produce a duplicate WM_CHAR.
            return 0;
        case WM_IME_SETCONTEXT:
            // Blink renders preedit text. Keep the native candidate UI, but
            // suppress the legacy OS composition window.
            lParam &= ~ISC_SHOWUICOMPOSITIONWINDOW;
            return ::DefWindowProcW(hWnd, message, wParam, lParam);
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lParam) != hWnd) {
                m_isMouseDown = false;
                ::PostMessageW(hWnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hWnd), MAKELPARAM(HTCLIENT, 0));
            }
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
            onMouseMessage(hWnd, message, wParam, lParam);
            break;
        case WM_CONTEXTMENU: {
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            const gfx::PointF screenPoint = display::win::ScreenWin::ScreenToDIPPoint(
                gfx::PointF(pt.x, pt.y));

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

            m_webContents->sendWindowsMouseEvent(
                WM_CONTEXTMENU, pt.x, pt.y, screenPoint.x(), screenPoint.y(), flags);
            break;
        }
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            if (m_isIgnoreMouseEvents)
                break;
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            const gfx::PointF screenPoint = display::win::ScreenWin::ScreenToDIPPoint(
                gfx::PointF(pt.x, pt.y));
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
                    message, targetX, targetY, screenPoint.x(), screenPoint.y(), flags, delta);
            }
            break;
        }
        case WM_MOUSEACTIVATE:
            if (m_createWindowParam && !m_createWindowParam->isFocusable)
                return MA_NOACTIVATE;
            break;

        case WM_SETFOCUS:
            mate::EventEmitter<BrowserWindow>::emit("focus");
            if (WebContents* target = focusedWebContents())
                target->setFocus(true);
            return 0;

        case WM_KILLFOCUS: {
            m_lastAcceleratorKey = 0;
            mate::EventEmitter<BrowserWindow>::emit("blur");
            hideAutoMenuBar();
            WebContents* target = focusedWebContents();
            stopImeComposition(true);
            clearTextInputState();
            if (target)
                target->setFocus(false);
            return 0;
        }

        case WM_SETCURSOR: {
            const bool rendererWakeup = HIWORD(lParam) == 0;
            if (LOWORD(lParam) != HTCLIENT)
                break;
            if (m_isIgnoreMouseEvents || !::IsWindowVisible(hWnd)
                || ::IsIconic(hWnd) || !::IsWindowEnabled(hWnd)) {
                if (rendererWakeup)
                    return TRUE;
                break;
            }
            // Renderer wakeups carry HTCLIENT but may arrive after the pointer
            // entered another window or native chrome. Do not let a stale
            // wakeup apply either a page cursor or DefWindowProc's class cursor.
            POINT point;
            if (!::GetCursorPos(&point) || ::WindowFromPoint(point) != hWnd)
                return rendererWakeup ? TRUE : FALSE;
            if (rendererWakeup && !m_isMouseDown) {
                const LRESULT hit = ::SendMessageW(hWnd, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
                if (hit != HTCLIENT)
                    return ::DefWindowProcW(hWnd, message, wParam, MAKELPARAM(hit, 0));
            }
            ::ScreenToClient(hWnd, &point);
            if (captionButtonFromPoint(point) != CaptionButtonNone) {
                ::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
                return TRUE;
            }
            if (applyContentsCursor(lParam))
                return TRUE;
            break;
        }
        case WM_IME_STARTCOMPOSITION:
            beginImeComposition();
            return 0;
        case WM_NCLBUTTONDOWN: {
            if (HTCAPTION == wParam && !m_createWindowParam->isMovable) {
                return 0;
            }
        }
            break;

        case WM_ACTIVATE:
            // Extending the frame again during activation discards DWM's
            // retained client presentation for opaque frameless windows.
            // Creation and WM_DWMCOMPOSITIONCHANGED establish the margins.
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
            updateVSyncDisplay();
            for (BrowserView* view : m_browserViews) {
                if (view)
                    view->onParentScaleFactorChanged();
            }
            publishScreenRects();
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
        if (m_createWindowParam && m_createWindowParam->isFocusable)
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
        const bool wasVisible = !!::IsWindowVisible(m_hWnd);
        const bool focusable = m_createWindowParam && m_createWindowParam->isFocusable;
        ::ShowWindow(m_hWnd, focusable ? SW_SHOW : SW_SHOWNOACTIVATE);
        if (focusable)
            ::SetFocus(m_hWnd);
        if (!wasVisible) {
            ::InvalidateRect(m_hWnd, nullptr, FALSE);
            ::UpdateWindow(m_hWnd);
        }
    }

    void showInactiveApi()
    {
        const bool wasVisible = !!::IsWindowVisible(m_hWnd);
        ::ShowWindow(m_hWnd, SW_SHOWNOACTIVATE);
        // Static transparent content may have finished painting while hidden.
        // Publish the retained frame instead of waiting for another animation.
        if (!wasVisible) {
            ::InvalidateRect(m_hWnd, nullptr, FALSE);
            ::UpdateWindow(m_hWnd);
        }
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

    void notifyRendererFullScreenState()
    {
        if (!m_webContents)
            return;
        base::Value::Dict state;
        state.Set("enabled", m_isFullScreen);
        WebContents::sendRendererMessage(
            m_webContents->getIdApi(), "fullscreen-state", std::move(state));
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
                SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
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
        notifyRendererFullScreenState();
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
        if (browserView != m_foucsBrowserView)
            browserView->getWebContents()->setFocus(false);
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
            if (m_dropTarget)
                m_dropTarget->browserViewWillDetach(browserView);
            if (m_foucsBrowserView == browserView)
                setFocusedBrowserView(nullptr);
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

    static BrowserWindow* windowFromValue(v8::Local<v8::Value> value)
    {
        if (!value->IsObject())
            return nullptr;
        return static_cast<BrowserWindow*>(GetNativePtr(value.As<v8::Object>(), &kWrapperInfo));
    }

    void setParentWindowApi(v8::Local<v8::Value> value)
    {
        BrowserWindow* parent = value->IsNull() ? nullptr : windowFromValue(value);
        if (!value->IsNull() && (!parent || parent->isClosed())) {
            isolate()->ThrowException(v8::Exception::TypeError(
                v8::String::NewFromUtf8Literal(isolate(), "Must pass BrowserWindow instance or null")));
            return;
        }
        isolate()->ThrowException(v8::Exception::Error(
            v8::String::NewFromUtf8Literal(isolate(), "Window parenting is not supported")));
    }

    v8::Local<v8::Value> getParentWindowApi()
    {
        HWND owner = ::GetWindow(m_hWnd, GW_OWNER);
        BrowserWindow* parent = static_cast<BrowserWindow*>(::GetPropW(owner, kPropW));
        if (!parent || parent->isClosed())
            return v8::Null(isolate());
        return parent->getWrapper();
    }

    v8::Local<v8::Array> getChildWindowsApi()
    {
        v8::Local<v8::Array> children = v8::Array::New(isolate());
        uint32_t index = 0;
        for (auto* window : *WindowList::getInstance()) {
            if (!window->isClosed() && ::GetWindow(window->getHWND(), GW_OWNER) == m_hWnd)
                children->CreateDataProperty(isolate()->GetCurrentContext(), index++, window->getWrapper()).Check();
        }
        return children;
    }

    bool isModalApi() const
    {
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
        ::SetWindowPos(m_hWnd, nullptr, pixelBounds.x(), pixelBounds.y(),
            pixelBounds.width(), pixelBounds.height(), SWP_NOZORDER | SWP_NOACTIVATE);
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

    v8::Local<v8::Object> getContentBoundsApi()
    {
        RECT client = {};
        ::GetClientRect(m_hWnd, &client);
        ::MapWindowPoints(m_hWnd, nullptr, reinterpret_cast<POINT*>(&client), 2);
        const gfx::Rect bounds = display::win::ScreenWin::ScreenToDIPRect(m_hWnd, gfx::Rect(client));
        auto result = v8::Object::New(isolate());
        gin_helper::Dictionary dictionary(isolate(), result);
        dictionary.Set("x", bounds.x());
        dictionary.Set("y", bounds.y());
        dictionary.Set("width", bounds.width());
        dictionary.Set("height", bounds.height());
        return result;
    }

    void setContentBoundsApi(v8::Local<v8::Object> value)
    {
        gin_helper::Dictionary current(isolate(), getContentBoundsApi());
        gin_helper::Dictionary requested(isolate(), value);
        int x, y, width, height;
        current.Get("x", &x);
        current.Get("y", &y);
        current.Get("width", &width);
        current.Get("height", &height);
        requested.Get("x", &x);
        requested.Get("y", &y);
        requested.Get("width", &width);
        requested.Get("height", &height);
        const gfx::Rect pixels = display::win::ScreenWin::DIPToScreenRect(
            nullptr, gfx::Rect(x, y, width, height));
        RECT window = {}, client = {};
        ::GetWindowRect(m_hWnd, &window);
        ::GetClientRect(m_hWnd, &client);
        ::MapWindowPoints(m_hWnd, nullptr, reinterpret_cast<POINT*>(&client), 2);
        ::SetWindowPos(m_hWnd, nullptr,
            pixels.x() - (client.left - window.left),
            pixels.y() - (client.top - window.top),
            pixels.width() + (window.right - window.left) - (client.right - client.left),
            pixels.height() + (window.bottom - window.top) - (client.bottom - client.top),
            SWP_NOZORDER | SWP_NOACTIVATE);
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
        ::SetWindowPos(m_hWnd, b ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
        rect.left = (screenX - (rect.right - rect.left)) / 2;
        rect.top = (screenY - (rect.bottom - rect.top)) / 2;

        //设置窗体位置
        ::SetWindowPos(m_hWnd, nullptr, rect.left, rect.top, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void setPositionApi(int x, int y)
    {
        const gfx::Point position =
            display::win::ScreenWin::DIPToScreenPoint(gfx::Point(x, y));
        ::SetWindowPos(m_hWnd, nullptr, position.x(), position.y(), 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    std::vector<int> getPositionApi()
    {
        RECT rect = { 0 };
        ::GetWindowRect(m_hWnd, &rect);
        const gfx::Rect bounds =
            display::win::ScreenWin::ScreenToDIPRect(m_hWnd, gfx::Rect(rect));
        return { bounds.x(), bounds.y() };
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

    void updateTaskbarVisibility()
    {
        if (!m_createWindowParam || !::IsWindow(m_hWnd))
            return;

        const bool skip = m_createWindowParam->skipTaskbar
            || !m_createWindowParam->isFocusable;
        LONG_PTR style = ::GetWindowLongPtrW(m_hWnd, GWL_EXSTYLE);
        if (skip) {
            style |= WS_EX_TOOLWINDOW;
            style &= ~WS_EX_APPWINDOW;
        } else {
            style &= ~WS_EX_TOOLWINDOW;
            style |= WS_EX_APPWINDOW;
        }
        ::SetWindowLongPtrW(m_hWnd, GWL_EXSTYLE, style);
        m_createWindowParam->styleEx = static_cast<unsigned>(style);
        if (m_isFullScreen)
            m_fullScreenExStyle = static_cast<DWORD>(style);
        ::SetWindowPos(m_hWnd, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE
            | SWP_NOZORDER | SWP_NOACTIVATE);

        ITaskbarList* taskbar = nullptr;
        if (SUCCEEDED(::CoCreateInstance(CLSID_TaskbarList, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar)))) {
            if (SUCCEEDED(taskbar->HrInit())) {
                if (skip)
                    taskbar->DeleteTab(m_hWnd);
                else
                    taskbar->AddTab(m_hWnd);
            }
            taskbar->Release();
        }
    }

    void setSkipTaskbarApi(bool skip)
    {
        m_createWindowParam->skipTaskbar = skip;
        updateTaskbarVisibility();
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

    void setFocusableApi(bool focusable)
    {
        if (!m_createWindowParam || m_createWindowParam->isFocusable == focusable)
            return;
        m_createWindowParam->isFocusable = focusable;

        LONG_PTR style = ::GetWindowLongPtrW(m_hWnd, GWL_EXSTYLE);
        if (focusable)
            style &= ~WS_EX_NOACTIVATE;
        else
            style |= WS_EX_NOACTIVATE;
        ::SetWindowLongPtrW(m_hWnd, GWL_EXSTYLE, style);
        m_createWindowParam->styleEx = static_cast<unsigned>(style);
        updateTaskbarVisibility();

        if (!focusable && ::GetFocus() == m_hWnd) {
            ::SetFocus(nullptr);
            if (m_focusedWebContents)
                m_focusedWebContents->setFocus(false);
        }
    }

    void focusOnWebViewApi()
    {
        setFocusedBrowserView(nullptr);
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
        return m_state == WindowDestroyed;
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

    void setAspectRatioApi(const v8::FunctionCallbackInfo<v8::Value>& info)
    {
        double ratio;
        if (info.Length() < 1 || !info[0]->IsNumber()
            || !info[0]->NumberValue(isolate()->GetCurrentContext()).To(&ratio)
            || !std::isfinite(ratio) || ratio < 0) {
            isolate()->ThrowException(v8::Exception::TypeError(
                v8::String::NewFromUtf8Literal(isolate(), "Aspect ratio must be a finite non-negative number")));
            return;
        }
        int width = 0, height = 0;
        if (info.Length() > 1 && !info[1]->IsUndefined()) {
            if (!info[1]->IsObject()) {
                isolate()->ThrowException(v8::Exception::TypeError(
                    v8::String::NewFromUtf8Literal(isolate(), "extraSize must be a size object")));
                return;
            }
            gin_helper::Dictionary extra(isolate(), info[1].As<v8::Object>());
            if (!extra.Get("width", &width) || !extra.Get("height", &height) || width < 0 || height < 0) {
                isolate()->ThrowException(v8::Exception::TypeError(
                    v8::String::NewFromUtf8Literal(isolate(), "extraSize requires non-negative width and height")));
                return;
            }
        }
        isolate()->ThrowException(v8::Exception::Error(
            v8::String::NewFromUtf8Literal(isolate(), "Aspect-ratio constraints are not supported")));
    }

    void setKioskApi(bool)
    {
        isolate()->ThrowException(v8::Exception::Error(
            v8::String::NewFromUtf8Literal(isolate(), "Kiosk mode is not supported")));
    }

    bool isKioskApi() const { return false; }

    void setHasShadowApi(bool shadow)
    {
        DWMNCRENDERINGPOLICY policy = shadow ? DWMNCRP_ENABLED : DWMNCRP_DISABLED;
        HRESULT result = ::DwmSetWindowAttribute(m_hWnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
        if (FAILED(result)) {
            isolate()->ThrowException(v8::Exception::Error(
                v8::String::NewFromUtf8Literal(isolate(), "Failed to change window shadow")));
            return;
        }
        m_hasShadow = shadow;
        updateWindowFrame(m_hWnd);
    }

    bool hasShadowApi() const { return m_hasShadow; }

    // Electron 34 NativeWindow's non-Cocoa defaults: these macOS-only
    // facilities have no Windows presentation. Keep separate typed methods,
    // not a catch-all stub (native_window.h/.cc).
    void previewFileApi(const std::string& path) {}
    void closeFilePreviewApi() {}
    void setRepresentedFilenameApi(const std::string& filename) {}
    std::string getRepresentedFilenameApi() const { return std::string(); }
    void setSheetOffsetApi(double offsetY) {}
    void setVibrancyApi(v8::Local<v8::Value> type) {}

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
        v8::Local<v8::Value> parentValue;
        if (options->Get("parent", &parentValue) && !parentValue->IsNullOrUndefined()) {
            BrowserWindow* parent = windowFromValue(parentValue);
            if (!parent || parent->isClosed() || !::IsWindow(parent->m_hWnd)) {
                options->isolate()->ThrowException(v8::Exception::TypeError(
                    v8::String::NewFromUtf8Literal(options->isolate(),
                        "parent must be a live BrowserWindow")));
                return nullptr;
            }
            options->isolate()->ThrowException(v8::Exception::Error(
                v8::String::NewFromUtf8Literal(options->isolate(), "Window parenting is not supported")));
            return nullptr;
        }
        bool modal = false, kiosk = false;
        options->Get("modal", &modal);
        options->Get("kiosk", &kiosk);
        if (modal || kiosk) {
            options->isolate()->ThrowException(v8::Exception::Error(
                v8::String::NewFromUtf8Literal(options->isolate(), "Modal and kiosk windows are not supported")));
            return nullptr;
        }
        v8::Local<v8::Value> existingContents;
        if (options->Get("webContents", &existingContents) && !existingContents->IsUndefined()) {
            WebContents* contents = existingContents->IsObject()
                ? static_cast<WebContents*>(GetNativePtr(
                    existingContents.As<v8::Object>(), &WebContents::kWrapperInfo))
                : nullptr;
            if (!contents || contents->isDestroyedApi() || contents->getOwner()) {
                options->isolate()->ThrowException(v8::Exception::TypeError(
                    v8::String::NewFromUtf8Literal(options->isolate(),
                        "webContents must be a live, unattached WebContents")));
            } else {
                options->isolate()->ThrowException(v8::Exception::Error(
                    v8::String::NewFromUtf8Literal(options->isolate(),
                        "Adopting an existing WebContents is not supported")));
            }
            return nullptr;
        }
        BrowserWindow* self = new BrowserWindow(options->isolate(), wrapper);

        WebContents::BrowserWindowConstructorOptions* createWindowParam = new WebContents::BrowserWindowConstructorOptions();
        createWindowParam->styles = 0;
        createWindowParam->styleEx = 0;
        createWindowParam->transparent = false;

        WebContents* webContents = nullptr;

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
        options->GetBydefaultVal(options::kFocusable, true, &createWindowParam->isFocusable);
        options->GetBydefaultVal(options::kSkipTaskbar, false, &createWindowParam->skipTaskbar);
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

        if (!createWindowParam->isFocusable)
            createWindowParam->styleEx |= WS_EX_NOACTIVATE;
        if (createWindowParam->skipTaskbar || !createWindowParam->isFocusable) {
            createWindowParam->styleEx |= WS_EX_TOOLWINDOW;
            createWindowParam->styleEx &= ~WS_EX_APPWINDOW;
        }

        if (createWindowParam->isMinimizable)
            createWindowParam->styles |= WS_MINIMIZEBOX;
        if (createWindowParam->isMaximizable)
            createWindowParam->styles |= WS_MAXIMIZEBOX;

        if (createWindowParam->isResizable && (createWindowParam->isFrame || !createWindowParam->transparent))
            createWindowParam->styles |= WS_THICKFRAME;

        if (createWindowParam->isShow)
            createWindowParam->styles |= WS_VISIBLE;

        webContents = WebContents::create(options->isolate(), webPreferences, self);
        if (!webContents || webContents->isDestroyedApi()) {
            delete createWindowParam;
            self->m_state = WindowDestroyed;
            if (!options->isolate()->HasPendingException()) {
                options->isolate()->ThrowException(v8::Exception::Error(
                    v8::String::NewFromUtf8Literal(
                        options->isolate(), "WebContents was destroyed during BrowserWindow creation")));
            }
            return nullptr;
        }
        self->m_webContents = webContents;
        self->m_focusedWebContents = webContents;
        self->m_webContents->addObserver(self);
        self->newWindowTaskInUiThread(createWindowParam);
        bool shadow = true;
        if (options->Get("hasShadow", &shadow))
            self->setHasShadowApi(shadow);

        return self;
    }


    void newWindowTaskInUiThread(WebContents::BrowserWindowConstructorOptions* createWindowParam)
    {
        m_createWindowParam = createWindowParam;
        m_webContents->setCreateWindowParam(createWindowParam);
        const base::CommandLine* commandLine = base::CommandLine::ForCurrentProcess();
        if (!commandLine->HasSwitch("disable-gpu")
            && !commandLine->HasSwitch("disable-gpu-compositing")
            && mini_electron::win::CanUseD3DPresentation()) {
            createWindowParam->styleEx |= WS_EX_NOREDIRECTIONBITMAP;
            createWindowParam->styleEx &= ~WS_EX_LAYERED;
        }


        m_hWnd = ::CreateWindowEx(createWindowParam->styleEx, kElectronClassName, createWindowParam->title.c_str(), createWindowParam->styles,
            createWindowParam->x, createWindowParam->y, createWindowParam->width, createWindowParam->height, NULL, NULL, ::GetModuleHandleW(NULL), this);

        if (!::IsWindow(m_hWnd))
            return;
        // Only associate the native IME while Blink has an editable text client.
        ::ImmAssociateContextEx(m_hWnd, nullptr, 0);

        registerNativeDropTarget();

        HWND dwFlag = HWND_NOTOPMOST;
        if (createWindowParam->isAlwaysOnTop)
            dwFlag = HWND_TOPMOST;
        // Z-order/frame setup is not an activation request, even while hidden.
        // Presentation below applies the requested show/focus policy.
        ::SetWindowPos(m_hWnd, dwFlag, 0, 0, 0, 0,
            SWP_NOSIZE | SWP_NOMOVE | SWP_NOREPOSITION | SWP_FRAMECHANGED | SWP_NOACTIVATE);
        updateWindowFrame(m_hWnd);


        if (!createWindowParam->isClosable)
            ::EnableMenuItem(::GetSystemMenu(m_hWnd, false), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);

        ::GetClientRect(m_hWnd, &m_clientRect);

        if (createWindowParam->isCenter)
            centerApi();

        int width = m_clientRect.right - m_clientRect.left;
        int height = m_clientRect.bottom - m_clientRect.top;

        HWND hWnd = m_hWnd;
        const gfx::Size viewportSize = display::win::ScreenWin::ScreenToDIPSize(
            hWnd, gfx::Size(width, height));
        m_webContents->resize(viewportSize.width(), viewportSize.height(),
            display::win::ScreenWin::GetScaleFactorForHWND(hWnd));
        updateVSyncDisplay();
        publishScreenRects();
        rebuildDraggableRegion();
        m_webContents->setFocus(createWindowParam->isFocusable);
        //setIcon(m_hWnd);

        MenuEventNotif::onWindowDidCreated(this);

        ::ShowWindow(m_hWnd, !createWindowParam->isShow ? SW_HIDE
            : createWindowParam->isFocusable ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
        ::UpdateWindow(m_hWnd);

        m_state = WindowInited;
    }

    virtual void onWebContentsCreated(WebContents* contents) override
    {
    }

    virtual void onWebContentsDeleted(WebContents* contents) override
    {
        const bool was_focused = contents == m_focusedWebContents;
        if (contents == m_textInputContents || contents == m_imeTarget) {
            stopImeComposition(false);
            clearTextInputState();
        }
        if (!was_focused)
            return;
        if (contents != m_webContents)
            m_foucsBrowserView = nullptr;
        m_focusedWebContents = contents == m_webContents ? nullptr : m_webContents;
        if (m_focusedWebContents && m_state == WindowInited
            && ::GetFocus() == m_hWnd) {
            m_focusedWebContents->setFocus(true);
        }
    }

    virtual void onWebContentsReadyToShow(WebContents* contents) override
    {
        if (contents == m_webContents)
            mate::EventEmitter<BrowserWindow>::emit("ready-to-show");
    }

    void onWebContentsTextInputState(
        WebContents* contents, const base::Value::Dict& state) override
    {
        if (!contents || contents != m_focusedWebContents
            || ::GetFocus() != m_hWnd) {
            return;
        }
        const std::optional<bool> editable = state.FindBool("editable");
        if (state.empty() || !editable || !*editable) {
            stopImeComposition(false);
            clearTextInputState();
            return;
        }
        const std::string* serial_value = state.FindString("imeSerial");
        uint64_t serial = 0;
        if (!serial_value
            || !base::StringToUint64(*serial_value, &serial)) {
            stopImeComposition(false);
            clearTextInputState();
            return;
        }
        if (m_pendingImeContents == contents && serial < m_pendingImeSerial)
            return;

        const std::optional<int> selection_start = state.FindInt("selectionStart");
        const std::optional<int> selection_length = state.FindInt("selectionLength");
        const std::optional<int> marked_start = state.FindInt("markedStart");
        const std::optional<int> marked_length = state.FindInt("markedLength");
        const std::optional<double> scale = state.FindDouble("scale");
        RECT caret;
        if (!selection_start || *selection_start < 0
            || !selection_length || *selection_length < 0
            || !marked_start || *marked_start < -1
            || !marked_length || *marked_length < 0
            || !scale || !std::isfinite(*scale) || *scale <= 0
            || !parseTextInputRect(state, "selectionRect", &caret)) {
            stopImeComposition(false);
            clearTextInputState();
            return;
        }

        m_pendingImeContents = nullptr;
        m_pendingImeSerial = 0;
        m_textInputContents = contents;
        m_textInputCaret = caret;
        if (!m_textInputEditable)
            ::ImmAssociateContextEx(m_hWnd, nullptr, IACE_DEFAULT);
        m_textInputEditable = true;
        m_rendererCompositionActive = *marked_start >= 0 && *marked_length > 0;
        if (m_nativeImeActive && m_imeTarget == contents)
            updateImePosition();
    }

    static void newFunction(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        v8::Isolate* isolate = args.GetIsolate();

        if (args.IsConstructCall()) {
            if (args.Length() > 1)
                return;

            v8::Local<v8::Object> optionsObject;
            if (args.Length() && !args[0]->IsUndefined()) {
                if (!args[0]->ToObject(isolate->GetCurrentContext()).ToLocal(&optionsObject))
                    return;
            } else {
                optionsObject = v8::Object::New(isolate);
            }
            gin_helper::Dictionary options(isolate, optionsObject);
            BrowserWindow* self = newWindow(&options, args.This());
            if (!self)
                return;
            WindowList::getInstance()->addWindow(self);
            if (self->m_webContents && self->m_state == WindowInited
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

    WebContents* m_focusedWebContents;
    WebContents* m_textInputContents;
    WebContents* m_pendingImeContents;
    WebContents* m_imeTarget;
    RECT m_textInputCaret;
    bool m_textInputEditable;
    bool m_nativeImeActive;
    bool m_rendererCompositionActive;
    bool m_imeCleanup;
    bool m_ownsImeCaret;
    int m_imeCaretHeight;
    uint64_t m_imeSerial;
    uint64_t m_pendingImeSerial;

    mini_electron::win::NativeCursor m_nativeCursor;
    CRITICAL_SECTION m_memoryCanvasLock;
    HBITMAP m_memoryBMP;
    HDC m_memoryDC;
    RECT m_clientRect;
    SIZE m_memoryBmpSize;
    HDC m_captionDC;
    HBITMAP m_captionBitmap;
    SIZE m_captionBitmapSize;
    sk_sp<SkSurface> m_captionSurface;
    std::shared_ptr<mini_electron::win::DCompPresenter> m_gpuPresenter;
    std::vector<mini_electron::win::DCompLayer> m_gpuLayers;
    bool m_gpuPresenting = false;
    bool m_gpuCaptionDirty = true;
    uint64_t m_gpuCaptionGeneration = 1;
    HMONITOR m_vsyncMonitor = nullptr;

    HRGN m_draggableRegion;
    struct DraggableRegion {
        gfx::Rect bounds;
        bool draggable;
    };
    std::vector<DraggableRegion> m_draggableRegions;

    bool m_isMaximized;
    bool m_isMinimized;
    bool m_isFullScreen;
    bool m_hasShadow = true;
    bool m_isDocumentEdited;
    bool m_isIgnoreMouseEvents;
    bool m_isMouseDown;

    bool m_titleBarOverlayEnabled;
    int m_titleBarOverlayHeight;
    SkColor m_titleBarOverlayColor;
    SkColor m_titleBarOverlaySymbolColor;
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
    BrowserWindowDropTarget* m_dropTarget;

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

BrowserWindowDropTarget::BrowserWindowDropTarget(BrowserWindow* owner)
    : owner_(owner)
    , references_(1)
    , tracking_(false)
    , destination_selected_(false)
    , destination_active_(false)
    , destination_is_main_(false)
    , destination_view_(nullptr)
    , source_allows_copy_(false)
    , last_point_({ 0, 0 })
    , last_key_state_(0)
{
}

HRESULT STDMETHODCALLTYPE BrowserWindowDropTarget::QueryInterface(
    REFIID iid, void** object)
{
    if (!object)
        return E_POINTER;
    *object = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDropTarget)) {
        *object = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE BrowserWindowDropTarget::AddRef()
{
    return static_cast<ULONG>(::InterlockedIncrement(&references_));
}

ULONG STDMETHODCALLTYPE BrowserWindowDropTarget::Release()
{
    const LONG references = ::InterlockedDecrement(&references_);
    if (!references)
        delete this;
    return static_cast<ULONG>(references);
}

bool BrowserWindowDropTarget::readPaths(
    IDataObject* data_object, std::vector<std::string>* paths)
{
    if (!data_object || !paths)
        return false;
    paths->clear();
    FORMATETC format = {
        static_cast<CLIPFORMAT>(CF_HDROP), nullptr, DVASPECT_CONTENT, -1,
        TYMED_HGLOBAL
    };
    if (FAILED(data_object->QueryGetData(&format)))
        return false;
    STGMEDIUM medium = {};
    if (FAILED(data_object->GetData(&format, &medium)))
        return false;

    bool valid = false;
    if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal) {
        const HDROP drop = reinterpret_cast<HDROP>(medium.hGlobal);
        const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (count > 0 && count <= 1024) {
            std::vector<std::string> extracted;
            extracted.reserve(count);
            valid = true;
            for (UINT index = 0; index < count; ++index) {
                const UINT length =
                    ::DragQueryFileW(drop, index, nullptr, 0);
                if (!length || length > 4096) {
                    valid = false;
                    break;
                }
                std::vector<wchar_t> buffer(
                    static_cast<size_t>(length) + 1, L'\0');
                const UINT copied = ::DragQueryFileW(
                    drop, index, buffer.data(), length + 1);
                if (copied != length) {
                    valid = false;
                    break;
                }
                std::string path = base::WideToUTF8(
                    std::wstring(buffer.data(), copied));
                if (path.empty() || path.size() > 4096) {
                    valid = false;
                    break;
                }
                extracted.push_back(std::move(path));
            }
            if (valid)
                *paths = std::move(extracted);
        }
    }
    ::ReleaseStgMedium(&medium);
    return valid && !paths->empty();
}

WebContents* BrowserWindowDropTarget::currentContents() const
{
    if (!owner_ || !destination_selected_)
        return nullptr;
    return owner_->nativeDragContents(
        destination_is_main_ ? nullptr : destination_view_);
}

void BrowserWindowDropTarget::leaveDestination(
    POINTL point, DWORD key_state)
{
    WebContents* contents = currentContents();
    if (destination_active_ && contents) {
        base::Value::Dict event;
        if (owner_->buildNativeDragEvent("dragLeave", point, key_state,
                destination_is_main_ ? nullptr : destination_view_, &event)) {
            contents->sendNativeFileDrag(std::move(event));
        }
    }
    destination_selected_ = false;
    destination_active_ = false;
    destination_is_main_ = false;
    destination_view_ = nullptr;
}

bool BrowserWindowDropTarget::updateDestination(
    POINTL point, DWORD key_state)
{
    if (!owner_ || !tracking_)
        return false;
    BrowserView* view = nullptr;
    WebContents* contents = nullptr;
    if (!owner_->nativeDragDestinationAt(point, &view, &contents)) {
        leaveDestination(point, key_state);
        return false;
    }
    const bool is_main = view == nullptr;
    const bool same_destination = destination_selected_
        && destination_is_main_ == is_main
        && (is_main || destination_view_ == view);
    if (same_destination) {
        if (!destination_active_)
            return false;
        base::Value::Dict event;
        if (!owner_->buildNativeDragEvent(
                "dragOver", point, key_state, view, &event)) {
            return false;
        }
        return contents->sendNativeFileDrag(std::move(event));
    }

    leaveDestination(point, key_state);
    destination_selected_ = true;
    destination_is_main_ = is_main;
    destination_view_ = view;
    base::Value::Dict event;
    if (!owner_->buildNativeDragEvent(
            "dragEnter", point, key_state, view, &event)) {
        return false;
    }
    destination_active_ =
        contents->sendNativeFileDrag(std::move(event), paths_);
    return destination_active_;
}

void BrowserWindowDropTarget::resetDrag()
{
    tracking_ = false;
    destination_selected_ = false;
    destination_active_ = false;
    destination_is_main_ = false;
    destination_view_ = nullptr;
    source_allows_copy_ = false;
    last_point_ = { 0, 0 };
    last_key_state_ = 0;
    paths_.clear();
}

void BrowserWindowDropTarget::cancelDrag()
{
    if (owner_ && destination_selected_)
        leaveDestination(last_point_, last_key_state_);
    resetDrag();
}

void BrowserWindowDropTarget::browserViewWillDetach(BrowserView* view)
{
    if (!owner_ || !destination_selected_ || destination_is_main_
        || destination_view_ != view) {
        return;
    }
    leaveDestination(last_point_, last_key_state_);
}

void BrowserWindowDropTarget::detachOwner()
{
    owner_ = nullptr;
    resetDrag();
}

HRESULT STDMETHODCALLTYPE BrowserWindowDropTarget::DragEnter(
    IDataObject* data_object, DWORD key_state, POINTL point, DWORD* effect)
{
    if (!effect)
        return E_INVALIDARG;
    const bool copy_allowed = (*effect & DROPEFFECT_COPY) != 0;
    cancelDrag();
    last_point_ = point;
    last_key_state_ = key_state;
    source_allows_copy_ = copy_allowed;
    if (!owner_ || !copy_allowed || !readPaths(data_object, &paths_)) {
        *effect = DROPEFFECT_NONE;
        resetDrag();
        return S_OK;
    }
    tracking_ = true;
    *effect = updateDestination(point, key_state)
        ? DROPEFFECT_COPY
        : DROPEFFECT_NONE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE BrowserWindowDropTarget::DragOver(
    DWORD key_state, POINTL point, DWORD* effect)
{
    if (!effect)
        return E_INVALIDARG;
    last_point_ = point;
    last_key_state_ = key_state;
    const bool accepted = owner_ && tracking_ && source_allows_copy_
        && ((*effect & DROPEFFECT_COPY) != 0)
        && updateDestination(point, key_state);
    *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE BrowserWindowDropTarget::DragLeave()
{
    cancelDrag();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE BrowserWindowDropTarget::Drop(
    IDataObject* data_object, DWORD key_state, POINTL point, DWORD* effect)
{
    if (!effect)
        return E_INVALIDARG;
    (void)data_object;
    last_point_ = point;
    last_key_state_ = key_state;
    const bool routed = owner_ && tracking_ && source_allows_copy_
        && ((*effect & DROPEFFECT_COPY) != 0)
        && updateDestination(point, key_state);
    WebContents* contents = routed ? currentContents() : nullptr;
    bool sent = false;
    if (contents && contents->canPerformNativeFileDrop()) {
        base::Value::Dict event;
        if (owner_->buildNativeDragEvent("drop", point, key_state,
                destination_is_main_ ? nullptr : destination_view_, &event)) {
            sent = contents->sendNativeFileDrag(std::move(event));
        }
    }
    if (!sent)
        leaveDestination(point, key_state);
    resetDrag();
    *effect = sent ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}


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

    WebContents* content = self->focusedWebContents();
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
