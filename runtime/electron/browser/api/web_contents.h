#ifndef BROWSER_API_WEB_CONTENTS_H_
#define BROWSER_API_WEB_CONTENTS_H_

#if defined(_WIN32)
#include <windows.h>
#endif

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/files/file_path.h"
#include "base/values.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "runtime/electron/common/gin_helper/dictionary.h"
#include "runtime/electron/common/gin_helper/public/wrapper_info.h"
#include "runtime/electron/common/renderer_client.h"

namespace node {
class Environment;
}

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
    virtual void onWebContentsPaint(WebContents*) { }
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

    WebContents(v8::Isolate*, v8::Local<v8::Object>, const gin_helper::Dictionary&);
    ~WebContents();

    void destroyed();
    void addObserver(WebContentsObserver*);
    void removeObserver(WebContentsObserver*);

    WindowInterface* getOwner() const { return m_owner; }
    WebContents* host() const { return m_host; }
    int getIdApi() const { return m_id; }
    bool isDestroyedApi() const;
    bool isAlive() const;

    void setCreateWindowParam(BrowserWindowConstructorOptions* options) { m_createWindowParam = options; }
    void resize(int width, int height, double scale_factor = 1.0);
    void setFocus(bool focused);
    bool sendInput(base::Value::Dict event);
    void sendWindowsMouseEvent(unsigned message, int x, int y, unsigned flags, int delta = 0);
    void sendWindowsKeyEvent(const char* type, unsigned key_code, unsigned flags);
    bool copyFrame(RendererFrameSnapshot* snapshot) const;
#if defined(_WIN32)
    bool paintFrame(HDC target, int dest_x, int dest_y, int src_x, int src_y, int width, int height) const;
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
    void getMainFrameApi(const v8::FunctionCallbackInfo<v8::Value>&) const;
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
    bool isRendererFrameCurrent(uint64_t frame_id) const;

    int m_id = 0;
    WindowInterface* m_owner = nullptr;
    WebContents* m_host = nullptr;
    BrowserWindowConstructorOptions* m_createWindowParam = nullptr;
    std::unique_ptr<RendererClient> m_renderer;
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
    std::unique_ptr<DevToolsGateway> m_devtools_gateway;
#endif
    std::set<WebContentsObserver*> m_observers;
    std::set<int> m_guest_ids;
    std::unordered_map<uint64_t, RendererFrameState> m_renderer_frames;
    std::unordered_map<uint64_t, RendererFrameEpoch> m_file_chooser_frames;
    uint64_t m_main_frame_id = 0;
    uint64_t m_frame_generation = 0;
    RendererNavigation m_pending_main_navigation;
    std::vector<base::FilePath> m_application_resource_roots;

    bool m_isLoading = false;
    bool m_canGoBack = false;
    bool m_canGoForward = false;
    bool m_crashed = false;
    bool m_destroyed = false;
    bool m_destroy_event_emitted = false;
    bool m_ready_to_show_emitted = false;
    bool m_focused = false;
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
