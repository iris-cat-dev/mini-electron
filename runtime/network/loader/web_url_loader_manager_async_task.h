#ifndef net_WebURLLoaderManagerAsynTask_h
#define net_WebURLLoaderManagerAsynTask_h

#include "runtime/network/loader/web_url_loader_manager.h"
#include "runtime/network/loader/web_url_loader_internal.h"
#include "runtime/engine/browser/web_view_host.h"
#include "runtime/engine/common/live_id_detect.h"
#include "services/network/public/cpp/resource_request.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader_client.h"
#include "base/threading/thread.h"

namespace mini_electron {

class WebURLLoaderManager::IoTask /*: public blink::WebThread::Task*/ {
public:
    IoTask(WebURLLoaderManager* manager, base::Thread* thread, bool start)
        : m_manager(manager)
        , m_thread(thread)
        , m_start(start)
    {
    }

    ~IoTask()
    {
    }

    void run()
    {
        ShutdownReadLocker locker(&m_manager->m_shutdownLock);
        bool needLoop = false;
        do {
            if (!locker.lock())
                break;
            if (!m_manager->downloadOnIoThread())
                break;
            if (m_manager->m_isShutdown)
                break;
            needLoop = true;
        } while (false);

        if (needLoop)
            m_thread->task_runner()->PostTask(FROM_HERE, base::BindOnce(&IoTask::run, base::Unretained(this)));
        else
            delete this;
    }

private:
    WebURLLoaderManager* m_manager;
    base::Thread* m_thread;
    bool m_start;
};

const int kBlackListCancelJobId = -2;

static void releaseJobWithoutCurl(WebURLLoaderManager* manager, WebURLLoaderInternal* job, int jobId)
{
    if (kBlackListCancelJobId == jobId)
        return;

    //     job->m_destroingMutex.lock();
    //     job->m_state = WebURLLoaderInternal::kDestroyed;
    //     while (job->m_ref != 1) { ::Sleep(5); }
    //
    //     job->m_handle = nullptr;
    //     manager->removeLiveJobs(jobId);
    //     delete job;

    WebURLLoaderInternal::release(jobId); // WebURLLoaderManager::doCancel 可能会占用
}


String getMIMETypeForPath(const String& path);

class HookAsynTask {
public:
    HookAsynTask(WebURLLoaderManager* manager, int jobId)
    {
        m_manager = manager;
        m_jobId = jobId;
    }

    ~HookAsynTask()
    {
    }

    void run()
    {
        runImpl();
        delete this;
    }

private:
    void runImpl()
    {
        AutoLockJob autoLockJob(m_manager, m_jobId);
        JobHead* jobHead = autoLockJob.lock();

        if (!jobHead || JobHead::kLoaderInternal != jobHead->getType())
            return;
        WebURLLoaderInternal* job = (WebURLLoaderInternal*)jobHead;
        if (kNormalCancelled == job->m_cancelledReason) {
            releaseJobWithoutCurl(m_manager, job, m_jobId);
            return;
        }

        GURL url = job->firstRequest()->url;
        std::string urlString = url.possibly_invalid_spec();
        job->m_url = urlString;
        job->m_response.SetCurrentRequestUrl(blink::KURL(WTF::String(urlString)));

        setMIMEType(job, urlString);

        size_t size = 0;
        const char* data = nullptr;

        if (job->m_responseOverrideData) {
            size = job->m_responseOverrideData->size();
            data = job->m_responseOverrideData->data();
        }

        //         if (job->firstRequest()->downloadToFile() && size > 0) {
        //             String tempPath = m_manager->handleHeaderForBlobOnMainThread(job, size);
        //             job->m_response.setDownloadFilePath(tempPath);
        //         }

        if (job->m_response.HttpStatusCode() == 0) {
            job->m_response.SetHttpStatusCode(200);
            job->m_response.SetHttpStatusText(blink::WebString::FromUTF8("OK"));
        }

        job->m_response.SetExpectedContentLength(static_cast<long long int>(size));
        m_manager->handleDidReceiveResponse(job);

        if ((job->m_responseOverrideData) && kNormalCancelled != job->m_cancelledReason
            && kHookRedirectCancelled != job->m_cancelledReason) { // 可能在didReceiveResponse里被cancel

            dispatchUrlEndHook(job, data, size);

            if (size)
                m_manager->didReceiveDataOrDownload(job, data, size, 0);
            m_manager->handleDidFinishLoading(job, base::Time::Now().ToInternalValue(), size); // 这里会走到WebURLLoaderManager::doCancel，然后导致job被占用
        }

        if (kHookRedirectCancelled != job->m_cancelledReason)
            releaseJobWithoutCurl(m_manager, job, m_jobId);
    }

    void setMIMEType(WebURLLoaderInternal* job, const std::string& urlString)
    {
        if (!(job->m_response.MimeType().IsNull()) && !(job->m_response.MimeType().IsEmpty()))
            return;

        int urlHostLength = urlString.length();
        for (int i = 0; i < urlHostLength; ++i) {
            if ('?' != urlString[i])
                continue;
            urlHostLength = i;
            break;
        }
        String urlWithoutQuery(base::span<const char>(urlString.c_str(), (size_t)urlHostLength));
        job->m_response.SetMimeType(/*blink::MIMETypeRegistry::*/ getMIMETypeForPath(urlWithoutQuery));
    }

    void dispatchUrlEndHook(WebURLLoaderInternal* job, const char* data, size_t size)
    {
        content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr((int64_t)job->m_engineViewId);
        if (!webview || !job->m_isHookRequest)
            return;
        if (!webview->getClosure().m_LoadUrlEndCallback)
            return;

        void* param = webview->getClosure().m_LoadUrlEndParam;
        webview->getClosure().m_LoadUrlEndCallback(job->m_engineViewId, param, job->m_url.c_str(), job, (void*)data, size);
    }

    WebURLLoaderManager* m_manager;
    int m_jobId;
};

class BlackListCancelTask {
public:
    BlackListCancelTask(WebURLLoaderManager* manager, int jobId)
    {
        m_manager = manager;
        m_jobId = jobId;

        JobHead* jobHead = m_manager->checkJob(m_jobId);
        if (!jobHead || JobHead::kLoaderInternal != jobHead->getType())
            return;
        WebURLLoaderInternal* job = (WebURLLoaderInternal*)jobHead;

        job->m_isBlackList = true;
    }

    ~BlackListCancelTask()
    {
    }

    static void cancel(WebURLLoaderManager* manager, WebURLLoaderInternal* job, int jobId)
    {
        job->m_isBlackList = true;
        job->m_response.SetCurrentRequestUrl(blink::KURL(job->firstRequest()->url));

        WebURLLoaderManager::sharedInstance()->handleDidReceiveResponse(job);
        //job->client()->didReceiveResponse(job->loader(), job->m_response);

        // 可能在didReceiveResponse里被cancel
        // WebURLLoaderManager::sharedInstance()->didReceiveDataOrDownload(job, static_cast<char*>(""), 0, 0);
        if (!job->isCancelled()) {
            blink::WebURLError error(net::ERR_ABORTED, blink::KURL(WTF::String(job->m_url)));
            WebURLLoaderManager::sharedInstance()->handleDidFail(job, error);

            if (!job->isCancelled())
                WebURLLoaderManager::sharedInstance()->doCancel(job, kNormalCancelled);
            CHECK(job->isCancelled());
        }
        releaseJobWithoutCurl(manager, job, jobId);
    }

    void run()
    {
        runImpl();
        delete this;
    }

    void runImpl()
    {
        AutoLockJob autoLockJob(m_manager, m_jobId);
        JobHead* jobHead = autoLockJob.lock();

        //JobHead* jobHead = m_manager->checkJob(m_jobId);
        if (!jobHead || JobHead::kLoaderInternal != jobHead->getType())
            return;
        WebURLLoaderInternal* job = (WebURLLoaderInternal*)jobHead;
        if (job->isCancelled())
            return;

        cancel(m_manager, job, m_jobId);
    }

private:
    WebURLLoaderManager* m_manager;
    int m_jobId;
};

}

#endif // net_WebURLLoaderManagerAsynTask_h