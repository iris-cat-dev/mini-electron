
#include "runtime/engine/browser/local_main_frame_host_impl.h"

#include "runtime/engine/browser/web_view_host.h"
#include "runtime/engine/renderer/web_local_frame_client_impl.h"
#include "runtime/engine/common/live_id_detect.h"
#include "runtime/engine/common/thread_call.h"
#include "base/functional/bind.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/renderer/core/exported/web_view_impl.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/page/focus_controller.h"

void mini_electron_destroy_web_view_impl(mini_electron_web_view webviewHandle);

namespace content {

void requestNativeWindowCommand(WebLocalFrameClientImpl* frame_client,
    base::Value::Dict command, base::OnceCallback<void(bool)> reply)
{
    auto* host = frame_client ? static_cast<WebViewHost*>(
        common::LiveIdDetect::getWebViewIds()->getPtr(frame_client->getEngineViewId())) : nullptr;
    if (!host || !host->getClosure().m_nativeWindowCommand) {
        std::move(reply).Run(false);
        return;
    }
    // The transport has copyable completion handlers; retain the Mojo reply
    // without retaining a frame or view that can disappear during a native event.
    auto completion = std::make_shared<base::OnceCallback<void(bool)>>(std::move(reply));
    host->getClosure().m_nativeWindowCommand(
        reinterpret_cast<mini_electron_web_frame_handle>(frame_client->getFrameId()),
        std::move(command), [completion](bool granted) {
            if (*completion)
                std::move(*completion).Run(granted);
        });
}

void LocalMainFrameHostImpl::FocusPage()
{
    requestNativeWindowCommand(m_frameClient,
        base::Value::Dict().Set("command", "focus"), base::BindOnce([](bool) {}));
}

void LocalMainFrameHostImpl::TakeFocus(bool reverse)
{
    const auto id = m_frameClient->getEngineViewId();
    requestNativeWindowCommand(m_frameClient,
        base::Value::Dict().Set("command", "take-focus").Set("reverse", reverse),
        base::BindOnce([](int64_t id, bool reverse, bool transferred) {
            auto* host = static_cast<WebViewHost*>(common::LiveIdDetect::getWebViewIds()->getPtr(id));
            if (!transferred && host && host->getWebView()) {
                auto* view = static_cast<blink::WebViewImpl*>(host->getWebView());
                view->GetPage()->GetFocusController().SetInitialFocus(
                    reverse ? blink::mojom::blink::FocusType::kBackward : blink::mojom::blink::FocusType::kForward);
            }
        }, id, reverse));
}

void LocalMainFrameHostImpl::UpdateTargetURL(const blink::KURL& url, UpdateTargetURLCallback callback)
{
    const auto id = m_frameClient->getEngineViewId();
    ThreadCall::callUiThreadAsync(FROM_HERE, [id, target = url.GetString().Utf8()] {
        auto* host = static_cast<WebViewHost*>(common::LiveIdDetect::getWebViewIds()->getPtr(id));
        if (host && host->getClosure().m_MouseOverUrlChangedCallback)
            host->getClosure().m_MouseOverUrlChangedCallback(
                id, host->getClosure().m_MouseOverUrlChangedParam, target.c_str());
    });
    std::move(callback).Run();
}

void LocalMainFrameHostImpl::ShowCreatedWindow(const blink::LocalFrameToken&,
    ui::mojom::blink::WindowOpenDisposition, blink::mojom::blink::WindowFeaturesPtr,
    bool, ShowCreatedWindowCallback callback)
{
    requestNativeWindowCommand(m_frameClient, base::Value::Dict().Set("command", "show"),
        base::BindOnce([](ShowCreatedWindowCallback reply, bool) {
            std::move(reply).Run();
        }, std::move(callback)));
}

void LocalMainFrameHostImpl::SetWindowRect(const gfx::Rect& bounds, SetWindowRectCallback callback)
{
    requestNativeWindowCommand(m_frameClient, base::Value::Dict()
        .Set("command", "bounds").Set("x", bounds.x()).Set("y", bounds.y())
        .Set("width", bounds.width()).Set("height", bounds.height()),
        base::BindOnce([](SetWindowRectCallback reply, bool) {
            std::move(reply).Run();
        }, std::move(callback)));
}

void LocalMainFrameHostImpl::Maximize()
{
    requestNativeWindowCommand(m_frameClient,
        base::Value::Dict().Set("command", "maximize"), base::BindOnce([](bool) {}));
}

void LocalMainFrameHostImpl::Minimize()
{
    requestNativeWindowCommand(m_frameClient,
        base::Value::Dict().Set("command", "minimize"), base::BindOnce([](bool) {}));
}

void LocalMainFrameHostImpl::Restore()
{
    requestNativeWindowCommand(m_frameClient,
        base::Value::Dict().Set("command", "restore"), base::BindOnce([](bool) {}));
}

void LocalMainFrameHostImpl::SetResizable(bool resizable)
{
    requestNativeWindowCommand(m_frameClient, base::Value::Dict()
        .Set("command", "resizable").Set("enabled", resizable), base::BindOnce([](bool) {}));
}

LocalMainFrameHostImpl::LocalMainFrameHostImpl(WebLocalFrameClientImpl* frameClient)
{
    m_frameClient = frameClient;
}

void LocalMainFrameHostImpl::DidFirstVisuallyNonEmptyPaint()
{
    // Painting is not document completion; WebLocalFrameClient::DidFinishLoad is.
}

void LocalMainFrameHostImpl::RequestClose()
{
    if (!m_frameClient)
        return;
    const auto id = m_frameClient->getEngineViewId();
    auto* host = static_cast<WebViewHost*>(common::LiveIdDetect::getWebViewIds()->getPtr(id));
    if (host && !host->getClosure().m_nativeWindowCommand) {
        mini_electron_destroy_web_view_impl(id);
        return;
    }
    requestNativeWindowCommand(m_frameClient,
        base::Value::Dict().Set("command", "close"), base::BindOnce([](bool) {}));
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
