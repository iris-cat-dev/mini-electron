
#include <stdlib.h>
#include <crtdbg.h>
#include <limits>
#include <array>
#include "src/runtime/engine/public/engine_api.h"
#include "src/runtime/engine/download/simple_download.h"
#include "src/runtime/engine/bindings/js_value.h"
#include "src/runtime/engine/browser/web_view_host.h"
#include "src/runtime/engine/browser/shared_timer_win.h"
#include "src/runtime/engine/common/live_id_detect.h"
#include "src/runtime/engine/common/thread_call.h"
#include "src/runtime/engine/common/utf16.h"
#include "src/runtime/engine/renderer/render_thread_impl.h"
#include "src/runtime/network/loader/web_url_loader_internal.h"
#include "src/runtime/network/loader/web_url_loader_manager.h"
#include "src/runtime/network/loader/initialize_handle_info.h"
#include "src/runtime/network/loader/web_url_loader_manager_setup_info.h"
#include "src/runtime/network/loader/flatten_http_body_element.h"
#include "src/runtime/storage/default_local_storage_dir.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/platform/wtf/allocator/partitions.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/public/platform/web_http_header_visitor.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/resource_request_body.h"
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
void onNetSetHTTPStatus(mini_electron_net_job jobPtr, int status,
    const char* status_text);
void onNetSetResponseURL(mini_electron_net_job jobPtr, const char* url);
void onNetSetHTTPHeaderFieldCommon(mini_electron_net_job jobPtr, const utf8* key, const utf8* value, BOOL response);
void changeRequestUrl(mini_electron_net_job jobPtr, const char* url);
mini_electron_resource_type webURLRequestToResourceType(
    const network::ResourceRequest& request,
    blink::mojom::RequestContextFrameType frameType);

namespace {

// Self-owned on the job's curl IO sequence until EOF, failure or job teardown.
// Only the broker receives these bytes; the moved upload is never sent by curl.
class BrokerBodyReader {
public:
    using Reply = base::OnceCallback<void(std::vector<uint8_t>, std::string)>;

    BrokerBodyReader(size_t limit, scoped_refptr<base::SingleThreadTaskRunner> reply_runner, Reply reply)
        : limit_(limit), reply_runner_(std::move(reply_runner)), reply_(std::move(reply))
    {
    }

    base::WeakPtr<BrokerBodyReader> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

    void Start(std::vector<std::unique_ptr<FlattenHTTPBodyElement>> elements, bool failed)
    {
        auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();
        stream_ = std::make_unique<FlattenHTTPBodyElementStream>(std::move(elements), failed,
            base::BindRepeating([](scoped_refptr<base::SingleThreadTaskRunner> runner,
                                    base::WeakPtr<BrokerBodyReader> reader) {
                // Getter callbacks may run inline from read(); never re-enter it.
                runner->PostTask(FROM_HERE, base::BindOnce(&BrokerBodyReader::Read, reader));
            }, std::move(runner), GetWeakPtr()));
        Read();
    }

    void Cancel() { Finish("Upload canceled"); }

private:
    void Read()
    {
        std::array<uint8_t, 64 * 1024> buffer;
        for (;;) {
            const size_t count = stream_->read(buffer.data(), 1, buffer.size());
            if (count == CURL_READFUNC_PAUSE)
                return;
            if (count == CURL_READFUNC_ABORT) {
                Finish("Unable to read upload body");
                return;
            }
            if (!count) {
                Finish({});
                return;
            }
            if (count > limit_ - bytes_.size()) {
                Finish("Upload body exceeds broker message limit");
                return;
            }
            bytes_.insert(bytes_.end(), buffer.begin(), buffer.begin() + count);
        }
    }

    void Finish(std::string error)
    {
        if (!error.empty())
            bytes_.clear();
        reply_runner_->PostTask(FROM_HERE,
            base::BindOnce(std::move(reply_), std::move(bytes_), std::move(error)));
        delete this;
    }

    const size_t limit_;
    scoped_refptr<base::SingleThreadTaskRunner> reply_runner_;
    Reply reply_;
    std::vector<uint8_t> bytes_;
    std::unique_ptr<FlattenHTTPBodyElementStream> stream_;
    base::WeakPtrFactory<BrokerBodyReader> weak_factory_ { this };
};

} // namespace

void ReadHeldRequestBodyForBroker(WebURLLoaderInternal* job, size_t max_bytes,
    BrokerBodyReader::Reply reply)
{
    auto reply_runner = base::SingleThreadTaskRunner::GetCurrentDefault();
    if (!job || !job->m_isHoldJobToAsynCommit || !job->m_ioThread
        || job->m_brokerBodyReadCleanup) {
        reply_runner->PostTask(FROM_HERE,
            base::BindOnce(std::move(reply), std::vector<uint8_t> {}, "Upload job is not available"));
        return;
    }

    SetupDataInfo* prepared = nullptr;
    if (auto* info = job->m_initializeHandleInfo) {
        if (info->methodInfo) {
            if (info->methodInfo->post)
                prepared = info->methodInfo->post->data;
            else if (info->methodInfo->put)
                prepared = info->methodInfo->put->data;
        }
    }
    std::vector<std::unique_ptr<FlattenHTTPBodyElement>> elements;
    bool failed = false;
    if (prepared) {
        elements = std::move(prepared->flattenElements);
        failed = prepared->failed;
        // Ownership was transferred to the broker. A mistaken curl continuation
        // must fail instead of uploading an empty body or replaying a spent pipe.
        prepared->failed = true;
    } else if (const auto& body = job->firstRequest()->request_body) {
        // Methods outside POST/PUT have no prepared curl body. Preserve their
        // real request body too, without cloning a read-once getter.
        for (auto& element : *body->elements_mutable()) {
            auto flattened = std::make_unique<FlattenHTTPBodyElement>();
            if (element.type() == network::DataElement::Tag::kBytes) {
                const auto& bytes = element.As<network::DataElementBytes>().bytes();
                flattened->data.assign(bytes.begin(), bytes.end());
            } else if (element.type() == network::DataElement::Tag::kFile) {
                const auto& file = element.As<network::DataElementFile>();
                flattened->type = FlattenHTTPBodyElement::TypeFile;
                flattened->file.Initialize(file.path(), base::File::FLAG_OPEN | base::File::FLAG_READ);
                base::File::Info info;
                if (!flattened->file.GetInfo(&info) || info.is_directory || info.size < 0
                    || file.offset() > static_cast<uint64_t>(info.size)) {
                    failed = true;
                    break;
                }
                flattened->fileStart = file.offset();
                flattened->fileLength = std::min(file.length(), static_cast<uint64_t>(info.size) - file.offset());
                flattened->expectedModificationTime = file.expected_modification_time();
            } else if (element.type() == network::DataElement::Tag::kDataPipe) {
                flattened->type = FlattenHTTPBodyElement::TypeDataPipe;
                flattened->pendingGetter = element.As<network::DataElementDataPipe>().ReleaseDataPipeGetter();
            } else if (element.type() == network::DataElement::Tag::kChunkedDataPipe) {
                auto& pipe = element.As<network::DataElementChunkedDataPipe>();
                flattened->type = FlattenHTTPBodyElement::TypeChunkedDataPipe;
                flattened->readOnlyOnce = pipe.read_only_once().value();
                flattened->pendingChunkedGetter = pipe.ReleaseChunkedDataPipeGetter();
            } else {
                failed = true;
                break;
            }
            elements.push_back(std::move(flattened));
        }
    }

    if (elements.empty() && !failed) {
        reply_runner->PostTask(FROM_HERE,
            base::BindOnce(std::move(reply), std::vector<uint8_t> {}, std::string {}));
        return;
    }

    auto io_runner = job->m_ioThread->task_runner();
    auto* reader = new BrokerBodyReader(max_bytes, std::move(reply_runner), std::move(reply));
    job->m_brokerBodyReadCleanup.ReplaceClosure(base::BindOnce(
        [](scoped_refptr<base::SingleThreadTaskRunner> runner, base::WeakPtr<BrokerBodyReader> reader) {
            runner->PostTask(FROM_HERE, base::BindOnce(&BrokerBodyReader::Cancel, reader));
        }, io_runner, reader->GetWeakPtr()));
    io_runner->PostTask(FROM_HERE,
        base::BindOnce(&BrokerBodyReader::Start, base::Unretained(reader), std::move(elements), failed));
}

} // namespace mini_electron

void MINI_ELECTRON_CALL_TYPE mini_electron_net_set_http_header_field_utf8(mini_electron_net_job jobPtr, const utf8* key, const utf8* value, BOOL response)
{
    mini_electron::onNetSetHTTPHeaderFieldCommon(jobPtr, key, value, response);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_set_response_url(
    mini_electron_net_job jobPtr, const char* url)
{
    mini_electron::onNetSetResponseURL(jobPtr, url ? url : "");
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_set_http_status(
    mini_electron_net_job jobPtr, int status, const char* status_text)
{
    mini_electron::onNetSetHTTPStatus(jobPtr, status,
        status_text ? status_text : "");
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
const char* MINI_ELECTRON_CALL_TYPE
mini_electron_net_get_request_method_string(void* jobPtr)
{
    checkThreadCallIsValid(__FUNCTION__);
    auto* job = static_cast<mini_electron::WebURLLoaderInternal*>(jobPtr);
    if (job->m_initializeHandleInfo)
        return job->m_initializeHandleInfo->method.c_str();
    return job->firstRequest()->method.c_str();
}
BOOL MINI_ELECTRON_CALL_TYPE mini_electron_net_get_request_info(
    mini_electron_net_job jobPtr, mini_electron_net_request_info* info)
{
    checkThreadCallIsValid(__FUNCTION__);
    if (!jobPtr || !info)
        return FALSE;
    auto* job = static_cast<mini_electron::WebURLLoaderInternal*>(jobPtr);
    const network::ResourceRequest* request = job->firstRequest();
    if (!request)
        return FALSE;
    *info = {};
    info->frame_id = job->m_frameId;
    info->parent_frame_id = job->m_parentFrameId;
    info->is_main_frame = job->m_isMainFrame ? TRUE : FALSE;
    info->is_document =
        job->m_frameType != blink::mojom::RequestContextFrameType::kNone
            || request->destination == network::mojom::RequestDestination::kDocument
            || request->destination == network::mojom::RequestDestination::kFrame
            || request->destination == network::mojom::RequestDestination::kIframe
        ? TRUE
        : FALSE;
    info->initiator = job->m_requestInitiator.c_str();
    info->credentials_mode = static_cast<int>(request->credentials_mode);
    info->request_mode = static_cast<int>(request->mode);
    info->resource_type = request->resource_type;
    info->destination = static_cast<int>(request->destination);
    return TRUE;
}
mini_electron_resource_type MINI_ELECTRON_CALL_TYPE
mini_electron_net_get_resource_type(mini_electron_net_job jobPtr)
{
    checkThreadCallIsValid(__FUNCTION__);
    if (!jobPtr)
        return MINI_ELECTRON_RESOURCE_TYPE_LAST_TYPE;
    auto* job = static_cast<mini_electron::WebURLLoaderInternal*>(jobPtr);
    const network::ResourceRequest* request = job->firstRequest();
    return request
        ? mini_electron::webURLRequestToResourceType(
            *request, job->m_frameType)
        : MINI_ELECTRON_RESOURCE_TYPE_LAST_TYPE;
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
    if (!element)
        return;
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

// Snapshot request metadata, not the upload stream. Flattened elements own file
// handles/getters and move to the curl IO sequence; introspection must never read,
// clone or release a pipe (in particular, a read-once chunked getter).
static mini_electron_post_body_elements* requestBodyToEmbedder(const network::ResourceRequestBody* body)
{
    if (body) {
        for (const auto& element : *body->elements()) {
            if (element.type() == network::DataElement::Tag::kFile) {
                const auto& file = element.As<network::DataElementFile>();
                if (file.path().empty()
                    || file.offset() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                    || (file.length() != std::numeric_limits<uint64_t>::max()
                        && file.length() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))
                    return nullptr;
            } else if (element.type() != network::DataElement::Tag::kBytes) {
                // The public bytes/path representation cannot describe a pipe.
                // Refuse the entire snapshot, rather than silently dropping it.
                return nullptr;
            }
        }
    }

    const size_t count = body ? body->elements()->size() : 0;
    auto* result = count
        ? mini_electron_net_create_post_body_elements(NULL_WEBVIEW, count)
        : new mini_electron_post_body_elements {};
    result->size = sizeof(mini_electron_post_body_elements);
    result->isDirty = false;
    for (size_t i = 0; i < count; ++i) {
        auto* bodyElement = mini_electron_net_create_post_body_element(NULL_WEBVIEW);
        result->element[i] = bodyElement;
        const auto& element = (*body->elements())[i];
        if (element.type() == network::DataElement::Tag::kFile) {
            const auto& file = element.As<network::DataElementFile>();
            const std::string path = file.path().AsUTF8Unsafe();
            bodyElement->type = mini_electron_http_body_element_type_file;
            bodyElement->filePath = mini_electron_create_string_with_copy(path.data(), path.size());
            bodyElement->fileStart = static_cast<int64_t>(file.offset());
            bodyElement->fileLength = file.length() == std::numeric_limits<uint64_t>::max()
                ? -1 : static_cast<int64_t>(file.length());
        } else {
            const auto& bytes = element.As<network::DataElementBytes>().bytes();
            bodyElement->type = mini_electron_http_body_element_type_data;
            if (!bytes.empty()) {
                bodyElement->data = mini_electron_create_mem_buf(NULL_WEBVIEW,
                    const_cast<uint8_t*>(bytes.data()), bytes.size());
                if (!bodyElement->data) {
                    mini_electron_net_free_post_body_elements(result);
                    return nullptr;
                }
            }
        }
    }
    return result;
}

mini_electron_post_body_elements* MINI_ELECTRON_CALL_TYPE mini_electron_net_get_post_body(void* jobPtr)
{
    checkThreadCallIsValid(__FUNCTION__);
    auto* job = static_cast<mini_electron::WebURLLoaderInternal*>(jobPtr);
    const network::ResourceRequest* request = job ? job->firstRequest() : nullptr;
    return request ? requestBodyToEmbedder(request->request_body.get()) : nullptr;
}

static void netFreePostBodyElements(mini_electron_post_body_elements* elements)
{
    checkThreadCallIsValid(__FUNCTION__);
    if (!elements)
        return;
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
