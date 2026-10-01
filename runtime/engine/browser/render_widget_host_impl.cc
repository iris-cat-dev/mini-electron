
#include "runtime/engine/browser/render_widget_host_impl.h"

#include <algorithm>
#include "runtime/engine/renderer/platform_event_handler.h"
#include "runtime/engine/renderer/render_thread_impl.h"
#include "runtime/engine/viz/viz_host.h"
#include "runtime/engine/browser/web_view_host.h"
#include "runtime/engine/common/thread_call.h"
#include "runtime/engine/common/live_id_detect.h"
#if defined(OS_WIN)
#include "runtime/engine/ui/tool_tip.h"
#include "base/strings/utf_string_conversions.h"
#endif
#include "third_party/blink/public/web/web_frame_widget.h"
#include "ui/display/screen.h"
#include "ui/gfx/geometry/size.h"

extern "C" MojoResult MojoMakeIsMessageChannelFlag(MojoHandle handle);

namespace content {

void RenderFrameMetadataObserverClientImpl::OnRenderFrameMetadataChanged(uint32_t frame_token, const ::cc::RenderFrameMetadata& metadata)
{
    m_parent->allowResize();
    if (m_parent->m_sinkClient)
        m_parent->m_sinkClient->m_firstSetEmbeds = true;

    if (m_parent->m_visualProperties.local_surface_id == metadata.local_surface_id)
        return;

    m_parent->m_visualProperties.local_surface_id = metadata.local_surface_id;

    m_parent->m_runner->PostTask(FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<RenderWidgetHostImpl> self) {
                if (self->m_blinkWidget.get())
                    self->m_blinkWidget->UpdateVisualProperties(self->m_visualProperties);
            },
            m_parent->m_weakPtr.GetWeakPtr()));
}

void RenderFrameMetadataObserverClientImpl::OnFrameSubmissionForTesting(uint32_t frame_token)
{
    OutputDebugStringA("RenderFrameMetadataObserverClient::OnFrameSubmissionForTesting impl\n");
}

RenderWidgetHostImpl::RenderWidgetHostImpl(int64_t parentWebviewId, scoped_refptr<base::SequencedTaskRunner> runner)
    : m_parentWebviewId(parentWebviewId)
    , m_runner(runner)
{
    uint64_t rand = base::RandUint64();
    viz::FrameSinkId frameSinkId(rand >> 32, rand & 0xffffffff);
    m_frameSinkId = frameSinkId;

    char output[100] = { 0 };
    sprintf(output, "RenderWidgetHostImpl: %p\n", this);
    OutputDebugStringA(output);
}

void RenderWidgetHostImpl::destroy()
{
    if (m_webWiew)
        m_webWiew->Close(); // ������AsyncLayerTreeFrameSinkͬ������
    m_webWiew = nullptr;

    if (getHostFrameSinkManager()) {
        viz::FrameSinkId rootFrameSinkId(0xdead, 0xbeef);
        getHostFrameSinkManager()->InvalidateFrameSinkId(rootFrameSinkId, m_sinkHost);
        if (isSinkReady())
            getHostFrameSinkManager()->InvalidateFrameSinkId(m_frameSinkId, this);
    }
}

RenderWidgetHostImpl::~RenderWidgetHostImpl()
{
    if (m_toolTip)
        delete m_toolTip;

    char output[100] = { 0 };
    sprintf(output, "~RenderWidgetHostImpl: %p\n", this);
    OutputDebugStringA(output);
}

bool RenderWidgetHostImpl::isSinkReady() const
{
    return !!m_sinkClient;
}

float RenderWidgetHostImpl::deviceScaleFactor() const
{
    if (m_visualProperties.screen_infos.screen_infos.empty())
        return 1.f;
    const float dsf = m_visualProperties.screen_infos.current().device_scale_factor;
    return dsf > 0.f ? dsf : 1.f;
}

void RenderWidgetHostImpl::setDeviceScaleFactor(float dsf)
{
    if (dsf <= 0.f)
        dsf = 1.f;
    if (m_visualProperties.screen_infos.screen_infos.empty()) {
        display::ScreenInfo info;
        info.device_scale_factor = dsf;
        m_visualProperties.screen_infos = display::ScreenInfos(info);
    } else {
        m_visualProperties.screen_infos.mutable_current().device_scale_factor = dsf;
    }

    if (m_webWiew) {
        m_webWiew->SetZoomFactorForDeviceScaleFactor(dsf);
        blink::WebFrameWidget* widget = m_webWiew->MainFrameWidget();
        if (widget)
            widget->SetZoomLevel(widget->GetZoomLevel());
    }


    gfx::Size dip_size = m_visualProperties.new_size;
    if (dip_size.IsEmpty())
        dip_size = gfx::Size(1, 1);
    m_visualProperties.compositor_viewport_pixel_rect = gfx::Rect(gfx::ScaleToCeiledSize(dip_size, dsf));
}

void RenderWidgetHostImpl::allowResize()
{
    m_allowResize = true;
}

void RenderWidgetHostImpl::resizeOnBlinkThread(int w, int h)
{
    if (!m_sinkClient)
        return;

    disallowResize();
    gfx::Size dip_size(std::max(1, w), std::max(1, h));
    const float dsf = deviceScaleFactor();
    gfx::Size pixel_size = gfx::ScaleToCeiledSize(dip_size, dsf);
    if (pixel_size.IsEmpty())
        pixel_size = gfx::Size(1, 1);
    gfx::Rect viewportRect(pixel_size);
    m_visualProperties.new_size = dip_size;
    m_visualProperties.visible_viewport_size = dip_size;
    m_visualProperties.compositor_viewport_pixel_rect = viewportRect;

    viz::LocalSurfaceId newLocalSurfaceId;
    gfx::Rect bounds = m_visualProperties.compositor_viewport_pixel_rect;

    m_allocator.GenerateId();
    newLocalSurfaceId = m_allocator.GetCurrentLocalSurfaceId();
    newLocalSurfaceId = m_sinkClient->embedOnBlinkThread(m_frameSinkId, bounds, newLocalSurfaceId);
    // ����ÿһ֡embedSurfaceDrawquad��surfaceid�����⻹�и�root��surfaceid��ΪSubmitCompositorFrame�Ĳ���
    m_visualProperties.local_surface_id = newLocalSurfaceId;

    time_t now;
    struct tm* current;
    now = time(NULL);
    current = localtime(&now);

    char* output = (char*)malloc(400);
    sprintf(output, "RenderWidgetHostImpl::resizeOnBlinkThread: hash:%d dip:%d pixel:%d scale:%.2f, (%02d:%02d)\n",
        newLocalSurfaceId.hash(), w, pixel_size.width(), dsf, current->tm_min, current->tm_sec);
    OutputDebugStringA(output);
    free(output);

    if (m_blinkWidget.get())
        m_blinkWidget->UpdateVisualProperties(m_visualProperties);

    if (m_webWiew) {
        gfx::Rect widgetScreenRect(dip_size);
        m_webWiew->MainFrameWidget()->SetScreenRects(widgetScreenRect, widgetScreenRect);
    }
    m_sinkHost->resizeOnBlinkThread(pixel_size, dsf);
}

void RenderWidgetHostImpl::bindPopupWidget(::mojo::PendingAssociatedReceiver<::blink::mojom::blink::PopupWidgetHost> popupHost,
    ::mojo::PendingAssociatedReceiver<::blink::mojom::blink::WidgetHost> blinkWidgetHost,
    ::mojo::PendingAssociatedRemote<::blink::mojom::blink::Widget> blinkWidget)
{
    m_agentGroupScheduler = blink::scheduler::WebThreadScheduler::MainThreadScheduler().CreateWebAgentGroupScheduler();
    m_popupWidgetHost.Bind(std::move(popupHost));
    m_blinkWidgetHostReceiver.Bind(std::move(blinkWidgetHost));
    m_blinkWidget.Bind(std::move(blinkWidget));

    //m_popupWidgetHost.set_disconnect_handler(base::BindOnce(&RenderWidgetHostImpl::onClientConnectionLost, base::Unretained(this)));
    m_blinkWidgetHostReceiver.set_disconnect_handler(base::BindOnce(&RenderWidgetHostImpl::onClientConnectionLost, base::Unretained(this)));
    //m_blinkWidget.set_disconnect_handler(base::BindOnce(&RenderWidgetHostImpl::onClientConnectionLost, base::Unretained(this)));

    initVisualProperties();
}

display::Screen* getScreenOrCreate()
{
    display::Screen* screen = display::Screen::GetScreen();
    if (!screen) {
#if defined(OS_WIN)
        display::win::ScreenWin* screenNew = new display::win::ScreenWin();
#else
        display::Screen* screenNew = display::CreateNativeScreen();
#endif // OS_WIN
        display::Screen::SetScreenInstance(screenNew, base::Location::Current());
        screen = display::Screen::GetScreen();
    }
    return screen;
}

void RenderWidgetHostImpl::initVisualProperties()
{
    display::Screen* screen = getScreenOrCreate();
    std::vector<display::Display> displays = screen->GetAllDisplays();
    if (displays.size() == 0)
        DebugBreak();
    for (size_t i = 0; i < displays.size(); ++i) {
        const display::Display& dis = displays[i];
        display::ScreenInfo screenInfo;
        display::DisplayUtil::DisplayToScreenInfo(&screenInfo, dis);
        m_visualProperties.screen_infos.screen_infos.push_back(screenInfo);
    }

    m_visualProperties.screen_infos.current_display_id = displays[0].id();
    gfx::Size dip_size(960, 480);
    gfx::Size pixel_size = gfx::ScaleToCeiledSize(dip_size, deviceScaleFactor());
    if (pixel_size.IsEmpty())
        pixel_size = gfx::Size(1, 1);
    m_visualProperties.display_mode = blink::mojom::DisplayMode::kBrowser;
    m_visualProperties.new_size = dip_size;
    m_visualProperties.visible_viewport_size = dip_size;
    m_visualProperties.compositor_viewport_pixel_rect = gfx::Rect(pixel_size);

    // ֻ�е���WasShown�Ż���
    // ProxyMain::SetVisible
    // -> SchedulerStateMachine::ShouldBeginLayerTreeFrameSinkCreation
    // -> CreateFrameSink
    wasShown();
}

std::unique_ptr<CreateFrameWidgetParams> RenderWidgetHostImpl::bindAndGenerateCreateFrameWidgetParams()
{
    m_agentGroupScheduler = blink::scheduler::WebThreadScheduler::MainThreadScheduler().CreateWebAgentGroupScheduler();

    std::unique_ptr<CreateFrameWidgetParams> params = std::make_unique<CreateFrameWidgetParams>();
    initVisualProperties();
    params->blinkWidget = m_blinkWidget.BindNewEndpointAndPassReceiver();
    params->frameWidget = m_frameWidget.BindNewEndpointAndPassReceiver();
    params->visualProperties = m_visualProperties;

    m_blinkFrameWidgetHostReceiver.Bind(params->frameWidgetHost.InitWithNewEndpointAndPassReceiver());
    m_blinkWidgetHostReceiver.Bind(params->widgetHost.InitWithNewEndpointAndPassReceiver());

    return std::move(params);
}

void RenderWidgetHostImpl::bindLocalSurfaceId()
{
    gfx::Rect bounds = m_visualProperties.compositor_viewport_pixel_rect;
    if (bounds.IsEmpty())
        bounds.set_size(gfx::Size(1, 1));

        // bounds.set_width(bounds.width() / 2  + 600);
        // bounds.set_height(bounds.height() / 2 + 800);

    //newLocalSurfaceId = m_sinkClient->embedOnBlinkThread(m_frameSinkId, bounds);
    m_allocator.GenerateId();
    m_localSurfaceId = m_allocator.GetCurrentLocalSurfaceId();
    //m_visualProperties.local_surface_id = m_sinkClient->GetLocalSurfaceId(); // root client��surface
    m_visualProperties.local_surface_id = m_localSurfaceId;

    m_blinkWidget->UpdateVisualProperties(m_visualProperties);

    m_sinkClient->embedOnBlinkThread(m_frameSinkId, bounds, m_localSurfaceId);
}

void RenderWidgetHostImpl::CreateFrameSink(
    ::mojo::PendingReceiver<::viz::mojom::blink::CompositorFrameSink> receiver, ::mojo::PendingRemote<::viz::mojom::blink::CompositorFrameSinkClient> client)
{
    while (/*!m_sinkClient ||*/ !m_sinkHost || !m_sinkHost->getRootClient()) {
        base::PlatformThread::Sleep(base::Milliseconds(10));
    }
    m_sinkClient = m_sinkHost->getRootClient();
    m_sinkClient->setWebviewId(m_engineWebView->getId());

    getHostFrameSinkManager()->RegisterFrameSinkId(m_frameSinkId, this, viz::ReportFirstSurfaceActivation::kNo);
    getHostFrameSinkManager()->RegisterFrameSinkHierarchy(m_sinkClient->frameSinkId(), m_frameSinkId);
    //viz::FrameSinkId rootGrameSinkId(0xdead, 0xbeef);
    //getHostFrameSinkManager()->RegisterFrameSinkHierarchy(rootGrameSinkId/*m_sinkClient->frame_sink_id()*/, m_frameSinkId);
    //m_frameSinkId = rootGrameSinkId;

    mojo::ScopedMessagePipeHandle h = receiver.PassPipe();
    //MojoMakeIsMessageChannelFlag(h->value());

    getHostFrameSinkManager()->CreateCompositorFrameSink(m_frameSinkId,
        mojo::PendingReceiver<viz::mojom::CompositorFrameSink>(std::move(h) /*receiver.PassPipe()*/),
        mojo::PendingRemote<viz::mojom::CompositorFrameSinkClient>(client.PassPipe(), 0));

    bindLocalSurfaceId();
    //     gfx::Rect bounds = m_visualProperties.compositor_viewport_pixel_rect;
    //     if (bounds.IsEmpty())
    //         bounds.set_size(gfx::Size(1, 1));
    //     m_sinkClient->embedOnBlinkThread(m_frameSinkId, bounds, m_localSurfaceId);

    mojo::PendingReceiver<blink::mojom::blink::RenderInputRouterClient> vizReceiver = mojo::NullReceiver();
    mojo::PendingRemote<blink::mojom::blink::RenderInputRouterClient> browserRemotePending;
    m_blinkWidget->SetupRenderInputRouterConnections(browserRemotePending.InitWithNewPipeAndPassReceiver(), std::move(vizReceiver));
    m_browserRemote.Bind(std::move(browserRemotePending));

    m_browserRemote->GetWidgetInputHandler(
        m_platformEventHandler->m_blinkWidgetInputHandler.BindNewPipeAndPassReceiver(base::SequencedTaskRunner::GetCurrentDefault()),
        m_platformEventHandler->m_hostReceiver.BindNewPipeAndPassRemote(base::SequencedTaskRunner::GetCurrentDefault()));

    if (m_webWiew) {
        m_webWiew->MainFrameWidget()->SetFocus(true);
        m_webWiew->SetIsActive(true);
    }
}

void RenderWidgetHostImpl::SetCursor(const ::ui::Cursor& cursor)
{
    m_engineWebView->setCursor(cursor);
}

void RenderWidgetHostImpl::TextInputStateChanged(::ui::mojom::blink::TextInputStatePtr state)
{
    if (!m_engineWebView->getWebView())
        return;
    blink::WebLocalFrame* focusedFrame = m_engineWebView->getWebView()->FocusedFrame();
    if (!focusedFrame)
        return;
    gfx::Rect rect;
    focusedFrame->FirstRectForCharacterRange(0, state->selection.end(), rect);

    gfx::Point p = rect.origin();
    p.Offset(rect.width(), 0);

    m_engineWebView->setCaretPos(p);
}

void RenderWidgetHostImpl::onClientConnectionLost()
{
    WebViewHost* webview = m_engineWebView;
    ThreadCall::callUiThreadAsync(FROM_HERE, [webview] {
        webview->preDestroyOnUiThread();

        ::PostMessageW(webview->getHostWnd(), WM_CLOSE, 0, 0);
        content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [webview] { webview->preDestroyOnBlinkThread(); });
    });
}

void RenderWidgetHostImpl::RequestClosePopup()
{
    DCHECK(kHasPopup == m_popupState);
    if (kRequestClosePopup == m_popupState)
        return;
    m_popupState = kRequestClosePopup;

    m_popupWidgetHost.Unbind();

    WebViewHost* webview = m_engineWebView;
    ThreadCall::callUiThreadAsync(FROM_HERE, [webview] { webview->setShow(SW_HIDE /*false, false*/); });
}

void RenderWidgetHostImpl::ShowPopup(const ::gfx::Rect& initialRect, const ::gfx::Rect& anchor_rect, ShowPopupCallback callback)
{
    DCHECK(kNoPopup == m_popupState);
    if (m_popupState != kNoPopup)
        return;
    m_popupState = kHasPopup;

    mini_electron_web_view parentWebviewId = m_parentWebviewId;
    int64_t id = m_engineWebView->getId();
    ThreadCall::callUiThreadAsync(FROM_HERE, [parentWebviewId, id, initialRect] {
        WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(id);
        WebViewHost* parentWebview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(parentWebviewId);
        if (!webview || !parentWebview)
            return;

        POINT point = { 0 };
        ::ClientToScreen(parentWebview->getHostWnd(), &point);
        webview->createWebWindowImplInUiThread(nullptr, WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, /*WS_EX_NOACTIVATE*/ 0x08000000L,
            initialRect.x() + point.x, initialRect.y() + point.y, initialRect.width(), initialRect.height());
        webview->setShow(SW_SHOWNOACTIVATE /*true, false*/);
    });

    RenderThreadImpl::get()->getTaskRunner()->PostNonNestableTask(
        FROM_HERE, base::BindOnce([](ShowPopupCallback&& callback) { std::move(callback).Run(); }, std::move(callback)));
}

void RenderWidgetHostImpl::SetPopupBounds(const ::gfx::Rect& bounds, SetPopupBoundsCallback callback)
{
    RenderThreadImpl::get()->getTaskRunner()->PostNonNestableTask(
        FROM_HERE, base::BindOnce([](SetPopupBoundsCallback&& callback) { std::move(callback).Run(); }, std::move(callback)));
}

void RenderWidgetHostImpl::RegisterRenderFrameMetadataObserver(::mojo::PendingReceiver<::cc::mojom::blink::RenderFrameMetadataObserverClient> receiver,
    ::mojo::PendingRemote<::cc::mojom::blink::RenderFrameMetadataObserver> render_frame_metadata_observer)
{
    m_renderFrameMetadataObserverClient = new RenderFrameMetadataObserverClientImpl(this);
    m_renderFrameMetadataObserverClient->m_receiver.Bind(std::move(receiver));

    m_renderFrameMetadataObserverClient->m_receiver.set_disconnect_handler(
        base::BindOnce([](RenderFrameMetadataObserverClientImpl* ptr) { delete ptr; }, base::Unretained(m_renderFrameMetadataObserverClient)));
}

void RenderWidgetHostImpl::UpdateTooltipUnderCursor(const ::WTF::String& tooltip_text, ::base::i18n::TextDirection text_direction_hint)
{
#if defined(OS_WIN)
    if (!m_toolTip)
        m_toolTip = new ToolTip(true, 0.02);

    std::string txt = tooltip_text.Utf8();
    std::u16string u16txt = base::UTF8ToUTF16(txt);
    m_toolTip->show((const WCHAR*)u16txt.c_str(), nullptr);
#endif
}

}