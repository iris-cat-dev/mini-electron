
#ifndef net_WebURLLoaderManagerMainTask_h
#define net_WebURLLoaderManagerMainTask_h

#include "runtime/network/loader/web_url_loader_internal.h"
#include "runtime/network/loader/web_url_loader_manager_util.h"
#include "runtime/network/loader/web_url_loader_manager_async_task.h"
#include "runtime/network/loader/request_extra_data.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/renderer/platform/network/http_parsers.h"
#include "net/http/http_util.h"
#include "third_party/blink/renderer/platform/wtf/text/string_utf8_adaptor.h"
#include "third_party/blink/public/mojom/loader/resource_load_info.mojom-shared.h"
#include "base/synchronization/lock.h"


namespace mini_electron {

struct URLError {
    std::string domain;
    int reason;
    bool staleCopyInCache;
    bool isCancellation;
    bool wasIgnoredByHandler;
    std::string unreachableURL;
    std::string localizedDescription;

    URLError()
        : reason(0)
        , staleCopyInCache(false)
        , isCancellation(false)
        , wasIgnoredByHandler(false)
    {
    }

    blink::WebURLError toWebURLError()
    {
        blink::WebURLError result(reason, blink::KURL(unreachableURL.c_str()));

        //         result.domain = blink::WebString::fromUTF8(domain.c_str());
        //         result.reason = reason;
        //         result.staleCopyInCache = staleCopyInCache;
        //         result.isCancellation = isCancellation;
        //         result.wasIgnoredByHandler = wasIgnoredByHandler;
        //         result.unreachableURL = blink::KURL(blink::ParsedURLString, unreachableURL.c_str());
        //         result.localizedDescription = blink::WebString::fromUTF8(localizedDescription.c_str());
        return result;
    }
};

struct MainTaskArgs {
    void* ptr;
    size_t size;
    size_t nmemb;
    long httpCode;
    double contentLength;
    char* hdr;
    URLError* resourceError;

    int ref;

    ~MainTaskArgs()
    {
        free(ptr);
        free(hdr);
        delete resourceError;
    }

    static MainTaskArgs* build(void* ptr, size_t size, size_t nmemb, size_t totalSize, CURL* handle, bool isProxyConnect)
    {
        MainTaskArgs* args = new MainTaskArgs();
        args->size = size;
        args->nmemb = nmemb;
        args->ptr = malloc(totalSize);
        args->resourceError = new URLError();
        args->ref = 0;
        memcpy(args->ptr, ptr, totalSize);

        //         curl_easy_getinfo(handle, !isProxy ? CURLINFO_RESPONSE_CODE : CURLINFO_HTTP_CONNECTCODE, &args->httpCode);
        //         if (isProxy && 0 == args->httpCode)
        //             args->httpCode = 200;
        curl_easy_getinfo(
            handle, !isProxyConnect ? CURLINFO_RESPONSE_CODE : CURLINFO_HTTP_CONNECTCODE, &args->httpCode); // 只有使用了代理的Connect请求才需要特殊处理

        curl_easy_getinfo(handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &args->contentLength);

        const char* hdr = nullptr;
        args->hdr = nullptr;
        int hdrLen = 0;
        curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &hdr);
        if (hdr)
            hdrLen = strlen(hdr);

        args->hdr = (char*)malloc(hdrLen + 1);
        strncpy(args->hdr, hdr, hdrLen);
        args->hdr[hdrLen] = '\0';
        return args;
    }
};

class MainTaskRunner {
public:
    MainTaskRunner()
        : m_isDestroying(false)
    {
    }
    ~MainTaskRunner()
    {
    }
    void willProcessTask();

    static void destroy();
    static void add(WebURLLoaderManagerMainTask* task)
    {
        if (!m_inst) {
            m_inst = new MainTaskRunner();
            //blink::Platform::current()->mainThread()->addTaskObserver(m_inst);
        }

        //m_inst->addTask(task);
    }

private:
    void destroyImpl();
    //void addTask(WebURLLoaderManagerMainTask* task);

    static MainTaskRunner* m_inst;
    bool m_isDestroying;
    base::Lock m_mutex;
    WTF::Vector<WebURLLoaderManagerMainTask*> m_list;
};

void WebURLLoaderInternal::release(int jobId)
{
    //DCHECK(WTF::IsMainThread()); // 可能在web worker线程
    WebURLLoaderManager* manager = WebURLLoaderManager::sharedInstance();
    AutoLockJob autoLockJob(manager, jobId);
    autoLockJob.setNotDerefForDelete();
    WebURLLoaderInternal* job = autoLockJob.lock();
    if (!job)
        return;

    WTF::RecursiveMutex& liveJobsMutex = manager->m_liveJobsMutex;

    while (true) {
        job->m_destroingMutex->lock(); // 不需要unlock了，因为job直接会被析构
        liveJobsMutex.lock();
        if (2 < job->getRefCount()) {
            liveJobsMutex.unlock();
            job->m_destroingMutex->unlock();
            ::Sleep(16);
            continue;
        }

        job->m_handle = nullptr;
        manager->removeLiveJobs(jobId);

        std::unique_ptr<WTF::RecursiveMutex> destroingMutex(job->m_destroingMutex.release());
        delete job; // !!!!!!!!!!!!!!!!!!!!!!!!!
        destroingMutex->unlock();
        destroingMutex.reset();

        liveJobsMutex.unlock();
        break;
    }
}

// 回调回main线程的task
class WebURLLoaderManagerMainTask /*: public blink::WebThread::Task*/ {
public:
    enum TaskType {
        kWriteCallback,
        kHeaderCallback,
        kDidFinishLoading,
        kRemoveFromCurl,
        kDidCancel,
        kHandleLocalReceiveResponse,
        kContentEnded,
        kDidFail,
        kHandleHookRequest,
        kDidSentData,
    };

    WebURLLoaderManagerMainTask()
    {
    }

    /*virtual*/ ~WebURLLoaderManagerMainTask() /*override*/
    {
        delete m_args;
    }

    /*virtual*/ void run() /*override*/
    {
        AutoLockJob autoLockJob(WebURLLoaderManager::sharedInstance(), m_jobId);
        WebURLLoaderInternal* job = autoLockJob.lock();
        if (!job)
            return;

        dispatchHook(job, m_type, m_args->ptr, m_args->size, m_args->nmemb);

        if (kRemoveFromCurl == m_type || kDidCancel == m_type) {
            WebURLLoaderInternal::release(m_jobId);
            return;
        }

        if (WebURLLoaderManager::sharedInstance()->isShutdown() || job->isCancelled())
            return;

        switch (m_type) {
        case kWriteCallback:
            handleWriteCallbackOnMainThread(m_args, job);
            break;
        case kHeaderCallback:
            handleHeaderCallbackOnMainThread(m_args, job);
            break;
        case kDidFinishLoading:
            if (job->m_hookBufForEndHook)
                WebURLLoaderManager::sharedInstance()->didReceiveDataOrDownload(
                    job, job->m_hookBufForEndHook->data(), job->m_hookBufForEndHook->size(), job->m_dataLength);
            WebURLLoaderManager::sharedInstance()->handleDidFinishLoading(job, base::TimeTicks::Now().ToInternalValue(), job->m_dataLength);
            break;
        case kRemoveFromCurl:
            break;
        case kDidCancel:
            break;
        case kHandleLocalReceiveResponse:
            handleLocalReceiveResponseOnMainThread(m_args, job);
            break;
        case kContentEnded:
            break;
        case kDidFail:
            WebURLLoaderManager::sharedInstance()->handleDidFail(job, m_args->resourceError->toWebURLError());
            break;
        case kHandleHookRequest:
            handleHookRequestOnMainThread(job);
            break;
        case kDidSentData:
            handleDidSendData(m_args, job);
            break;
        default:
            break;
        }
    }

    static void dispatchHook(WebURLLoaderInternal* job, TaskType type, void* ptr, size_t size, size_t nmemb)
    {
        if (kWriteCallback == type) {
            if (WebURLLoaderInternal::kCacheForDownloadNot != job->m_cacheForDownloadOpt) {
                //job->m_dataCacheForDownload.append((char*)ptr, size * nmemb);
                job->m_dataCacheForDownload.insert(job->m_dataCacheForDownload.end(), (char*)ptr, (char*)ptr + size * nmemb);
            }

            if (job->m_dataBind) {
                job->m_dataBind->recvCallback(job->m_dataBind->param, job, job->m_dataCacheForDownload.data(), job->m_dataCacheForDownload.size());
                job->m_dataCacheForDownload.clear();
            }
        } else if (kDidCancel == type) {
            if (job->m_dataBind) {
                job->m_dataBind->finishCallback(job->m_dataBind->param, job, MINI_ELECTRON_LOADING_CANCELED);
                job->m_dataCacheForDownload.clear();
            }
        } else if (kDidFail == type) {
            if (job->m_dataBind) {
                job->m_dataBind->finishCallback(job->m_dataBind->param, job, MINI_ELECTRON_LOADING_FAILED);
                job->m_dataCacheForDownload.clear();
            }
        } else if (kDidFinishLoading == type) {
            if (job->m_dataBind) {
                job->m_dataBind->finishCallback(job->m_dataBind->param, job, MINI_ELECTRON_LOADING_SUCCEEDED);
                job->m_dataCacheForDownload.clear();
            }
        }
    }

    static void createAndPushTask(int jobId, TaskType type, void* ptr, size_t size, size_t nmemb, size_t totalSize)
    {
        AutoLockJob autoLockJob(WebURLLoaderManager::sharedInstance(), jobId);
        WebURLLoaderInternal* job = autoLockJob.lock();
        if (!job)
            return;

        MainTaskArgs* args = MainTaskArgs::build(ptr, size, nmemb, totalSize, job->m_handle, job->m_isProxyConnect);
        WebURLLoaderManagerMainTask* task = new WebURLLoaderManagerMainTask(jobId, type, args);

        pushTask(job, job->m_taskRunner, task);
    }

private:
    static void runMainTasks()
    {
        WebURLLoaderManager* manager = WebURLLoaderManager::sharedInstance();
        CHECK(manager->getMainRunner()->BelongsToCurrentThread());

        while (true) {
            manager->m_mainTasksMutex.Acquire();
            WebURLLoaderManagerMainTask* task = manager->m_mainTasksBegin; // 从头部读起
            if (!task) {
                manager->m_mainTasksMutex.Release();
                break;
            }
            manager->m_mainTasksBegin = task->m_next;
            if (nullptr == manager->m_mainTasksBegin)
                manager->m_mainTasksEnd = nullptr;
            manager->m_mainTasksMutex.Release();

            if (!task)
                break;
            task->run();
            delete task;
        }
    }

    static void runCrossThreadTasks(int jobId)
    {
        while (true) {
            WebURLLoaderManagerMainTask* task = nullptr;
            {
                AutoLockJob autoLockJob(WebURLLoaderManager::sharedInstance(), jobId);
                WebURLLoaderInternal* job = autoLockJob.lock();
                if (!job)
                    break;

                job->m_syncTasksLock.Acquire();
                task = job->m_crossThreadTasksBegin; // 从头部读起
                if (!task) {
                    job->m_syncTasksLock.Release();
                    break;
                }
                job->m_crossThreadTasksBegin = task->m_next;
                job->m_syncTasksLock.Release();
            }
            if (task) {
                task->run();
                delete task;
            }
        }
    }

public:
    static void pushTask(WebURLLoaderInternal* job, scoped_refptr<base::SequencedTaskRunner> taskRunner, WebURLLoaderManagerMainTask* task)
    {
        if (!task)
            return;

        if (job && job->m_isSynchronous) { // 现在只有web worker的同步请求才会走到这里
            job->m_syncTasksLock.Acquire();
            job->m_syncTasks.push_back(task);
            job->m_syncTasksLock.Release();
            return;
        }

        if (job && job->m_isCrossThread) { // 老版本的非web worker的同步请求在mbnet里处理。新版本是blink创建线程来处理了
            job->m_syncTasksLock.Acquire();
            
            if (job->m_crossThreadTasksBegin) {
                WebURLLoaderManagerMainTask* itTask = job->m_crossThreadTasksBegin; // 尾部插入
                while (itTask) {
                    if (!itTask->m_next) {
                        itTask->m_next = task;
                        break;
                    }
                    itTask = itTask->m_next;
                }
            } else {
                job->m_crossThreadTasksBegin = task;
            }
            job->m_syncTasksLock.Release();

            taskRunner->PostTask(FROM_HERE, base::BindOnce(&WebURLLoaderManagerMainTask::runCrossThreadTasks, job->m_jobId));
            return;
        }
        WebURLLoaderManager* manager = WebURLLoaderManager::sharedInstance();

        base::AutoLock locker(manager->m_mainTasksMutex);
        bool needPost = false;
        if (nullptr == manager->m_mainTasksEnd) { // 感觉好像不需要这个m_mainTasksEnd？
            manager->m_mainTasksEnd = task;
            needPost = true;
            DCHECK(nullptr == manager->m_mainTasksBegin);
        } else {
            DCHECK(nullptr != manager->m_mainTasksBegin);
            manager->m_mainTasksEnd->m_next = task; // 在尾部插入
            manager->m_mainTasksEnd = task;
        }

        if (nullptr == manager->m_mainTasksBegin)
            manager->m_mainTasksBegin = task;

        // removeFromCurlOnIoThread的时候，job->m_taskRunner可能和getMainRunner不相同
        CHECK(taskRunner == WebURLLoaderManager::sharedInstance()->getMainRunner());
        if (needPost)
            taskRunner->PostTask(FROM_HERE, base::BindOnce(&WebURLLoaderManagerMainTask::runMainTasks));
    }

    static WebURLLoaderManagerMainTask* createTask(int jobId, TaskType type, void* ptr, size_t size, size_t nmemb, size_t totalSize)
    {
        AutoLockJob autoLockJob(WebURLLoaderManager::sharedInstance(), jobId);
        WebURLLoaderInternal* job = autoLockJob.lock();
        if (!job)
            return nullptr;
        MainTaskArgs* args = MainTaskArgs::build(ptr, size, nmemb, totalSize, job->m_handle, job->m_isProxyConnect);
        WebURLLoaderManagerMainTask* task = new WebURLLoaderManagerMainTask(jobId, type, args);
        return task;
    }

    static void createAndPushResErrTask(int jobId, URLError* resourceError)
    {
        AutoLockJob autoLockJob(WebURLLoaderManager::sharedInstance(), jobId);
        WebURLLoaderInternal* job = autoLockJob.lock();
        if (!job)
            return;

        WebURLLoaderManagerMainTask* task = createTask(jobId, WebURLLoaderManagerMainTask::TaskType::kDidFail, nullptr, 0, 0, 0);
        delete task->m_args->resourceError;

        task->m_args->resourceError = resourceError;

        pushTask(job, job->m_taskRunner, task);
    }

    static size_t handleWriteCallbackOnMainThread(MainTaskArgs* args, WebURLLoaderInternal* job);
    static size_t handleHeaderCallbackOnMainThread(MainTaskArgs* args, WebURLLoaderInternal* job);
    static void handleLocalReceiveResponseOnMainThread(MainTaskArgs* args, WebURLLoaderInternal* job);
    static void handleHookRequestOnMainThread(WebURLLoaderInternal* job);
    static void handleDidSendData(MainTaskArgs* args, WebURLLoaderInternal* job);

private:
    int m_jobId;
    TaskType m_type;
    MainTaskArgs* m_args;
    WebURLLoaderManagerMainTask* m_next;

    WebURLLoaderManagerMainTask(int jobId, TaskType type, MainTaskArgs* args)
        : m_jobId(jobId)
        , m_type(type)
        , m_args(args)
        , m_next(nullptr)
    {
    }
};

static void checkList(const WTF::Vector<WebURLLoaderManagerMainTask*>& tasks)
{
    for (size_t i = 0; i < tasks.size(); ++i) {
        WebURLLoaderManagerMainTask* task = tasks[i];
        CHECK(!task);
    }
}

void MainTaskRunner::willProcessTask()
{
    for (size_t i = 0;; ++i) {
        m_mutex.Acquire();
        if (i >= m_list.size()) {
            checkList(m_list);
            m_list.clear();
            m_mutex.Release();
            break;
        }
        WebURLLoaderManagerMainTask* task = m_list[i];
        m_list[i] = nullptr;
        m_mutex.Release();

        if (!task)
            continue;
        task->run();
        delete task;
    }
}

void MainTaskRunner::destroy()
{
    if (!m_inst)
        return;
    m_inst->destroyImpl();
}

void MainTaskRunner::destroyImpl()
{
    m_mutex.Acquire();
    m_isDestroying = true;

    size_t size = m_list.size();
    for (size_t i = 0; i < size; ++i) {
        WebURLLoaderManagerMainTask* task = m_list[i];
        delete task;
    }
    CHECK(size == m_list.size());

    m_list.clear();
    m_mutex.Release();
}

mini_electron_resource_type webURLRequestToResourceType(const network::ResourceRequest& request, blink::mojom::RequestContextFrameType frameType)
{
    blink::mojom::ResourceType requestContext = (blink::mojom::ResourceType)request.resource_type;
    if (frameType != blink::mojom::RequestContextFrameType::kNone) {
        //         DCHECK(requestContext == blink::WebURLRequest::RequestContextForm ||
        //             requestContext == blink::WebURLRequest::RequestContextFrame ||
        //             requestContext == blink::WebURLRequest::RequestContextHyperlink ||
        //             requestContext == blink::WebURLRequest::RequestContextIframe ||
        //             requestContext == blink::WebURLRequest::RequestContextInternal ||
        //             requestContext == blink::WebURLRequest::RequestContextLocation);
        if (frameType == blink::mojom::RequestContextFrameType::kTopLevel || frameType == blink::mojom::RequestContextFrameType::kAuxiliary) {
            return MINI_ELECTRON_RESOURCE_TYPE_MAIN_FRAME;
        }
        if (frameType == blink::mojom::RequestContextFrameType::kNested)
            return MINI_ELECTRON_RESOURCE_TYPE_SUB_FRAME;
        return MINI_ELECTRON_RESOURCE_TYPE_SUB_RESOURCE;
    }

    switch (requestContext) {
    case blink::mojom::ResourceType::kMainFrame:
        return MINI_ELECTRON_RESOURCE_TYPE_MAIN_FRAME;
    case blink::mojom::ResourceType::kSubFrame:
        return MINI_ELECTRON_RESOURCE_TYPE_SUB_FRAME;

        // Favicon
    case blink::mojom::ResourceType::kFavicon:
        return MINI_ELECTRON_RESOURCE_TYPE_FAVICON;

        // Font
    case blink::mojom::ResourceType::kFontResource:
        return MINI_ELECTRON_RESOURCE_TYPE_FONT_RESOURCE;

        // Image
    case blink::mojom::ResourceType::kImage:
        //case blink::mojom::ResourceType::kImageSet:
        return MINI_ELECTRON_RESOURCE_TYPE_IMAGE;

        // Media
    case blink::mojom::ResourceType::kMedia:
        return MINI_ELECTRON_RESOURCE_TYPE_MEDIA;

        // Object
    //case blink::mojom::ResourceType::kEmbed:
    case blink::mojom::ResourceType::kObject:
        return MINI_ELECTRON_RESOURCE_TYPE_OBJECT;

        // Ping
    //case blink::mojom::ResourceType::kBeacon:
    //case blink::mojom::ResourceType::kCSPReport:
    case blink::mojom::ResourceType::kPing:
        return MINI_ELECTRON_RESOURCE_TYPE_PING;

        // Prefetch
    case blink::mojom::ResourceType::kPrefetch:
        return MINI_ELECTRON_RESOURCE_TYPE_PREFETCH;

        // Script
    //case blink::mojom::ResourceType::kImport:
    case blink::mojom::ResourceType::kScript:
        return MINI_ELECTRON_RESOURCE_TYPE_SCRIPT;

        // Style
    //case blink::mojom::ResourceType::kXSLT:
    case blink::mojom::ResourceType::kStylesheet:
        return MINI_ELECTRON_RESOURCE_TYPE_STYLESHEET;

        // Subresource
    //case blink::mojom::ResourceType::kDownload:
    //case blink::mojom::ResourceType::kManifest:
    case blink::mojom::ResourceType::kSubResource:
    case blink::mojom::ResourceType::kPluginResource:
        return MINI_ELECTRON_RESOURCE_TYPE_SUB_RESOURCE;

        // TextTrack
        //     case blink::mojom::ResourceType::kTrack:
         

        // Workers
    case blink::mojom::ResourceType::kServiceWorker:
        return MINI_ELECTRON_RESOURCE_TYPE_SERVICE_WORKER;
    case blink::mojom::ResourceType::kNavigationPreloadMainFrame:
        return MINI_ELECTRON_RESOURCE_TYPE_MAIN_FRAME;
    case blink::mojom::ResourceType::kNavigationPreloadSubFrame:
        return MINI_ELECTRON_RESOURCE_TYPE_SUB_FRAME;
    case blink::mojom::ResourceType::kSharedWorker:
        return MINI_ELECTRON_RESOURCE_TYPE_SHARED_WORKER;
    case blink::mojom::ResourceType::kWorker:
        return MINI_ELECTRON_RESOURCE_TYPE_WORKER;

        // Unspecified
        //     case blink::mojom::ResourceType::kInternal:
        //     case blink::mojom::ResourceType::kUnspecified:
         

        // XHR
    case blink::mojom::ResourceType::kXhr:
        //     case blink::mojom::ResourceType::kEventSource:
        //     case blink::mojom::ResourceType::kFetch:
        //     case blink::mojom::ResourceType::kXMLHttpRequest:
        return MINI_ELECTRON_RESOURCE_TYPE_XHR;

        // These should be handled by the FrameType checks at the top of the
        // function.
        //     case blink::mojom::ResourceType::kForm:
        //     case blink::mojom::ResourceType::kHyperlink:
        //     case blink::mojom::ResourceType::kLocation:
        //     case blink::mojom::ResourceType::kFrame:
        //     case blink::mojom::ResourceType::kIframe:
        //         DebugBreak();
         

    case blink::mojom::ResourceType::kCspReport:
    case blink::mojom::ResourceType::kJson:
    default:
        return MINI_ELECTRON_RESOURCE_TYPE_SUB_RESOURCE;
    }
}

static bool isResourceTypeFrame(WebURLLoaderInternal* job)
{
    network::ResourceRequest* request = job->firstRequest();
    if (!request)
        return false;
    //
    //     blink::WebURLRequest::RequestContext requestContext = request->requestContext();
    //     if (request->frameType() != blink::WebURLRequest::FrameTypeNone) {
    //         DCHECK(requestContext == blink::WebURLRequest::RequestContextForm ||
    //             requestContext == blink::WebURLRequest::RequestContextFrame ||
    //             requestContext == blink::WebURLRequest::RequestContextHyperlink ||
    //             requestContext == blink::WebURLRequest::RequestContextIframe ||
    //             requestContext == blink::WebURLRequest::RequestContextInternal ||
    //             requestContext == blink::WebURLRequest::RequestContextLocation);
    //
    //         if (request->frameType() == blink::WebURLRequest::FrameTypeTopLevel || request->frameType() == blink::WebURLRequest::FrameTypeAuxiliary) {
    //             return true; //  RESOURCE_TYPE_MAIN_FRAME;
    //         }
    //         if (request->frameType() == WebURLRequest::FrameTypeNested)
    //             return true; //  RESOURCE_TYPE_SUB_FRAME
    //
    //         DCHECK(false);
    //         return false;
    //     }
    //
    //     return false;

    mini_electron_resource_type type = webURLRequestToResourceType(*request, job->m_frameType);
    if (MINI_ELECTRON_RESOURCE_TYPE_MAIN_FRAME == type || MINI_ELECTRON_RESOURCE_TYPE_SUB_FRAME == type)
        return true;

    return false;
}

// content\public\common\resource_type.cc
// content\browser\loader\mime_type_resource_handler.cc
static bool isDownloadResponse(WebURLLoaderInternal* job, const AtomicString& contentType, const AtomicString& contentLength)
{
    if (!isResourceTypeFrame(job))
        return false;

    if (contentLength == "0")
        return false;

    if (IsContentDispositionAttachment(job->m_response.HttpHeaderField("Content-Disposition")))
        return true;

    if (contentType.empty() || contentType.IsNull())
        return false;

    String contentMime = contentType.LowerASCII();

    const char* disableDownloadMimes[] = { "text/css", "text/javascript", "text/plain", "text/html", "text/xml", "text/xsl", "image/png", "image/gif",
        "image/jpeg", "image/bmp", "image/webp", "image/x-icon", "image/svg+xml", "audio/ogg", "audio/midi", "audio/x-midi", "video/x-msvideo", "video/mpeg",
        "video/mp4", "video/x-ms-wmv", "font/woff2", "font/opentype", "application/xhtml+xml", "application/font-woff", "application/json",
        "application/xhtml+xml", "application/x-javascript", "application/javascript", "application/pdf", nullptr };
    for (int i = 0;; ++i) {
        const char* type = disableDownloadMimes[i];
        if (!type)
            break;
        if (contentMime.StartsWith(type))
            return false;
    }

    return true;
}


bool dispatchDownloadToEmbedder(content::WebViewHost* webview, WebURLLoaderInternal* job, const utf8* url, const AtomicString& contentType, String* downloadName)
{
    job->m_cacheForDownloadOpt = WebURLLoaderInternal::kCacheForDownloadYes;

    String mime = contentType;

    String contentDisposition;
    if (downloadName && !downloadName->empty() && !downloadName->IsNull())
        contentDisposition = String("attachment; filename=") + *downloadName;
    else
        contentDisposition = job->m_response.HttpHeaderField("Content-Disposition");

    mini_electron_net_job_data_bind dataBind = { 0 };
    mini_electron_download_in_blink_thread_callback cb = webview->getClosure().m_DownloadInBlinkThreadCallback;
    if (!cb) {
        job->m_cacheForDownloadOpt = WebURLLoaderInternal::kCacheForDownloadNot;
        return false;
    }

    if (contentDisposition.empty())
        contentDisposition = "";
    if (mime.empty())
        mime = "";

    size_t expectedContentLength = job->m_response.ExpectedContentLength();
    void* param = webview->getClosure().m_DownloadInBlinkThreadParam;
    mini_electron_download_opt opt = cb(webview->getId(), param, expectedContentLength, url, mime.Utf8().c_str(), contentDisposition.Utf8().c_str(), job, &dataBind);
    if (kMiniElectronDownloadOptCancel == opt) {
        job->m_cacheForDownloadOpt = WebURLLoaderInternal::kCacheForDownloadNot;
        WebURLLoaderManager::sharedInstance()->cancel(job->m_jobId);
        return true;
    } else if (kMiniElectronDownloadOptCacheData == opt) {
        curl_easy_setopt(job->m_handle, CURLOPT_TIMEOUT, 60 * 60 * 3); // 如果是下载模式，就把总时间设置为N小时
        job->m_dataBind = new mini_electron_net_job_data_bind();
        *(job->m_dataBind) = dataBind;

        return false;
    }

    CHECK(false);
    return true;
}

static bool dispatchResponseToEmbedder(WebURLLoaderInternal* job, const AtomicString& contentType, const AtomicString& contentLength, bool isRedirect)
{
    std::string url = job->firstRequest()->url.possibly_invalid_spec();


    bool result = false;
    do {
        content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)job->m_engineViewId);
        if (!webview)
            break;
        if (webview->getClosure().m_NetResponseCallback) {
            if (webview->getClosure().m_NetResponseCallback(job->m_engineViewId, webview->getClosure().m_NetResponseParam, url.c_str(), job)) {
                result = true;
                break;
            }
        }

        // TODO: <a download>的情况还没考虑
        if (job->m_downloadName.get() || (isDownloadResponse(job, contentType, contentLength) && !isRedirect)) {
            if (dispatchDownloadToEmbedder(webview, job, url.c_str(), contentType, job->m_downloadName.get())) {
                result = true;
                break;
            }
            //             if (webview->getClosure().m_DownloadInBlinkThreadCallback &&
             
            //                 result = true;
            //                 break;
            //             }
        } else {
            job->m_cacheForDownloadOpt = WebURLLoaderInternal::kCacheForDownloadNot;
            job->m_dataCacheForDownload.clear();
        }
    } while (false);

    //     if (result)
    //         frame->stopLoading();
    //     temInfo->size = 0;

    return result;
}


static void dispatchEmbedderWillSendRequest(WebURLLoaderInternal* job, const blink::KURL* newURL, long httpCode)
{
}

static void doRedirect(WebURLLoaderInternal* job, const std::string& location, MainTaskArgs* args, bool isRedirectByHttpCode)
{
    blink::URLLoaderClient* client = job->client();
    blink::KURL newURL = blink::KURL((blink::KURL)(job->firstRequest()->url), String(location));

    network::ResourceRequest* oldRequest = job->firstRequest();

    std::unique_ptr<network::ResourceRequest> redirectedRequest = std::make_unique<network::ResourceRequest>(*job->firstRequest());
    redirectedRequest->url = (GURL)(newURL);
    redirectedRequest->headers.RemoveHeader("Origin");
    //redirectedRequest->SetHttpOriginIfNeeded(blink::WebSecurityOrigin::CreateFromString(newURL.Host())); // 其实这里添加没啥用，http.c里会删掉这个Origin
    redirectedRequest->request_initiator = url::Origin::Create(GURL(newURL.Host().Utf8().c_str()));

    blink::WebString newMethod = blink::WebString::FromASCII(oldRequest->method);
    blink::WebString newReferrer = blink::WebString::FromASCII(oldRequest->referrer.possibly_invalid_spec());

    if (client && job->loader() && (WebURLLoaderInternal::kCacheForDownloadYes != job->m_cacheForDownloadOpt)) {
        //client->willSendRequest(job->loader(), *redirectedRequest, job->m_response);

        bool reportRawHeaders = false;
        std::vector<std::string> removedHeaders;
        net::HttpRequestHeaders modified_headers;
        client->WillFollowRedirect(newURL, net::SiteForCookies(), newReferrer,
            /*network::mojom::ReferrerPolicy()*/ network::mojom::ReferrerPolicy::kAlways, newMethod, job->m_response /*passed_redirect_response*/,
            reportRawHeaders, &removedHeaders, modified_headers, /*insecure_scheme_was_upgraded*/ false);
    }

    if (isRedirectByHttpCode)
        job->m_response = blink::WebURLResponse();

    job->resetFirstRequest(std::move(redirectedRequest));
}

std::string extractCharset(const WTF::String& contentType)
{
    if (contentType.empty())
        return "";
    std::string mimeType;
    std::string charset;
    bool hadCharset = false;
    std::string boundary;
    net::HttpUtil::ParseContentType(contentType.Utf8(), &mimeType, &charset, &hadCharset, &boundary);
    size_t pos = charset.find(',');
    if (std::string::npos != pos)
        charset = charset.substr(0, pos);

    return charset;
}

static bool setHttpResponseDataToJobWhenDidReceiveResponseOnMainThread(WebURLLoaderInternal* job, MainTaskArgs* args)
{
    size_t size = args->size;
    size_t nmemb = args->nmemb;

    if (isHttpInfo(args->httpCode)) {
        // Just return when receiving http info, e.g. HTTP/1.1 100 Continue.
        // If not, the request might be cancelled, because the MIME type will be empty for this response.
        return false;
    }

    blink::WebString contentType = job->m_response.HttpHeaderField(blink::WebString::FromUTF8("Content-Type"));
    if (!contentType.IsEmpty())
        job->m_response.SetMimeType(blink::ExtractMIMETypeFromMediaType(contentType).LowerASCII());

    blink::WebString contentLength = job->m_response.HttpHeaderField(blink::WebString::FromUTF8("Content-Length"));

    std::string textEncodingName = extractCharset(contentType);
    if (textEncodingName.empty()) {
        //textEncodingName = "utf-8"; // 不再设置默认utf8，而是跟随document
    } else
        job->m_response.SetTextEncodingName(blink::WebString::FromUTF8(textEncodingName));
    if (dispatchResponseToEmbedder(job, contentType, contentLength, isHttpRedirect(args->httpCode)))
        return false;


    if (job->m_effectiveUrl.empty())
        job->m_effectiveUrl = args->hdr;

    bool isRedirectByHttpCode = isHttpRedirect(args->httpCode);
    bool isRedirectByUrl = (!job->m_effectiveUrl.empty() && job->m_effectiveUrl != job->m_url); // 有时有代理时，url会变，但没有30x码

    job->m_effectiveUrl = args->hdr;
    if (!job->m_fragment.empty())
        job->m_effectiveUrl += "#" + job->m_fragment;

    // HTTP redirection 重定向
    if (isRedirectByHttpCode || isRedirectByUrl) {
        std::string location = job->m_response.HttpHeaderField(blink::WebString::FromUTF8("location")).Utf8();
        std::string nonAuthoritativeReason = job->m_response.HttpHeaderField(blink::WebString::FromUTF8("Non-Authoritative-Reason")).Utf8();

        if (isRedirectByHttpCode)
            OutputDebugStringA("isRedirectByHttpCode: ");

        if (isRedirectByUrl)
            OutputDebugStringA("isRedirectByUrl: ");

        // https://passport.liepin.com/account/v1/elogin#sfrom=click-pc_homepage-front_navigation-ecomphr_new
        // 可能没location，或者开启了HSTS强制要求跳转到HTTPS。不过发现改这里没用，只需要在curl里把http改成https处理即可
        // 见third_party\libcurl\src\url.c
        // 另外发现不能用这个m_effectiveUrl，因为https://zm12.sm-tc.cn/rec/person_entity?wd=shenma_query&title=%E7%BB%BF%E7%98%A6%EF%BC%9A%
        // E5%87%A0%E5%A4%A7%E9%AA%97%E4%BA%BA%E8%AF%AF%E5%8C%BA%EF%BC%8C%E8%AE%A9%E4%BD%A0%E7%9A%84%E7%98%A6%E8%BA%AB%
        // E4%B9%8B%E8%B7%AF%E6%9B%B4%E5%8A%A0%E6%9B%B2%E6%8A%98 这个网址会发生崩溃
        //         if (location.isEmpty() || WTF::equalIgnoringCase(nonAuthoritativeReason, "HSTS"))
        //             location = job->m_effectiveUrl.c_str();

        if (!location.empty()) {
            if (job->m_fragment.empty()) {
                blink::KURL kurl = (blink::KURL)(job->firstRequest()->url);
                WTF::StringUTF8Adaptor fragment(kurl.FragmentIdentifier());
                if (fragment.size() != 0)
                    job->m_fragment = std::string(fragment.data(), fragment.size());
            }

            if (!job->m_fragment.empty())
                location += ("#");

            location += (job->m_fragment);

            //             Vector<char> locationBuffer = WTF::ensureStringToUTF8(location, false);
            //             locationBuffer.append('\n');
            //             locationBuffer.append('\0');
            //             OutputDebugStringA(locationBuffer.data());

            doRedirect(job, location, args, isRedirectByHttpCode);
            if (isRedirectByHttpCode)
                return false;
            return true;
        } else {
            OutputDebugStringA("location.isEmpty\n"); // fiddler产生的代理会出现这个情况
        }

    } else if (isHttpAuthentication(args->httpCode)) {

    } else
        dispatchEmbedderWillSendRequest(job, nullptr, args->httpCode);

    return true;
}

// 知乎网的某个url和curl的响应url，只查差最后少了个"?"号，这种就直接用原版的url了
static bool checkNeedSetResponseUrl(const std::string& responseUrl, const std::string& requestUrl)
{
    if (responseUrl.size() != requestUrl.size() - 1)
        return true;
    char c = requestUrl.at(requestUrl.size() - 1);
    if (c != '?')
        return true;
    if (0 != memcmp(requestUrl.c_str(), responseUrl.c_str(), responseUrl.size()))
        return true;
    return false;
}

static void setResponseDataToJobWhenDidReceiveResponseOnMainThread(WebURLLoaderInternal* job, MainTaskArgs* args)
{
    GURL url = job->firstRequest()->url;
    bool needSetResponseFired = true;

    job->m_response.SetExpectedContentLength(static_cast<long long int>(args->contentLength));
    if (checkNeedSetResponseUrl(args->hdr, job->m_url))
        job->m_response.SetCurrentRequestUrl(blink::KURL(args->hdr));
    else
        job->m_response.SetCurrentRequestUrl(blink::KURL(url));
    job->m_response.SetHttpStatusCode(args->httpCode);

    if (url.SchemeIsFile() && 0 == job->m_response.HttpStatusCode())
        job->m_response.SetHttpStatusCode(200);

    if (url.SchemeIsHTTPOrHTTPS() || url.SchemeIsFile())
        needSetResponseFired = setHttpResponseDataToJobWhenDidReceiveResponseOnMainThread(job, args);

    if (needSetResponseFired && !job->isCancelled() && !job->responseFired()) {
        if (job->client() && job->loader()) {
            WebURLLoaderManager::sharedInstance()->handleDidReceiveResponse(job);
        }
        job->setResponseFired(true);
    }
}

void WebURLLoaderManagerMainTask::handleLocalReceiveResponseOnMainThread(MainTaskArgs* args, WebURLLoaderInternal* job)
{
    if (job->responseFired())
        return;

    // since the code in headerCallbackOnIoThread will not have run for local files
    // the code to set the KURL and fire didReceiveResponse is never run,
    // which means the ResourceLoader's response does not contain the KURL.
    // Run the code here for local files to resolve the issue.
    // TODO: See if there is a better approach for handling this.
    std::string url = args->hdr;
    if (std::string::npos != url.find("file:")) {
        size_t questionMaskPos = url.find('?');
        if (std::string::npos != questionMaskPos) {
            url = url.substr(0, questionMaskPos);
        }
    }
    job->m_response.SetCurrentRequestUrl(blink::KURL(url.c_str()));

    setResponseDataToJobWhenDidReceiveResponseOnMainThread(job, args);
}

void WebURLLoaderManagerMainTask::handleDidSendData(MainTaskArgs* args, WebURLLoaderInternal* job)
{
    void* ptr = args->ptr;
    size_t size = args->size;
    size_t nmemb = args->nmemb;

    unsigned long long sentData = size * nmemb;

    base::subtle::NoBarrier_AtomicIncrement(reinterpret_cast<base::subtle::Atomic32*>(&job->m_sentDataBytes), static_cast<long>(sentData));
    WebURLLoaderManager::sharedInstance()->handleDidSentData(job, job->m_sentDataBytes, job->m_totalBytesToBeSent);
}

// called with data after all headers have been processed via headerCallbackOnIoThread
size_t WebURLLoaderManagerMainTask::handleWriteCallbackOnMainThread(MainTaskArgs* args, WebURLLoaderInternal* job)
{
    void* ptr = args->ptr;
    size_t size = args->size;
    size_t nmemb = args->nmemb;

    size_t totalSize = size * nmemb;

    if (!job->responseFired()) {
        handleLocalReceiveResponseOnMainThread(args, job);
        if (job->isCancelled())
            return 0;
    }

    //     std::vector<char> temp;
    //     temp.resize(totalSize + 1);
    //     memset(temp.data(), 0, temp.size());
    //     memcpy(temp.data(), ptr, totalSize);
    //     if (nullptr != strstr(temp.data(), "L.subtle.exportKey(\"jwk\""))
    //         OutputDebugStringA("");

    if (job->m_isHookRequest) {
        if (!job->m_hookBufForEndHook)
            job->m_hookBufForEndHook = new Vector<char>();
        job->m_hookBufForEndHook->Append((char*)ptr, totalSize);
        return totalSize;
    }

//     if (job->m_multipartHandle) {
//         job->m_multipartHandle->contentReceived(static_cast<const char*>(ptr), totalSize);
//     } else
    if (job->client() && job->loader()) {
        WebURLLoaderManager::sharedInstance()->didReceiveDataOrDownload(job, static_cast<char*>(ptr), totalSize, 0);
    }

    return totalSize;
}

size_t WebURLLoaderManagerMainTask::handleHeaderCallbackOnMainThread(MainTaskArgs* args, WebURLLoaderInternal* job)
{
    if (job->isCancelled())
        return 0;

    // We should never be called when deferred loading is activated.
    DCHECK(!job->m_defersLoading);

    size_t totalSize = args->size * args->nmemb;

    String header(base::span<const char>(static_cast<const char*>(args->ptr), totalSize));

    std::string url = job->firstRequest()->url.possibly_invalid_spec();

    //     if (WTF::kNotFound != header.Find("Set-Cookie:")) {
    //         std::string output = "handleHeaderCallbackOnMainThread:[";
    //         output += url;
    //         output += "][";
    //         output += header.Utf8();
    //         output += "]\n";
    //         OutputDebugStringA(output.c_str());
    //         OutputDebugStringA("");
    //     }

    /*
    * a) We can finish and send the ResourceResponse
    * b) We will add the current header to the HTTPHeaderMap of the ResourceResponse
    *
    * The HTTP standard requires to use \r\n but for compatibility it recommends to
    * accept also \n.
    */
    if (header == String("\r\n") || header == String("\n")) {
        setResponseDataToJobWhenDidReceiveResponseOnMainThread(job, args);
        return totalSize;
    } else {
        int splitPos = header.Find(":");
        if (splitPos != -1) {
            String key = header.Left(splitPos).StripWhiteSpace();
            String value = header.Substring(splitPos + 1).StripWhiteSpace();

            if (isAppendableHeader(key))
                job->m_response.AddHttpHeaderField(key, value);
            else
                job->m_response.SetHttpHeaderField(key, value);
        } else if (header.StartsWith("HTTP", WTF::kTextCaseASCIIInsensitive)) {
            // This is the first line of the response.
            // Extract the http status text from this.
            //
            // If the FOLLOWLOCATION option is enabled for the curl handle then
            // curl will follow the redirections internally. Thus this header callback
            // will be called more than one time with the line starting "HTTP" for one job.
            String httpCodeString = String::Number(args->httpCode);
            //             if (job->m_isProxy && 0 == args->httpCode)
            //                 httpCodeString = "200";
            int statusCodePos = header.Find(httpCodeString);

            if (statusCodePos != -1) {
                // The status text is after the status code.
                String status = header.Substring(statusCodePos + httpCodeString.length());
                job->m_response.SetHttpStatusText(status.StripWhiteSpace());
            }
        }
    }

    return totalSize;
}

void WebURLLoaderManagerMainTask::handleHookRequestOnMainThread(WebURLLoaderInternal* job)
{
    if (1 != job->m_isHookRequest)
        return;
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)job->m_engineViewId);
    if (!webview || !webview->getClosure().m_LoadUrlEndCallback)
        return;

    void* data = job->m_hookBufForEndHook ? job->m_hookBufForEndHook->data() : nullptr;
    size_t size = job->m_hookBufForEndHook ? job->m_hookBufForEndHook->size() : 0;

    webview->getClosure().m_LoadUrlEndCallback(job->m_engineViewId, webview->getClosure().m_LoadUrlEndParam, job->m_url.c_str(), job, data, size);
}

}

#endif // net_WebURLLoaderManagerMainTask_h