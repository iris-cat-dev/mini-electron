
#include "runtime/engine/browser/local_frame_host_impl.h"

#include "runtime/engine/browser/web_view_host.h"
#include "runtime/engine/browser/local_main_frame_host_impl.h"
#include "runtime/engine/renderer/web_local_frame_client_impl.h"
#include "runtime/engine/common/live_id_detect.h"
#include "runtime/engine/common/common.h"
#include "runtime/engine/common/create_and_bind_templ.h"
#include "runtime/engine/common/thread_call.h"
#include "runtime/engine/ui/context_menu.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/mojom/frame/remote_frame.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/frame_owner_properties.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/fullscreen.mojom-blink.h"
#include "third_party/blink/public/mojom/timing/resource_timing.mojom-blink.h"
#include "third_party/blink/public/mojom/scroll/scroll_into_view_params.mojom-blink.h"
#include "third_party/blink/public/mojom/choosers/popup_menu.mojom-blink.h"
#include "third_party/blink/public/mojom/favicon/favicon_url.mojom-blink.h"
#include "third_party/blink/public/mojom/loader/keep_alive_handle.mojom-blink.h"
#include "third_party/blink/public/mojom/loader/keep_alive_handle_factory.mojom-blink.h"
#include "base/notreached.h"
#include "base/strings/utf_string_conversions.h"
#include "base/functional/bind.h"
#include "third_party/blink/renderer/core/exported/web_view_impl.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/navigation_api/navigation_api.h"
#include "third_party/blink/public/mojom/close_watcher/close_listener.mojom-blink.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "third_party/blink/public/mojom/blob/blob.mojom-blink.h"
#include "net/base/net_errors.h"
#include "third_party/blink/public/web/web_console_message.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/mojom/drag/drag.mojom-blink.h"

bool blink::mojom::blink::LocalFrameHost::RunModalAlertDialog(class WTF::String const&, bool)
{
    NOTREACHED();
    return false;
}

bool blink::mojom::blink::LocalFrameHost::RunModalConfirmDialog(class WTF::String const&, bool, bool*)
{
    NOTREACHED();
    return false;
}

bool blink::mojom::blink::LocalFrameHost::RunModalPromptDialog(class WTF::String const&, class WTF::String const&, bool, bool*, class WTF::String*)
{
    NOTREACHED();
    return false;
}

bool blink::mojom::blink::LocalFrameHost::RunBeforeUnloadConfirm(bool, bool*)
{
    NOTREACHED();
    return false;
}

namespace content {

LocalFrameHostImpl::LocalFrameHostImpl(WebLocalFrameClientImpl* frameClient)
    : m_frameClient(frameClient)
{
#if defined(OS_WIN)
    base::SequencedTaskRunner::GetCurrentDefault()->PostNonNestableDelayedTask(FROM_HERE,
        base::BindOnce([]() { ContextMenu::get();}), base::Seconds(3));
#endif // 延迟后初始化一下，这样真正弹出菜单的时候不会抢焦点了

}

LocalFrameHostImpl::~LocalFrameHostImpl()
{
}

void LocalFrameHostImpl::EnterFullscreen(
    ::blink::mojom::blink::FullscreenOptionsPtr options, ::blink::mojom::blink::LocalFrameHost::EnterFullscreenCallback callback)
{
    const auto id = m_frameClient->getEngineViewId();
    requestNativeWindowCommand(m_frameClient,
        base::Value::Dict().Set("command", "fullscreen").Set("enabled", true),
        base::BindOnce([](mini_electron_web_view id, EnterFullscreenCallback reply, bool granted) {
            auto* host = static_cast<WebViewHost*>(common::LiveIdDetect::getWebViewIds()->getPtr(id));
            granted = granted && host && host->getWebView();
            std::move(reply).Run(granted);
        }, id, std::move(callback)));
}

void LocalFrameHostImpl::ExitFullscreen()
{
    requestNativeWindowCommand(m_frameClient,
        base::Value::Dict().Set("command", "fullscreen").Set("enabled", false),
        base::BindOnce([](bool) {}));
}

void LocalFrameHostImpl::FullscreenStateChanged(bool is_fullscreen, ::blink::mojom::blink::FullscreenOptionsPtr options)
{
    // A fullscreen element can be removed without a document.exitFullscreen()
    // request. Reconcile that transition with the native window too.
    if (!is_fullscreen)
        ExitFullscreen();
}

void LocalFrameHostImpl::RegisterProtocolHandler(const WTF::String& scheme, const ::blink::KURL& url, bool user_gesture)
{
    // No per-profile web-handler permission/registry is installed. Registration
    // may be denied by the user agent; do not claim or install an OS handler.
    if (m_frameClient && m_frameClient->getFrame())
        m_frameClient->getFrame()->AddMessageToConsole(blink::WebConsoleMessage(
            blink::mojom::ConsoleMessageLevel::kWarning,
            blink::WebString::FromUTF8("Protocol handler registration was denied by this browser.")));
}

void LocalFrameHostImpl::UnregisterProtocolHandler(const WTF::String& scheme, const ::blink::KURL& url, bool user_gesture)
{
    // RegisterProtocolHandler never grants registration in this embedder, so
    // there is no matching web handler to remove (and no OS registry to edit).
}

void LocalFrameHostImpl::DidDisplayInsecureContent()
{
    printFuncName(__FUNCTION__, false, false);
}

void LocalFrameHostImpl::DidContainInsecureFormAction()
{
    printFuncName(__FUNCTION__, false, false);
}

void LocalFrameHostImpl::MainDocumentElementAvailable(bool uses_temporary_zoom_level)
{
    // FrameLoader has already installed the document element and calls the
    // local WebLocalFrameClient below this notification. There is no separate
    // browser DOM or plugin temporary-zoom observer to update in this embedder.
}

void LocalFrameHostImpl::SetNeedsOcclusionTracking(bool needs_tracking)
{
    printFuncName(__FUNCTION__, false, false);
}

void LocalFrameHostImpl::SetVirtualKeyboardMode(::ui::mojom::blink::VirtualKeyboardMode type)
{
    printFuncName(__FUNCTION__, false, false);
}

void LocalFrameHostImpl::VisibilityChanged(::blink::mojom::blink::FrameVisibility visibility)
{
    printFuncName(__FUNCTION__, false, false);
}

void LocalFrameHostImpl::DidChangeThemeColor(absl::optional<::SkColor> theme_color)
{
    printFuncName(__FUNCTION__, false, false);
}

void LocalFrameHostImpl::DidChangeBackgroundColor(const ::SkColor4f& background_color, bool color_adjust)
{
    printFuncName(__FUNCTION__, false, false);
}

void LocalFrameHostImpl::DidFailLoadWithError(const ::blink::KURL& url, int32_t error_code)
{
    if (!m_frameClient)
        return;
    mini_electron_web_view webviewHandle = m_frameClient->getEngineViewId();
    intptr_t id = m_frameClient->getFrameId();
    WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview || !webview->getClosure().m_LoadingFinishCallback)
        return;

    std::string* urlStr = new std::string(url.GetString().Utf8());
    ThreadCall::callUiThreadAsync(FROM_HERE, [webviewHandle, urlStr, id]() {
        WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
        if (webview && webview->getClosure().m_LoadingFinishCallback)
            webview->getClosure().m_LoadingFinishCallback(
                webviewHandle, webview->getClosure().m_LoadingFinishParam, (mini_electron_web_frame_handle)id, urlStr->c_str(), MINI_ELECTRON_LOADING_FAILED, "");
        delete urlStr;
    });
}

void LocalFrameHostImpl::DidFocusFrame()
{
    WebViewHost* host = getEngineWebView();
    if (host && host->getWebView())
        host->getWebView()->SetFocusedFrame(m_frameClient->getFrame());
}

void LocalFrameHostImpl::DidCallFocus()
{
    requestNativeWindowCommand(m_frameClient,
        base::Value::Dict().Set("command", "focus"), base::BindOnce([](bool) {}));
}

void LocalFrameHostImpl::EnforceInsecureRequestPolicy(::blink::mojom::blink::InsecureRequestPolicy policy_bitmap)
{
    printFuncName(__FUNCTION__, false, false);
}

void LocalFrameHostImpl::EnforceInsecureNavigationsSet(const WTF::Vector<uint32_t>& set)
{
    // ExecutionContextCSPDelegate updates the live SecurityContext before
    // notifying us. All child frames are local; no remote replicas need this set.
}

void LocalFrameHostImpl::SuddenTerminationDisablerChanged(bool present, ::blink::mojom::blink::SuddenTerminationDisablerType disabler_type)
{
    // This embedder never uses Chromium's sudden renderer-process termination
    // optimization. Before-unload decisions use RunBeforeUnloadConfirm instead.
}

void LocalFrameHostImpl::HadStickyUserActivationBeforeNavigationChanged(bool has_gesture)
{
    // Only remote-frame replication needs this notification (frame.mojom).
    // The local frame's activation state has already been updated by Blink.
}

void LocalFrameHostImpl::ScrollRectToVisibleInParentFrame(const ::gfx::RectF& rect_to_scroll, ::blink::mojom::blink::ScrollIntoViewParamsPtr params)
{
    printFuncName(__FUNCTION__, true, false);
}

void LocalFrameHostImpl::BubbleLogicalScrollInParentFrame(::blink::mojom::blink::ScrollDirection direction, ::ui::ScrollGranularity granularity)
{
    printFuncName(__FUNCTION__, true, false);
}

void LocalFrameHostImpl::DidBlockNavigation(
    const ::blink::KURL& blocked_url, const ::blink::KURL& initiator_url, ::blink::mojom::blink::NavigationBlockedReason reason)
{
    // Blink has already blocked this navigation. The embedder has no blocked-
    // navigation reporting UI; do not turn a denied navigation into a load.
}

void LocalFrameHostImpl::DidChangeLoadProgress(double load_progress)
{
    char* output = (char*)malloc(0x100);
    sprintf(output, "LocalFrameHostImpl::DidChangeLoadProgress: %f\n", (float)load_progress);
    OutputDebugStringA(output);
    free(output);
}

void LocalFrameHostImpl::DidFinishLoad(const ::blink::KURL& validatedUrl)
{
    printFuncName(__FUNCTION__, false, false);
    if (!m_frameClient)
        return;

}

void LocalFrameHostImpl::DispatchLoad()
{
    printFuncName(__FUNCTION__, true, false);
}

WebViewHost* LocalFrameHostImpl::getEngineWebView() const
{
    if (!m_frameClient)
        return nullptr;

    mini_electron_web_view webviewHandle = m_frameClient->getEngineViewId();
    WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    return webview;
}

void LocalFrameHostImpl::GoToEntryAtOffset(int32_t offset, bool has_user_gesture, std::optional<::blink::scheduler::TaskAttributionId> soft_navigation_heuristics_task_id)
{
    //printFuncName(__FUNCTION__, true, false);
    WebViewHost* webview = getEngineWebView();
    if (webview)
        webview->navigateBackForwardSoon(offset);
}

void LocalFrameHostImpl::NavigateToNavigationApiKey(const WTF::String& key, bool has_user_gesture, std::optional<::blink::scheduler::TaskAttributionId> soft_navigation_heuristics_task_id)
{
    WebViewHost* host = getEngineWebView();
    if (!host || !m_frameClient->getFrame())
        return;
    if (!host->m_navigationController->navigateToNavigationApiKey(m_frameClient->getFrame(), blink::WebString(key))) {
        auto* frame = static_cast<blink::WebLocalFrameImpl*>(m_frameClient->getFrame())->GetFrame();
        frame->DomWindow()->navigation()->TraverseCancelled(key, blink::mojom::blink::TraverseCancelledReason::kNotFound);
    }
}

void LocalFrameHostImpl::UpdateTitle(const ::WTF::String& title, ::base::i18n::TextDirection titleDirection)
{
    printFuncName(__FUNCTION__, false, false);
    if (!m_frameClient)
        return;

    mini_electron_web_view webviewHandle = m_frameClient->getEngineViewId();
    WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return;

    std::string* titleStr = new std::string(title.Utf8());
    ThreadCall::callUiThreadAsync(FROM_HERE, [webviewHandle, titleStr]() {
        WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
        if (webview) {
            webview->setWindowTitle(*titleStr);
            if (webview->getClosure().m_TitleChangedCallback)
                webview->getClosure().m_TitleChangedCallback(webviewHandle, webview->getClosure().m_TitleChangedParam, titleStr->c_str());
        }
        delete titleStr;
    });
}

void LocalFrameHostImpl::UpdateUserActivationState(
    ::blink::mojom::blink::UserActivationUpdateType update_type, ::blink::mojom::blink::UserActivationNotificationType notification_type)
{
    printFuncName(__FUNCTION__, false, false);
}

void LocalFrameHostImpl::HandleAccessibilityFindInPageResult(::blink::mojom::blink::FindInPageResultAXParamsPtr params)
{
    // Find selection/highlighting remains in Blink. This embedder has no
    // browser accessibility tree consumer for the optional AX result event.
}

void LocalFrameHostImpl::HandleAccessibilityFindInPageTermination()
{
    // There is no browser AX find-result object to clear; Blink owns the find
    // selection and terminates it independently of this notification.
}

void LocalFrameHostImpl::DocumentOnLoadCompleted()
{
    printFuncName(__FUNCTION__, false, false);
    if (!m_frameClient)
        return;
    mini_electron_web_view webviewHandle = m_frameClient->getEngineViewId();
    WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview)
        return;

    blink::LocalFrameToken token = m_frameClient->getFrame()->GetLocalFrameToken();
    mini_electron_web_frame_handle frameId = (void*)(blink::LocalFrameToken::Hasher()(token));

    if (webview->getClosure().m_DocumentReadyInBlinkCallback)
        webview->getClosure().m_DocumentReadyInBlinkCallback(webviewHandle, webview->getClosure().m_DocumentReadyInBlinkParam, frameId);

    ThreadCall::callUiThreadAsync(FROM_HERE, [webviewHandle, frameId]() {
        WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
        if (webview && webview->getClosure().m_DocumentReadyCallback)
            webview->getClosure().m_DocumentReadyCallback(webviewHandle, webview->getClosure().m_DocumentReadyParam, frameId);
    });
}

void LocalFrameHostImpl::ForwardResourceTimingToParent(::blink::mojom::blink::ResourceTimingInfoPtr timing)
{
    printFuncName(__FUNCTION__, true, false);
}

void LocalFrameHostImpl::DidDispatchDOMContentLoadedEvent()
{
    printFuncName(__FUNCTION__, false, false);
}

bool LocalFrameHostImpl::RunModalAlertDialog(const ::WTF::String& alertMsg, bool disable_third_party_subframe_suppresion)
{
    std::string* txt = new std::string(alertMsg.Utf8());
    bool needPopupNativeMsgBox = true;
    do {
        if (!m_frameClient)
            break;
        mini_electron_web_view webviewHandle = m_frameClient->getEngineViewId();
        WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
        if (!webview || !webview->getClosure().m_AlertBoxCallback)
            break;

        auto invoke_dialog = [webviewHandle, txt]() {
            WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
            if (webview)
                webview->getClosure().m_AlertBoxCallback(webviewHandle, webview->getClosure().m_AlertBoxParam, txt->c_str());
        };
        if (webview->getClosure().m_javascriptDialogsOnBlinkThread)
            invoke_dialog();
        else
            ThreadCall::callUiThreadSync(FROM_HERE, std::move(invoke_dialog));
        needPopupNativeMsgBox = false;
    } while (false);

#ifdef _WIN32
    if (needPopupNativeMsgBox)
        MessageBoxW(0, (const WCHAR*)(base::UTF8ToUTF16(*txt).c_str()), L"Alert", 0);
#endif // _WIN32
    delete txt;
    return true;
}

void LocalFrameHostImpl::RunModalAlertDialog(
    const ::WTF::String& alertMsg, bool disable_third_party_subframe_suppresion, ::blink::mojom::blink::LocalFrameHost::RunModalAlertDialogCallback callback)
{
    RunModalAlertDialog(alertMsg, disable_third_party_subframe_suppresion);
    std::move(callback).Run();
}

bool LocalFrameHostImpl::RunModalConfirmDialog(const ::WTF::String& alertMsg, bool disable_third_party_subframe_suppresion, bool* outSuccess)
{
    std::string* txt = new std::string(alertMsg.Utf8());
    bool needPopupNativeMsgBox = true;
    BOOL ret = FALSE;

    do {
        if (!m_frameClient)
            break;
        mini_electron_web_view webviewHandle = m_frameClient->getEngineViewId();
        WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
        if (!webview || !webview->getClosure().m_ConfirmBoxCallback)
            break;

        auto invoke_dialog = [webviewHandle, txt, &ret]() {
            WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
            if (webview)
                ret = webview->getClosure().m_ConfirmBoxCallback(webviewHandle, webview->getClosure().m_ConfirmBoxParam, txt->c_str());
        };
        if (webview->getClosure().m_javascriptDialogsOnBlinkThread)
            invoke_dialog();
        else
            ThreadCall::callUiThreadSync(FROM_HERE, std::move(invoke_dialog));
        needPopupNativeMsgBox = false;
    } while (false);

    if (needPopupNativeMsgBox)
        ret = MessageBoxA(0, txt->c_str(), "Confirm", MB_YESNO) == IDYES;

    delete txt;
    *outSuccess = ret;
    return true;
}

void LocalFrameHostImpl::RunModalConfirmDialog(
    const ::WTF::String& alertMsg, bool disable_third_party_subframe_suppresion, ::blink::mojom::blink::LocalFrameHost::RunModalConfirmDialogCallback callback)
{
    bool accepted = false;
    RunModalConfirmDialog(alertMsg, disable_third_party_subframe_suppresion, &accepted);
    std::move(callback).Run(accepted);
}

bool LocalFrameHostImpl::RunModalPromptDialog(
    const ::WTF::String& alertMsg, const ::WTF::String& defaultVal, bool disableThirdPartySubframeSuppresion, bool* outSuccess, ::WTF::String* outResult)
{
    std::string* txt = new std::string(alertMsg.Utf8());
    std::string* defaultValue = new std::string(defaultVal.Utf8());
    mini_electron_string_ptr ret = nullptr;
    BOOL isOk = FALSE;

    do {
        if (!m_frameClient)
            break;
        mini_electron_web_view webviewHandle = m_frameClient->getEngineViewId();
        WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
        if (!webview || !webview->getClosure().m_PromptBoxCallback)
            break;

        auto invoke_dialog = [webviewHandle, txt, defaultValue, &ret, &isOk]() {
            WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
            if (webview)
                ret = webview->getClosure().m_PromptBoxCallback(
                    webviewHandle, webview->getClosure().m_PromptBoxParam, txt->c_str(), defaultValue->c_str(), &isOk);
        };
        if (webview->getClosure().m_javascriptDialogsOnBlinkThread)
            invoke_dialog();
        else
            ThreadCall::callUiThreadSync(FROM_HERE, std::move(invoke_dialog));
    } while (false);

    delete txt;
    delete defaultValue;

    if (ret) {
        *outResult = ::WTF::String::FromUTF8(std::string_view(mini_electron_get_string(ret), mini_electron_get_string_len(ret)));
        mini_electron_delete_string(ret);
        *outSuccess = isOk;
        return true;
    }

    *outSuccess = isOk;
    return false;
}

void LocalFrameHostImpl::RunModalPromptDialog(const ::WTF::String& alertMsg, const ::WTF::String& defaultVal, bool disable_third_party_subframe_suppresion,
    ::blink::mojom::blink::LocalFrameHost::RunModalPromptDialogCallback callback)
{
    bool accepted = false;
    WTF::String result;
    RunModalPromptDialog(alertMsg, defaultVal, disable_third_party_subframe_suppresion, &accepted, &result);
    std::move(callback).Run(accepted, result);
}

// Sync method. This signature is used by the client side; the service side
// should implement the signature with callback below.
bool LocalFrameHostImpl::RunBeforeUnloadConfirm(bool is_reload, bool* outSuccess)
{
    *outSuccess = false;
    WebViewHost* host = getEngineWebView();
    if (!host)
        return false;
    if (host->getClosure().m_beforeUnloadConfirm) {
        *outSuccess = host->getClosure().m_beforeUnloadConfirm(is_reload);
        return true;
    }
    return RunModalConfirmDialog(WTF::String(is_reload ? "Reload this page?" : "Leave this page?"), false, outSuccess);
}

void LocalFrameHostImpl::RunBeforeUnloadConfirm(bool is_reload, RunBeforeUnloadConfirmCallback callback)
{
    bool accepted = false;
    RunBeforeUnloadConfirm(is_reload, &accepted);
    std::move(callback).Run(accepted);
}

void LocalFrameHostImpl::UpdateFaviconURL(WTF::Vector<::blink::mojom::blink::FaviconURLPtr> favicon_urls)
{
    WebViewHost* host = getEngineWebView();
    if (!host || !m_frameClient->getFrame() || m_frameClient->getFrame()->Parent())
        return;
    std::vector<std::string> urls;
    urls.reserve(favicon_urls.size());
    for (const auto& favicon : favicon_urls)
        urls.push_back(favicon->icon_url.GetString().Utf8());
    const auto id = host->getId();
    ThreadCall::callUiThreadAsync(FROM_HERE, [id, urls = std::move(urls)]() mutable {
        auto* host = static_cast<WebViewHost*>(common::LiveIdDetect::getWebViewIds()->getPtr(id));
        if (host && host->getClosure().m_faviconURLsChanged)
            host->getClosure().m_faviconURLsChanged(std::move(urls));
    });
}

void beginNavigation(std::unique_ptr<blink::WebNavigationInfo> info, blink::WebNavigationControl* navigationControl, int64_t mbwebviewId,
    MojoHandle dataPipeProducerHandle, blink::WebLocalFrame* frame);

// A data-URL blob contains the URL text itself, not Blob side metadata.
// Both the pipe EOF and BlobReaderClient completion must arrive before starting
// the download; neither callback retains the frame client.
class DataURLDownload final : public blink::mojom::blink::BlobReaderClient,
                              public mojo::DataPipeDrainer::Client {
public:
    DataURLDownload(blink::LocalFrameToken frame,
        std::unique_ptr<blink::WebNavigationInfo> info, WTF::String name,
        mojo::PendingRemote<blink::mojom::blink::Blob> blob)
        : m_frame(frame), m_info(std::move(info)), m_name(std::move(name)),
          m_blob(std::move(blob)), m_receiver(this)
    {
    }

    void start()
    {
        mojo::ScopedDataPipeProducerHandle producer;
        mojo::ScopedDataPipeConsumerHandle consumer;
        if (mojo::CreateDataPipe(nullptr, producer, consumer) != MOJO_RESULT_OK) {
            delete this;
            return;
        }
        auto client = m_receiver.BindNewPipeAndPassRemote();
        m_receiver.set_disconnect_handler(base::BindOnce(&DataURLDownload::disconnected, base::Unretained(this)));
        m_blob.set_disconnect_handler(base::BindOnce(&DataURLDownload::disconnected, base::Unretained(this)));
        m_drainer = std::make_unique<mojo::DataPipeDrainer>(this, std::move(consumer));
        m_blob->ReadAll(std::move(producer), std::move(client));
    }

private:
    void OnCalculatedSize(uint64_t total_size, uint64_t expected_size) override
    {
        m_expectedSize = expected_size;
    }
    void OnDataAvailable(base::span<const uint8_t> data) override
    {
        m_url.append(reinterpret_cast<const char*>(data.data()), data.size());
    }
    void OnDataComplete() override
    {
        m_drained = true;
        finish();
    }
    void OnComplete(int32_t status, uint64_t data_length) override
    {
        if (status != net::OK || data_length != m_expectedSize) {
            delete this;
            return;
        }
        m_completed = true;
        finish();
    }
    void disconnected()
    {
        if (!m_completed)
            delete this;
    }
    void finish()
    {
        if (!m_completed || !m_drained)
            return;
        blink::WebLocalFrame* frame = blink::WebLocalFrame::FromFrameToken(m_frame);
        if (frame && m_url.size() == m_expectedSize) {
            blink::KURL url(WTF::String::FromUTF8(m_url));
            if (url.IsValid() && url.ProtocolIsData()) {
                m_info->url_request.SetUrl(url);
                static_cast<WebLocalFrameClientImpl*>(frame->Client())->beginDownload(
                    std::move(m_info), std::make_unique<WTF::String>(std::move(m_name)));
            }
        }
        delete this;
    }

    blink::LocalFrameToken m_frame;
    std::unique_ptr<blink::WebNavigationInfo> m_info;
    WTF::String m_name;
    mojo::Remote<blink::mojom::blink::Blob> m_blob;
    mojo::Receiver<blink::mojom::blink::BlobReaderClient> m_receiver;
    std::unique_ptr<mojo::DataPipeDrainer> m_drainer;
    std::string m_url;
    uint64_t m_expectedSize = 0;
    bool m_drained = false;
    bool m_completed = false;
};

void LocalFrameHostImpl::DownloadURL(::blink::mojom::blink::DownloadURLParamsPtr params)
{
    printFuncName(__FUNCTION__, false, false);

    std::unique_ptr<blink::WebNavigationInfo> info = std::make_unique<blink::WebNavigationInfo>();
    info->url_request.SetUrl(blink::WebURL(params->url));
    info->url_request.SetReferrerString(params->referrer->url.GetString());
    info->url_request.SetReferrerPolicy(params->referrer->policy);
    //info->navigation_policy = blink::kWebNavigationPolicyDownload;

    if (params->data_url_blob) {
        auto* download = new DataURLDownload(m_frameClient->getFrame()->GetLocalFrameToken(),
            std::move(info), params->suggested_name, std::move(params->data_url_blob));
        download->start();
        return;
    }

    m_frameClient->beginDownload(std::move(info), std::make_unique<String>(params->suggested_name));
}

void LocalFrameHostImpl::FocusedElementChanged(bool is_editable_element, bool is_richly_editable_element, const ::gfx::Rect& bounds_in_frame_widget, ::blink::mojom::blink::FocusType focus_type)
{
    //printFuncName(__FUNCTION__, true, false);
}

void LocalFrameHostImpl::TextSelectionChanged(const ::WTF::String& text, uint32_t offset, const ::gfx::Range& range)
{
    //printFuncName(__FUNCTION__, true, false);
}

void LocalFrameHostImpl::ShowPopupMenu(::mojo::PendingRemote<::blink::mojom::blink::PopupMenuClient> popup_client, const ::gfx::Rect& bounds,
    int32_t item_height, double font_size, int32_t selected_item, WTF::Vector<::blink::mojom::blink::MenuItemPtr> menu_items, bool right_aligned,
    bool allow_multiple_selection)

{
    // External menus have not been enabled by this embedder. If a caller opts
    // into them without a native presenter, complete an actual cancellation.
    mojo::Remote<blink::mojom::blink::PopupMenuClient> client(std::move(popup_client));
    client->DidCancel();
}

void LocalFrameHostImpl::CreateNewPopupWidget(::mojo::PendingAssociatedReceiver<::blink::mojom::blink::PopupWidgetHost> popupHost,
    ::mojo::PendingAssociatedReceiver<::blink::mojom::blink::WidgetHost> blinkWidgetHost,
    ::mojo::PendingAssociatedRemote<::blink::mojom::blink::Widget> blinkWidget)
{
    //printFuncName(__FUNCTION__, true, false);
    mini_electron_web_view parentWebviewId = m_frameClient->getEngineViewId();
    WebViewHost* newWebview = new WebViewHost(true);
    newWebview->initPopupWidgetModeInBlinkThread(parentWebviewId, std::move(popupHost), std::move(blinkWidgetHost), std::move(blinkWidget));
}

void LocalFrameHostImpl::ShowContextMenu(
    ::mojo::PendingAssociatedRemote<::blink::mojom::blink::ContextMenuClient> client, const ::blink::UntrustworthyContextMenuParams& params)
{
    WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(m_frameClient->getEngineViewId());
    if (!webview || !webview->m_isContextMenuEnable)
        return;
    auto& callback = webview->getClosure().m_contextMenuCallback;
    if (callback) {
        callback(reinterpret_cast<mini_electron_web_frame_handle>(
                     static_cast<uintptr_t>(getFrameIdByWebLocalFrame(m_frameClient->getFrame()))),
            params);
        return;
    }
#if defined(OS_WIN)
    // Standalone engine consumers retain their default menu. Electron consumers
    // own the event and must never get a second legacy menu.
    ContextMenu::get()->setCurrentWebview(webview);
    ContextMenu::get()->show(params, getFrameIdByWebLocalFrame(m_frameClient->getFrame()));
#endif
}

void LocalFrameHostImpl::DidLoadResourceFromMemoryCache(const ::blink::KURL& url, const WTF::String& http_method, const WTF::String& mime_type,
    ::network::mojom::blink::RequestDestination request_destination, bool include_credentials)
{
    printFuncName(__FUNCTION__, false, false);
}

void LocalFrameHostImpl::DidChangeFrameOwnerProperties(
    const ::blink::FrameToken& child_frame_token, ::blink::mojom::blink::FrameOwnerPropertiesPtr frame_owner_properties)
{
    printFuncName(__FUNCTION__, false, false);
}

void LocalFrameHostImpl::DidChangeOpener(const absl::optional<::blink::LocalFrameToken>& opener_frame)
{
    // LocalFrame::SetOpener updates the local opener after sending this
    // notification. There are no browser-side remote opener replicas.
}

void LocalFrameHostImpl::DidChangeFramePolicy(const ::blink::FrameToken& child_frame_token, const ::blink::FramePolicy& frame_policy)
{
    // HTMLFrameOwnerElement owns the live child FramePolicy. CreateChildFrame
    // creates local children, so there is no remote-frame policy replica.
}

void LocalFrameHostImpl::DidChangeIframeAttributes(const ::blink::FrameToken& child_frame_token, ::blink::mojom::blink::IframeAttributesPtr attributes)
{
    printFuncName(__FUNCTION__, false, false);
}

void LocalFrameHostImpl::CapturePaintPreviewOfSubframe(const ::gfx::Rect& clip_rect, const ::base::UnguessableToken& guid)
{
    printFuncName(__FUNCTION__, true, false);
}

void LocalFrameHostImpl::SetCloseListener(::mojo::PendingRemote<::blink::mojom::blink::CloseListener> listener)
{
    m_closeListener.reset();
    m_closeListener.Bind(std::move(listener));
    // Desktop Escape is dispatched by CloseWatcher::WatcherStack locally.
    // Retain the pipe for the lifetime of this frame rather than disconnecting
    // the registered watcher merely because there is no mobile Back affordance.
}

void LocalFrameHostImpl::Detach()
{
    // WebLocalFrameClientImpl::FrameDetached publishes the detach event and
    // clears the actual frame. This associated-host notification must not
    // detach the same frame a second time or retain a dangling client pointer.
    m_closeListener.reset();
    m_frameClient = nullptr;
}

class KeepAliveHandleImpl : public blink::mojom::blink::KeepAliveHandle {
public:
    KeepAliveHandleImpl()
    {
    }

    ~KeepAliveHandleImpl() override
    {
    }
};

class KeepAliveHandleFactoryImpl : public blink::mojom::blink::KeepAliveHandleFactory {
public:
    KeepAliveHandleFactoryImpl()
    {
    }

    void IssueKeepAliveHandle(::mojo::PendingReceiver<::blink::mojom::blink::KeepAliveHandle> keepAliveHandle) override
    {
        createAndBindInterface<::blink::mojom::blink::KeepAliveHandle, KeepAliveHandleImpl>(keepAliveHandle.PassPipe());
    }
};

void LocalFrameHostImpl::GetKeepAliveHandleFactory(::mojo::PendingReceiver<::blink::mojom::blink::KeepAliveHandleFactory> factory)
{
    printFuncName(__FUNCTION__, false, false);
    createAndBindInterface<::blink::mojom::blink::KeepAliveHandleFactory, KeepAliveHandleFactoryImpl>(factory.PassPipe());
}

void LocalFrameHostImpl::DidAddMessageToConsole(::blink::mojom::blink::ConsoleMessageLevel log_level, const ::WTF::String& msg, uint32_t line_number,
    const ::WTF::String& source_id, const ::WTF::String& untrusted_stack_trace)
{
//     std::string output = "DidAddMessageToConsole:[";
//     output += msg.Utf8();
//     output += "],[";
//     output += source_id.Utf8();
//     output += "]\n";
//     OutputDebugStringA(output.c_str());
}

void LocalFrameHostImpl::FrameSizeChanged(const ::gfx::Size& size)
{
    //printFuncName(__FUNCTION__, true, false);

    //     char* output = (char*)malloc(0x100);
    //     sprintf_s(output, 0x99, "LocalFrameHostImpl::FrameSizeChanged not impl: %d %d\n", size.width(), size.height());
    //     OutputDebugStringA(output);
    //     free(output);
}

void LocalFrameHostImpl::DidInferColorScheme(::blink::mojom::blink::PreferredColorScheme color_scheme)
{
    printFuncName(__FUNCTION__, false, false);
}

void LocalFrameHostImpl::DidChangeSrcDoc(const ::blink::FrameToken& childFrameToken, const WTF::String& srcdocValue)
{
    blink::LocalFrameToken frameToken = childFrameToken.GetAs<blink::LocalFrameToken>();
    blink::WebLocalFrame* child = blink::WebLocalFrame::FromFrameToken(frameToken);
    if (!child)
        return;
    WebLocalFrameClientImpl* client = (WebLocalFrameClientImpl*)(child->Client());
    if (!client)
        return;
    client->m_srcdoc = srcdocValue;
}


void LocalFrameHostImpl::ReceivedDelegatedCapability(::blink::mojom::blink::DelegatedCapability delegated_capability)
{
    // LocalDOMWindow delivers the delegated capability on the MessageEvent.
    // There is no separate browser-side capability-token cache in this
    // single-local-frame-tree embedder; this does not grant a native permission.
}

// bool LocalFrameHostImpl::CreatePortal(::mojo::PendingAssociatedReceiver<::blink::mojom::blink::Portal> portal,
//     ::mojo::PendingAssociatedRemote<::blink::mojom::blink::PortalClient> client,
//     ::blink::mojom::blink::RemoteFrameInterfacesFromRendererPtr remote_frame_interfaces,
//     ::blink::mojom::blink::FrameReplicationStatePtr* out_initial_replicated_state, ::blink::PortalToken* out_portal_token,
//     ::blink::RemoteFrameToken* out_frame_token, ::base::UnguessableToken* out_devtools_frame_token)
// {
//     printFuncName(__FUNCTION__, true, false);
//     return false;
// }
// 
// void LocalFrameHostImpl::CreatePortal(::mojo::PendingAssociatedReceiver<::blink::mojom::blink::Portal> portal,
//     ::mojo::PendingAssociatedRemote<::blink::mojom::blink::PortalClient> client,
//     ::blink::mojom::blink::RemoteFrameInterfacesFromRendererPtr remote_frame_interfaces, CreatePortalCallback callback)
// 
// {
//     printFuncName(__FUNCTION__, true, false);
// }

// bool LocalFrameHostImpl::AdoptPortal(const ::blink::PortalToken& portal_token,
//     ::blink::mojom::blink::RemoteFrameInterfacesFromRendererPtr remote_frame_interfaces, ::blink::mojom::blink::FrameReplicationStatePtr* out_replicated_state,
//     ::blink::RemoteFrameToken* out_frame_token, ::base::UnguessableToken* out_devtools_frame_token)
// {
//     printFuncName(__FUNCTION__, true, false);
//     return false;
// }
// 
// void LocalFrameHostImpl::AdoptPortal(
//     const ::blink::PortalToken& portal_token, ::blink::mojom::blink::RemoteFrameInterfacesFromRendererPtr remote_frame_interfaces, AdoptPortalCallback callback)
// 
// {
//     printFuncName(__FUNCTION__, true, false);
// }

void LocalFrameHostImpl::CreateFencedFrame(::mojo::PendingAssociatedReceiver<::blink::mojom::blink::FencedFrameOwnerHost> fenced_frame,
    ::blink::mojom::blink::RemoteFrameInterfacesFromRendererPtr remote_frame_interfaces, const ::blink::RemoteFrameToken& frame_token,
    const ::base::UnguessableToken& devtools_frame_token)
{
    printFuncName(__FUNCTION__, true, false);
}

void LocalFrameHostImpl::StartLoadingForAsyncNavigationApiCommit()
{
    if (m_frameClient)
        m_frameClient->StartLoadingForAsyncNavigationApiCommit();
}

void LocalFrameHostImpl::NavigateEventHandlerPresenceChanged(bool present)
{
    // NavigationApi dispatches/intercepts navigation in the local renderer.
    // There is no browser-side navigation event fast path to enable or disable.
}

void LocalFrameHostImpl::UpdateAppTitle(const ::WTF::String& app_title) 
{
    UpdateTitle(app_title, base::i18n::UNKNOWN_DIRECTION);
}

void LocalFrameHostImpl::DidConsumeHistoryUserActivation()
{
    // LocalFrame::ConsumeHistoryUserActivation visits every local frame in the
    // page immediately after sending this message; no remote replicas exist.
}

void LocalFrameHostImpl::SendFencedFrameReportingBeacon(const WTF::String& event_data, const WTF::String& event_type,
    const WTF::Vector<::blink::FencedFrame::ReportingDestination>& destinations, bool cross_origin_exposed)
{
    printFuncName(__FUNCTION__, true, false);
}

void LocalFrameHostImpl::SendFencedFrameReportingBeaconToCustomURL(const ::blink::KURL& destination_url, bool cross_origin_exposed)
{
    printFuncName(__FUNCTION__, true, false);
}

void LocalFrameHostImpl::DisableUntrustedNetworkInFencedFrame(DisableUntrustedNetworkInFencedFrameCallback callback)
{
    printFuncName(__FUNCTION__, true, false);
}

void LocalFrameHostImpl::ExemptUrlFromNetworkRevocationForTesting(const ::blink::KURL& exempted_url, ExemptUrlFromNetworkRevocationForTestingCallback callback)
{
    printFuncName(__FUNCTION__, true, false);
}

void LocalFrameHostImpl::SendLegacyTechEvent(const WTF::String& type, ::blink::mojom::blink::LegacyTechEventCodeLocationPtr code_location)
{
    // Enterprise legacy-technology reporting is not configured in this
    // embedder. Preserve that privacy choice instead of adding an uploader.
}


void LocalFrameHostImpl::SetFencedFrameAutomaticBeaconReportEventData(::blink::mojom::blink::AutomaticBeaconType event_type, const WTF::String& event_data,
    const WTF::Vector<::blink::FencedFrame::ReportingDestination>& destinations, bool once, bool cross_origin_exposed)
{
    printFuncName(__FUNCTION__, true, false);
}

void LocalFrameHostImpl::SendPrivateAggregationRequestsForFencedFrameEvent(const WTF::String& event_type)
{
    printFuncName(__FUNCTION__, true, false);
}

void LocalFrameHostImpl::ForwardFencedFrameEventAndUserActivationToEmbedder(const WTF::String& event_type)
{
    printFuncName(__FUNCTION__, true, false);
}

void LocalFrameHostImpl::OnViewTransitionOptInChanged(::blink::mojom::blink::ViewTransitionSameOriginOptIn view_transition_opt_in)
{
    // Cross-document transitions are not orchestrated by this browser. Blink
    // retains the opt-in for the document; same-document transitions stay local.
}

void LocalFrameHostImpl::StartDragging(const ::blink::WebDragData& drag_data, ::blink::DragOperationsMask operations_allowed, const ::SkBitmap& image,
    const ::gfx::Vector2d& cursor_offset_in_dip, const ::gfx::Rect& drag_obj_rect_in_dip, ::blink::mojom::blink::DragEventSourceInfoPtr event_info)
{
    if (!m_frameClient || !m_frameClient->getFrame())
        return;
    auto* widget = m_frameClient->getLocalRootWebFrameWidget();
    if (!widget)
        return;
    // This embedding has an inbound file-drop bridge, but no outbound native
    // drag session. Finish the Blink source with kNone, including dragend and
    // controller cleanup, instead of claiming a successful drop.
    const auto token = m_frameClient->getFrame()->GetLocalFrameToken();
    const gfx::Point screen = event_info ? event_info->location : widget->ViewRect().origin();
    const gfx::Point viewport = screen - widget->ViewRect().OffsetFromOrigin();
    widget->DragSourceEndedAt(gfx::PointF(viewport), gfx::PointF(screen), ui::mojom::DragOperation::kNone,
        base::BindOnce([](const blink::LocalFrameToken& token) {
            auto* frame = blink::WebLocalFrame::FromFrameToken(token);
            if (!frame)
                return;
            auto* client = static_cast<WebLocalFrameClientImpl*>(frame->Client());
            if (auto* widget = client->getLocalRootWebFrameWidget())
                widget->DragSourceSystemDragEnded();
        }, token));
}

void LocalFrameHostImpl::IssueKeepAliveHandle(::mojo::PendingReceiver<::blink::mojom::blink::NavigationStateKeepAliveHandle> receiver)
{
    // Navigation parameters carry the local initiator's policy/origin snapshot;
    // there is no separately indexed browser RenderFrameHost state to retain.
    // Bind the marker interface for the requested lifetime, like KeepAliveHandle.
    class NavigationStateKeepAliveHandleImpl final : public blink::mojom::blink::NavigationStateKeepAliveHandle { };
    createAndBindInterface<blink::mojom::blink::NavigationStateKeepAliveHandle,
        NavigationStateKeepAliveHandleImpl>(receiver.PassPipe());
}

void LocalFrameHostImpl::NotifyStorageAccessed(::blink::mojom::blink::StorageTypeAccessed storageType, bool blocked)
{
    // LocalFrame has already obtained/enforced the content-settings decision.
    // This notification is for browser storage indicators, which are not exposed.
}

void LocalFrameHostImpl::RecordWindowProxyUsageMetrics(const ::blink::FrameToken& target_frame_token, ::blink::mojom::blink::WindowProxyAccessType access_type)
{
    //printFuncName(__FUNCTION__, true, false);
}

void LocalFrameHostImpl::NotifyDocumentInteractive()
{
    // Document::SetReadyState has already changed the live document to
    // interactive. This embedder's DOM-ready event comes from the existing
    // WebLocalFrameClient path, not this optional browser timing notification.
}

}