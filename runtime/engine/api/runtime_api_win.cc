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
#include "runtime/engine/renderer/web_local_frame_client_impl.h"
#include "runtime/engine/renderer/render_thread_impl.h"
#include "runtime/network/loader/web_url_loader_internal.h"
#include "runtime/network/cookies/web_cookie_jar_curl_impl.h"
#include "third_party/blink/public/web/web_frame_serializer.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/public/web/web_frame_serializer_client.h"
#include "third_party/blink/public/web/web_css_origin.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_element.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/platform/wtf/allocator/partitions.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "gen/services/network/public/mojom/url_loader.mojom.h"
#include "gen/services/network/public/mojom/early_hints.mojom.h"
#include "third_party/libcurl/include/curl/curl.h"
#include "ui/display/win/screen_win.h"
#include "base/command_line.h"
#include "base/run_loop.h"
#include "base/synchronization/waitable_event.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_executor.h"
#include "base/at_exit.h"
#include "base/base64.h"

namespace blink {
LocalFrame* FromFrameTokenHash(const size_t& frame_token_hash);
}

class WebFrameSerializerClientImpl : public blink::WebFrameSerializerClient {
public:
    void DidSerializeDataForFrame(const blink::WebVector<char>& data, blink::WebFrameSerializerClient::FrameSerializationStatus) override
    {
        if (data.empty())
            return;

        m_result.resize(data.size());
        memcpy(m_result.data(), data.data(), data.size());
    }
    std::vector<char> m_result;
};

class LinkRewritingDelegateImpl : public blink::WebFrameSerializer::LinkRewritingDelegate {
public:
    bool RewriteFrameSource(blink::WebFrame*, blink::WebString* rewrittenLink) override
    {
        return false;
    }

    bool RewriteLink(const blink::WebURL&, blink::WebString* rewrittenLink) override
    {
        return false;
    }
};

static WebFrameSerializerClientImpl* getSource(mini_electron_web_view webviewHandle)
{
    WebFrameSerializerClientImpl* serializerImpl = new WebFrameSerializerClientImpl();
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return serializerImpl;

    LinkRewritingDelegateImpl delegateImpl;
    if (!blink::WebFrameSerializer::Serialize((blink::WebLocalFrame*)webview->getMainFrame(), serializerImpl, &delegateImpl, false))
        return nullptr;
    if (serializerImpl->m_result.empty())
        return nullptr;

    return serializerImpl;
}

void getSourceOrMHTML(mini_electron_web_view webviewHandle, mini_electron_get_source_callback calback, void* param, bool isSource)
{
    if (!calback)
        return;

    content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [webviewHandle, calback, param, isSource](content::WebViewHost* webview) {
        WebFrameSerializerClientImpl* serializer = isSource ? getSource(webviewHandle) : nullptr /*wkeUtilSerializeToMHTML(webview->getWkeWebView())*/;

        content::ThreadCall::callUiThreadAsync(FROM_HERE, [webviewHandle, calback, param, serializer] {
            content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
            if (webview && serializer) {
                if (serializer->m_result.size() > 0) {
                    serializer->m_result.push_back('\0');
                    calback(webviewHandle, param, serializer->m_result.data());
                } else
                    calback(webviewHandle, param, nullptr);
            } else {
                calback(webviewHandle, param, nullptr);
            }
            if (serializer)
                delete serializer;
        });
    });
}

mini_electron_string_ptr getSourceOrMhtmlSync(mini_electron_web_view webviewHandle, bool isSource)
{
    WebFrameSerializerClientImpl* serializer = nullptr;
    base::WaitableEvent* evt = new base::WaitableEvent(base::WaitableEvent::ResetPolicy::MANUAL, base::WaitableEvent::InitialState::NOT_SIGNALED);
    content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [webviewHandle, isSource, evt, &serializer](content::WebViewHost* webview) {
        serializer = isSource ? getSource(webviewHandle) : nullptr;
        evt->Signal();
    });

    evt->Wait();

    if (!serializer) {
        delete evt;
        return nullptr;
    }

    if (serializer->m_result.size() > 0) {
        serializer->m_result.push_back('\0');
    }

    mini_electron_string_ptr ret = mini_electron_create_string(serializer->m_result.data(), serializer->m_result.size());

    if (serializer)
        delete serializer;
    delete evt;

    return ret;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_util_serialize_to_mhtml(mini_electron_web_view webviewHandle, mini_electron_get_source_callback calback, void* param)
{
    getSourceOrMHTML(webviewHandle, calback, param, false);
}

mini_electron_mem_buf* MINI_ELECTRON_CALL_TYPE mini_electron_create_mem_buf(mini_electron_web_view webView, void* buf, size_t length)
{
    if (!buf || 0 == length)
        return nullptr;
    mini_electron_mem_buf* result = (mini_electron_mem_buf*)malloc(sizeof(mini_electron_mem_buf));
    result->unuse = sizeof(mini_electron_mem_buf);
    result->length = length;
    result->data = malloc(length);
    memcpy(result->data, buf, length);
    return result;
}

namespace mini_electron::engine {

typedef enum _MemBufType {
    kMemBufTypeNormal = sizeof(mini_electron_mem_buf),
    kMemBufTypeSkdata = 110001,
    kMemBufTypeStdString = 110002,
} MemBufType;

struct MemBufSkdata { // 扩展原版的mini_electron_mem_buf，以mini_electron_mem_buf::unuse做区分
    mini_electron_mem_buf buf;
    sk_sp<SkData> skdata;
};

struct MemBufStdString { // 扩展原版的mini_electron_mem_buf，以mini_electron_mem_buf::unuse做区分
    mini_electron_mem_buf buf;
    std::string* stdstring;
};

mini_electron_mem_buf* createMemBufFromSkdata(sk_sp<SkData> data)
{
    if (!data.get() || 0 == data->size())
        return nullptr;
    MemBufSkdata* result = new MemBufSkdata();
    result->buf.unuse = kMemBufTypeSkdata;
    result->buf.length = data->size();
    result->buf.data = (void*)data->data();
    result->skdata.swap(data);
    return &(result->buf);
}

mini_electron_mem_buf* createMemBufFromStdString(std::string* data)
{
    if (!data || 0 == data->size())
        return nullptr;
    MemBufStdString* result = new MemBufStdString();
    result->buf.unuse = kMemBufTypeStdString;
    result->buf.length = data->size();
    result->buf.data = (void*)data->data();
    return &(result->buf);
}

void freeMemBuf(mini_electron_mem_buf* buf)
{
    if (!buf)
        return;

    if (0 == buf->unuse || kMemBufTypeNormal == buf->unuse) {
        if (buf->data)
            free(buf->data);
        free(buf);
    }

    if (kMemBufTypeSkdata == buf->unuse) {
        MemBufSkdata* bufSkdata = (MemBufSkdata*)buf;
        delete bufSkdata;
    }
    if (kMemBufTypeStdString == buf->unuse) {
        MemBufStdString* bufStdString = (MemBufStdString*)buf;
        delete bufStdString->stdstring;
        delete bufStdString;
    }
}

void* getWindowScreenshotSync(void* webviewPtr)
{
    DebugBreak();
    return nullptr;
}

}

void MINI_ELECTRON_CALL_TYPE mini_electron_free_mem_buf(mini_electron_mem_buf* buf)
{
    mini_electron::engine::freeMemBuf(buf);
}

mini_electron_mem_buf* MINI_ELECTRON_CALL_TYPE mini_electron_util_base64_encode_buffer(const void* str, int len)
{
    if (len <= 0)
        return nullptr;
    std::string* base64Str = new std::string();
    base::Base64EncodeAppend(base::span<const uint8_t>((const uint8_t*)str, (size_t)len), base64Str);
    mini_electron_mem_buf* ret = mini_electron::engine::createMemBufFromStdString(base64Str);
    return ret;
}

mini_electron_mem_buf* MINI_ELECTRON_CALL_TYPE mini_electron_get_window_screenshot_sync(mini_electron_web_view webviewHandle, mini_electron_image_format format)
{
    if (format != kMiniElectronImageFormatPng)
        return nullptr;
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview) {
        g_outputDebugString("mini_electron_get_window_screenshot_sync fail 1\n");
        return nullptr;
    }
    webview->getViewDC();
    mini_electron_mem_buf* out = (mini_electron_mem_buf*)mini_electron::engine::getWindowScreenshotSync(webview);
    webview->unlockViewDC();
    return out;
}

#ifdef OS_WIN

typedef enum XP_PROCESS_DPI_AWARENESS {
    XP_PROCESS_DPI_UNAWARE = 0,
    XP_PROCESS_SYSTEM_DPI_AWARE = 1,
    XP_PROCESS_PER_MONITOR_DPI_AWARE = 2
} XP_PROCESS_DPI_AWARENESS;

// inline BOOL SetProcessDPIAwareXp()
// {
//     typedef BOOL(__stdcall* PFN_SetProcessDPIAware)();
//     static PFN_SetProcessDPIAware s_SetProcessDPIAware = NULL;
//     static BOOL s_isInit = FALSE;
//     if (!s_isInit) {
 
//         s_SetProcessDPIAware = (PFN_SetProcessDPIAware)GetProcAddress(handle, "SetProcessDPIAware");
//         s_isInit = TRUE;
//     }
//
//     if (s_SetProcessDPIAware)
//         return s_SetProcessDPIAware();
//
//     return FALSE;
// }

inline HRESULT SetProcessDpiAwarenessXp(XP_PROCESS_DPI_AWARENESS value)
{
    typedef HRESULT(__stdcall * PFN_SetProcessDpiAwareness)(XP_PROCESS_DPI_AWARENESS value);
    static PFN_SetProcessDpiAwareness s_SetProcessDpiAwareness = NULL;
    static BOOL s_isInit = FALSE;
    if (!s_isInit) {
        HMODULE handle = GetModuleHandleW(L"User32.dll");
        s_SetProcessDpiAwareness = (PFN_SetProcessDpiAwareness)GetProcAddress(handle, "SetProcessDpiAwareness");
        s_isInit = TRUE;
    }

    if (s_SetProcessDpiAwareness)
        return s_SetProcessDpiAwareness(value);

    return E_INVALIDARG;
}
#endif // OS_WIN

namespace content {
display::Screen* getScreenOrCreate();
}

void MINI_ELECTRON_CALL_TYPE mini_electron_enable_high_dpi_support()
{
#ifdef OS_WIN
    SetProcessDpiAwarenessXp(XP_PROCESS_PER_MONITOR_DPI_AWARE);
    SetProcessDPIAware();

    content::getScreenOrCreate();
    float f = display::win::ScreenWin::GetScaleFactorForHWND(NULL);

    //HDC screenDC = ::GetDC(nullptr);
    //int dpiX = ::GetDeviceCaps(screenDC, LOGPIXELSX); // 96?是100%、120?是125%
    content::RenderThreadImpl::get()->setZoom(/*dpiX / 96.0*/f);

    //::ReleaseDC(nullptr, screenDC);
#endif
}

// 暂时只能获取main world
BOOL MINI_ELECTRON_CALL_TYPE mini_electron_get_context_by_v8_object(void* isolate, void* obj, int worldID, v8ContextPtr cxtOut)
{
    v8::Local<v8::Object>* object = (v8::Local<v8::Object>*)obj;

    v8::MaybeLocal<v8::Context> context = (*object)->GetCreationContext();
    if (context.IsEmpty())
        return FALSE;
    blink::WebLocalFrame* frame = blink::WebLocalFrame::FrameForContext(context.ToLocalChecked());
    if (!frame)
        return FALSE;

    if (0 == worldID) {
        *(v8::Local<v8::Context>*)cxtOut = frame->MainWorldScriptContext();
        return TRUE;
    }

    *(v8::Local<v8::Context>*)cxtOut = frame->GetScriptContextFromWorldId((v8::Isolate*)isolate, worldID);
    return TRUE;
}

mini_electron_web_view MINI_ELECTRON_CALL_TYPE mini_electron_get_web_view_for_current_context()
{
    blink::WebLocalFrame* frame = blink::WebLocalFrame::FrameForCurrentContext();
    if (!frame)
        return NULL_WEBVIEW;

    content::WebLocalFrameClientImpl* client = (content::WebLocalFrameClientImpl*)(frame->Client());
    if (!frame)
        return NULL_WEBVIEW;

    return (mini_electron_web_view)(client->getEngineViewId());
}

mini_electron_web_frame_handle MINI_ELECTRON_CALL_TYPE mini_electron_get_web_frame_for_current_context()
{
    blink::WebLocalFrame* frame = blink::WebLocalFrame::FrameForCurrentContext();
    if (!frame)
        return 0;

    const blink::LocalFrameToken& localFrameToken = frame->GetLocalFrameToken();
    size_t hash = blink::LocalFrameToken::Hasher()(localFrameToken);
    return (mini_electron_web_frame_handle)(hash);
}

static blink::Element* webElementFromV8Value(v8::Local<v8::Value> value)
{
    if (value.IsEmpty())
        return nullptr;
    blink::Element* element = blink::V8Element::ToWrappable(v8::Isolate::GetCurrent(), value);
    return element;
}

static v8::Local<v8::Value> nodeToV8Value(blink::Element* elem, v8::Local<v8::Object> creation_context, v8::Isolate* isolate)
{
    // We no longer use |creation_context| because it's often misused and points
    // to a context faked by user script.
    CHECK(creation_context->GetCreationContext().ToLocalChecked() == isolate->GetCurrentContext());
    if (!elem)
        return v8::Local<v8::Value>();

    blink::ScriptState* scriptState = blink::ScriptState::From(isolate, isolate->GetCurrentContext());
    return blink::ToV8Traits<blink::Element>::ToV8(scriptState, elem);
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_pass_web_element_value_to_other_context(void* val, void* destCtx, void* outVal)
{
    v8::Local<v8::Value>* value = (v8::Local<v8::Value>*)val;
    v8::Local<v8::Context>* destContext = (v8::Local<v8::Context>*)destCtx;

    blink::Element* elem = webElementFromV8Value(*value);
    if (!elem)
        return false;

    v8::Context::Scope destination_context_scope(*destContext);
    *(v8::Local<v8::Value>*)outVal = nodeToV8Value(elem, (*destContext)->Global(), (*destContext)->GetIsolate());
    return true;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_get_world_script_context_by_web_frame(mini_electron_web_view webviewHandle, mini_electron_web_frame_handle frameId, int worldID, v8ContextPtr contextOut)
{
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return;

    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    // 不需要v8::HandleScope，因为这个会导致v8::Context一返回就失效了

    blink::WebLocalFrame* webFrame = nullptr;
    if ((mini_electron_web_frame_handle)-2 == frameId) {
        content::WebLocalFrameClientImpl* client = webview->getFrameClient();
        if (!client)
            return;
        webFrame = client->getFrame();
    } else {
        blink::LocalFrame* blinkFrame = blink::FromFrameTokenHash((size_t)(frameId));
        if (!blinkFrame)
            return;
        webFrame = blink::WebLocalFrameImpl::FromFrame(blinkFrame);
    }
    if (!webFrame)
        return;

    v8::Local<v8::Context> result;
    if (0 == worldID) {
        result = webFrame->MainWorldScriptContext();
    } else {
        result = webFrame->GetScriptContextFromWorldId(isolate, worldID);
    }
    v8::Local<v8::Context>* contextOutPtr = (v8::Local<v8::Context>*)contextOut;
    *contextOutPtr = result;
}

namespace content {
scoped_refptr<network::SharedURLLoaderFactory> CreateURLLoaderFactoryForWebView(int64_t id);
}

class DownloadUrlClient : public ::network::mojom::URLLoaderClient {
public:
    // network::mojom::URLLoaderClient implementation:
    void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr early_hints) override
    {

    }

    void OnReceiveResponse(network::mojom::URLResponseHeadPtr response_head, mojo::ScopedDataPipeConsumerHandle body,
        std::optional<mojo_base::BigBuffer> cached_metadata) override
    {

    }

    void OnReceiveRedirect(const net::RedirectInfo& redirect_info, network::mojom::URLResponseHeadPtr response_head) override
    {

    }

    void OnUploadProgress(int64_t current_position, int64_t total_size, OnUploadProgressCallback ack_callback) override
    {

    }

    void OnTransferSizeUpdated(int32_t transfer_size_diff) override
    {

    }

    void OnComplete(const network::URLLoaderCompletionStatus& status) override
    {
        OutputDebugStringA("DownloadUrlClient.OnComplete\n");
    }

    void onClientConnectionError()
    {
        OutputDebugStringA("DownloadUrlClient.onClientConnectionError\n");

        DownloadUrlClient* self = this;
        content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [self] {
            delete self;
        });
    }

    mojo::Receiver<network::mojom::URLLoaderClient> m_clientReceiver{ this };
protected:
private:
    
};

void MINI_ELECTRON_CALL_TYPE mini_electron_download_url(mini_electron_web_view webviewHandle, mini_electron_web_frame_handle frameId, const std::string& url)
{
    std::string* urlCopy = new std::string(url);
    content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [webviewHandle, urlCopy](content::WebViewHost* self) {
        scoped_refptr<network::SharedURLLoaderFactory> factory = content::CreateURLLoaderFactoryForWebView(webviewHandle);
        DownloadUrlClient* client = new DownloadUrlClient();

        ::network::ResourceRequest request;
        request.url = GURL(*urlCopy);

        mojo::Remote<network::mojom::URLLoader> urlLoader;
        factory->CreateLoaderAndStart(urlLoader.BindNewPipeAndPassReceiver(),
            0x1234, // request_id
            0, // options
            request,
            client->m_clientReceiver.BindNewPipeAndPassRemote(),
            net::MutableNetworkTrafficAnnotationTag());

        client->m_clientReceiver.set_disconnect_handler(base::BindOnce(&DownloadUrlClient::onClientConnectionError, base::Unretained(client)));
        delete urlCopy;
    });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_clear_cookie(mini_electron_web_view webView)
{
    mini_electron_perform_cookie_command(webView, mini_electron_cookie_command_clear_all_cookies);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_perform_cookie_command(mini_electron_web_view webviewHandle, mini_electron_cookie_command command)
{
    content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [command](content::WebViewHost* self) {
        CURL* curl = curl_easy_init();
        if (!curl)
            return;

        mini_electron::WebCookieJarImpl* cookieJar = self->getWebCookieJarImpl();
        std::string cookiesPath = cookieJar->getCookieJarFullPath();
        CURLSH* curlsh = cookieJar->getCurlShareHandle();

        curl_easy_setopt(curl, CURLOPT_SHARE, curlsh);
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR, cookiesPath.c_str());
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookiesPath.c_str());

        switch (command) {
            case mini_electron_cookie_command_clear_all_cookies:
            {
                curl_easy_setopt(curl, CURLOPT_COOKIELIST, "ALL");
                std::u16string cookiesPathW = base::UTF8ToUTF16(cookiesPath);
                ::DeleteFileW((LPCWSTR)(cookiesPathW.c_str()));
            }
            break;
            case mini_electron_cookie_command_clear_session_cookies:
                curl_easy_setopt(curl, CURLOPT_COOKIELIST, "SESS");
                break;
            case mini_electron_cookie_command_flush_cookies_to_file:
                curl_easy_setopt(curl, CURLOPT_COOKIELIST, "FLUSH");
                break;
            case mini_electron_cookie_command_reload_cookies_from_file:
                curl_easy_setopt(curl, CURLOPT_COOKIELIST, "RELOAD");
                break;
        }
        curl_easy_cleanup(curl);
    });
}

namespace content {
bool g_headlessEnabled = false;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_headless_enabled(mini_electron_web_view webviewHandle, BOOL b)
{
    content::g_headlessEnabled = b;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_language(mini_electron_web_view webviewHandle, const char* language)
{
    //wke::g_language = WTF::adoptPtr(new std::string(language));

    std::string* lang = new std::string(language);
    content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [lang](content::WebViewHost* self) {
        self->setSetLanguage(*lang);
        delete lang;
    });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_move_window(mini_electron_web_view webviewHandle, int x, int y, int w, int h)
{
    HWND hwnd = mini_electron_get_host_hwnd(webviewHandle);
    if (!hwnd)
        return;
    ::SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}


static void callCSSByFrameWithResultHelper(mini_electron_web_view webviewHandle, const utf8* key, mini_electron_insert_css_by_frame_result_callback callback, void* param)
{
    std::string keyStr = key;
    content::ThreadCall::callUiThreadAsync(FROM_HERE, [webviewHandle, keyStr, callback, param] {
        callback(webviewHandle, param, keyStr.c_str());
    });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_insert_css_by_frame_with_result(
    mini_electron_web_view webviewHandle, mini_electron_web_frame_handle frameId, const utf8* cssText, int cssOrigin, mini_electron_insert_css_by_frame_result_callback callback, void* param)
{
    std::string cssTextStr = cssText;
    int cssOriginCopy = cssOrigin;
    content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [webviewHandle, frameId, cssTextStr, cssOriginCopy, callback, param] {
        blink::WebLocalFrame* webFrame = nullptr;
        do {
            content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
            if (!webview)
                break;

            if ((mini_electron_web_frame_handle)-2 == frameId) {
                content::WebLocalFrameClientImpl* client = webview->getFrameClient();
                if (!client) 
                    break;
                webFrame = client->getFrame();
            } else {
                blink::LocalFrame* blinkFrame = blink::FromFrameTokenHash((size_t)(frameId));
                if (!blinkFrame) 
                    break;
                webFrame = blink::WebLocalFrameImpl::FromFrame(blinkFrame);
            }
        } while (false);

        if (!webFrame) {
            callCSSByFrameWithResultHelper(webviewHandle, "", callback, param);
            return;
        }

        blink::WebCssOrigin webCssOrigin = cssOriginCopy == 0 ? blink::WebCssOrigin::kAuthor : blink::WebCssOrigin::kUser;
        std::string key = webFrame->GetDocument().InsertStyleSheet(blink::WebString::FromUTF8(cssTextStr), nullptr, webCssOrigin).Utf8();
        callCSSByFrameWithResultHelper(webviewHandle, key.c_str(), callback, param);
    });
}
