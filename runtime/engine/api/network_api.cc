
#include <stdlib.h>
#include <crtdbg.h>
#include "runtime/engine/public/engine_api.h"
#include "runtime/engine/download/simple_download.h"
#include "runtime/engine/bindings/js_value.h"
#include "runtime/engine/browser/web_view_host.h"
#include "runtime/engine/browser/shared_timer_win.h"
#include "runtime/engine/common/live_id_detect.h"
#include "runtime/engine/common/thread_call.h"
#include "runtime/engine/common/utf16.h"
#include "runtime/engine/renderer/render_thread_impl.h"
#include "runtime/network/loader/web_url_loader_internal.h"
#include "runtime/network/loader/web_url_loader_manager.h"
#include "runtime/network/loader/initialize_handle_info.h"
#include "runtime/network/loader/flatten_http_body_element.h"
#include "runtime/network/loader/web_url_loader_manager_setup_info.h"
#include "runtime/storage/default_local_storage_dir.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/platform/wtf/allocator/partitions.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/public/platform/web_http_header_visitor.h"
#include "services/network/public/cpp/resource_request.h"
#include "base/command_line.h"
#include "base/run_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_executor.h"
#include "base/at_exit.h"
#include "v8.h"

bool checkThreadCallIsValid(const char* funcName);
const char* createTempCharString(const char* str, size_t length);


namespace mini_electron {
void onNetSetData(mini_electron_net_job jobPtr, void* buf, int len);
void onNetSetMIMEType(mini_electron_net_job jobPtr, const char* type);
void onNetSetHTTPHeaderFieldCommon(mini_electron_net_job jobPtr, const utf8* key, const utf8* value, BOOL response);
void changeRequestUrl(mini_electron_net_job jobPtr, const char* url);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_set_http_header_field_utf8(mini_electron_net_job jobPtr, const utf8* key, const utf8* value, BOOL response)
{
    mini_electron::onNetSetHTTPHeaderFieldCommon(jobPtr, key, value, response);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_set_mime_type(mini_electron_net_job jobPtr, const char* type)
{
    mini_electron::WebURLLoaderInternal* job = (mini_electron::WebURLLoaderInternal*)jobPtr;
    if (job->m_isUrlBegining || content::ThreadCall::isBlinkThread()) {
        mini_electron::onNetSetMIMEType(jobPtr, type);
    } else {
        std::string* typeCopy = new std::string(type);
        content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [jobPtr, typeCopy] {
            mini_electron::onNetSetMIMEType(jobPtr, typeCopy->c_str());
            delete typeCopy;
        });
    }
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_set_data(mini_electron_net_job jobPtr, void* buf, int len)
{
    mini_electron::WebURLLoaderInternal* job = (mini_electron::WebURLLoaderInternal*)jobPtr;
    if (job->m_isUrlBegining || content::ThreadCall::isBlinkThread()) {
        mini_electron::onNetSetData(jobPtr, buf, len);
    } else {
        std::vector<char>* bufferCopy = new std::vector<char>();
        bufferCopy->resize(len);
        if (len)
            memcpy(bufferCopy->data(), buf, len);
        content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [jobPtr, bufferCopy] {
            mini_electron::onNetSetData(jobPtr, bufferCopy->data(), (int)bufferCopy->size());
            delete bufferCopy;
        });
    }
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_change_request_url(mini_electron_net_job jobPtr, const char* url)
{
    if (content::ThreadCall::isBlinkThread())
        mini_electron::changeRequestUrl(jobPtr, url);
    else {
        std::string* urlCopy = new std::string(url);
        content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [jobPtr, urlCopy] {
            mini_electron::changeRequestUrl(jobPtr, urlCopy->c_str());
            delete urlCopy;
        });
    }
}

mini_electron_request_type MINI_ELECTRON_CALL_TYPE mini_electron_net_get_request_method(void* jobPtr)
{
    checkThreadCallIsValid(__FUNCTION__);
    mini_electron::WebURLLoaderInternal* job = (mini_electron::WebURLLoaderInternal*)jobPtr;
    mini_electron::InitializeHandleInfo* info = job->m_initializeHandleInfo;
    std::string method;
    if (!info) {
        method = job->firstRequest()->method;
        if (method.empty())
            return kMiniElectronRequestTypeInvalidation;
    } else
        method = info->method;

    if ("POST" == method) {
        return kMiniElectronRequestTypePost;
    } else if ("PUT" == method) {
        return kMiniElectronRequestTypePut;
    } else if ("GET" == method) {
        return kMiniElectronRequestTypeGet;
    }
    return kMiniElectronRequestTypeInvalidation;
}

const mini_electron_slist* MINI_ELECTRON_CALL_TYPE mini_electron_net_get_raw_http_head_in_blink_thread(mini_electron_net_job jobPtr)
{
    if (content::ThreadCall::isBlinkThread())
        checkThreadCallIsValid(__FUNCTION__);
    mini_electron::WebURLLoaderInternal* job = (mini_electron::WebURLLoaderInternal*)jobPtr;
    if (!job->m_initializeHandleInfo)
        return nullptr;
    return (const mini_electron_slist*)job->m_initializeHandleInfo->headers;
}

class HTTPHeaderVisitor : public blink::WebHTTPHeaderVisitor {
public:
    HTTPHeaderVisitor(curl_slist** result)
    {
        m_result = result;
    }

    virtual void VisitHeader(const blink::WebString& name, const blink::WebString& value) override
    {
        *m_result = curl_slist_append(*m_result, name.Utf8().c_str());
        *m_result = curl_slist_append(*m_result, value.Utf8().c_str());
    }

private:
    curl_slist** m_result;
};

const mini_electron_slist* MINI_ELECTRON_CALL_TYPE mini_electron_net_get_raw_response_head_in_blink_thread(mini_electron_net_job jobPtr)
{
    if (content::ThreadCall::isBlinkThread()) {
        mini_electron::WebURLLoaderInternal* job = (mini_electron::WebURLLoaderInternal*)jobPtr;
        mini_electron_slist* result = nullptr;
        HTTPHeaderVisitor visitor((curl_slist**)&result);
        job->m_response.VisitHttpHeaderFields(&visitor);

        content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [result] {
            curl_slist_free_all((curl_slist*)result);
        });

        return result;
    }
    return nullptr;
}

mini_electron_post_body_elements* MINI_ELECTRON_CALL_TYPE mini_electron_net_create_post_body_elements(mini_electron_web_view webView, size_t length)
{
    checkThreadCallIsValid(__FUNCTION__);
    if (0 == length)
        return nullptr;

    mini_electron_post_body_elements* result = new mini_electron_post_body_elements();
    result->size = sizeof(mini_electron_post_body_elements);
    result->isDirty = true;

    size_t allocLength = sizeof(mini_electron_post_body_element*) * length;
    result->element = (mini_electron_post_body_element**)malloc(allocLength);
    memset(result->element, 0, allocLength);

    result->elementSize = length;

    return result;
}

struct mini_electron_string {
    mini_electron_string(const utf8* utfString, size_t length)
    {
        str = utfString;
        len = length;
        freeStrFunc = nullptr;
    }

    ~mini_electron_string()
    {
        if (freeStrFunc)
            freeStrFunc((utf8*)str, len);
    }

    static void defaultFreeStr(utf8* str, size_t len)
    {
        free(str);
    }

    const utf8* str;
    size_t len;
    void (*freeStrFunc)(utf8* str, size_t len);
};

mini_electron_string_ptr MINI_ELECTRON_CALL_TYPE mini_electron_create_string(const utf8* str, size_t len)
{
    mini_electron_string_ptr mini_electron_str = new mini_electron_string(str, len);
    return mini_electron_str;
}

mini_electron_string_ptr MINI_ELECTRON_CALL_TYPE mini_electron_create_string_with_copy(const utf8* str, size_t len)
{
    if (!str || 0 == len)
        return nullptr;

    utf8* strCopy = (utf8*)malloc(len + 1);
    memcpy(strCopy, str, len);
    strCopy[len] = 0;

    mini_electron_string_ptr mini_electron_str = new mini_electron_string(strCopy, len);
    mini_electron_str->freeStrFunc = mini_electron_string::defaultFreeStr;
    return mini_electron_str;
}

mini_electron_string_ptr MINI_ELECTRON_CALL_TYPE mini_electron_create_string_without_null_termination(const utf8* str, size_t len)
{
    return mini_electron_create_string_with_copy(str, len);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_delete_string(mini_electron_string_ptr str)
{
    delete str;
}

const utf8* MINI_ELECTRON_CALL_TYPE mini_electron_get_string(mini_electron_string_ptr s)
{
    return s ? s->str : "";
}

size_t MINI_ELECTRON_CALL_TYPE mini_electron_get_string_len(mini_electron_string_ptr s)
{
    return s ? s->len : 0;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_free_post_body_element(mini_electron_post_body_element* element)
{
    mini_electron_free_mem_buf(element->data);
    mini_electron_delete_string(element->filePath);
    delete element;
}

mini_electron_post_body_element* MINI_ELECTRON_CALL_TYPE mini_electron_net_create_post_body_element(mini_electron_web_view webView)
{
    checkThreadCallIsValid(__FUNCTION__);
    mini_electron_post_body_element* bodyElement = new mini_electron_post_body_element();
    bodyElement->size = sizeof(mini_electron_post_body_element);
    return bodyElement;
}

static mini_electron_post_body_elements* flattenHTTPBodyElementToEmbedder(const std::vector<mini_electron::FlattenHTTPBodyElement*>& body)
{
    if (0 == body.size())
        return nullptr;

    mini_electron_post_body_elements* result = mini_electron_net_create_post_body_elements(NULL_WEBVIEW, body.size());
    result->isDirty = false;
    for (size_t i = 0; i < result->elementSize; ++i) {
        mini_electron_post_body_element* bodyElement = mini_electron_net_create_post_body_element(NULL_WEBVIEW);
        result->element[i] = bodyElement;
        const mini_electron::FlattenHTTPBodyElement* element = body[i];

        if (mini_electron::FlattenHTTPBodyElement::TypeFile == element->type
            /*|| mbnet::FlattenHTTPBodyElement::TypeFileSystemURL == element->type*/) {

            bodyElement->type = mini_electron_http_body_element_type_file;

            std::string filePathUtf8;
            base::UTF16ToUTF8(element->filePath.c_str(), element->filePath.size(), &filePathUtf8);

            bodyElement->filePath = mini_electron_create_string(filePathUtf8.c_str(), filePathUtf8.size());
            bodyElement->fileLength = element->fileLength;
            bodyElement->fileStart = element->fileStart;
            bodyElement->data = nullptr;
        } else {
            bodyElement->type = mini_electron_http_body_element_type_data;
            bodyElement->filePath = nullptr;
            bodyElement->fileLength = 0;
            bodyElement->fileStart = 0;
            bodyElement->data = mini_electron_create_mem_buf(NULL_WEBVIEW, (void*)element->data.data(), element->data.size());
        }
    }
    return result;
}

mini_electron_post_body_elements* MINI_ELECTRON_CALL_TYPE mini_electron_net_get_post_body(void* jobPtr)
{
    checkThreadCallIsValid(__FUNCTION__);
    mini_electron::WebURLLoaderInternal* job = (mini_electron::WebURLLoaderInternal*)jobPtr;
    mini_electron::InitializeHandleInfo* info = job->m_initializeHandleInfo;
    if (!info)
        return nullptr;

    std::vector<mini_electron::FlattenHTTPBodyElement*>* flattenElements = nullptr;
    if ("POST" == info->method) {
        if (!info->methodInfo || !info->methodInfo->post || !info->methodInfo->post->data)
            return nullptr;
        flattenElements = &(info->methodInfo->post->data->flattenElements);
    } else if ("PUT" == info->method) {
        if (!info->methodInfo || !info->methodInfo->put || !info->methodInfo->put->data)
            return nullptr;
        flattenElements = &info->methodInfo->put->data->flattenElements;
    }
    if (!flattenElements)
        return nullptr;

    mini_electron_post_body_elements* postBody = flattenHTTPBodyElementToEmbedder(*flattenElements);
    return postBody;
}

static void netFreePostBodyElements(mini_electron_post_body_elements* elements)
{
    checkThreadCallIsValid(__FUNCTION__);
    for (size_t i = 0; i < elements->elementSize; ++i) {
        mini_electron_net_free_post_body_element(elements->element[i]);
    }
    free(elements->element);
    delete elements;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_free_post_body_elements(mini_electron_post_body_elements* elements)
{
    netFreePostBodyElements(elements);
}


void MINI_ELECTRON_CALL_TYPE mini_electron_set_view_proxy(mini_electron_web_view webviewHandle, const mini_electron_proxy* proxy)
{
    mini_electron_proxy* proxyCopy = new mini_electron_proxy();
    *proxyCopy = *proxy;

    content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [webviewHandle, proxyCopy] {
        content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (webview) {
        webview->setProxy(proxyCopy);
    }
        });
}

const char* MINI_ELECTRON_CALL_TYPE mini_electron_net_get_mime_type(mini_electron_net_job jobPtr)
{
    mini_electron::WebURLLoaderInternal* job = (mini_electron::WebURLLoaderInternal*)jobPtr;
    blink::WebString contentType = job->m_response.HttpHeaderField(blink::WebString::FromUTF8("Content-Type"));
    std::string contentTypeUtf8 = contentType.Utf8();
    return createTempCharString(contentTypeUtf8.c_str(), contentTypeUtf8.size());
}

const char* netGetHTTPHeaderField(mini_electron_net_job jobPtr, const char* key)
{
    checkThreadCallIsValid(__FUNCTION__);
    mini_electron::WebURLLoaderInternal* job = (mini_electron::WebURLLoaderInternal*)jobPtr;
    std::optional<std::string> value = job->firstRequest()->headers.GetHeader(key);
    if (!value.has_value())
        return nullptr;
    return createTempCharString(value->c_str(), value->size());
}

const char* netGetHTTPHeaderFieldFromResponse(mini_electron_net_job jobPtr, const char* key)
{
    checkThreadCallIsValid(__FUNCTION__);
    mini_electron::WebURLLoaderInternal* job = (mini_electron::WebURLLoaderInternal*)jobPtr;
    blink::WebString value = job->m_response.HttpHeaderField(blink::WebString::FromUTF8(key));
    std::string valueBuffer = value.Utf8();

    return createTempCharString(valueBuffer.c_str(), valueBuffer.size());
}

const utf8* MINI_ELECTRON_CALL_TYPE mini_electron_net_get_http_header_field(mini_electron_net_job jobPtr, const char* key, BOOL fromRequestOrResponse)
{
    if (fromRequestOrResponse)
        return netGetHTTPHeaderField(jobPtr, key);
    return netGetHTTPHeaderFieldFromResponse(jobPtr, key);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_cookie(mini_electron_web_view webviewHandle, const utf8* url, const utf8* cookie)
{
    //checkThreadCallIsValid(__FUNCTION__);
    //cookie = "cna22=111111; domain=.1688.com; path=/; expires=Tue, 23-Jan-2029 13:17:21 GMT;";

    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;

    std::string* urlString = new std::string(url);
    std::string* cookieString = new std::string(cookie);

    content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [webviewHandle, urlString, cookieString] {
        content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
        if (webview) {
            webview->setCookie(*cookieString);
        }

        OutputDebugStringA("mini_electron_set_cookie:");
        OutputDebugStringA(cookieString->c_str());
        OutputDebugStringA("\n");
        delete urlString;
        delete cookieString;
    });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_get_cookie(mini_electron_web_view webviewHandle, mini_electron_get_cookie_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    if (!callback)
        return;

    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview) {
        callback(NULL_WEBVIEW, param, MINI_ELECTRON_ASYNC_REQUEST_FAIL, nullptr);
        return;
    }

    content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [webviewHandle, callback, param] {
        std::string* cookie = nullptr;
        content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
        if (webview) {
            cookie = new std::string(webview->getCookie());
        } else
            cookie = new std::string("");

        content::ThreadCall::callUiThreadAsync(FROM_HERE, [webviewHandle, callback, param, cookie] {
            content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
            if (!webview) {
                callback(NULL_WEBVIEW, param, MINI_ELECTRON_ASYNC_REQUEST_FAIL, nullptr);
                delete cookie;
                return;
            }
            callback(webviewHandle, param, MINI_ELECTRON_ASYNC_REQUEST_OK, cookie->c_str());
            delete cookie;
        });
    });
}

const utf8* MINI_ELECTRON_CALL_TYPE mini_electron_get_cookie_on_blink_thread(mini_electron_web_view webviewHandle)
{
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return NULL;
    std::string cookie = webview->getCookie();
    return createTempCharString(cookie.c_str(), cookie.size());
}

static BOOL netHoldJobToAsynCommit(mini_electron_net_job jobPtr)
{
    checkThreadCallIsValid(__FUNCTION__);
    mini_electron::WebURLLoaderInternal* job = (mini_electron::WebURLLoaderInternal*)jobPtr;
    if (job->m_isRedirection || job->m_isSynchronous || job->m_isHoldJobToAsynCommit)
        return FALSE;

    job->m_hasResponseOverrideData = false;
    if (job->m_responseOverrideData)
        delete job->m_responseOverrideData;
    job->m_responseOverrideData = nullptr;

    if (job->m_hookBufForEndHook)
        delete job->m_hookBufForEndHook;
    job->m_hookBufForEndHook = nullptr;
    job->m_isHookRequest &= (~((unsigned int)1));
    job->m_isHoldJobToAsynCommit = true;

    return TRUE;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_hold_job_to_asyn_commit(mini_electron_net_job jobPtr)
{
    if (content::ThreadCall::isBlinkThread()) {
        netHoldJobToAsynCommit(jobPtr);
    } else {
        content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [jobPtr] { netHoldJobToAsynCommit(jobPtr); });
    }
}

static void netContinueJob(mini_electron_net_job jobPtr)
{
    checkThreadCallIsValid(__FUNCTION__);
    mini_electron::WebURLLoaderInternal* job = (mini_electron::WebURLLoaderInternal*)jobPtr;
    mini_electron::WebURLLoaderManager::sharedInstance()->continueJob(job);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_continue_job(mini_electron_net_job jobPtr)
{
    if (content::ThreadCall::isBlinkThread()) {
        netContinueJob(jobPtr);
    } else {
        content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [jobPtr] { netContinueJob(jobPtr); });
    }
}

static void setCookieJarFullPathImpl(mini_electron_web_view webView, const std::u16string& path)
{
    std::string jarPathA = base::UTF16ToUTF8(std::u16string_view(path));
    mini_electron::WebURLLoaderManager::setCookieJarFullPath(jarPathA.c_str());
}

static void setLocalStorageFullPathImpl(mini_electron_web_view webView, const std::u16string& path)
{
    std::string pathA = base::UTF16ToUTF8(path);
    mini_electron::setDefaultLocalStorageDir(pathA);
}

static void setFullPathOnBlinkThread(mini_electron_web_view webviewHandle, std::u16string* pathString, bool isCookiePath)
{
    if (!pathString)
        return;

    if (!webviewHandle) {
        isCookiePath ? setCookieJarFullPathImpl(NULL_WEBVIEW, *pathString) : setLocalStorageFullPathImpl(NULL_WEBVIEW, *pathString);
    } else {
        content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
        if (webview) {
            std::string pathUtf8 = base::UTF16ToUTF8(pathString->c_str());
            if (isCookiePath)
                webview->setCookieJarFullPath(pathUtf8.c_str());
            else
                webview->setLocalStorageFullPath(pathUtf8.c_str());
        }
    }
    delete pathString;
}

void setFullPath(mini_electron_web_view webviewHandle, const WCHAR* path, bool isCookiePath)
{
    if (!path)
        return;
    std::u16string* pathString = new std::u16string((const char16_t*)path);
    if (0 == pathString->size()) {
        delete pathString;
        return;
    }

    if (content::ThreadCall::isBlinkThread()) {
        setFullPathOnBlinkThread(webviewHandle, pathString, isCookiePath);
    } else {
        content::ThreadCall::callBlinkThreadAsync(
            FROM_HERE, [webviewHandle, pathString, isCookiePath] { setFullPathOnBlinkThread(webviewHandle, pathString, isCookiePath); });
    }
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_on_response(mini_electron_web_view webviewHandle, mini_electron_net_response_callback callback, void* param)
{
    checkThreadCallIsValid(__FUNCTION__);
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)webviewHandle);
    if (!webview)
        return;
    webview->getClosure().setNetResponseCallback(callback, param);
}
