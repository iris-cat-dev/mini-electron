
#include "runtime/network/loader/web_url_loader_impl_curl.h"

#include "runtime/network/loader/web_url_loader_internal.h"
#include "runtime/network/loader/web_url_request_extra_data_wrap.h"
#include "runtime/network/loader/web_url_loader_manager.h"
#include "runtime/network/loader/blob_resource_loader.h"
#include "runtime/network/loader/flatten_http_body_element.h"
#include "runtime/engine/renderer/blob_url_store_impl.h"

#include "third_party/blink/public/platform/web_url_request_extra_data.h"
#include "third_party/blink/public/platform/resource_load_info_notifier_wrapper.h"
#include "third_party/blink/public/platform/web_data.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader_client.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/cors/cors.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "url/origin.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "net/base/net_errors.h"
#include "third_party/blink/public/platform/web_url_error.h"
#include <algorithm>
#include <windows.h>

#ifndef NDEBUG
//DEFINE_DEBUG_ONLY_GLOBAL(WTF::RefCountedLeakCounter, webURLLoaderImplCurlCount, ("WebURLLoaderImplCurl"));
#endif

namespace mini_electron {

class BlinkSynchronousLoader : public blink::URLLoaderClient {
public:
    BlinkSynchronousLoader(absl::optional<blink::WebURLError>&, blink::WebURLResponse&, std::vector<char>*);
    ~BlinkSynchronousLoader();

    void DidReceiveResponse(const blink::WebURLResponse& response,
        absl::variant<mojo::ScopedDataPipeConsumerHandle, SegmentedBuffer>, std::optional<mojo_base::BigBuffer> cached_metadata) override;
    void DidReceiveDataForTesting(base::span<const char> data) override;
    void DidFinishLoading(base::TimeTicks finish_time, int64_t total_encoded_data_length, uint64_t total_encoded_body_length, int64_t total_decoded_body_length) override;
    void DidFail(const blink::WebURLError&, base::TimeTicks finish_time, int64_t total_encoded_data_length, uint64_t total_encoded_body_length,
        int64_t total_decoded_body_length) override;

    //void DidStartLoadingResponseBody(mojo::ScopedDataPipeConsumerHandle responseBodyConsumer) override;
private:
    std::optional<blink::WebURLError>& m_error;
    blink::WebURLResponse& m_response;
    std::vector<char>* m_data;
};

BlinkSynchronousLoader::BlinkSynchronousLoader(absl::optional<blink::WebURLError>& error, blink::WebURLResponse& response, std::vector<char>* data)
    : m_error(error)
    , m_response(response)
    , m_data(data)
{
}

BlinkSynchronousLoader::~BlinkSynchronousLoader()
{
}

void BlinkSynchronousLoader::DidReceiveResponse(
    const blink::WebURLResponse& response,
    absl::variant<mojo::ScopedDataPipeConsumerHandle, SegmentedBuffer>, 
    std::optional<mojo_base::BigBuffer> cached_metadata)
{
    m_response = response;
}

void BlinkSynchronousLoader::DidReceiveDataForTesting(base::span<const char> data)
{
    m_data->insert(m_data->end(), data.data(), data.data() + data.size());
}

void BlinkSynchronousLoader::DidFinishLoading(base::TimeTicks finishTime, 
    int64_t totalEncodedDataLength, uint64_t totalEncodedBodyLength, int64_t totalDecodedBodyLength)
{

}

void BlinkSynchronousLoader::DidFail(const blink::WebURLError& error, base::TimeTicks finishTime, int64_t totalEncodedDataLength, uint64_t totalEncodedBodyLength,
    int64_t totalDecodedBodyLength)
{
    m_error = error;
}

//------

WebURLLoaderImplCurl::WebURLLoaderImplCurl(
    scoped_refptr<base::SingleThreadTaskRunner> freezableTaskRunnerHandle, 
    scoped_refptr<base::SingleThreadTaskRunner> unfreezableTaskRunnerHandle, 
    base::WaitableEvent* terminateSyncLoadEvent,
    int64_t mbwebviewId, uint64_t frameId, uint64_t parentFrameId,
    bool isMainFrame)
    : m_freezableTaskRunner(std::move(freezableTaskRunnerHandle))
    , m_unfreezableTaskRunner(std::move(unfreezableTaskRunnerHandle))
    , m_terminateSyncLoadEvent(terminateSyncLoadEvent)
    , m_engineViewId(mbwebviewId)
    , m_frameId(frameId)
    , m_parentFrameId(parentFrameId)
    , m_isMainFrame(isMainFrame)
{
    CHECK(m_engineViewId > 0);
    m_jobId = -1;
#ifndef NDEBUG
    //webURLLoaderImplCurlCount.increment();
#endif
    const scoped_refptr<base::SingleThreadTaskRunner> currentRunner = base::SingleThreadTaskRunner::GetCurrentDefault();
    scoped_refptr<base::SingleThreadTaskRunner> runner = m_freezableTaskRunner;
    if (runner->BelongsToCurrentThread())
        m_freezableTaskRunner = currentRunner; // 有时候就算runner是本线程的，但和ThreadTaskRunnerHandle::Get还不是同一个实例
    else
        m_freezableTaskRunner = runner;
}

WebURLLoaderImplCurl::~WebURLLoaderImplCurl()
{
    delete m_blobLoader;
#ifndef NDEBUG
    //webURLLoaderImplCurlCount.decrement();
#endif

    mini_electron::WebURLLoaderManager* mgr = mini_electron::WebURLLoaderManager::sharedInstance();
    if (!mgr)
        return;
    if (m_isAutoDestroy) {
        if (m_autoFollowRedirects) {
            mgr->cancel(m_jobId);
        } else {
            AutoLockJob lock(mgr, m_jobId);
            if (auto* job = lock.lock()) {
                job->resetClient();
                job->setLoader(nullptr);
                mgr->doCancel(job, kNormalCancelled);
            }
        }
    }
}

void WebURLLoaderImplCurl::init()
{
    m_hadDestroied = nullptr;
    m_jobId = 0;
}

static bool shouldContentSniffURL(const GURL& url)
{
    // We shouldn't content sniff file URLs as their MIME type should be established via their extension.
    return !url.SchemeIs("file");
}

static bool checkIsResURL(const GURL& url)
{
    std::string urlStr = url.possibly_invalid_spec();
    urlStr = base::ToLowerASCII(urlStr);

    if (std::string::npos != urlStr.find("jpg") || std::string::npos != urlStr.find("gif") || std::string::npos != urlStr.find("bmp")
        || std::string::npos != urlStr.find("png") || std::string::npos != urlStr.find("mp3") || std::string::npos != urlStr.find("mp4"))
        return true;
    return false;
}

// 本函数一般是在web worker的xhr里调用
void WebURLLoaderImplCurl::LoadAsynchronously(
    std::unique_ptr<network::ResourceRequest> request, 
    scoped_refptr<const blink::SecurityOrigin> topFrameOrigin,
    bool noMimeSniffing, 
    std::unique_ptr<blink::ResourceLoadInfoNotifierWrapper> resourceLoadInfoNotifierWrapper,
    blink::CodeCacheHost* codeCacheHost,
    blink::URLLoaderClient* client
    )
{
    if (m_isOfflineMode && request->url.SchemeIsHTTPOrHTTPS()) {
        m_freezableTaskRunner->PostTask(FROM_HERE, base::BindOnce(
            [](base::WeakPtr<WebURLLoaderImplCurl> self, blink::URLLoaderClient* client, GURL url) {
                if (self)
                    client->DidFail(blink::WebURLError(net::ERR_INTERNET_DISCONNECTED, blink::KURL(url)), base::TimeTicks::Now(), 0, 0, 0);
            }, m_weakPtr.GetWeakPtr(), client, request->url));
        return;
    }
    mini_electron::WebURLLoaderManager* netManager = mini_electron::WebURLLoaderManager::sharedInstance();
    //WebURLRequestExtraDataWrap* extraDataWrap = (WebURLRequestExtraDataWrap*)extraData.get();
    if (!netManager) {
        //MojoClose(extraDataWrap->dataPipeProducerHandle);
        return;
    }

    init();

    GURL url = request->url;
    mini_electron::WebURLLoaderManager::IoThreadType type = checkIsResURL(url) ? mini_electron::WebURLLoaderManager::kIoThreadTypeRes : mini_electron::WebURLLoaderManager::kIoThreadTypeOther;

    int64_t mbwebviewId = m_engineViewId;//extraDataWrap->mbwebviewId;
    if (url.SchemeIs("blob") /*&& !extraDataWrap->isDownload()*/) {
        m_blobLoader = mini_electron::BlobResourceLoader::createAsync(mbwebviewId, url, client);
        m_blobLoader->start();
        return;
    }

    WebURLLoaderInternal* job = new WebURLLoaderInternal(netManager->getIoThread(type), this, std::move(request), client, false, shouldContentSniffURL(url));
    job->m_autoFollowRedirects = m_autoFollowRedirects;
    job->m_bodyPaused = m_bodyPaused;
    job->m_frozen = m_mode != blink::LoaderFreezeMode::kNone;
    job->m_engineViewId = mbwebviewId;
    job->setRequestFrameMetadata(blink::mojom::RequestContextFrameType::kNone,
        m_frameId, m_parentFrameId, m_isMainFrame);
    //job->m_dataPipeProducerHandle = extraDataWrap->dataPipeProducerHandle;
    //job->m_downloadName = extraDataWrap->releaseDownloadName();

    int jobIds = 0;
    jobIds = netManager->addAsynchronousJob(job);
    if (0 == jobIds)
        return;
    m_jobId = jobIds;
    job->m_jobId = m_jobId;

    // 执行完add后，this可能被销毁，当dataurl的时候
}

// 本函数也是在web worker的xhr里调用
void WebURLLoaderImplCurl::LoadSynchronously(
    std::unique_ptr<network::ResourceRequest> request,
    scoped_refptr<const blink::SecurityOrigin> topFrameOrigin,
    bool downloadToBlob, 
    bool noMimeSniffing, 
    base::TimeDelta timeoutInterval, 
    blink::URLLoaderClient* client, 
    blink::WebURLResponse& response,
    std::optional<blink::WebURLError>& error,
    scoped_refptr<WTF::SharedBuffer>& data, 
    int64_t& encodedDataLength, 
    uint64_t& encodedBodyLength,
    scoped_refptr<blink::BlobDataHandle>& downloadedBlob, 
    std::unique_ptr<blink::ResourceLoadInfoNotifierWrapper> resourceLoadInfoNotifierWrapper)
{
    if (m_isOfflineMode && request->url.SchemeIsHTTPOrHTTPS()) {
        error = blink::WebURLError(net::ERR_INTERNET_DISCONNECTED, blink::KURL(request->url));
        data = nullptr;
        encodedDataLength = 0;
        encodedBodyLength = 0;
        return;
    }
    if (!mini_electron::WebURLLoaderManager::sharedInstance())
        return;

    init();

    GURL url = request->url;
    if (url.SchemeIs("blob")) {
        std::vector<char> buffer;
        mini_electron::BlinkSynchronousLoader syncLoader(error, response, &buffer);
        mini_electron::BlobResourceLoader::loadSynchronously(m_engineViewId, url, &syncLoader);
        encodedDataLength = buffer.size();
        encodedBodyLength = buffer.size();
        data = WTF::SharedBuffer::Create(base::span<const unsigned char>(
            reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size()));
        return;
    }

    mini_electron::WebURLLoaderManager* netManager = mini_electron::WebURLLoaderManager::sharedInstance();
    mini_electron::WebURLLoaderManager::IoThreadType type = mini_electron::WebURLLoaderManager::kIoThreadTypeSync;

    std::vector<char> buffer;
    mini_electron::BlinkSynchronousLoader syncLoader(error, response, &buffer);
    mini_electron::WebURLLoaderInternal* job
        = new mini_electron::WebURLLoaderInternal(netManager->getIoThread(type), this, std::move(request), &syncLoader, false, shouldContentSniffURL(url));
    job->m_engineViewId = m_engineViewId;
    job->setRequestFrameMetadata(blink::mojom::RequestContextFrameType::kNone,
        m_frameId, m_parentFrameId, m_isMainFrame);
    netManager->dispatchSynchronousJob(job, m_terminateSyncLoadEvent);
    data = WTF::SharedBuffer::Create(base::span<const unsigned char>((const unsigned char*)buffer.data(), buffer.size()));
    memset(buffer.data(), 0, buffer.size());
}

static bool checkCorsFlagIfNeeded(network::ResourceRequest* request)
{
    if (!network::cors::ShouldCheckCors(request->url, request->request_initiator, request->mode))
        return false;

    //if (HasSpecialAccessToDestination())
    //    return false;

    return true;
}

static void addOriginHeaderIfNeeded(network::ResourceRequest* request)
{
    // services/network/cors/cors_url_loader.cc
    auto shouldIncludeOriginHeader = [request]() -> bool {
        if (!request->request_initiator)
            return false;

        //if (request->credentials_mode == network::mojom::CredentialsMode::kInclude
        //   && GetStorageAccessStatus() == net::cookie_util::StorageAccessStatus::kInactive
        //   ) {
        //    // Lower layers will add the Sec-Fetch-Storage-Access header, and the
        //    // server may respond with a "retry" header. The server needs to know the
        //    // origin in that event.
        //    return true;
        //}

        // If the `CORS flag` is set, `httpRequest`’s method is neither `GET` nor
        // `HEAD`, or `httpRequest`’s mode is "websocket", then append
        // `Origin`/the result of serializing a request origin with `httpRequest`,
        // to `httpRequest`’s header list.
        //
        // We exclude navigation requests to keep the existing behavior.
        // TODO(yhirano): Reconsider this.
        if (request->mode == network::mojom::RequestMode::kNavigate)
            return false;

        if (checkCorsFlagIfNeeded(request))
            return true;
        return request->method != net::HttpRequestHeaders::kGetMethod && request->method != net::HttpRequestHeaders::kHeadMethod;
    };

    if (shouldIncludeOriginHeader()) {
        //         if (tainted_) {
        //             request_.headers.SetHeader(net::HttpRequestHeaders::kOrigin, url::Origin().Serialize());
        //         } else {
        //             request_.headers.SetHeader(net::HttpRequestHeaders::kOrigin, request_.request_initiator->Serialize());
        //         }
        request->headers.SetHeader(net::HttpRequestHeaders::kOrigin, request->request_initiator->Serialize());
    }
}

void WebURLLoaderImplCurl::LoadAsynchronouslyEx(
    std::unique_ptr<network::ResourceRequest> request,
    scoped_refptr<const blink::SecurityOrigin> topFrameOrigin,
    bool noMimeSniffing, 
    std::unique_ptr<blink::ResourceLoadInfoNotifierWrapper> resourceLoadInfoNotifierWrapper,
    blink::CodeCacheHost* codeCacheHost,
    scoped_refptr<mini_electron::WebURLRequestExtraDataWrap> extraData,
    blink::URLLoaderClient* client)
{
    if (m_isOfflineMode && request->url.SchemeIsHTTPOrHTTPS()) {
        m_freezableTaskRunner->PostTask(FROM_HERE, base::BindOnce(
            [](base::WeakPtr<WebURLLoaderImplCurl> self, blink::URLLoaderClient* client, GURL url) {
                if (self)
                    client->DidFail(blink::WebURLError(net::ERR_INTERNET_DISCONNECTED, blink::KURL(url)), base::TimeTicks::Now(), 0, 0, 0);
            }, m_weakPtr.GetWeakPtr(), client, request->url));
        return;
    }
    mini_electron::WebURLLoaderManager* netManager = mini_electron::WebURLLoaderManager::sharedInstance();
    WebURLRequestExtraDataWrap* extraDataWrap = (WebURLRequestExtraDataWrap*)extraData.get();
    if (!netManager) {
        MojoClose(extraDataWrap->dataPipeProducerHandle);
        return;
    }

    init();

    GURL url = request->url;
    addOriginHeaderIfNeeded(request.get());

    mini_electron::WebURLLoaderManager::IoThreadType type
        = checkIsResURL(url) ? mini_electron::WebURLLoaderManager::kIoThreadTypeRes : mini_electron::WebURLLoaderManager::kIoThreadTypeOther;

    int64_t mbwebviewId = extraDataWrap->mbwebviewId;
    if (url.SchemeIs("blob") && !extraDataWrap->isDownload()) {
        m_blobLoader = mini_electron::BlobResourceLoader::createAsync(mbwebviewId, url, client);
        m_blobLoader->start();
        return;
    }

    WebURLLoaderInternal* job = new WebURLLoaderInternal(netManager->getIoThread(type), this, std::move(request), client, false, shouldContentSniffURL(url));
    job->m_autoFollowRedirects = m_autoFollowRedirects;
    job->m_bodyPaused = m_bodyPaused;
    job->m_frozen = m_mode != blink::LoaderFreezeMode::kNone;
    job->m_engineViewId = mbwebviewId;
    job->m_dataPipeProducerHandle = extraDataWrap->dataPipeProducerHandle;
    job->setRequestFrameMetadata(extraDataWrap->frameType, extraDataWrap->frameId,
        extraDataWrap->parentFrameId, extraDataWrap->isMainFrame);
    if (m_autoFollowRedirects)
        job->m_downloadName = extraDataWrap->releaseDownloadName();
    else if (extraDataWrap->downloadName)
        job->m_downloadName = std::make_unique<String>(*extraDataWrap->downloadName);

    int jobIds = 0;
    jobIds = netManager->addAsynchronousJob(job);
    if (0 == jobIds)
        return;
    m_jobId = jobIds;
    job->m_jobId = m_jobId;

    // 执行完add后，this可能被销毁，当dataurl的时候
}

void WebURLLoaderImplCurl::DidChangePriority(blink::WebURLRequest::Priority priority, int intra_priority_value)
{
    SetPriority(blink::WebURLRequest::ConvertToNetPriority(priority), intra_priority_value);
}

void WebURLLoaderImplCurl::SetPriority(net::RequestPriority priority, int intraPriority)
{
    auto* manager = WebURLLoaderManager::sharedInstance();
    if (!manager)
        return;
    AutoLockJob lock(manager, m_jobId);
    auto* job = lock.lock();
    if (!job || job->isCancelled())
        return;
    job->firstRequest()->priority = priority;
    // HTTP/2 stream weights are in [1, 256]. Reserve a band per net priority.
    long weight = std::clamp(24 + static_cast<int>(priority) * 48 + std::clamp(intraPriority, -23, 24), 1, 256);
    job->m_ioThread->task_runner()->PostTask(FROM_HERE, base::BindOnce([](int jobId, long weight) {
        AutoLockJob lock(WebURLLoaderManager::sharedInstance(), jobId);
        auto* job = lock.lock();
        if (!job || job->isCancelled())
            return;
        job->m_streamWeight = weight;
        if (job->m_handle)
            curl_easy_setopt(job->m_handle, CURLOPT_STREAM_WEIGHT, weight);
    }, m_jobId, weight));
}

scoped_refptr<base::SingleThreadTaskRunner> WebURLLoaderImplCurl::GetTaskRunnerForBodyLoader()
{
    return m_freezableTaskRunner;
}

void WebURLLoaderImplCurl::Freeze(blink::LoaderFreezeMode mode)
{
    m_mode = mode;
    updatePauseState();
}

void WebURLLoaderImplCurl::PauseReadingBody(bool paused)
{
    m_bodyPaused = paused;
    updatePauseState();
}

void WebURLLoaderImplCurl::updatePauseState()
{
    auto* manager = WebURLLoaderManager::sharedInstance();
    if (!manager)
        return;
    AutoLockJob lock(manager, m_jobId);
    auto* job = lock.lock();
    if (!job || job->isCancelled())
        return;
    job->m_ioThread->task_runner()->PostTask(FROM_HERE, base::BindOnce([](int jobId, bool bodyPaused, bool frozen) {
        AutoLockJob lock(WebURLLoaderManager::sharedInstance(), jobId);
        auto* job = lock.lock();
        if (!job || job->isCancelled())
            return;
        job->m_bodyPaused = bodyPaused;
        job->m_frozen = frozen;
        int pause = frozen ? CURLPAUSE_ALL : bodyPaused ? CURLPAUSE_RECV : CURLPAUSE_CONT;
        if (job->m_responseBufferedBytes.load(std::memory_order_relaxed) >= 64 * 1024)
            pause |= CURLPAUSE_RECV;
        if (job->m_formDataStream && job->m_formDataStream->isWaitingForData())
            pause |= CURLPAUSE_SEND;
        if (job->m_handle)
            curl_easy_pause(job->m_handle, pause);
    }, m_jobId, m_bodyPaused, m_mode != blink::LoaderFreezeMode::kNone));
}

} // content