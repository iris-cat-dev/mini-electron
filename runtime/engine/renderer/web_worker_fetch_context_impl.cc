
#include "runtime/engine/renderer/web_worker_fetch_context_impl.h"

#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader_factory.h"
#include "net/cookies/site_for_cookies.h"
#include "runtime/network/loader/loader_factory_impl.h"
#include "runtime/network/loader/web_url_request_extra_data_wrap.h"
#include <windows.h>

namespace blink {
class WebLocalFrame;
}

namespace content {

WebWorkerFetchContextImpl::WebWorkerFetchContextImpl(
    const blink::WebSecurityOrigin& orig, int64_t mbwebviewId,
    uint64_t frameId, uint64_t parentFrameId, bool isMainFrame)
    : m_orig(blink::WebSecurityOrigin::CreateFromString(orig.ToString()))
    , m_engineViewId(mbwebviewId)
    , m_frameId(frameId)
    , m_parentFrameId(parentFrameId)
    , m_isMainFrame(isMainFrame)
    , m_loaderFactoryImpl(new mini_electron::LoaderFactoryImpl(
          mbwebviewId, frameId, parentFrameId, isMainFrame))
{
}

WebWorkerFetchContextImpl::~WebWorkerFetchContextImpl()
{
    m_loaderFactoryImpl.reset();
}

void WebWorkerFetchContextImpl::SetTerminateSyncLoadEvent(base::WaitableEvent* evt)
{
    m_loaderFactoryImpl->SetTerminateSyncLoadEvent(evt);
}

void WebWorkerFetchContextImpl::InitializeOnWorkerThread(blink::AcceptLanguagesWatcher*)
{
    OutputDebugStringA("WebWorkerFetchContextImpl::InitializeOnWorkerThread Not impl\n");
}

blink::URLLoaderFactory* WebWorkerFetchContextImpl::GetURLLoaderFactory()
{
    return m_loaderFactoryImpl.get();
}

std::unique_ptr<blink::URLLoaderFactory> WebWorkerFetchContextImpl::WrapURLLoaderFactory(
    blink::CrossVariantMojoRemote<network::mojom::URLLoaderFactoryInterfaceBase> url_loader_factory)
{
    std::unique_ptr<blink::URLLoaderFactory> loaderFactoryImpl;
    loaderFactoryImpl.reset(new mini_electron::LoaderFactoryImpl(
        m_engineViewId, m_frameId, m_parentFrameId, m_isMainFrame));
    return std::move(loaderFactoryImpl);
}

void setRequestHead(blink::WebLocalFrame* webFrame, blink::WebURLRequest& request);

std::optional<blink::WebURL> WebWorkerFetchContextImpl::WillSendRequest(const blink::WebURL& url)
{
    return std::nullopt;
}

void WebWorkerFetchContextImpl::FinalizeRequest(blink::WebURLRequest& request)
{
    scoped_refptr<mini_electron::WebURLRequestExtraDataWrap> extraData = base::MakeRefCounted<mini_electron::WebURLRequestExtraDataWrap>();
    extraData->mbwebviewId = m_engineViewId;
    extraData->frameId = m_frameId;
    extraData->parentFrameId = m_parentFrameId;
    extraData->isMainFrame = m_isMainFrame;

    blink::WebURLRequest* req = (blink::WebURLRequest*)(&request);
    req->SetURLRequestExtraData(extraData);

    setRequestHead(nullptr, *req);
}

blink::WebVector<std::unique_ptr<blink::URLLoaderThrottle>> WebWorkerFetchContextImpl::CreateThrottles(const network::ResourceRequest& request)
{
    return blink::WebVector<std::unique_ptr<blink::URLLoaderThrottle>>();
}

net::SiteForCookies WebWorkerFetchContextImpl::SiteForCookies(void) const
{
    return net::SiteForCookies();
}

absl::optional<blink::WebSecurityOrigin> WebWorkerFetchContextImpl::TopFrameOrigin(void) const
{
    return m_orig;
}

blink::WebString WebWorkerFetchContextImpl::GetAcceptLanguages(void) const
{
    return blink::WebString::FromASCII("zh-CN");
}

void WebWorkerFetchContextImpl::SetIsOfflineMode(bool)
{
    DebugBreak();
    return;
}

blink::mojom::ControllerServiceWorkerMode WebWorkerFetchContextImpl::GetControllerServiceWorkerMode(void) const
{
    return blink::mojom::ControllerServiceWorkerMode::kNoController;
}

}