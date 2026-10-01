
#ifndef browser_api_ApiWebContents_h
#define browser_api_ApiWebContents_h

#include "runtime/electron/node_bindings.h"
#include "runtime/electron/browser/api/window_state.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "runtime/engine/public/engine_api.h"
#include "runtime/electron/common/gin_helper/dictionary.h"
#include "runtime/electron/common/gin_helper/public/wrapper_info.h"
#include <set>

namespace node {
class Environment;
}

namespace base {
class ListValue;
}

namespace mojo {
class Connector;
}

namespace atom {

static const int kNotSetXYFlag = 400 /*-8467*/;

class NodeBindings;
class WebContents;
class WindowInterface;
struct WindowOpenHandlerResult;

inline static bool isRectEqual(const RECT& a, const RECT& b)
{
    return (a.left == b.left) && (a.top == b.top) && (a.right == b.right) && (a.bottom == b.bottom);
}

inline static bool isPointInRect(const RECT& a, const POINT& b)
{
    return b.x >= a.left && b.x <= a.right && b.y >= a.top && b.y <= a.bottom;
}

class WebContentsObserver {
public:
    virtual void onWebContentsCreated(WebContents* contents)
    {
    }
    virtual void onWebContentsDeleted(WebContents* contents)
    {
    }
    virtual void onWebContentsReadyToShow(WebContents* contents)
    {
    }
};

class TransmitToWebContents;

class WebContents
    : public mate::EventEmitter<WebContents>
{
public:
    struct BrowserWindowConstructorOptions {
        int x;
        int y;
        int width;
        int height;
        unsigned styles;
        unsigned styleEx;
        bool transparent;
        std::wstring title;
        bool isShow;
        bool isCenter;
        bool isResizable;
        bool isMinimizable;
        bool isMaximizable;
        bool isFrame;
        bool isMovable;

        bool isUseContentSize;
        bool isAlwaysOnTop;
        bool isClosable;

        int minWidth;
        int minHeight;
        int maxWidth;
        int maxHeight;

        bool m_isNodeIntegration;
        bool m_isNodeIntegrationInSubframes;
        bool m_isContextIsolation;
        std::vector<std::string> m_customArgs;

        std::string m_iconPath;

        BrowserWindowConstructorOptions()
        {
            x = 0;
            y = 0;
            width = 0;
            height = 0;
            styles = 0;
            styleEx = 0;
            transparent = false;

            isShow = true;
            isCenter = false;
            isResizable = true;
            isMinimizable = true;
            isMaximizable = true;
            isFrame = true;
            isMovable = true;

            isUseContentSize = false;
            isAlwaysOnTop = false;
            isClosable = true;

            minWidth = 100;
            minHeight = 100;
            maxWidth = 500;
            maxHeight = 500;

            m_isNodeIntegration = false; // 新版本electron从12开始，默认关闭这个nodejs了
            m_isNodeIntegrationInSubframes = false;
            m_isContextIsolation = true;
        }
    };

    static void init(v8::Isolate* isolate, v8::Local<v8::Object> target, node::Environment* env);
    static WebContents* create(v8::Isolate* isolate, gin_helper::Dictionary options, WindowInterface* owner);

    explicit WebContents(v8::Isolate* isolate, v8::Local<v8::Object> wrapper, const gin_helper::Dictionary& options);
    ~WebContents();

    void destroyed();
    void addObserver(WebContentsObserver* observer);
    void removeObserver(WebContentsObserver* observer);

    mini_electron_web_view getEngineView() const
    {
        return m_view;
    }
    WindowInterface* getOwner() const
    {
        return m_owner;
    }

    std::vector<std::string> getPreloadScript();

    void rendererPostMessageToMain(mini_electron_web_frame_handle frame, const std::string& channel, std::unique_ptr<std::vector<blink::CloneableMessage>> listParams);
    void rendererSendMessageToMain(mini_electron_web_frame_handle frame, const std::string& channel, std::unique_ptr<std::vector<blink::CloneableMessage>> listParams, 
        std::vector<uint8_t>* encodedMessageRet);
    void anyPostMessageToRenderer(int64_t frameId, const std::string& channel, std::unique_ptr<std::vector<blink::CloneableMessage>> listParams);
    static void rendererSendMessageToRenderer(mini_electron_web_view view, mini_electron_web_frame_handle frame, const std::string& channel, const std::vector<blink::CloneableMessage>& args);

    int getIdApi() const;
    static WebContents* fromId(int id);

private:
    void runPreloadScript(mini_electron_web_view webView, mini_electron_web_frame_handle frame, int worldId, std::string& preloadScriptPath);

    static void newFunction(const v8::FunctionCallbackInfo<v8::Value>& args);
    void getMainFrameApi(const v8::FunctionCallbackInfo<v8::Value>& info) const;
    void getSessionApi(const v8::FunctionCallbackInfo<v8::Value>& info) const;
    void zoomFactorApi(const v8::FunctionCallbackInfo<v8::Value>& info) const;
    bool canGoBackApi() const;
    bool canGoForwardApi() const;
    void setZoomLevelApi(float level);
    float getZoomLevelApi() const;
    void printToPDFApi();
    void setWindowOpenHandlerApi(const v8::FunctionCallbackInfo<v8::Value>& info);

    void _loadURLApi(const std::string& url);
    int getProcessIdApi() const;
    bool equalApi() const;

    static void getFocusedWebContentsApi(const v8::FunctionCallbackInfo<v8::Value>& info);
    static void getAllWebContentsApi(const v8::FunctionCallbackInfo<v8::Value>& info);
    static void fromIdApi(const v8::FunctionCallbackInfo<v8::Value>& info);

    std::string _getURLApi();
    std::string getTitleApi();

    bool isLoadingApi();
    bool isLoadingMainFrameApi();
    bool isWaitingForResponseApi();
    void stopApi();
    void goBackApi();
    void goForwardApi();
    void goToOffsetApi(int offset);
    void goToIndexApi(int index);
    bool isCrashedApi();
    void setUserAgentApi(const std::string userAgent);
    std::string getUserAgentApi();
    void setZoomFactorApi(float factor);
    v8::Local<v8::Promise> insertCSSApi(const std::string& cssText, gin_helper::Arguments* args);
    void savePageApi();
    void enableDeviceEmulationApi();
    void disableDeviceEmulationApi();
    void setAudioMutedApi();
    void isAudioMutedApi();
    void undoApi();
    void redoApi();
    void cutApi();
    void copyApi();
    void pasteApi();
    void pasteAndMatchStyleApi();
    void _deleteApi();
    void selectAllApi();
    void unselectApi();
    void replaceApi();
    void replaceMisspellingApi();
    void findInPageApi();
    void stopFindInPageApi();
    void focusApi();
    bool isFocusedApi();
    void tabTraverseApi();
    bool _sendApi(
        //int64_t frameId, bool isAllFrames, const std::string& channel, const base::Value::List& args
        const v8::FunctionCallbackInfo<v8::Value>& info
    );
    bool _postMessageApi(const v8::FunctionCallbackInfo<v8::Value>& info);
    void _testPostMessageApi(const v8::FunctionCallbackInfo<v8::Value>& info);
    void sendInputEventApi();
    void beginFrameSubscriptionApi();
    void endFrameSubscriptionApi();
    void startDragApi();
    void setSizeApi();
    bool isGuestApi();
    bool isOffscreenApi();
    void startPaintingApi();
    void stopPaintingApi();
    bool isPaintingApi();
    void setFrameRateApi(int frameRate);
    int getFrameRateApi();
    void invalidateApi();
    void getTypeApi();
    void getWebPreferencesApi();
    v8::Local<v8::Value> getOwnerBrowserWindowApi();
    bool hasServiceWorkerApi();
    void unregisterServiceWorkerApi();
    void printApi();
    void _printToPDFApi();
    void addWorkSpaceApi();
    void reNullWorkSpaceApi();
    void showDefinitionForSelectionApi();
    void copyImageAtApi();
    void capturePageApi();
    void setEmbedderApi();
    bool isDestroyedApi() const;
    void reloadIgnoringCacheApi();
    void downloadURLApi(const std::string& url);

    void nullFunction();

    static void __stdcall staticDidCreateScriptContextCallback(mini_electron_web_view webView, void* param, void* frame, void* context, int extensionGroup, int worldId);
    void onDidCreateScriptContext(mini_electron_web_view webView, void* frame, v8::Local<v8::Context>* context, int extensionGroup, int worldId);
    static void __stdcall staticOnWillReleaseScriptContextCallback(mini_electron_web_view webView, void* param, void* frame, void* context, int worldId);
    void onWillReleaseScriptContextCallback(mini_electron_web_view webView, void* frame, v8::Local<v8::Context>* context, int worldId);
    static mini_electron_download_opt __stdcall staticOnDownloadCallback(mini_electron_web_view, void*, size_t, const char*, const char*, const char*, mini_electron_net_job, mini_electron_net_job_data_bind*);
    static void MINI_ELECTRON_CALL_TYPE onDocumentReadyInBlinkThread(mini_electron_web_view webView, void* param, mini_electron_web_frame_handle frameId);
    static BOOL MINI_ELECTRON_CALL_TYPE onNavigationCallback(mini_electron_web_view webView, void* param, mini_electron_navigation_type navigationType, const utf8* url);
    static mini_electron_web_view MINI_ELECTRON_CALL_TYPE onCreateViewCallback(
        mini_electron_web_view webView, void* param, mini_electron_navigation_type navigationType, const utf8* url, const mini_electron_window_features* windowFeatures);

    void onUrlChange(const std::string& url)
    {
        m_url = url;
    }
    static void MINI_ELECTRON_CALL_TYPE onTitleChanged(mini_electron_web_view webView, void* param, const utf8* title);
    static void MINI_ELECTRON_CALL_TYPE onURLChanged(mini_electron_web_view webView, void* param, const utf8* url, BOOL canGoBack, BOOL canGoForward);
    static void MINI_ELECTRON_CALL_TYPE onLoadingFinishCallback(
        mini_electron_web_view webView, void* param, mini_electron_web_frame_handle frameId, const utf8* url, mini_electron_loading_result result, const utf8* failedReason);

    void setCreateWindowParam(BrowserWindowConstructorOptions* createWindowParam)
    {
        m_createWindowParam = createWindowParam;
    }

public:
    static v8::Persistent<v8::Function> s_constructor;
    static gin_helper::WrapperInfo kWrapperInfo;

private:
    std::unique_ptr<WindowOpenHandlerResult> onWindowOpenHandler(v8::Local<v8::Context> context, const std::string& url);

    friend class BrowserView;
    friend class BrowserWindow;

    NodeBindings* m_nodeBindings = nullptr;
    int m_id;
    std::set<WebContentsObserver*> m_observers;
    std::set<node::Environment*> m_environments;

    friend class TransmitToWebContents;
//     std::unique_ptr <mojo::MessagePipe> m_portPipe;
//     std::unique_ptr<TransmitToWebContents> m_connectorOnMainUiThread;
//     std::unique_ptr<TransmitToWebContents> m_connectorOnBlinkUiThread;

    mini_electron_web_view m_view;
    WindowInterface* m_owner;

    BrowserWindowConstructorOptions* m_createWindowParam;

    bool m_isLoading;

    bool m_canGoBack;
    bool m_canGoForward;

    std::string m_ua;
    std::string m_url;
    std::string m_title;
    std::string m_preloadScriptPath;
    std::string m_sessionName;
    int m_frameRate;

    v8::Persistent<v8::Function> m_windowOpenHandlerCb;

    v8::Persistent<v8::Object> m_liveSelf;
};

} // atom

namespace gin_helper {
v8::Local<v8::Value> ConvertToV8(v8::Isolate* isolate, const atom::WebContents& content);
}

#endif // browser_api_ApiWebContents_h