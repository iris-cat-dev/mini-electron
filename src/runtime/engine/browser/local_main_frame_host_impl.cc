
#include "src/runtime/engine/browser/local_main_frame_host_impl.h"

#include "src/runtime/engine/browser/web_view_host.h"
#include "src/runtime/engine/renderer/web_local_frame_client_impl.h"
#include "src/runtime/engine/common/live_id_detect.h"
#include "src/runtime/engine/common/thread_call.h"
#include "base/functional/bind.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/renderer/core/exported/web_view_impl.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/page/focus_controller.h"

void mini_electron_destroy_web_view_impl(mini_electron_web_view webviewHandle);

namespace content {

blink::WebLocalFrame* resolveLiveFrame(int64_t view_id,
    const blink::LocalFrameToken& frame_token, WebViewHost** resolved_host)
{
    auto* host = static_cast<WebViewHost*>(
        common::LiveIdDetect::getWebViewIds()->getPtr(view_id));
    if (!host || !host->getWebView())
        return nullptr;
    auto* frame = blink::WebLocalFrame::FromFrameToken(frame_token);
    if (!frame || !frame->Client() || frame->View() != host->getWebView())
        return nullptr;
    if (resolved_host)
        *resolved_host = host;
    return frame;
}

void requestNativeWindowCommand(int64_t view_id,
    const blink::LocalFrameToken& frame_token, base::Value::Dict command,
    base::OnceCallback<void(bool)> reply)
{
    WebViewHost* host = nullptr;
    if (!resolveLiveFrame(view_id, frame_token, &host)
        || !host->getClosure().m_nativeWindowCommand) {
        std::move(reply).Run(false);
        return;
    }
    // The transport has copyable completion handlers; retain the Mojo reply
    // without retaining a frame or view that can disappear during a native event.
    auto completion = std::make_shared<base::OnceCallback<void(bool)>>(std::move(reply));
    host->getClosure().m_nativeWindowCommand(
        reinterpret_cast<mini_electron_web_frame_handle>(
            static_cast<uintptr_t>(blink::LocalFrameToken::Hasher()(frame_token))),
        std::move(command), [completion, view_id, frame_token](bool granted) {
            granted = granted && resolveLiveFrame(view_id, frame_token);
            if (*completion)
                std::move(*completion).Run(granted);
        });
}

void LocalMainFrameHostImpl::FocusPage()
{
    requestNativeWindowCommand(m_engineViewId, m_frameToken,
        base::Value::Dict().Set("command", "focus"), base::BindOnce([](bool) {}));
}

void LocalMainFrameHostImpl::TakeFocus(bool reverse)
{
    requestNativeWindowCommand(m_engineViewId, m_frameToken,
        base::Value::Dict().Set("command", "take-focus").Set("reverse", reverse),
        base::BindOnce([](int64_t id, blink::LocalFrameToken frame_token,
                           bool reverse, bool transferred) {
            auto* frame = resolveLiveFrame(id, frame_token);
            if (!transferred && frame) {
                auto* view = static_cast<blink::WebViewImpl*>(frame->View());
                view->GetPage()->GetFocusController().SetInitialFocus(
                    reverse ? blink::mojom::blink::FocusType::kBackward
                            : blink::mojom::blink::FocusType::kForward);
            }
        }, m_engineViewId, m_frameToken, reverse));
}

void LocalMainFrameHostImpl::UpdateTargetURL(const blink::KURL& url, UpdateTargetURLCallback callback)
{
    if (!resolveLiveFrame(m_engineViewId, m_frameToken)) {
        std::move(callback).Run();
        return;
    }
    const auto id = m_engineViewId;
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
    requestNativeWindowCommand(m_engineViewId, m_frameToken, base::Value::Dict().Set("command", "show"),
        base::BindOnce([](ShowCreatedWindowCallback reply, bool) {
            std::move(reply).Run();
        }, std::move(callback)));
}

void LocalMainFrameHostImpl::SetWindowRect(const gfx::Rect& bounds, SetWindowRectCallback callback)
{
    requestNativeWindowCommand(m_engineViewId, m_frameToken, base::Value::Dict()
        .Set("command", "bounds").Set("x", bounds.x()).Set("y", bounds.y())
        .Set("width", bounds.width()).Set("height", bounds.height()),
        base::BindOnce([](SetWindowRectCallback reply, bool) {
            std::move(reply).Run();
        }, std::move(callback)));
}

void LocalMainFrameHostImpl::Maximize()
{
    requestNativeWindowCommand(m_engineViewId, m_frameToken,
        base::Value::Dict().Set("command", "maximize"), base::BindOnce([](bool) {}));
}

void LocalMainFrameHostImpl::Minimize()
{
    requestNativeWindowCommand(m_engineViewId, m_frameToken,
        base::Value::Dict().Set("command", "minimize"), base::BindOnce([](bool) {}));
}

void LocalMainFrameHostImpl::Restore()
{
    requestNativeWindowCommand(m_engineViewId, m_frameToken,
        base::Value::Dict().Set("command", "restore"), base::BindOnce([](bool) {}));
}

void LocalMainFrameHostImpl::SetResizable(bool resizable)
{
    requestNativeWindowCommand(m_engineViewId, m_frameToken, base::Value::Dict()
        .Set("command", "resizable").Set("enabled", resizable), base::BindOnce([](bool) {}));
}

LocalMainFrameHostImpl::LocalMainFrameHostImpl(WebLocalFrameClientImpl* frame_client)
{
    if (!frame_client)
        return;
    m_engineViewId = frame_client->getEngineViewId();
    m_frameToken = frame_client->getFrameToken();
}

void LocalMainFrameHostImpl::DidFirstVisuallyNonEmptyPaint()
{
    // Painting is not document completion; WebLocalFrameClient::DidFinishLoad is.
}

void LocalMainFrameHostImpl::RequestClose()
{
    WebViewHost* host = nullptr;
    if (!resolveLiveFrame(m_engineViewId, m_frameToken, &host))
        return;
    if (!host->getClosure().m_nativeWindowCommand) {
        mini_electron_destroy_web_view_impl(m_engineViewId);
        return;
    }
    requestNativeWindowCommand(m_engineViewId, m_frameToken,
        base::Value::Dict().Set("command", "close"), base::BindOnce([](bool) {}));
}

void LocalMainFrameHostImpl::DraggableRegionsChanged(WTF::Vector<::blink::mojom::blink::DraggableRegionPtr> regions)
{
    WebViewHost* webview = nullptr;
    if (!resolveLiveFrame(m_engineViewId, m_frameToken, &webview))
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
