
#include "runtime/engine/browser/web_view_host.h"

#include <utility>
#include <vector>

#include "runtime/engine/renderer/web_view_client_impl.h"
#include "runtime/engine/renderer/render_thread_impl.h"
#include "runtime/engine/renderer/policy_container_host_impl.h"
#include "runtime/engine/renderer/web_local_frame_client_impl.h"
#include "runtime/engine/renderer/blink_interface_registry_impl.h"
#include "runtime/engine/renderer/platform_event_handler.h"
#include "runtime/engine/browser/render_widget_host_impl.h"
#include "runtime/engine/browser/web_frame_main.h"
#include "runtime/engine/common/bind_js_query.h"
#include "runtime/engine/common/utf16.h"
#include "runtime/engine/common/live_id_detect.h"
#include "runtime/engine/common/thread_call.h"
#include "runtime/engine/viz/viz_service.h"
#include "runtime/engine/viz/viz_host.h"
#include "runtime/network/loader/page_net_extra_data.h"
#include "runtime/network/loader/web_url_loader_manager.h"
#include "runtime/network/cookies/web_cookie_jar_curl_impl.h"
#include "runtime/engine/bindings/js_value.h"
#include "runtime/engine/api/internal_api.h"
#include "third_party/blink/renderer/core/loader/document_loader.h"
#include "third_party/blink/renderer/platform/scheduler/public/compositor_thread_scheduler.h"
#include "third_party/blink/public/web/web_settings.h"
#include "third_party/blink/public/web/web_navigation_control.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_navigation_params.h"
#include "third_party/blink/public/web/web_draggable_region.h"
#include "third_party/blink/public/platform/web_policy_container.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/platform/cross_variant_mojo_util.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "third_party/blink/public/common/page/browsing_context_group_info.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "third_party/blink/renderer/core/exported/web_view_impl.h"
#include "third_party/blink/renderer/core/frame/page_scale_constraints_set.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/settings.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/testing/internal_runtime_flags.h"
#include "third_party/blink/renderer/platform/wtf/casting.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "gen/ui/base/cursor/mojom/cursor_type.mojom-shared.h"
#include "gen/third_party/blink/public/mojom/partitioned_popins/partitioned_popin_params.mojom.h"
#include "gen/third_party/blink/public/mojom/page/prerender_page_param.mojom.h"
#include "gen/third_party/blink/public/mojom/frame/policy_container.mojom-blink.h"
#include "gen/services/viz/privileged/mojom/compositing/frame_sink_manager.mojom.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#if defined(USE_OZONE)
#include <cairo.h>
#include "platform/posix/win32/gl_compat.h"
#endif
#include "runtime/engine/common/string_util.h"
// test
#include "runtime/engine/common/util.h"
#include <windowsx.h>
#include <shellapi.h>

namespace mini_electron {
extern std::vector<char>* s_htmlData;
}
// test==========

namespace blink {
LocalFrame* FromFrameTokenHash(const size_t& frame_token_hash);
}

namespace content {

const WCHAR* kClassWndName = MINI_ELECTRON_U16("MiniElectronWebWindow");
extern unsigned int g_engineSettingsMask;
bool g_enableNativeSetCapture = true;
bool g_enableNativeSetFocus = true;

void freeTempCharStrings();
void clearWebViewInContextMenuIfNeeded(WebViewHost* webview);

WebViewHost::WebViewHost(bool isPopup)
{
    m_id = common::LiveIdDetect::getWebViewIds()->constructed(this);
    m_clientSize.cx = 0;
    m_clientSize.cy = 0;

    m_offset.x = 0;
    m_offset.y = 0;

    m_isPopup = isPopup;

#if !defined(OS_WIN)
    //::InitializeCriticalSection(&m_memoryCanvasLock);
    //m_memoryCanvasLock = new base::Lock();
#endif
    ::InitializeCriticalSection(&m_mouseMsgQueueLock);
    ::InitializeCriticalSection(&m_dirtyRectLock);
    ::InitializeCriticalSection(&m_clientSizeLock);
    ::InitializeCriticalSection(&m_userKeyValuesLock);

    char output[100] = { 0 };
    snprintf(output, sizeof(output), "WebViewHost: %p\n", this);
    OutputDebugStringA(output);
}

// ��������blink�߳�
WebViewHost::~WebViewHost()
{
    char output[100] = { 0 };
    snprintf(output, sizeof(output), "~WebViewHost: %p %p\n", m_hWnd, this);
    OutputDebugStringA(output);

    m_renderWidgetHostImpl.reset();
}


// �˺�����������ui��blink�߳�
static void clearUiHwnd(HWND hWnd, UINT_PTR self)
{
    ::KillTimer(hWnd, self);
    ::RemovePropW(hWnd, kClassWndName);
}

void WebViewHost::preDestroyOnBlinkThread()
{
    CHECK(ThreadCall::isBlinkThread());
#if !defined(OS_WIN)
    //delete m_memoryCanvasLock;
#endif
    m_navigationController = nullptr;

    m_state = kPageDestroyed;

    if (m_renderWidgetHostImpl)
        m_renderWidgetHostImpl->destroy(); // ������AsyncLayerTreeFrameSinkͬ������
    //m_renderWidgetHostImpl.release(); // TODO:��ʱ������

    WebViewHost* self = this;
    int* aysnCount = (int*)malloc(sizeof(int));
    *aysnCount = 2;
    auto destroyCb = [self, aysnCount] {
        *aysnCount -= 1;
        if (0 != *aysnCount)
            return;
        ThreadCall::delayDestroySelf(self, RenderThreadImpl::get()->getTaskRunner(), 2000);
        free(aysnCount);
    };

    m_host->destroy(destroyCb);
    m_host.release(); // TODO:

    m_platformEventHandler->destroy();
    m_platformEventHandler.release();

    m_service->destroy(destroyCb);
    m_service.release(); // TODO:

    clearUiHwnd(m_hWnd, (UINT_PTR)this);

    int count = 0;
    while (m_hWnd && ::IsWindow(m_hWnd)) {
        ++count;
        ::Sleep(100);
#ifndef _DEBUG
        if (count > 6)
            break;
#endif // DEBUG
    }
    CHECK(ThreadCall::isBlinkThread());

    char output[100] = { 0 };
    sprintf(output, "WebViewHost::preDestroyOnBlinkThread: %p %p\n", m_hWnd, this);
    OutputDebugStringA(output);
    m_hWnd = nullptr;

#if defined(OS_WIN)
    if (m_memoryBMP)
        ::DeleteObject(m_memoryBMP);

        // if (m_memoryDC)
        //     ::DeleteDC(m_memoryDC);

        //if (m_draggableRegion)
        //  ::DeleteObject(m_draggableRegion);
#else
    if (m_bitmap)
        delete m_bitmap;
    if (m_memoryCanvas)
        delete m_memoryCanvas;
        //if (m_surface)
        //   cairo_surface_destroy((cairo_surface_t*)m_surface); // ~OffscreenWindowUpdater����ͷ����m_surface
#endif
    //::DeleteCriticalSection(&m_memoryCanvasLock);
    ::DeleteCriticalSection(&m_mouseMsgQueueLock);
    ::DeleteCriticalSection(&m_dirtyRectLock);
    ::DeleteCriticalSection(&m_clientSizeLock);
    ::DeleteCriticalSection(&m_userKeyValuesLock);
}

bool WebViewHost::preDestroyOnUiThread()
{
    if (m_state >= kPageDestroying)
        return false;

    char output[100] = { 0 };
    sprintf(output, "WebViewHost::preDestroyOnUiThread: %p %p\n", m_hWnd, this);
    OutputDebugStringA(output);

    clearWebViewInContextMenuIfNeeded(this);

    //m_renderWidgetHostImpl->destroy();
    //m_host->destroy();
    //m_service.reset();

    common::LiveIdDetect::getWebViewIds()->deconstructed(m_id);
    m_state = kPageDestroying;
    ::RevokeDragDrop(m_hWnd);
    //::SetPropW(m_hWnd, kClassWndName, NULL);
    //::SetWindowLongPtrW(m_hWnd, GWLP_USERDATA, 0);

#ifdef OS_LINUX
    // ��linux�£����ھ���п�����LinuxGdiBindWindowByGtk�����ģ���ʱ���ղ������ڹرյ���Ϣ����Ϊ�ⲿ�ֶ��ر��ˣ�������Ҫ�ֶ���һ��
    ::DestroyWindow(m_hWnd);
#endif
    return true;
}

// ��������webkit�߳�
void WebViewHost::initializeCompositorInBlinkThread(/*gfx::AcceleratedWidget hwnd*/ bool isTransparent)
{
    //m_hWnd = hwnd;

    //DCHECK(m_hWnd != gfx::kNullAcceleratedWidget);

    // We finally have a valid gfx::AcceleratedWidget. We can now start the
    // actual process of setting up the viz host and the service.
    // First, set up the mojo message-pipes that the host and the service will
    // use to communicate with each other.
    mojo::PendingRemote<viz::mojom::FrameSinkManager> frameSinkManager;
    mojo::PendingReceiver<viz::mojom::FrameSinkManager> frameSinkManagerReceiver = frameSinkManager.InitWithNewPipeAndPassReceiver();
    mojo::PendingRemote<viz::mojom::FrameSinkManagerClient> frameSinkManagerClient;
    mojo::PendingReceiver<viz::mojom::FrameSinkManagerClient> frameSinkManagerClientReceiver = frameSinkManagerClient.InitWithNewPipeAndPassReceiver();

    // Next, create the host and the service, and pass them the right ends of
    // the message-pipes.
    m_service = std::make_unique<content::VizService>(std::move(frameSinkManagerReceiver), std::move(frameSinkManagerClient));

    m_host = std::make_unique<content::VizHost>(this,
        //m_hWnd,
        isTransparent, gfx::Size(1, 1), // �����дһ���ٵĴ�С��������һ��С��ɫ������
        std::move(frameSinkManagerClientReceiver), std::move(frameSinkManager), content::RenderThreadImpl::get()->m_hostThread.task_runner(),
        m_service->GetCompositorThreadRunner());

    if (m_renderWidgetHostImpl)
        m_renderWidgetHostImpl->setSinkHost(m_host.get());
}

void WebViewHost::setHostWnd(HWND hWnd)
{
    m_hWnd = hWnd;

    if (!m_isTransparent)
        m_isTransparent = !!(::GetWindowLongW(hWnd, GWL_EXSTYLE) & WS_EX_LAYERED);

    mini_electron_web_view webviewHandle = (mini_electron_web_view)m_id;
    content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [](WebViewHost* self) { self->setIsTransparent(self->m_isTransparent); });
}

void WebViewHost::createWebWindowImplInUiThread(HWND parent, DWORD style, DWORD styleEx, int x, int y, int width, int height)
{
    const WCHAR* szClassName = MINI_ELECTRON_U16("_WebWindow_");
    WNDCLASSEXW wndClass = { 0 };
    static bool isFirstRegister = true;
    if (isFirstRegister) {
        isFirstRegister = false;
        wndClass.cbSize = sizeof(WNDCLASSEXW);
        wndClass.style = CS_HREDRAW | CS_VREDRAW;
        wndClass.lpfnWndProc = &WebViewHost::windowProc;
        wndClass.cbClsExtra = 200;
        wndClass.cbWndExtra = 200;
        wndClass.hInstance = GetModuleHandleW(NULL);
        wndClass.hIcon = LoadIconW(NULL, IDI_APPLICATION);
        wndClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wndClass.hbrBackground = NULL;
        wndClass.lpszMenuName = NULL;
        wndClass.lpszClassName = szClassName;
        RegisterClassExW(&wndClass);
    }

    m_hWnd = CreateWindowExW(styleEx, // window ex-style
        szClassName, // window class name
        kClassWndName, // window caption
        style, // window style
        x, // initial x position
        y, // initial y position
        width, // initial x size
        height, // initial y size
        parent, // parent window handle
        NULL, // window menu handle
        GetModuleHandleW(NULL), // program instance handle
        this); // creation parameters

    if (!IsWindow(m_hWnd))
        return;

    if (m_isShow)
        mini_electron_show_window_impl(getWebviewHandle(), true);

    m_isWebWindowMode = true;
}

void WebViewHost::bindGtkWindow(void* rootWindow, void* drawingArea, bool isGl, DWORD style, DWORD styleEx, int width, int height)
{
#if !defined(WIN32)
    const WCHAR* szClassName = MINI_ELECTRON_U16("MiniElectronEmbeddedWindow");
    WNDCLASSEXW wndClass = { 0 };
    static bool isFirstRegister = true;
    if (isFirstRegister) {
        isFirstRegister = false;
        wndClass.cbSize = sizeof(WNDCLASSEXW);
        wndClass.style = CS_HREDRAW | CS_VREDRAW;
        wndClass.lpfnWndProc = &WebViewHost::windowProc;
        wndClass.cbClsExtra = 200;
        wndClass.cbWndExtra = 200;
        wndClass.hInstance = GetModuleHandleW(NULL);
        wndClass.hIcon = LoadIconW(NULL, IDI_APPLICATION);
        wndClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wndClass.hbrBackground = NULL;
        wndClass.lpszMenuName = NULL;
        wndClass.lpszClassName = szClassName;
        RegisterClassExW(&wndClass);
    }

    m_hWnd = LinuxGdiBindWindowByGtk(rootWindow, drawingArea, isGl,
        styleEx, // window ex-style
        szClassName, // window class name
        style, // window style
        width, // initial x size
        height, // initial y size
        this); // creation parameters

    if (!IsWindow(m_hWnd))
        return;

    if (m_isShow)
        mini_electron_show_window_impl(getWebviewHandle(), true);

    m_isWebWindowMode = true;
#endif
}

blink::WebView* WebViewHost::createWebWindowOrViewInBlinkThread(blink::WebView* opener, blink::WebFrame* openerWebFrame, bool isWebWindowMode, bool isTransparent)
{
    if (m_service.get())
        return m_renderWidgetHostImpl->m_webWiew;

    initializeCompositorInBlinkThread(isTransparent);
    return initializeViewInBlinkThread(opener, openerWebFrame);
}

// ����WebViewHost��Ϊ��������ⲿ����ʱ�ߵ�����
void WebViewHost::initPopupWidgetModeInBlinkThread(mini_electron_web_view parentWebviewId, ::mojo::PendingAssociatedReceiver<::blink::mojom::blink::PopupWidgetHost> popupHost,
    ::mojo::PendingAssociatedReceiver<::blink::mojom::blink::WidgetHost> blinkWidgetHost,
    ::mojo::PendingAssociatedRemote<::blink::mojom::blink::Widget> blinkWidget)
{
    m_isPopupWidgetMode = true;
    initializeCompositorInBlinkThread(/*m_hWnd*/ false);
    m_sessionStorageNamespaceId = blink::AllocateSessionStorageNamespaceId();

    m_renderWidgetHostImpl = std::make_unique<RenderWidgetHostImpl>(parentWebviewId, base::SequencedTaskRunner::GetCurrentDefault());
    m_renderWidgetHostImpl->setHostFrameSinkManager(m_host->host_frame_sink_manager());
    m_renderWidgetHostImpl->setSinkHost(m_host.get());

    m_platformEventHandler = std::make_unique<content::PlatformEventHandler>(m_id, nullptr, (blink::WebViewImpl*)nullptr);
    m_renderWidgetHostImpl->m_platformEventHandler = m_platformEventHandler.get();

    m_host->getRootClient()->setWidgetHost(m_renderWidgetHostImpl.get());

    m_renderWidgetHostImpl->m_mainFrame = nullptr;
    m_renderWidgetHostImpl->m_webWiew = nullptr;
    m_renderWidgetHostImpl->m_engineWebView = this;
    m_renderWidgetHostImpl->m_webFrameWidget = nullptr;
    m_renderWidgetHostImpl->bindPopupWidget(std::move(popupHost), std::move(blinkWidgetHost), std::move(blinkWidget));
}

blink::WebView* WebViewHost::initializeViewInBlinkThread(blink::WebView* opener, blink::WebFrame* openerWebFrame)
{
    if (!m_navigationController)
        m_navigationController = blink::MakeGarbageCollected<content::PageNavController>(this);

    m_sessionStorageNamespaceId = blink::AllocateSessionStorageNamespaceId();

    blink::BrowsingContextGroupInfo browsingContextGroupInfo = blink::BrowsingContextGroupInfo::CreateUnique();
    blink::WebView* webWiew = blink::WebView::Create(
        new content::WebViewClientImpl(this), 
        false, // is_hidden
        nullptr, // blink::mojom::PrerenderParamPtr prerender_param 
        absl::nullopt, // std::optional<blink::FencedFrame::DeprecatedFencedFrameMode> fenced_frame_mode, 
        /*compositing_enabled=*/true, 
        false, // widgets_never_composited
        opener, 
        blink::CrossVariantMojoAssociatedReceiver<blink::mojom::PageBroadcastInterfaceBase>(),
        *(content::RenderThreadImpl::get()->m_agentGroupScheduler), 
        m_sessionStorageNamespaceId, 
        absl::optional<SkColor>(0xffffffff),
        browsingContextGroupInfo,
        nullptr, // const ColorProviderColorMaps * color_provider_colors,
        nullptr // blink::mojom::PartitionedPopinParamsPtr partitioned_popin_params
        );

    mojo::PendingAssociatedRemote<blink::mojom::blink::PolicyContainerHost> policyContainerRemote;
    mojo::PendingAssociatedReceiver<blink::mojom::blink::PolicyContainerHost> policyContainerReceiver
        = policyContainerRemote.InitWithNewEndpointAndPassReceiver();

    mojo::AssociatedReceiver<blink::mojom::blink::PolicyContainerHost>* blinkPolicyContainerHostReceiver
        = new mojo::AssociatedReceiver<blink::mojom::blink::PolicyContainerHost>(new content::PolicyContainerHostImpl());
    blinkPolicyContainerHostReceiver->Bind(std::move(policyContainerReceiver)); // TODO: �ڴ�й¶

    std::unique_ptr<blink::WebPolicyContainer> policyContainer = std::make_unique<blink::WebPolicyContainer>(
        blink::WebPolicyContainerPolicies(), blink::ToCrossVariantAssociatedMojoType(std::move(policyContainerRemote)));

    m_frameClient = new WebLocalFrameClientImpl(m_id);
    BlinkInterfaceRegistryImpl* blinkInterfaceRegistryImpl = new BlinkInterfaceRegistryImpl(m_frameClient);
    m_frameClient->m_blinkInterfaceRegistryImpl = blinkInterfaceRegistryImpl;

    mojo::PendingRemote<blink::mojom::BrowserInterfaceBroker> browserInterfaceBroker = m_frameClient->getBrowserInterfaceBrokerProxyReceiver().BindNewPipeAndPassRemote();

    blink::WebLocalFrame* webframe
        = blink::WebLocalFrame::CreateMainFrame(
            webWiew, 
            m_frameClient, 
            blinkInterfaceRegistryImpl, 
            // CrossVariantMojoRemote<mojom::BrowserInterfaceBrokerInterfaceBase>
            blink::CrossVariantMojoRemote<blink::mojom::BrowserInterfaceBrokerInterfaceBase>(std::move(browserInterfaceBroker)),
            blink::LocalFrameToken(), 
            blink::DocumentToken(),
            std::move(policyContainer), 
            openerWebFrame, 
            blink::WebString::FromUTF8("MiniElectronPopupFrame"), 
            network::mojom::WebSandboxFlags::kNone, blink::WebURL());
    //--
    if (openerWebFrame) {
        blink::WebLocalFrameImpl* webframeimpl = (blink::WebLocalFrameImpl*)webframe;
        blink::LocalFrame* localFrame = webframeimpl->GetFrame();
        blink::LocalDOMWindow* domwin = localFrame->DomWindow();
        blink::SecurityContext& securityContext = domwin->GetSecurityContext();
        securityContext.SetSecurityOriginForTesting(nullptr);
        securityContext.SetSecurityOrigin(blink::SecurityOrigin::CreateFromString(openerWebFrame->GetSecurityOrigin().ToString()));
    }
    //--
    m_frameClient->setFrame(webframe);
    new WebFrameMain(blink::LocalFrameToken::Hasher()(webframe->GetLocalFrameToken()), 0, true);

    m_frameClient->m_blinkPolicyContainerHostReceiver = blinkPolicyContainerHostReceiver;
    m_frameClient->m_isMainFrame = true;

    m_renderWidgetHostImpl = std::make_unique<RenderWidgetHostImpl>(NULL_WEBVIEW, base::SequencedTaskRunner::GetCurrentDefault());
    m_renderWidgetHostImpl->setHostFrameSinkManager(m_host->host_frame_sink_manager());
    m_renderWidgetHostImpl->setSinkHost(m_host.get());

    m_platformEventHandler = std::make_unique<content::PlatformEventHandler>(m_id, nullptr, (blink::WebViewImpl*)webWiew);
    m_renderWidgetHostImpl->m_platformEventHandler = m_platformEventHandler.get();

    m_host->getRootClient()->setWidgetHost(m_renderWidgetHostImpl.get());

    std::unique_ptr<CreateFrameWidgetParams> createFrameWidgetParams = m_renderWidgetHostImpl->bindAndGenerateCreateFrameWidgetParams();
    if (m_pendingDeviceScaleFactor > 0.f) {
        m_renderWidgetHostImpl->setDeviceScaleFactor(m_pendingDeviceScaleFactor);
        createFrameWidgetParams->visualProperties = m_renderWidgetHostImpl->m_visualProperties;
    }
    m_renderWidgetHostImpl->m_mainFrame = webframe;
    m_renderWidgetHostImpl->m_webWiew = webWiew;
    m_renderWidgetHostImpl->m_engineWebView = this;

    blink::DocumentLoader::DisableCodeCacheForTesting();

    //constexpr viz::FrameSinkId root_frame_sink_id(0xdead, 0xbeef);
    //m_renderWidgetHostImpl->setFrameSinkId(root_frame_sink_id);
    //m_renderWidgetHostImpl->bindLocalSurfaceId();

    blink::WebFrameWidget* webFrameWidget
        = webframe->InitializeFrameWidget(blink::ToCrossVariantAssociatedMojoType(std::move(createFrameWidgetParams->frameWidgetHost)),
            blink::ToCrossVariantAssociatedMojoType(std::move(createFrameWidgetParams->frameWidget)),
            blink::ToCrossVariantAssociatedMojoType(std::move(createFrameWidgetParams->widgetHost)),
            blink::ToCrossVariantAssociatedMojoType(std::move(createFrameWidgetParams->blinkWidget)),
            viz::FrameSinkId(/*RenderThread::Get()->GetClientId()*/ 1, /*params->widget_params->routing_id*/ 2),
            /*is_for_nested_main_frame*/ false, /*is_for_scalable_page*/ true,
            /*hidden=*/true);
    webFrameWidget->InitializeCompositing(
        /**m_renderWidgetHostImpl->m_agentGroupScheduler, */createFrameWidgetParams->visualProperties.screen_infos, /*settings=*/nullptr);

    setDefaultPreferences((blink::WebViewImpl*)webWiew);
    m_renderWidgetHostImpl->m_webFrameWidget = webFrameWidget;

    webWiew->DidAttachLocalMainFrame();

    if (m_isTransparent)
        setIsTransparent(m_isTransparent);
    return webWiew;
}

void WebViewHost::handlePopup(UINT message)
{
    if (m_isPopup || !m_renderWidgetHostImpl || !m_renderWidgetHostImpl->m_webWiew)
        return;
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
        //     case WM_LBUTTONDBLCLK:
        //     case WM_MBUTTONDBLCLK:
        //     case WM_RBUTTONDBLCLK:
        //     case WM_LBUTTONUP:
        //     case WM_MBUTTONUP:
        //     case WM_RBUTTONUP:

    case WM_NCLBUTTONDOWN:
        //     case WM_NCLBUTTONUP:
        //     case WM_NCLBUTTONDBLCLK:
    case WM_NCRBUTTONDOWN:
        //     case WM_NCRBUTTONUP:
        //     case WM_NCRBUTTONDBLCLK:
        //     case WM_NCMBUTTONDOWN:
        //     case WM_NCMBUTTONUP:
        //     case WM_NCMBUTTONDBLCLK:
        break;
    default:
        return;
    }

    mini_electron_web_view webviewHandle = (mini_electron_web_view)m_id;
    ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [](content::WebViewHost* self) {
        if (self->m_renderWidgetHostImpl && self->m_renderWidgetHostImpl->m_webWiew) {
            blink::WebViewImpl* webview = (blink::WebViewImpl*)self->m_renderWidgetHostImpl->m_webWiew;
            webview->CancelPagePopup();
        }
    });
}

void WebViewHost::setBackgroundColor(COLORREF c)
{
    m_backgroundColor = c;
    m_hasBackgroundColor = true;

    mini_electron_web_view webviewHandle = (mini_electron_web_view)m_id;
    ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [c](content::WebViewHost* self) {
        if (self->m_renderWidgetHostImpl && self->m_renderWidgetHostImpl->m_webWiew) {
            blink::WebViewImpl* webview = (blink::WebViewImpl*)self->m_renderWidgetHostImpl->m_webWiew;
            absl::optional<SkColor> color(c);
            webview->SetPageBaseBackgroundColor(color);
        }
    });
}

void WebViewHost::setDefaultPreferences(blink::WebViewImpl* webWiew)
{
#if defined(OS_WIN)
    LANGID langId = GetUserDefaultUILanguage();
    if (0x0804 == langId) {
        setSetLanguage("zh-cn");
    }
#endif

    blink::web_pref::WebPreferences webPreferences = webWiew->GetWebPreferences();
    webPreferences.touch_event_feature_detection_enabled = false; // ����治�ṩontouchstart��document
    webPreferences.allow_universal_access_from_file_urls = true;
    webPreferences.allow_file_access_from_file_urls = true;
    webWiew->SetWebPreferences(webPreferences);

    webWiew->SetPageAttributionSupport(network::mojom::AttributionSupport::kWeb);
    webWiew->SetSupportsDraggableRegions(true);

    blink::WebSettings* websettings = webWiew->GetSettings();
    websettings->SetDefaultFontSize(16);
    websettings->SetDefaultFixedFontSize(16);
    websettings->SetJavaScriptEnabled(true);
    websettings->SetLoadsImagesAutomatically(true);
    websettings->SetLocalStorageEnabled(true);
    websettings->SetAllowScriptsToCloseWindows(true);
    websettings->SetJavaScriptCanAccessClipboard(true);
    websettings->SetDOMPasteAllowed(true);

//     blink::Page* page = webWiew->GetPage();
//     page->GetSettings().SetAcceleratedCompositingEnabled(false);
    blink::InternalRuntimeFlags* internalRuntimeFlags = blink::InternalRuntimeFlags::create();
    internalRuntimeFlags->setAccelerated2dCanvasEnabled(false);

    blink::RendererPreferences preferences;
    preferences.accept_languages = "cn-ZH,cn";
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
    //preferences.system_font_family_name;
#endif
#if BUILDFLAG(IS_WIN)
    //const char16_t* fontStr = u"����";
    const char16_t* fontStr = u"Microsoft YaHei";

    preferences.caption_font_family_name = fontStr;
    preferences.caption_font_height = 16;
    preferences.small_caption_font_family_name = fontStr;
    preferences.small_caption_font_height = 12;
    preferences.menu_font_family_name = fontStr;
    preferences.menu_font_height = 12;
    preferences.status_font_family_name = fontStr;
    preferences.status_font_height = 12;
    preferences.message_font_family_name = fontStr;
    preferences.message_font_height = 12;
    preferences.vertical_scroll_bar_width_in_dips = 23;
    preferences.horizontal_scroll_bar_height_in_dips = 23;
    preferences.arrow_bitmap_height_vertical_scroll_bar_in_dips = 17;
    preferences.arrow_bitmap_width_horizontal_scroll_bar_in_dips = 17;
    preferences.accept_languages = "zh-CN";
#endif
    //(preferences.local_storage_enabled);
    webWiew->UpdateRendererPreferences(preferences);

    propagatedZoomFactor();
}

void WebViewHost::setZoomFactor(float factor)
{
    if (factor < 0.2 || factor > 3 || !m_renderWidgetHostImpl || !m_renderWidgetHostImpl->m_webWiew)
        return;
    m_hasSetZoomFactor = true;
    m_zoomFactor = factor;
    propagatedZoomFactor();
}

float WebViewHost::getZoomFactor() const
{
    return m_zoomFactor;
}

void WebViewHost::propagatedZoomFactor()
{
    float zoom = RenderThreadImpl::get()->getZoom();
    if (hasSetZoomFactor())
        zoom = getZoomFactor();
    //zoom = 1;

    if (!m_renderWidgetHostImpl || !m_renderWidgetHostImpl->m_webWiew)
        return;
    blink::WebViewImpl* webWiew = (blink::WebViewImpl*)m_renderWidgetHostImpl->m_webWiew;
    // Page zoom must not replace the display's DIP-to-pixel scale.
    webWiew->MainFrameWidget()->SetZoomLevel(blink::ZoomFactorToZoomLevel(zoom));
}

void WebViewHost::createWebWindowInUiThread(mini_electron_window_type type, HWND parent, int x, int y, int width, int height)
{
    if (IsWindow(m_hWnd))
        return;

    DWORD style = 0;
    DWORD styleEx = 0;
    switch (type) {
    case MINI_ELECTRON_WINDOW_TYPE_CONTROL:
        style = WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
        styleEx = 0;
        break;

    case MINI_ELECTRON_WINDOW_TYPE_TRANSPARENT:
        m_isTransparent = true;
        style = WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
        styleEx = WS_EX_LAYERED;
        break;

    case MINI_ELECTRON_WINDOW_TYPE_POPUP:
    default:
        style = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
        styleEx = 0;
    }

    createWebWindowImplInUiThread(parent, style, styleEx, x, y, width, height);
}

LRESULT WebViewHost::windowProcImpl(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_NCPAINT:
        break;

    case WM_ERASEBKGND:
        break;
    case WM_GETDLGCODE: // ʹ��MB�ؼ���Ϊ�Ի����Ӵ���ʱ�ɽ��յ�������Ϣ
        return DLGC_WANTARROWS | DLGC_WANTALLKEYS | DLGC_WANTCHARS;

    case WM_CREATE:
        ::DragAcceptFiles(hWnd, TRUE);
        ::SetTimer(hWnd, (UINT_PTR)this, 20, NULL);
        break;

    case WM_CLOSE:
        if (mini_electron_close_callback closingCallback = getClosure().m_ClosingCallback) {
            getClosure().m_ClosingCallback = nullptr;
            if (!closingCallback(getWebviewHandle(), getClosure().m_ClosingParam, nullptr))
                return 0;
        }
        ::ShowWindow(hWnd, SW_HIDE);
        ::DestroyWindow(hWnd);
        return 0;

    case WM_NCDESTROY:
        clearUiHwnd(hWnd, (UINT_PTR)this);
        m_state = kPageDestroying;
        mini_electron_destroy_web_view_impl(getWebviewHandle());
        break;

    case WM_TIMER:
        break;

    case WM_PAINT:
        onPaint(hWnd, wParam);
        break;

    case WM_SIZE: {
        RECT rc = { 0 };
        ::GetClientRect(hWnd, &rc);
        int width = rc.right - rc.left;
        int height = rc.bottom - rc.top;

        onResize(width, height, false);
        content::ThreadCall::wake();
        return 0;
    }
    case WM_DROPFILES:
        return 0;

    case WM_SYSKEYDOWN: // no break
    case WM_KEYDOWN: {
        unsigned int virtualKeyCode = (unsigned int)wParam;
        unsigned int flags = 0;
        if (HIWORD(lParam) & KF_REPEAT)
            flags |= MINI_ELECTRON_REPEAT;
        if (HIWORD(lParam) & KF_EXTENDED)
            flags |= MINI_ELECTRON_EXTENDED;

        if (mini_electron_fire_key_down_event_impl(getWebviewHandle(), virtualKeyCode, flags, false))
            return 0;
        break;
    }
    case WM_SYSKEYUP:
    case WM_KEYUP: {
        unsigned int virtualKeyCode = (unsigned int)wParam;
        unsigned int flags = 0;
        if (HIWORD(lParam) & KF_REPEAT)
            flags |= MINI_ELECTRON_REPEAT;
        if (HIWORD(lParam) & KF_EXTENDED)
            flags |= MINI_ELECTRON_EXTENDED;

        if (mini_electron_fire_key_up_event_impl(getWebviewHandle(), virtualKeyCode, flags, false))
            return 0;
        break;
    }
    case WM_NCMOUSEMOVE:
    case WM_NCMOUSEHOVER:
        m_isInNotClient = true;
        break;

    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONUP:
    case WM_NCLBUTTONDBLCLK:
    case WM_NCRBUTTONDOWN:
    case WM_NCRBUTTONUP:
    case WM_NCRBUTTONDBLCLK:
    case WM_NCMBUTTONDOWN:
    case WM_NCMBUTTONUP:
    case WM_NCMBUTTONDBLCLK:
        handlePopup(message);
        break;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MOUSEMOVE: {
        m_isInNotClient = false;
        handlePopup(message);

        int x = LOWORD(lParam);
        int y = HIWORD(lParam);

        unsigned int flags = 0;

        if (wParam & MK_CONTROL)
            flags |= MINI_ELECTRON_CONTROL;
        if (wParam & MK_SHIFT)
            flags |= MINI_ELECTRON_SHIFT;

        if (wParam & MK_LBUTTON)
            flags |= MINI_ELECTRON_LBUTTON;
        if (wParam & MK_MBUTTON)
            flags |= MINI_ELECTRON_MBUTTON;
        if (wParam & MK_RBUTTON)
            flags |= MINI_ELECTRON_RBUTTON;
        if (mini_electron_fire_mouse_event_impl(getWebviewHandle(), message, x, y, flags))
            return 0;
        break;
    }
    case WM_TOUCH: {
        break;
    }
    case WM_CONTEXTMENU: {
        break;
    }
    case WM_MOUSEWHEEL: {
        POINT pt;
        POINT pt2;
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);

        ::ScreenToClient(hWnd, &pt);

        int delta = GET_WHEEL_DELTA_WPARAM(wParam);

        unsigned int flags = 0;

        if (wParam & MK_CONTROL)
            flags |= MINI_ELECTRON_CONTROL;
        if (wParam & MK_SHIFT)
            flags |= MINI_ELECTRON_SHIFT;

        if (wParam & MK_LBUTTON)
            flags |= MINI_ELECTRON_LBUTTON;
        if (wParam & MK_MBUTTON)
            flags |= MINI_ELECTRON_MBUTTON;
        if (wParam & MK_RBUTTON)
            flags |= MINI_ELECTRON_RBUTTON;

        if (mini_electron_fire_mouse_wheel_event_impl(getWebviewHandle(), pt.x, pt.y, delta, flags))
            return 0;
        break;
    }
    case WM_CAPTURECHANGED:
        break;
    case WM_SETFOCUS:
        return 0;

    case WM_KILLFOCUS:
        return 0;

    case WM_SETCURSOR:
        if (mini_electron_fire_windows_message_impl(getWebviewHandle(), hWnd, WM_SETCURSOR, 0, 0, nullptr))
            return 0;
        break;

    case WM_NCHITTEST:
#ifndef _WIN32
        return onNcHittest(lParam); // ֻ��linux�汾�´���
#endif // _WIN32
        break;
    case WM_CHAR:
    {
        unsigned int charCode = (unsigned int)wParam;
        unsigned int flags = 0;
        if (HIWORD(lParam) & KF_REPEAT)
            flags |= MINI_ELECTRON_REPEAT;
        if (HIWORD(lParam) & KF_EXTENDED)
            flags |= MINI_ELECTRON_EXTENDED;

        // if (message == WM_IME_CHAR)
        //     OutputDebugStringA("WM_IME_CHAR\n");
        // else
        //     OutputDebugStringA("WM_CHAR\n");
        if (mini_electron_fire_key_press_event_impl(getWebviewHandle(), charCode, flags, WM_IME_CHAR == message))
            return 0;
        break;
    }
    case WM_IME_CHAR: {
        mini_electron_fire_windows_message_impl(getWebviewHandle(), hWnd, WM_IME_CHAR, wParam, lParam, nullptr);
        return 0; // ������뷵��0���������WM_IME_STARTCOMPOSITION����Ϣ�ڼ��յ�WM_CHAR�������ظ���ʾ����
    }
    case WM_IME_STARTCOMPOSITION:
        if (mini_electron_fire_windows_message_impl(getWebviewHandle(), hWnd, WM_IME_STARTCOMPOSITION, wParam, lParam, nullptr))
            return 0;
        break;
    case WM_IME_COMPOSITION: // ��������뷨���ַ�ʱ����Ϣ��������뷨���˿ո񣬾ͻ���WM_IME_CHAR��ʾȷ�����ַ�д����
        if (mini_electron_fire_windows_message_impl(getWebviewHandle(), hWnd, WM_IME_COMPOSITION, wParam, lParam, nullptr))
            return 0;
        break;
    case WM_IME_ENDCOMPOSITION:
        if (mini_electron_fire_windows_message_impl(getWebviewHandle(), hWnd, WM_IME_ENDCOMPOSITION, wParam, lParam, nullptr))
            return 0;
        break;
    }

    return ::DefWindowProcW(hWnd, message, wParam, lParam);
}

LRESULT CALLBACK WebViewHost::windowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    WebViewHost* self = (WebViewHost*)GetPropW(hWnd, kClassWndName);
    if (!self) {
        if (message == WM_CREATE) {
            LPCREATESTRUCTW cs = (LPCREATESTRUCTW)lParam;
            self = (WebViewHost*)cs->lpCreateParams;
            ((WebViewHost*)cs->lpCreateParams)->setHostWnd(hWnd);
            ::SetPropW(hWnd, kClassWndName, (HANDLE)self);
        }
    }

    if (self)
        return self->windowProcImpl(hWnd, message, wParam, lParam);
    else
        return ::DefWindowProcW(hWnd, message, wParam, lParam);
}

void WebViewHost::setClientSizeLocked(int w, int h)
{
    ::EnterCriticalSection(&m_clientSizeLock);
    m_clientSize.cx = w;
    m_clientSize.cy = h;
    m_clientSizeDirty = true;
    m_clientResizeRepaintDirty = true;
    ::LeaveCriticalSection(&m_clientSizeLock);

    time_t now;
    struct tm* current;
    now = time(NULL);
    current = localtime(&now);

    char output[100] = { 0 };
    sprintf(output, "WebViewHost::setClientSizeLocked: %d, %d, (%02d:%02d)\n", w, h, current->tm_min, current->tm_sec);
    OutputDebugStringA(output);
}

SIZE WebViewHost::getClientSizeLocked()
{
    SIZE size = { 0 };
    ::EnterCriticalSection(&m_clientSizeLock);
    size = m_clientSize;
    ::LeaveCriticalSection(&m_clientSizeLock);
    return size;
}

void WebViewHost::setCaretPos(const gfx::Point& pos)
{
    ::EnterCriticalSection(&m_clientSizeLock);
    m_caretPos = pos;
    ::LeaveCriticalSection(&m_clientSizeLock);
}

gfx::Point WebViewHost::getCaretPos() const
{
    ::EnterCriticalSection(&m_clientSizeLock);
    gfx::Point pos = m_caretPos;
    ::LeaveCriticalSection(&m_clientSizeLock);
    return pos;
}

void WebViewHost::setFocus()
{
    if (m_renderWidgetHostImpl && m_renderWidgetHostImpl->m_webFrameWidget)
        m_renderWidgetHostImpl->m_webFrameWidget->SetFocus(true);
}

void WebViewHost::killFocus()
{
    if (m_renderWidgetHostImpl && m_renderWidgetHostImpl->m_webFrameWidget)
        m_renderWidgetHostImpl->m_webFrameWidget->SetFocus(false);
}

void WebViewHost::debugShowDomNode()
{
#if !defined(NDEBUG)
    blink::WebLocalFrame* frame = (blink::WebLocalFrame*)(m_renderWidgetHostImpl->m_webWiew->MainFrame());
    if (!frame)
        return;
    blink::WebDocument doc = frame->GetDocument();
    blink::Document* document = (doc);
    std::string out = "debugShowDomNode: " + document->ToTreeStringForThis().Utf8();
    out += "\n";
    OutputDebugStringA(out.c_str());

#endif
}

blink::WebFrame* WebViewHost::getMainFrame() const
{
    if (!m_renderWidgetHostImpl || !m_renderWidgetHostImpl->m_webWiew)
        return nullptr;
    blink::WebLocalFrame* frame = (blink::WebLocalFrame*)(m_renderWidgetHostImpl->m_webWiew->MainFrame());
    return frame;
}

void WebViewHost::draggableRegionsChanged(blink::WebVector<blink::WebDraggableRegion> regions)
{
    m_draggableRegion = std::move(regions);
    mini_electron_draggable_regions_changed_callback callback =
        getClosure().m_DraggableRegionsChangedCallback;
    if (!callback)
        return;

    std::vector<mini_electron_draggable_region> callback_regions;
    callback_regions.reserve(m_draggableRegion.size());
    for (const blink::WebDraggableRegion& region : m_draggableRegion) {
        mini_electron_draggable_region output = {};
        output.bounds.left = region.bounds.x();
        output.bounds.top = region.bounds.y();
        output.bounds.right = region.bounds.right();
        output.bounds.bottom = region.bounds.bottom();
        output.draggable = region.draggable ? TRUE : FALSE;
        callback_regions.push_back(output);
    }
    callback(getWebviewHandle(), getClosure().m_DraggableRegionsChangedParam,
        callback_regions.empty() ? nullptr : callback_regions.data(),
        static_cast<int>(callback_regions.size()));
}

void WebViewHost::onPaintUpdatedInUiThread(const HDC hdc, int x, int y, int cx, int cy)
{
#if defined(OS_WIN)
    mini_electron_paint_updated_callback paintUpdatedCallback = getClosure().m_PaintUpdatedCallback;
    if (paintUpdatedCallback) {
        paintUpdatedCallback(getWebviewHandle(), getClosure().m_PaintUpdatedParam, (const HDC)m_surface, x, y, cx, cy);
    }
#endif

#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    mini_electron_paint_bit_updated_callback paintBitUpdatedCallback = getClosure().m_PaintBitUpdatedCallback;
    if (paintBitUpdatedCallback && m_bitmapByte) {
        mini_electron_rect r = { x, y, cx, cy };
        paintBitUpdatedCallback(getWebviewHandle(), getClosure().m_PaintBitUpdatedParam, m_bitmapByte, &r,
            m_bitmapByteSize.width(), m_bitmapByteSize.height());
    }
#endif

    ::EnterCriticalSection(&m_clientSizeLock);
    m_clientSizeDirty = false;
    m_isAsynResizing = false;
    bool sizeChange = !(m_clientSizeCache.cx == m_clientSize.cx && m_clientSizeCache.cy == m_clientSize.cy);
    ::LeaveCriticalSection(&m_clientSizeLock);

    if (sizeChange) // ����ϴ�resize��ʱ��ûPaint��Ϣ���ͻ�ȱһ��resize
        updataBlinkSize();
}

void WebViewHost::updataBlinkSize()
{
    mini_electron_web_view webviewHandle = (mini_electron_web_view)m_id;
    if (m_updataBlinkSizeAsyn || m_isAsynResizing)
        return;
    m_updataBlinkSizeAsyn = true;

    ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [](WebViewHost* self) {
        SIZE clientSize = self->getClientSizeLocked();
        self->m_updataBlinkSizeAsyn = false;

        if (self->m_host && self->m_renderWidgetHostImpl && self->m_renderWidgetHostImpl->isSinkReady()) {
            //RenderFrameMetadataObserverClientImpl::OnRenderFrameMetadataChangedû�ߵ�ʱ��isAllowResizeΪfalse
            if (self->m_renderWidgetHostImpl->isAllowResize()) {
                ::EnterCriticalSection(&self->m_clientSizeLock);
                self->m_clientSizeCache = clientSize;
                self->m_isAsynResizing = true; // һ֡������������resize
                ::LeaveCriticalSection(&self->m_clientSizeLock);

                self->m_renderWidgetHostImpl->resizeOnBlinkThread(clientSize.cx, clientSize.cy);
            }
        } else {
            self->updataBlinkSize();
        }
    });
}

LRESULT WebViewHost::onNcHittest(LPARAM lParam)
{
#ifndef _WIN32
    if (0 == m_ncHittestPadding)
        return HTCLIENT;

    POINT pt;
    pt.x = ((int)(short)LOWORD(lParam));
    pt.y = ((int)(short)HIWORD(lParam));
    ScreenToClient(m_hWnd, &pt);
    RECT rc;
    GetClientRect(m_hWnd, &rc);

    if (pt.y < m_ncHittestPadding) {
        if (pt.x < m_ncHittestPadding)
            return HTTOPLEFT;
        if (pt.x > rc.right - m_ncHittestPadding)
            return HTTOPRIGHT;
        return HTTOP;
    }
    if (pt.y > rc.bottom - m_ncHittestPadding) {
        if (pt.x < m_ncHittestPadding)
            return HTBOTTOMLEFT;
        if (pt.x > rc.right - m_ncHittestPadding)
            return HTBOTTOMRIGHT;
        return HTBOTTOM;
    }
    if (pt.x < m_ncHittestPadding)
        return HTLEFT;

    if (pt.x > rc.right - m_ncHittestPadding)
        return HTRIGHT;
#endif
    return HTCLIENT;
}

void WebViewHost::setDeviceScaleFactor(float device_scale_factor)
{
    if (device_scale_factor <= 0.f)
        device_scale_factor = 1.f;
    m_pendingDeviceScaleFactor = device_scale_factor;
    if (!m_renderWidgetHostImpl)
        return;
    m_renderWidgetHostImpl->setDeviceScaleFactor(device_scale_factor);
    SIZE size = getClientSizeLocked();
    if (size.cx > 0 && size.cy > 0 && m_renderWidgetHostImpl->isSinkReady() &&
        m_renderWidgetHostImpl->isAllowResize()) {
        m_renderWidgetHostImpl->resizeOnBlinkThread(size.cx, size.cy);
    }
}



// in ui thread
void WebViewHost::onResize(int w, int h, bool needSetHostWnd)
{
    if (0 >= w * h)
        return;
    SIZE clientSize = getClientSizeLocked();
    if (clientSize.cx == w && clientSize.cy == h)
        return;

    setClientSizeLocked(w, h);
    updataBlinkSize();

    //copyBitmapWhenResize(w, h, clientSize);

    if (m_isWebWindowMode && needSetHostWnd) {
        HWND hWnd = m_hWnd;
        content::ThreadCall::callUiThreadAsync(FROM_HERE, [hWnd, w, h] { ::SetWindowPos(hWnd, NULL, 0, 0, w, h, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE); });
    }
}

::blink::WebView* WebViewHost::getWebView() const
{
    if (!m_renderWidgetHostImpl)
        return nullptr;
    return m_renderWidgetHostImpl->m_webWiew;
}

 
// {
//     bool handle = false;
//     if (!m_draggableRegion)
//         return handle;
//
//     POINT pos;
//     ::GetCursorPos(&pos);
//     ::ScreenToClient(hWnd, &pos);
//
//     handle = !!::PtInRegion(m_draggableRegion, pos.x, pos.y);
//     return handle;
// }

bool WebViewHost::doDraggableRegionNcHitTest(HWND hWnd, int x, int y)
{
    bool handle = PlatformEventHandler::isDraggableRegionNcHitTest(hWnd, gfx::Point(x, y), m_draggableRegion);
    if (handle) {
        HWND hRootWnd = hWnd;
#ifdef _WIN32
        while (true) {
            hWnd = ::GetParent(hWnd);
            if (!hWnd)
                break;
            hRootWnd = hWnd;
        }
#endif // _WIN32
        if (hRootWnd) {
            ::ReleaseCapture();
            ::PostMessageW(hRootWnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
        }
    }
    return handle;
}

void WebViewHost::onImeComposition(ImeCompositioType type, WCHAR c)
{
    int64_t id = (int64_t)getWebviewHandle();
    blink::CompositorThreadScheduler* compositingThread = blink::ThreadScheduler::CompositorThreadScheduler();
    scoped_refptr<base::SingleThreadTaskRunner> runner = compositingThread->InputTaskRunner();

    runner->PostTask(FROM_HERE, base::BindOnce([](int64_t id, ImeCompositioType type, WCHAR c) {
        WebViewHost* self = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtrLocked(id);
        if (!self)
            return;
        self->m_platformEventHandler->fireImeComposition((PlatformEventHandler::ImeCompositioHandleType)type, c);
        common::LiveIdDetect::getWebViewIds()->unlock(id, self);
    }, id, type, c));
}

void WebViewHost::onImeText(const std::u16string& text, bool committed)
{
    int64_t id = (int64_t)getWebviewHandle();
    blink::CompositorThreadScheduler* compositingThread = blink::ThreadScheduler::CompositorThreadScheduler();
    scoped_refptr<base::SingleThreadTaskRunner> runner = compositingThread->InputTaskRunner();

    runner->PostTask(FROM_HERE, base::BindOnce([](int64_t id, std::u16string text, bool committed) {
        WebViewHost* self = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtrLocked(id);
        if (!self)
            return;
        self->m_platformEventHandler->fireImeText(text, committed);
        common::LiveIdDetect::getWebViewIds()->unlock(id, self);
    }, id, text, committed));
}

void WebViewHost::onMouseMessage(unsigned int message, int x, int y, unsigned int flags)
{
    if (!m_enableMouseKeyMessage)
        return;

    if (message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_RBUTTONDOWN) {
        content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, getWebviewHandle(), [](WebViewHost* self) {
            blink::WebViewImpl* webWiew = (blink::WebViewImpl*)(self->m_renderWidgetHostImpl->m_webWiew);
            if (webWiew) {
                webWiew->Focus();
                webWiew->SetIsActive(true);
            }
        });

        bool isNotInDraggableRegion = !doDraggableRegionNcHitTest(m_hWnd, x, y);
        if (::GetFocus() != m_hWnd && isNotInDraggableRegion && g_enableNativeSetFocus) {
            ::SetFocus(m_hWnd);
        }
        if (isNotInDraggableRegion && g_enableNativeSetCapture)
            ::SetCapture(m_hWnd);
    } else if (message == WM_LBUTTONUP || message == WM_MBUTTONUP || message == WM_RBUTTONUP) {
        ::ReleaseCapture();
    }

    //onCursorChange();

    bool needCommit = true;
    ::EnterCriticalSection(&m_mouseMsgQueueLock);
    needCommit = m_mouseMsgQueue.size() == 0;
    m_mouseMsgQueue.push_back(new MouseMsg(message, x, y, flags));
    ::LeaveCriticalSection(&m_mouseMsgQueueLock);

    if (!needCommit)
        return;

    int64_t id = (int64_t)getWebviewHandle();
    blink::CompositorThreadScheduler* compositingThread = blink::ThreadScheduler::CompositorThreadScheduler();
    scoped_refptr<base::SingleThreadTaskRunner> runner = compositingThread->InputTaskRunner();
    if (m_isPopupWidgetMode) { // ������������ֱ����blink�߳�����Ӧ��Ϣ
        runner = RenderThreadImpl::get()->getTaskRunner();
    }

    runner->PostTask(FROM_HERE, base::BindOnce([](int64_t id) {
        WebViewHost* self = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtrLocked(id);
        if (!self)
            return;
        self->delayDoMouseMsgInCompositorThread();
        common::LiveIdDetect::getWebViewIds()->unlock(id, self);
    }, id));
}

bool WebViewHost::onKeyUp(unsigned int virtualKeyCode, unsigned int flags, BOOL isSystemKey)
{
    WPARAM wParam = virtualKeyCode;
    LPARAM lParam = 0;

    if (flags & MINI_ELECTRON_REPEAT)
        lParam |= ((KF_REPEAT) >> 16);
    if (flags & MINI_ELECTRON_EXTENDED)
        lParam |= ((KF_EXTENDED) >> 16);

    blink::WebKeyboardEvent keyEvent = content::PlatformEventHandler::buildKeyboardEvent(blink::WebInputEvent::Type::kKeyUp, WM_KEYUP, wParam, lParam, flags);
    if (m_platformEventHandler.get())
        m_platformEventHandler->fireInputEventToCompositingThread(keyEvent);
    return true;
}

bool WebViewHost::onKeyDown(unsigned int virtualKeyCode, unsigned int flags, BOOL isSystemKey)
{
    //     CHECK_FOR_REENTER(this, false);
    //     freeV8TempObejctOnOneFrameBefore();

    WPARAM wParam = virtualKeyCode;
    LPARAM lParam = flags;
#if defined(OS_WIN)
//     if (m_popup && m_popup->isVisible())
//         return m_popup->fireKeyUpEvent(message, wParam, lParam);
#endif
    //WTF::TemporaryChange<bool> temporaryChange(g_isBackKeyDown, true);

    blink::WebKeyboardEvent keyEvent = content::PlatformEventHandler::buildKeyboardEvent(blink::WebInputEvent::Type::kRawKeyDown, WM_KEYDOWN, wParam, lParam, flags);
    if (m_platformEventHandler.get())
        m_platformEventHandler->fireInputEventToCompositingThread(keyEvent);
    bool systemKey = false;
    // These events cannot be canceled, and we have no default handling for them.
    // FIXME: match IE list more closely, see <http://msdn2.microsoft.com/en-us/library/ms536938.aspx>.
    if (systemKey && virtualKeyCode != VK_RETURN)
        return false;

    return true;
}

bool WebViewHost::onKeyPress(unsigned int charCode, unsigned int flags, BOOL isSystemKey)
{
    //     if (fireImeEventToNpPlugin(message, wParam, m_webViewImpl->focusedCoreFrame(), m_webViewImpl->focusedElement()))
    //         return true;

    WPARAM wParam = charCode;
    LPARAM lParam = flags;
    UINT message = WM_CHAR;
    if (isSystemKey)
        message = WM_IME_CHAR;

    blink::WebKeyboardEvent keyEvent = content::PlatformEventHandler::buildKeyboardEvent(blink::WebInputEvent::Type::kChar, message, wParam, lParam, flags);
    if (m_platformEventHandler.get())
        m_platformEventHandler->fireInputEventToCompositingThread(keyEvent);
    return true;
}

void WebViewHost::delayDoMouseMsgInCompositorThread()
{
    ::EnterCriticalSection(&m_mouseMsgQueueLock);
    if (0 == m_mouseMsgQueue.size()) {
        ::LeaveCriticalSection(&m_mouseMsgQueueLock);
        return;
    }

    std::list<MouseMsg*> mouseMsgQueue;
    bool hasMouseMove = false;
    MouseMsg* mouseMsg = nullptr;
    std::list<MouseMsg*>::reverse_iterator it = m_mouseMsgQueue.rbegin();
    for (; it != m_mouseMsgQueue.rend(); ++it) {
        mouseMsg = *it;
        if (WM_MOUSEMOVE != (*it)->message) {
            mouseMsgQueue.push_back(new MouseMsg(mouseMsg->message, mouseMsg->x, mouseMsg->y, mouseMsg->flags));
        } else if (!hasMouseMove) {
            hasMouseMove = true;
            mouseMsgQueue.push_back(new MouseMsg(mouseMsg->message, mouseMsg->x, mouseMsg->y, mouseMsg->flags));
        }
        delete mouseMsg;
    }
    m_mouseMsgQueue.clear();
    ::LeaveCriticalSection(&m_mouseMsgQueueLock);

    for (it = mouseMsgQueue.rbegin(); it != mouseMsgQueue.rend(); ++it) {
        mouseMsg = *it;

        if (mouseMsg->message == WM_MOUSELEAVE) {
            mouseMsg->x = MINSHORT;
            mouseMsg->y = MINSHORT;
        }

        BOOL handled = TRUE;
        WPARAM wParam = 0;
        LPARAM lParam = MAKELPARAM(mouseMsg->x, mouseMsg->y);
        if (mouseMsg->flags & MINI_ELECTRON_CONTROL)
            wParam |= MK_CONTROL;
        if (mouseMsg->flags & MINI_ELECTRON_SHIFT)
            wParam |= MK_SHIFT;

        if (mouseMsg->flags & MINI_ELECTRON_LBUTTON)
            wParam |= MK_LBUTTON;
        if (mouseMsg->flags & MINI_ELECTRON_MBUTTON)
            wParam |= MK_MBUTTON;
        if (mouseMsg->flags & MINI_ELECTRON_RBUTTON)
            wParam |= MK_RBUTTON;

        content::PlatformEventHandler::MouseEvtInfo info = { true, false /*pagePreDestroy <= m_state*/, m_draggableRegion, m_renderWidgetHostImpl.get() };
        if (m_platformEventHandler.get())
            m_platformEventHandler->fireMouseEvent(m_hWnd, mouseMsg->message, wParam, lParam, info, &handled);

        delete mouseMsg;
    }
}

bool WebViewHost::setCursorInfoTypeByCache()
{
    m_isCursorInfoTypeAsynChanged = false;
    if (m_isInNotClient)
        return false;

    HCURSOR hCur = NULL;
    switch (m_cursor.type()) {
    case ui::mojom::CursorType::kPointer:
        hCur = ::LoadCursorW(NULL, IDC_ARROW);
        break;
    case ui::mojom::CursorType::kIBeam:
        hCur = ::LoadCursorW(NULL, IDC_IBEAM);
        break;
    case ui::mojom::CursorType::kProgress: //
        hCur = ::LoadCursorW(NULL, IDC_APPSTARTING);
        break;
    case ui::mojom::CursorType::kCross:
        hCur = ::LoadCursorW(NULL, IDC_CROSS);
        break;
    case ui::mojom::CursorType::kMove:
        hCur = ::LoadCursorW(NULL, IDC_SIZEALL);
        break;
    case ui::mojom::CursorType::kColumnResize:
        hCur = ::LoadCursorW(NULL, IDC_SIZEWE);
        break;
    case ui::mojom::CursorType::kRowResize:
        hCur = ::LoadCursorW(NULL, IDC_SIZENS);
        break;
    case ui::mojom::CursorType::kHand:
        hCur = ::LoadCursorW(NULL, IDC_HAND);
        break;
    case ui::mojom::CursorType::kWait:
        hCur = ::LoadCursorW(NULL, IDC_WAIT);
        break;
    case ui::mojom::CursorType::kHelp:
        hCur = ::LoadCursorW(NULL, IDC_HELP);
        break;
    case ui::mojom::CursorType::kEastResize:
        hCur = ::LoadCursorW(NULL, IDC_SIZEWE);
        break;
    case ui::mojom::CursorType::kNorthResize:
        hCur = ::LoadCursorW(NULL, IDC_SIZENS);
        break;
    case ui::mojom::CursorType::kSouthWestResize:
    case ui::mojom::CursorType::kNorthEastResize:
        hCur = ::LoadCursorW(NULL, IDC_SIZENESW);
        break;
    case ui::mojom::CursorType::kSouthResize:
    case ui::mojom::CursorType::kNorthSouthResize:
        hCur = ::LoadCursorW(NULL, IDC_SIZENS);
        break;
    case ui::mojom::CursorType::kNorthWestResize:
    case ui::mojom::CursorType::kSouthEastResize:
        hCur = ::LoadCursorW(NULL, IDC_SIZENWSE);
        break;
    case ui::mojom::CursorType::kWestResize:
    case ui::mojom::CursorType::kEastWestResize:
        hCur = ::LoadCursorW(NULL, IDC_SIZEWE);
        break;
    case ui::mojom::CursorType::kNorthEastSouthWestResize:
    case ui::mojom::CursorType::kNorthWestSouthEastResize:
        hCur = ::LoadCursorW(NULL, IDC_SIZEALL);
        break;
    case ui::mojom::CursorType::kNoDrop:
    case ui::mojom::CursorType::kNotAllowed:
        hCur = ::LoadCursorW(NULL, IDC_NO);
        break;
    case ui::mojom::CursorType::kCustom:
    {
#ifdef _WIN32
        base::win::ScopedHICON customHicon = IconUtil_CreateCursorFromSkBitmap(m_cursor.custom_bitmap(), m_cursor.custom_hotspot());
        ::SetCursor(customHicon.get());
        return true;
#endif
    }
        break;
    default:
        break;
    }

    if (hCur) {
#if defined(WIN32)
        ::SetCursor(hCur);
#else
        ::linuxSetCursor(m_hWnd, hCur);
#endif
        return true;
    }

    return false;
}

void WebViewHost::setCursor(const ::ui::Cursor& cursor)
{
    if (m_cursor == cursor)
        return;
    m_cursor = cursor;

    if (m_hWnd)
        ::PostMessageW(m_hWnd, WM_SETCURSOR, (WPARAM)m_hWnd, MAKELONG(HTCLIENT, 0)); // COleControl::OnSetCursor
}

int WebViewHost::getCursorInfoType() const
{
    return (int)(m_cursor.type());
}

HDC WebViewHost::getViewDC()
{
    if (!m_memoryCanvasLock)
        return nullptr;

    if (m_memoryCanvasLockCount > 0)
        MessageBoxA(0, "WebViewHost::getViewDC lock is not matching", 0, 0);
    m_memoryCanvasLockCount++;
    m_memoryCanvasLock->Acquire();
    return (HDC)m_surface;
}

void WebViewHost::unlockViewDC()
{
    if (m_memoryCanvasLock) {
        --m_memoryCanvasLockCount;
        m_memoryCanvasLock->Release();
    }
}

void WebViewHost::onAllocatedBitmapMemory(const gfx::Size& pixelSize, void* surface, unsigned char* bitmap, void* lock)
{
    if (!m_memoryCanvasLock)
        m_memoryCanvasLock = (base::Lock*)lock;
    else
        CHECK(m_memoryCanvasLock == lock);

    m_bitmapByteSize = pixelSize;
    m_bitmapByte = bitmap;
    m_surface = surface;
}

void WebViewHost::onAllocatedSharedMemory(const gfx::Size& pixelSize, HDC dibDC, void* lock)
{
#if defined(OS_WIN)
    m_surface = dibDC;

    if (!m_memoryCanvasLock)
        m_memoryCanvasLock = (base::Lock*)lock;
    else
        CHECK(m_memoryCanvasLock == lock);
#endif
}

void WebViewHost::onPaint(HWND hWnd, WPARAM wParam)
{
    if (WS_EX_LAYERED == (WS_EX_LAYERED & GetWindowLongW(hWnd, GWL_EXSTYLE)))
        return;

    PAINTSTRUCT ps = { 0 };
    HDC hdc = ::BeginPaint(hWnd, &ps);

    RECT rcClip = ps.rcPaint;

    RECT rcClient;
    ::GetClientRect(hWnd, &rcClient);

    RECT rcInvalid = rcClient;
    if (rcClip.right != rcClip.left && rcClip.bottom != rcClip.top)
        ::IntersectRect(&rcInvalid, &rcClip, &rcClient);

    int srcX = rcInvalid.left - rcClient.left;
    int srcY = rcInvalid.top - rcClient.top;
    int destX = rcInvalid.left;
    int destY = rcInvalid.top;
    int width = rcInvalid.right - rcInvalid.left;
    int height = rcInvalid.bottom - rcInvalid.top;

    if (0 != width && 0 != height) {
#if defined(OS_WIN)
        if (m_clientResizeRepaintDirty) {
            //::FillRect(hdc, &ps.rcPaint, (HBRUSH)::GetStockObject(LTGRAY_BRUSH)); // resize��ʱ���������
            m_clientResizeRepaintDirty = false;
        }

        HDC engineDC = getViewDC();
        if (engineDC) {
            ::BitBlt(hdc, destX, destY, width, height, engineDC, srcX, srcY, SRCCOPY);
        } else if (m_hasBackgroundColor) {
            HBRUSH br = ::CreateSolidBrush(m_backgroundColor & 0x00ffffff);
            ::FillRect(hdc, &ps.rcPaint, br);
            ::DeleteObject(br);
        } else
            ::FillRect(hdc, &ps.rcPaint, (HBRUSH)::GetStockObject(WHITE_BRUSH));

        unlockViewDC();

        //RECT rcPaintxx = { 10, 10, 200, 150 };
        //HBRUSH brush = CreateSolidBrush(RGB(255, 22, 34));
        //::FillRect(hdc, &ps.rcPaint, brush);
        //::DeleteObject(brush);
#elif defined(OS_LINUX)
        if (IsLinuxOpenglDraw() && m_bitmapByte) {
            m_memoryCanvasLock->Acquire();
            //unsigned int glTexture = (unsigned int)wParam;
            //glBindTexture(GL_TEXTURE_2D, glTexture);

            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_bitmapByteSize.width(), m_bitmapByteSize.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, m_bitmapByte);
            glGenerateMipmap(GL_TEXTURE_2D);

            m_memoryCanvasLock->Release();
        } else if (m_surface) { // m_surface from OffscreenWindowUpdater::OnAllocatedBitmapMemory
            m_memoryCanvasLock->Acquire();
            cairo_t* cr = (cairo_t*)wParam;

            cairo_surface_t* surface = (cairo_surface_t*)m_surface;
            cairo_surface_flush(surface);
            //--
            //unsigned char* byteData = cairo_image_surface_get_data(surface);
            //unsigned __int32* sourceData = (unsigned __int32*)byteData;
            //int stride = cairo_image_surface_get_stride(surface) / 4;
            //
            //for (int y = 0; y < 10; y++) {
            //    for (int x = 0; x < 10; x++) {
            //        sourceData[y * stride + x] = 0xff112233;
            //    }
            //}
            //--
            cairo_surface_mark_dirty(surface);
            cairo_set_source_surface(cr, surface, 0, 0);
            m_memoryCanvasLock->Release();
        }
#endif
    }

    ::EndPaint(hWnd, &ps);
}

void WebViewHost::setIsTransparent(bool b)
{
    m_isTransparent = b;

    if (ThreadCall::isBlinkThread()) {
        if (m_renderWidgetHostImpl && m_renderWidgetHostImpl->m_webWiew) {
            blink::WebViewImpl* webview = (blink::WebViewImpl*)m_renderWidgetHostImpl->m_webWiew;
            webview->SetBaseBackgroundColorOverrideTransparent(b);
            return;
        }
    }

    mini_electron_web_view webviewHandle = (mini_electron_web_view)m_id;
    ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [b](content::WebViewHost* webview) { webview->setIsTransparent(b); });
}

void WebViewHost::setContextMenuEnable(bool b)
{
    m_isContextMenuEnable = b;
}

void WebViewHost::setShow(int nCmdShow /*, bool isActivate*/)
{
    m_isShow = nCmdShow == 1;
    ::ShowWindow(m_hWnd, nCmdShow);
}

static void setWindowTitleDalay(content::WebViewHost* webview, int count)
{
    int64_t id = webview->getId();
    base::SequencedTaskRunner::GetCurrentDefault()->PostNonNestableDelayedTask(FROM_HERE,
        base::BindOnce(
            [](int64_t id, int count) {
                count++;
                content::WebViewHost* webview = (content::WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(id);
                if (!webview || count > 3)
                    return;

                if (webview->getHostWnd()) {
                    std::u16string titleW = utf8ToUtf16(webview->getWindowTitle());
                    ::SetWindowTextW(webview->getHostWnd(), (LPCWSTR)titleW.c_str());
                    return;
                }
                setWindowTitleDalay(webview, count);
            }, id, count),
        base::Microseconds(1000));
}

bool WebViewHost::setWindowTitle(const std::string& title)
{
    m_windowTitle = title;

    if (m_hWnd) {
        std::u16string titleW = utf8ToUtf16(m_windowTitle);
        ::SetWindowTextW(m_hWnd, (LPCWSTR)titleW.c_str());
        return true;
    }

    int count = 0;
    setWindowTitleDalay(this, count);
    return false;
}

void setRequestHead(blink::WebLocalFrame* webFrame, blink::WebURLRequest& request);

extern const char* kAcceptHeader;
extern const char* kDefaultAcceptHeader;

void WebViewHost::loadUrl(const char* urlStr)
{
    std::unique_ptr<std::string> urlPtr = urlNormalization(urlStr);

    if (!m_frameClient) { // gtkģʽ�£��п���Ҫ�ȵ�s_gtkActivate�Ժ����m_frameClient
        mini_electron_web_view webviewHandle = (mini_electron_web_view)m_id;
        std::string* urlPtrTemp = urlPtr.release();
        content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, webviewHandle, [urlPtrTemp](WebViewHost* self) {
            self->loadUrl(urlPtrTemp->c_str());
            delete urlPtrTemp;
        });
        return;
    }

    blink::WebNavigationControl* navigationControl = m_frameClient->m_navigationControl;

    blink::KURL url(WTF::String::FromUTF8(std::string_view(urlPtr->c_str(), urlPtr->size())));
    if (!url.IsValid()) {
        String protocol = url.Protocol();
        if (protocol.empty() && protocol != "about:blank") {
            std::string urlTemp = *(urlPtr.get());
            urlTemp = "https://" + urlTemp;
            url = blink::KURL(WTF::String::FromUTF8(urlTemp.c_str()));
        }
    } else {
        mini_electron::WebURLLoaderManager::sharedInstance()->cancelAllJobsOfWebview(m_id);
    }

    blink::WebNavigationInfo info;
    info.url_request.SetUrl(url);
    if (!navigationControl->WillStartNavigation(info))
        DebugBreak();

    blink::WebURLRequest urlRequest;
    urlRequest.SetUrl(url);
    urlRequest.AddHttpHeaderField(blink::WebString::FromLatin1(kAcceptHeader), blink::WebString::FromLatin1(kDefaultAcceptHeader));

    setRequestHead((blink::WebLocalFrame*)navigationControl, urlRequest);
    navigationControl->Load(urlRequest, blink::WebFrameLoadType::kStandard, blink::WebHistoryItem());
}

bool WebViewHost::loadHTMLString(const std::string& html, const std::string& baseUrl)
{
    if (!m_renderWidgetHostImpl)
        return false;

    blink::WebLocalFrameImpl* impl = blink::To<blink::WebLocalFrameImpl>(m_renderWidgetHostImpl->m_mainFrame);
    if (!impl)
        return false;

    std::unique_ptr<blink::WebNavigationParams> navigationParams
        = blink::WebNavigationParams::CreateWithHTMLStringForTesting(base::span<const char>(html.c_str(), html.size()),
            blink::WebURL(blink::KURL(String::FromUTF8(baseUrl))));
    impl->CommitNavigation(std::move(navigationParams), nullptr /* extra_data */);

    return true;
}

void WebViewHost::reload(bool force)
{
    if (!m_renderWidgetHostImpl || !m_renderWidgetHostImpl->m_mainFrame)
        return;
    mini_electron::WebURLLoaderManager::sharedInstance()->cancelAllJobsOfWebview(m_id);
    m_renderWidgetHostImpl->m_mainFrame->DeprecatedStopLoading();
    m_renderWidgetHostImpl->m_mainFrame->StartReload(force ? blink::WebFrameLoadType::kReload : blink::WebFrameLoadType::kReloadBypassingCache);
}

LRESULT WebViewHost::fireWheelEventOnUiThread(WPARAM wParam, LPARAM lParam)
{
    if (m_platformEventHandler.get())
        m_platformEventHandler->fireWheelEvent(m_hWnd, wParam, lParam);
    return 0;
}

scoped_refptr<mini_electron::PageNetExtraData> WebViewHost::getPageNetExtraData()
{
    if (m_pageNetExtraData)
        return m_pageNetExtraData;
    return nullptr;
}

std::string WebViewHost::getCookie()
{
    blink::WebLocalFrame* frame = (blink::WebLocalFrame*)(m_renderWidgetHostImpl->m_webWiew->MainFrame());
    if (!frame)
        return "";

    blink::WebDocument webDocument = frame->GetDocument();
    if (webDocument.IsNull())
        return "";

    mini_electron::WebCookieJarImpl* cookieJar = getWebCookieJarImpl();
    if (!cookieJar)
        return "";

    const blink::Document* doc = webDocument.ConstUnwrap<blink::Document>();
    return cookieJar->getCookiesForSession(doc->CookieURL(), true);
}

mini_electron::WebCookieJarImpl* WebViewHost::getWebCookieJarImpl()
{
    mini_electron::WebCookieJarImpl* ret = nullptr;
    if (m_pageNetExtraData)
        ret = m_pageNetExtraData->getCookieJar();
    if (ret)
        return ret;
    return mini_electron::WebURLLoaderManager::sharedInstance()->getShareCookieJar();
}

void WebViewHost::setCookie(const std::string& ck)
{
    blink::WebLocalFrame* frame = (blink::WebLocalFrame*)(m_renderWidgetHostImpl->m_webWiew->MainFrame());
    if (!frame)
        return;

    blink::WebDocument webDocument = frame->GetDocument();
    if (webDocument.IsNull())
        return;

    mini_electron::WebCookieJarImpl* cookieJar = getWebCookieJarImpl();
    if (!cookieJar)
        return;

    const blink::Document* doc = webDocument.ConstUnwrap<blink::Document>();
    return cookieJar->setCookiesFromDOM(blink::KURL(), /*doc->CookieURL()*/blink::KURL(), ck);
}

void WebViewHost::setCookieJarFullPath(const char* path)
{
    if (!m_pageNetExtraData)
        m_pageNetExtraData = new mini_electron::PageNetExtraData();
    m_pageNetExtraData->setCookieJarFullPath(path);
}

void WebViewHost::setLocalStorageFullPath(const char* path)
{
    if (!m_pageNetExtraData)
        m_pageNetExtraData = new mini_electron::PageNetExtraData();
    m_pageNetExtraData->setLocalStorageDir(path);
}

base::FilePath WebViewHost::getLocalStorageDir()
{
    if (!m_pageNetExtraData)
        m_pageNetExtraData = new mini_electron::PageNetExtraData();
    return m_pageNetExtraData->getLocalStorageDir();
}

base::FilePath WebViewHost::getDownloadDirPath()
{
    if (!m_pageNetExtraData)
        m_pageNetExtraData = new mini_electron::PageNetExtraData();
    return m_pageNetExtraData->getDownloadDirPath();
}

void WebViewHost::dispatchUrlCheanged(const std::string& url)
{
    BOOL canGoBack = historyBackListCount() > 0;
    BOOL canGoForward = historyForwardListCount() > 0;

    m_url = url;


    if (!(getClosure().m_URLChangedCallback))
        return;

    mini_electron_web_view webviewHandle = m_frameClient->getEngineViewId();
    std::string* urlStr = new std::string(url);

    ThreadCall::callUiThreadAsync(FROM_HERE, [webviewHandle, urlStr, canGoBack, canGoForward]() {
        WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
        if (webview)
            webview->getClosure().m_URLChangedCallback(webviewHandle, webview->getClosure().m_URLChangedParam, urlStr->c_str(), canGoBack, canGoForward);
        delete urlStr;
    });
}

bool WebViewHost::canGoForward() const
{
    return historyForwardListCount() > 0;
}

bool WebViewHost::canGoBack() const
{
    return historyBackListCount() > 0;
}

void WebViewHost::didCommitProvisionalLoad(
    blink::WebLocalFrame* frame, const blink::WebHistoryItem& history, blink::WebHistoryCommitType type, bool isSameDocument)
{
    m_hasDispatchWillCommitProvisionalLoad = true; // �е�һ������������ʱ������
    m_navigationController->insertOrReplaceEntry(frame, history, type, isSameDocument);

    if ((m_renderWidgetHostImpl && m_renderWidgetHostImpl->m_mainFrame == frame) || frame->Top() == frame) {
        blink::WebDocument doc = frame->GetDocument();
        blink::WebURL url = doc.Url();
        dispatchUrlCheanged(url.GetString().Utf8());
    }
}

blink::WebHistoryItem WebViewHost::historyItemForNewChildFrame(blink::WebFrame* frame)
{
    return m_navigationController->historyItemForNewChildFrame(frame);
}

void WebViewHost::navigateBackForwardSoon(int offset)
{
    m_navigationController->navigateBackForwardSoon(offset);
}

int WebViewHost::historyBackListCount() const
{
    return m_navigationController->historyBackListCount();
}

int WebViewHost::historyForwardListCount() const
{
    return m_navigationController->historyForwardListCount();
}

void WebViewHost::navigateToIndex(int index)
{
    m_navigationController->navigateToIndex(index);
}

void WebViewHost::onDidCreateScriptContext(v8::Local<v8::Context> context, int32_t worldId, const blink::LocalFrameToken& token)
{
    BindJsQuery::bindFun(context, getClosure().m_jsQueryClosure, getClosure().m_jsQueryClosure2, this, token); // !!!!!!!


    void* param = getClosure().m_DidCreateScriptContextParam;
    mini_electron_did_create_script_context_callback callback = getClosure().m_DidCreateScriptContextCallback;
    if (callback)
        callback((mini_electron_web_view)m_id, param, (mini_electron_web_frame_handle)(blink::LocalFrameToken::Hasher()(token)), &context, 0, worldId);
}

void WebViewHost::onWillReleaseScriptContext(v8::Local<v8::Context> context, int32_t worldId, const blink::LocalFrameToken& token)
{
    void* param = getClosure().m_WillReleaseScriptContextParam;
    mini_electron_will_release_script_context_callback callback = getClosure().m_WillReleaseScriptContextCallback;
    if (callback)
        callback((mini_electron_web_view)m_id, param, (mini_electron_web_frame_handle)(blink::LocalFrameToken::Hasher()(token)), &context, worldId);
}

mini_electron::engine::JsValueBridge* WebViewHost::runJsOnBlinkThreadImpl(
    mini_electron_web_frame_handle frameId, int worldId, std::string* codeStr, BOOL isInClosure, mini_electron_run_js_callback callback, void* param)
{
    //RefPtr<blink::UserGestureToken> userGestureToken = blink::UserGestureIndicator::currentToken();
    blink::WebLocalFrame* frame = nullptr;
    if ((mini_electron_web_frame_handle)-2 != frameId) {
        blink::LocalFrame* blinkFrame = blink::FromFrameTokenHash((size_t)(frameId));
        if (!blinkFrame)
            return nullptr;
        frame = blink::WebLocalFrameImpl::FromFrame(blinkFrame);
    } else {
        if (!m_renderWidgetHostImpl)
            return nullptr;
        frame = m_renderWidgetHostImpl->m_mainFrame;
    }

    if (base::StartsWith(*codeStr, "javascript:", base::CompareCase::INSENSITIVE_ASCII))
        //scriptString->Remove(0, sizeof("javascript:") - 1);
        codeStr->erase(codeStr->end() - (sizeof("javascript:") - 1));

    if (isInClosure) {
        codeStr->insert(0, "(function(){");
        codeStr->append("})();");
    }
    blink::WebScriptSource code(blink::WebString::FromUTF8(*codeStr), blink::KURL("http://CWebView_runJS.com/"));

    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope handleScope(isolate);

    v8::Local<v8::Context> context;
    v8::Local<v8::Value> result;
    if (0 == worldId) {
        context = frame->MainWorldScriptContext();
        v8::Context::Scope contextScope(context);
        result = frame->ExecuteScriptAndReturnValue(code);
    } else {
        context = frame->GetScriptContextFromWorldId(isolate, worldId);
        v8::Context::Scope contextScope(context);
        result = frame->ExecuteScriptInIsolatedWorldAndReturnValue(worldId, code, blink::BackForwardCacheAware::kAllow);
    }

    //wke::AutoAllowRecordJsExceptionInfo autoAllowRecordJsExceptionInfo;
    return mini_electron::engine::JsValueBridge::v8ValueToEngineValue(isolate, context, result);
}

static void callRunJsCallbackOnUiThread(mini_electron_web_view webviewHandle, void* param, mini_electron_run_js_callback callback, void* es, mini_electron::engine::JsValueBridge* mini_electron_val)
{
    WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (webview)
        callback(webviewHandle, param, es, mini_electron_val->getId());
    mini_electron_val->deref();

    freeTempCharStrings();
}

void runJsOnBlinkThread(
    mini_electron_web_view webviewHandle, mini_electron_web_frame_handle frameId, int worldId, std::string* scriptString, BOOL isInClosure, mini_electron_run_js_callback callback, void* param)
{
    WebViewHost* webview = (WebViewHost*)common::LiveIdDetect::getWebViewIds()->getPtr(webviewHandle);
    if (!webview) {
        delete scriptString;
        return;
    }

    mini_electron::engine::JsValueBridge* mini_electron_val = webview->runJsOnBlinkThreadImpl(frameId, worldId, scriptString, isInClosure, callback, param);

    if (callback) {
        content::ThreadCall::callUiThreadAsync(
            FROM_HERE, [webviewHandle, param, callback, mini_electron_val] { callRunJsCallbackOnUiThread(webviewHandle, param, callback, nullptr, mini_electron_val); });
    }
    delete scriptString;
}

void WebViewHost::setUserKeyValue(const char* key, void* value)
{
    ::EnterCriticalSection(&m_userKeyValuesLock);
    m_userKeyValues[key] = value;
    ::LeaveCriticalSection(&m_userKeyValuesLock);
}

void* WebViewHost::getUserKeyValue(const char* key) const
{
    ::EnterCriticalSection(&m_userKeyValuesLock);
    std::map<std::string, void*>::const_iterator it = m_userKeyValues.find(key);
    if (m_userKeyValues.end() == it) {
        ::LeaveCriticalSection(&m_userKeyValuesLock);
        return nullptr;
    }
    void* ret = it->second;
    ::LeaveCriticalSection(&m_userKeyValuesLock);
    return ret;
}


void WebViewHost::setProxy(const mini_electron_proxy* proxy)
{
    if (!m_pageNetExtraData)
        m_pageNetExtraData = new mini_electron::PageNetExtraData();
    m_pageNetExtraData->setProxy(proxy);
}

const mini_electron_proxy* WebViewHost::getProxy() const
{
    if (!m_pageNetExtraData)
        return nullptr;
    return m_pageNetExtraData->getProxy();
}

void WebViewHost::setSetLanguage(const std::string& lang)
{
    blink::RendererPreferences pref = m_renderWidgetHostImpl->m_webWiew->GetRendererPreferences();
    pref.accept_languages = lang;// WTF::String::FromUTF8((const uint8_t*)lang.c_str(), lang.size());
    m_renderWidgetHostImpl->m_webWiew->SetRendererPreferences(pref);
}

std::string WebViewHost::getSetLanguage() const
{
    std::string result = "en";
    if (!m_renderWidgetHostImpl || !m_renderWidgetHostImpl->m_webWiew)
        return result;
    const blink::RendererPreferences& pref = m_renderWidgetHostImpl->m_webWiew->GetRendererPreferences();
    if (pref.accept_languages.empty())
        return result;
    return pref.accept_languages;
}

}