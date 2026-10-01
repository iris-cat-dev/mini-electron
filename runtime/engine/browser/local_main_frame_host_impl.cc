
#include "runtime/engine/browser/local_main_frame_host_impl.h"

#include "runtime/engine/browser/web_view_host.h"
#include "runtime/engine/renderer/web_local_frame_client_impl.h"
#include "runtime/engine/common/live_id_detect.h"

void mini_electron_destroy_web_view_impl(mini_electron_web_view webviewHandle);

namespace content {

LocalMainFrameHostImpl::LocalMainFrameHostImpl(WebLocalFrameClientImpl* frameClient)
{
    m_frameClient = frameClient;
}

void LocalMainFrameHostImpl::DidFirstVisuallyNonEmptyPaint()
{
    if (m_frameClient)
        m_frameClient->onLoadingSucceeded();
}

void LocalMainFrameHostImpl::RequestClose()
{
    if (!m_frameClient)
        return;
    int64_t id = m_frameClient->getEngineViewId();
    mini_electron_destroy_web_view_impl(id);
}

void LocalMainFrameHostImpl::DraggableRegionsChanged(WTF::Vector<::blink::mojom::blink::DraggableRegionPtr> regions)
{
    if (!m_frameClient)
        return;
    int64_t id = m_frameClient->getEngineViewId();
    content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(id);
    if (!webview)
        return;

    blink::WebVector<blink::WebDraggableRegion> regionsCopy;
    for (size_t i = 0; i < regions.size(); ++i) {
        const ::blink::mojom::blink::DraggableRegionPtr& r = regions[i];
        blink::WebDraggableRegion webR;
        webR.bounds = r->bounds;
        webR.draggable = r->draggable;
        regionsCopy.push_back(webR);
    }
    webview->draggableRegionsChanged(regionsCopy);
}

}
