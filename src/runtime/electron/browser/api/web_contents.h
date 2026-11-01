#ifndef BROWSER_API_WEB_CONTENTS_H_
#define BROWSER_API_WEB_CONTENTS_H_

#if defined(_WIN32)
#include <windows.h>
#endif

#include <memory>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>

#include "base/files/file_path.h"
#include "base/observer_list.h"
#include "build/build_config.h"
#include "base/values.h"
#include "src/runtime/electron/common/api/event_emitter.h"
#include "src/runtime/electron/common/gin_helper/dictionary.h"
#include "src/runtime/electron/common/gin_helper/public/wrapper_info.h"
#include "src/runtime/electron/common/renderer_client.h"
#include "ui/base/cursor/cursor.h"
#include "ui/gfx/geometry/rect.h"

namespace node {
class Environment;
}

#if BUILDFLAG(IS_WIN)
namespace mini_electron::win { struct DCompLayer; }
#endif

namespace atom {

static const int kNotSetXYFlag = 400;

class WebContents;
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
class DevToolsGateway;
#endif
class WindowInterface;
struct WindowOpenHandlerResult;

#if defined(_WIN32)
inline bool isRectEqual(const RECT& a, const RECT& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

inline bool isPointInRect(const RECT& a, const POINT& b)
{
    return b.x >= a.left && b.x <= a.right && b.y >= a.top && b.y <= a.bottom;
}
#endif

class WebContentsObserver {
public:
    virtual ~WebContentsObserver() = default;
    virtual void onWebContentsCreated(WebContents*) { }
    virtual void onWebContentsDeleted(WebContents*) { }
    virtual void onWebContentsReadyToShow(WebContents*) { }
    // An empty damage rectangle requests a full repaint.
    virtual void onWebContentsPaint(WebContents*, const gfx::Rect&) { }
    virtual void onWebContentsTextInputState(
        WebContents*, const base::Value::Dict&) { }
    virtual void onWebContentsCursorChanged(
        WebContents*, const ui::Cursor&) { }
    virtual bool supportsFramePreparation() const { return false; }
    // Returns the generation actually drawn into the hidden native backing.
    virtual uint64_t prepareWebContentsFrame(WebContents*, uint64_t) { return 0; }
    virtual void onWebContentsDraggableRegions(
        WebContents*, const base::Value::List&) { }
};

class WebContents : public mate::EventEmitter<WebContents> {
public:
    struct BrowserWindowConstructorOptions {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        unsigned styles = 0;
        unsigned styleEx = 0;
        bool transparent = false;
        std::wstring title;
        bool isShow = true;
        bool isCenter = false;
        bool isResizable = true;
        bool isMinimizable = true;
        bool isMaximizable = true;
        bool isFrame = true;
        bool isMovable = true;
        bool isUseContentSize = false;
        bool isAlwaysOnTop = false;
        bool isClosable = true;
        bool isFocusable = true;
        bool skipTaskbar = false;
        int minWidth = 100;
        int minHeight = 100;
        int maxWidth = 500;
        int maxHeight = 500;
        bool m_isNodeIntegration = false;
        bool m_isNodeIntegrationInSubframes = false;
        bool m_isContextIsolation = true;
        std::vector<std::string> m_customArgs;
        std::string m_iconPath;
    };

    static void init(v8::Isolate*, v8::Local<v8::Object>, node::Environment*);
    static WebContents* create(v8::Isolate*, gin_helper::Dictionary, WindowInterface* owner);
    static WebContents* fromId(int id);
    static bool sendRendererMessage(int contents_id, const std::string& method,
        base::Value::Dict params);
    // The packaged/default application directory that renderer file loads may
    // read. Set by the platform app implementation whenever its app path changes.
    static void setApplicationRoot(const std::string& path);

    WebContents(v8::Isolate*, v8::Local<v8::Object>, const gin_helper::Dictionary&);
    ~WebContents();

    void destroyed();
    void addObserver(WebContentsObserver*);
    void removeObserver(WebContentsObserver*);

    WindowInterface* getOwner() const { return m_owner; }
    WebContents* host() const { return m_host; }
    int getIdApi() const { return m_id; }
    const ui::Cursor& getCursor() const;
    // Object lifetime is independent of whether its renderer child is alive.
    bool isDestroyedApi() const;
    // Renderer-dependent operations must use this transport-liveness guard.
    bool isAlive() const;
    bool requestDownloadSource(std::string method, base::Value::Dict params,
        std::function<void(base::Value::Dict, std::string)> reply);

    void setCreateWindowParam(BrowserWindowConstructorOptions* options);
    void resize(int width, int height, double scale_factor = 1.0);
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    void setScreenRects(const gfx::Rect& widget_screen_rect,
        const gfx::Rect& window_screen_rect);
#endif
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
    void setVSyncDisplayId(int64_t display_id);
#endif
    void setFocus(bool focused);
    // Native/embedding visibility; renderer work may remain enabled by an
    // explicit backgroundThrottling:false preference or a first-frame lease.
    void setVisible(bool visible);
    bool sendInput(base::Value::Dict event);
    void sendWindowsMouseEvent(unsigned message, int x, int y,
        double screenX, double screenY, unsigned flags, int delta = 0);
    // Browser-native drag input. Native paths are canonicalized and replaced
    // with Session capabilities before crossing into the renderer.
    bool sendNativeFileDrag(base::Value::Dict event,
        const std::vector<std::string>& paths = {});
    bool canPerformNativeFileDrop() const
    {
        return m_native_drag_active && m_native_drag_operation_known
            && m_native_drag_operation_accepted;
    }
#if defined(_WIN32)
    void sendWindowsKeyEvent(const char* type, unsigned key_code, unsigned flags,
        unsigned message, LPARAM l_param);
#endif
    bool copyFrame(RendererFrameSnapshot* snapshot, bool include_native_pixels = false) const;
    void prepareToShowApi(const v8::FunctionCallbackInfo<v8::Value>&);
    void completePreparedFrames();
    void cancelPreparedFrames(const std::string& error);
#if defined(_WIN32)
    bool paintFrame(HDC target, int dest_x, int dest_y, int src_x, int src_y,
        int width, int height, const gfx::Size& target_size) const;
    void appendDCompLayers(std::vector<mini_electron::win::DCompLayer>& layers,
        const RECT& bounds, const RECT& clip, unsigned depth = 0) const;
    void getCompositorInfoApi(const v8::FunctionCallbackInfo<v8::Value>&);
#endif
    void closeRenderer();


    static v8::Persistent<v8::Function> s_constructor;
    static gin_helper::WrapperInfo kWrapperInfo;

private:
    static void newFunction(const v8::FunctionCallbackInfo<v8::Value>&);
    static void createGuestApi(const v8::FunctionCallbackInfo<v8::Value>&);
    static void getFocusedWebContentsApi(const v8::FunctionCallbackInfo<v8::Value>&);
    static void getAllWebContentsApi(const v8::FunctionCallbackInfo<v8::Value>&);
    static void fromIdApi(const v8::FunctionCallbackInfo<v8::Value>&);

    void getSessionApi(const v8::FunctionCallbackInfo<v8::Value>&) const;
    void authorizeFileSelectionApi(const v8::FunctionCallbackInfo<v8::Value>&);
    void getFrameApi(const v8::FunctionCallbackInfo<v8::Value>&) const;
    void requestApi(const v8::FunctionCallbackInfo<v8::Value>&);
    bool sendCommandApi(const v8::FunctionCallbackInfo<v8::Value>&);
    bool sendApi(const v8::FunctionCallbackInfo<v8::Value>&);
    void sendInputEventApi(const v8::FunctionCallbackInfo<v8::Value>&);
    void capturePageApi(const v8::FunctionCallbackInfo<v8::Value>&);
    void setWindowOpenHandlerApi(const v8::FunctionCallbackInfo<v8::Value>&);
    bool respondApi(const v8::FunctionCallbackInfo<v8::Value>&);
    bool attachGuestApi(const v8::FunctionCallbackInfo<v8::Value>&);
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
    void debuggerAttachApi(const v8::FunctionCallbackInfo<v8::Value>&);
    void debuggerDetachApi();
    bool debuggerIsAttachedApi() const;
    void debuggerSendCommandApi(const v8::FunctionCallbackInfo<v8::Value>&);
    std::string openDevToolsApi(const base::Value::Dict& options);
    void closeDevToolsApi();
    bool isDevToolsOpenedApi() const;
    bool inspectElementApi(int x, int y);
#endif
    void destroyApi();

    void loadURLApi(const std::string& url);
    void downloadURLApi(const std::string& url);
    std::string getURLApi() const { return m_url; }
    std::string getTitleApi() const { return m_title; }
    int getProcessIdApi() const;
    bool canGoBackApi() const { return m_canGoBack; }
    bool canGoForwardApi() const { return m_canGoForward; }
    bool isLoadingApi() const { return m_isLoading; }
    bool isLoadingMainFrameApi() const { return m_isLoading; }
    bool isWaitingForResponseApi() const { return m_isLoading; }
    bool isCrashedApi() const { return m_crashed; }
    bool isGuestApi() const { return m_type == "webview"; }
    bool isOffscreenApi() const { return true; }
    std::string getTypeApi() const { return m_type; }
    base::Value::Dict getWebPreferencesApi() const { return m_web_preferences.Clone(); }
    v8::Local<v8::Value> getOwnerBrowserWindowApi();

    void stopApi();
    void goBackApi();
    void goForwardApi();
    void goToOffsetApi(int offset);
    void goToIndexApi(int index);
    void reloadApi();
    void reloadIgnoringCacheApi();
    void setUserAgentApi(const std::string& user_agent);
    std::string getUserAgentApi() const { return m_user_agent; }
    void setZoomLevelApi(double level);
    double getZoomLevelApi() const { return m_zoom_level; }
    void setZoomFactorApi(double factor);
    double getZoomFactorApi() const;
    void focusApi() { setFocus(true); }
    bool isFocusedApi() const { return m_focused; }
    void invalidateApi();
    void setIgnoreMenuShortcutsApi(bool ignore);
    void updateVisibility();

    void onRendererEvent(base::Value::Dict event);
    void emitRendererEvent(const std::string& type, const base::Value::Dict& payload, uint64_t request_id);
    std::unique_ptr<WindowOpenHandlerResult> onWindowOpenHandler(
        v8::Local<v8::Context>, const base::Value::Dict& details);
    void unregisterAndNotify();
    struct RendererNavigation {
        std::string token;
        std::string url;
        std::string method;
        std::string initiator_origin;
        bool browser_initiated = false;
        bool consumed = false;
    };
    struct RendererFrameState {
        uint64_t parent_id = 0;
        uint64_t parent_generation = 0;
        uint64_t generation = 0;
        bool main_frame = false;
        std::string committed_url;
        std::string committed_origin;
        std::string response_url;
        RendererNavigation navigation;
    };
    struct RendererFrameEpoch {
        uint64_t frame_id = 0;
        uint64_t generation = 0;
    };
    void addApplicationResourceRoot(const base::FilePath&);
    std::string approveFrameNavigation(uint64_t frame_id,
        uint64_t parent_id, bool main_frame, const std::string& url,
        const std::string& method);
    bool registerRendererFrame(uint64_t frame_id, uint64_t parent_id,
        bool main_frame);
    bool commitRendererFrame(uint64_t frame_id, const std::string& url);
    void retireRendererFrameSockets(uint64_t frame_id);
    bool isRendererFrameCurrent(uint64_t frame_id) const;
#if BUILDFLAG(IS_WIN)
    void requestCapture(
        std::function<void(RendererFrameSnapshot, std::string)> complete,
        unsigned depth = 0);
#else
    void requestCapture(std::function<void(std::string)> complete);
#endif
#if BUILDFLAG(IS_MAC)
    void captureForSoftwareHost();
#endif

    int m_id = 0;
    WindowInterface* m_owner = nullptr;
    WebContents* m_host = nullptr;
    BrowserWindowConstructorOptions* m_createWindowParam = nullptr;
    std::unique_ptr<RendererClient> m_renderer;
    ui::Cursor m_cursor { ui::mojom::CursorType::kPointer };
#if defined(_WIN32)
    mutable RendererFrameSnapshot m_windowsPaintFrame;
    mutable std::vector<uint8_t> m_windowsPaintPixels;
    mutable uint64_t m_windowsPaintPixelsGeneration = 0;
    mutable bool m_windowsPaintNeedsFullRepaint = false;
#endif
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    gfx::Rect m_widget_screen_rect;
    gfx::Rect m_window_screen_rect;
    bool m_screen_rects_valid = false;
#endif
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
    std::unique_ptr<DevToolsGateway> m_devtools_gateway;
#endif
    base::ObserverList<WebContentsObserver>::Unchecked m_observers { base::ObserverListPolicy::EXISTING_ONLY };
    struct PreparedFrame {
        uint64_t generation;
        std::function<void(const std::string&)> complete;
    };
    std::vector<PreparedFrame> m_prepared_frames;
    uint64_t m_presentation_epoch = 0;
    std::set<int> m_guest_ids;
    std::unordered_map<int, gfx::Rect> m_guest_bounds;
    int m_cursor_guest_id = 0;
    std::unordered_map<uint64_t, RendererFrameState> m_renderer_frames;
    std::unordered_map<uint64_t, RendererFrameEpoch> m_file_chooser_frames;
    uint64_t m_main_frame_id = 0;
    uint64_t m_frame_generation = 0;
    RendererNavigation m_pending_main_navigation;
    std::vector<base::FilePath> m_application_resource_roots;
    uint64_t m_native_drag_generation = 0;
    bool m_native_drag_active = false;
    bool m_native_drag_operation_known = false;
    bool m_native_drag_operation_accepted = false;
    std::vector<std::string> m_native_drag_tokens;

    bool m_isLoading = false;
    bool m_canGoBack = false;
    bool m_canGoForward = false;
    bool m_crashed = false;
    bool m_destroyed = false;
    bool m_destroy_event_emitted = false;
    bool m_ready_to_show_emitted = false;
    bool m_ready_to_show_armed = false;
    bool m_initial_paint_pending = true;
    std::string m_ready_to_show_navigation_token;
    bool m_focused = false;
    bool m_visible = true;
    bool m_renderer_visible = true;
    bool m_background_throttling = true;
    unsigned m_visibility_preparations = 0;
    int m_viewport_width = 0;
    int m_viewport_height = 0;
    double m_viewport_scale = 0.0;
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
    int64_t m_vsync_display_id = -1;
#endif
#if BUILDFLAG(IS_MAC)
    uint32_t m_native_context_id = 0;
    uint32_t m_guest_context_id = 0;
    int m_guest_native_width = 0;
    int m_guest_native_height = 0;
    double m_guest_native_scale = 0.0;
    bool m_host_capture_pending = false;
    bool m_host_capture_dirty = false;
#endif
    bool m_ignore_menu_shortcuts = false;
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
    bool m_debugger_attached = false;
#endif
    std::string m_type = "window";
    std::string m_url;
    std::string m_title;
    std::string m_user_agent;
    std::string m_partition;
    double m_zoom_level = 0.0;
    base::Value::Dict m_web_preferences;
    v8::Persistent<v8::Function> m_window_open_handler;
    v8::Persistent<v8::Object> m_live_self;

    friend class BrowserWindow;
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
    friend class DevToolsGateway;
#endif
    friend class BrowserView;
};

} // namespace atom

namespace gin_helper {
v8::Local<v8::Value> ConvertToV8(v8::Isolate*, const atom::WebContents&);
}

#endif // BROWSER_API_WEB_CONTENTS_H_
