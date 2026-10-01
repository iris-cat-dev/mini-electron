
#include "runtime/engine/renderer/render_thread_impl.h"

#include "runtime/engine/renderer/renderer_blink_platform_impl.h"
#include "runtime/engine/renderer/web_local_frame_client_impl.h"
#include "runtime/engine/renderer/blink_interface_registry_impl.h"
#include "runtime/engine/renderer/policy_container_host_impl.h"
#include "runtime/engine/renderer/web_view_client_impl.h"
#include "runtime/engine/renderer/content_security_notifier_impl.h"
#include "runtime/engine/browser/local_frame_host_impl.h"
#include "runtime/engine/browser/local_main_frame_host_impl.h"
#include "runtime/engine/browser/back_forward_cache_controller_host_impl.h"
#include "runtime/engine/browser/render_widget_host_impl.h"
#include "runtime/engine/common/thread_call.h"
#include "runtime/engine/common/create_and_bind_templ.h"
#include "runtime/engine/common/live_id_detect.h"
#include "runtime/engine/viz/viz_service.h"
#include "runtime/engine/viz/viz_host.h"
#include "gen/services/viz/privileged/mojom/compositing/frame_sink_manager.mojom.h"
#if defined(OS_WIN)
#include "third_party/skia/include/ports/SkTypeface_win.h"
#elif BUILDFLAG(IS_MAC)
#include "third_party/skia/include/ports/SkFontMgr_mac_ct.h"
#endif
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "gen/third_party/blink/public/platform/web_runtime_features_base.h"
#include "gen/third_party/blink/public/mojom/frame/policy_container.mojom-blink.h"
#include "gen/third_party/blink/public/mojom/frame/frame.mojom-blink.h"
#include "third_party/blink/renderer/core/loader/document_loader.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/navigation_body_loader.h"
#include "third_party/blink/renderer/platform/network/network_state_notifier.h"
#include "third_party/blink/renderer/platform/mojo/browser_interface_broker_proxy_impl.h"
#include "third_party/blink/public/platform/web_policy_container.h"
#include "third_party/blink/public/platform/web_connection_type.h"
#include "third_party/blink/public/web/web_navigation_control.h"
#include "third_party/blink/public/web/web_settings.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_view_client.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/blink.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "third_party/blink/public/platform/scheduler/web_thread_scheduler.h"
#include "third_party/blink/public/platform/weak_wrapper_resource_load_info_notifier.h"
#include "services/network/public/cpp/resource_request.h"
#include "runtime/network/loader/web_url_loader_impl_curl.h"
#include "ui/gfx/native_widget_types.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "base/check.h"
#include "base/memory/discardable_memory_allocator.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "third_party/icu/source/common/unicode/uclean.h"
#include "third_party/icu/source/common/unicode/udata.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/ports/SkFontMgr_directory.h"
#include "third_party/skia/include/core/SkGraphics.h"
#include "third_party/blink/renderer/platform/fonts/font_cache.h"
#include "third_party/blink/renderer/platform/wtf/threading_primitives.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#if !defined(OS_WIN)
#include <sys/mman.h>
#else
#include "third_party/blink/public/web/win/web_font_rendering.h"
#include "skia/ext/font_utils.h"
#include "base/win/windows_version.h"
#include "base/allocator/partition_alloc_support.h"
#include "base/threading/sequence_local_storage_slot.h"
#include <Shlwapi.h>
#endif
#include "url/url_util.h"

#if !BUILDFLAG(IS_MAC)
extern unsigned char icudtlData[1884304];
#endif
extern unsigned char SnapshotBlobBinX86[328619];
extern unsigned char SnapshotBlobBinX64[327326];
extern unsigned char SnapshotBlobBinX64Linux[471944];
extern unsigned char SnapshotBlobBinArm64Linux[471936];

extern "C" const uint8_t v8_Default_embedded_blob_code_[];
extern "C" uint32_t v8_Default_embedded_blob_code_size_;
extern "C" const uint8_t v8_Default_embedded_blob_data_[];
extern "C" uint32_t v8_Default_embedded_blob_data_size_;

namespace cc {
extern size_t kMemoryThresholdTSoftwareImageDecodeCache;
}


namespace v8 {
namespace internal {
uint32_t RSHash(const char* str, uint32_t len);
}
}

namespace content {

RenderThreadImpl* RenderThreadImpl::s_inst = nullptr;

RenderThreadImpl* RenderThreadImpl::get()
{
    if (s_inst)
        return s_inst;
    s_inst = new RenderThreadImpl();
    return s_inst;
}

RenderThreadImpl::ThreadDelegate::ThreadDelegate(RenderThreadImpl* parent)
{
    m_parent = parent;
}

scoped_refptr<base::SingleThreadTaskRunner> RenderThreadImpl::ThreadDelegate::GetDefaultTaskRunner()
{
    return m_parent->m_agentGroupScheduler->DefaultTaskRunner();
}

void RenderThreadImpl::ThreadDelegate::BindToCurrentThread()
{
    m_parent->initializeWebKitOnThread();
    m_parent->m_inited = true;
    //m_parent->createTestView();
}

RenderThreadImpl::RenderThreadImpl()
    : m_thread("RenderThreadImpl")
    , m_hostThread("HostThread")
{
    m_uiThreadTask = std::make_unique<base::SingleThreadTaskExecutor>(base::MessagePumpType::UI);
    base::PlatformThread::SetName("Mb132UiThread");

    ThreadCall::init(nullptr);

    base::Thread::Options opt;
    opt.stack_size = 3000000;
    opt.delegate = std::make_unique<RenderThreadImpl::ThreadDelegate>(this);
    m_thread.StartWithOptions(std::move(opt));
    while (!m_inited) {
        ::Sleep(1);
    }

    m_hostThread.Start();
}

static void initializeICUData()
{
    UErrorCode error = U_ZERO_ERROR;
#if BUILDFLAG(IS_MAC)
    // The macOS target links //third_party/icu:icudata. Let ICU discover that
    // full data package instead of replacing it with MiniBlink's legacy copy.
    u_init(&error);
#else
    udata_setCommonData(const_cast<uint8_t*>(icudtlData), &error);
#endif
    CHECK(U_SUCCESS(error)) << "ICU initialization failed: " << u_errorName(error);
}

// TestDiscardableMemoryAllocator is a simple DiscardableMemoryAllocator
// implementation that can be used for testing. It allocates one-shot
// DiscardableMemory instances backed by heap memory.
class TestDiscardableMemoryAllocator : public base::DiscardableMemoryAllocator {
public:
    TestDiscardableMemoryAllocator() = default;

    TestDiscardableMemoryAllocator(const TestDiscardableMemoryAllocator&) = delete;
    TestDiscardableMemoryAllocator& operator=(const TestDiscardableMemoryAllocator&) = delete;

    // Overridden from DiscardableMemoryAllocator:
    std::unique_ptr<base::DiscardableMemory> AllocateLockedDiscardableMemory(size_t size) override;

    size_t GetBytesAllocated() const override;

    void ReleaseFreeMemory() override
    {
        // Do nothing since it is backed by heap memory.
    }

    void onDiscardableMemoryDestroy(int64_t id);
    void onDiscardableMemoryAllocator(size_t size);
    void onDiscardableMemoryRelease(size_t size);

private:
    WTF::RecursiveMutex m_lock;
    std::set<int64_t> m_ids;
    int64_t m_discardableMemorySize = 0;
};

const int kDebugSizeExt = 1
#ifdef _DEBUG
    + 1
#endif
    ;

class DiscardableMemoryImpl : public base::DiscardableMemory {
public:
    explicit DiscardableMemoryImpl(TestDiscardableMemoryAllocator* allocator, size_t size)
        : m_data(new uint8_t[size * kDebugSizeExt])
        , m_size(size)
        , m_allocator(allocator)
    {
        allocator->onDiscardableMemoryAllocator(size * kDebugSizeExt);

        m_id = common::LiveIdDetect::get()->constructed(this);
#ifdef _DEBUG
        memset(m_data, 0xf4, size * kDebugSizeExt);
#endif // _DEBUG
    }

    ~DiscardableMemoryImpl()
    {
        common::LiveIdDetect::get()->deconstructed(m_id);
        m_allocator->onDiscardableMemoryDestroy(m_id);

        DCHECK(!m_isLocked);
        releaseMem();
    }

    // Overridden from DiscardableMemory:
    bool Lock() override
    {
        DCHECK(!m_isLocked);
        if (!m_data)
            return false;
        m_isLocked = true;
        return true;
    }

    void Unlock() override
    {
        DCHECK(m_isLocked);
        m_isLocked = false;
        // Force eviction to catch clients not correctly checking the return value of Lock().
        //memset(m_data, 0, m_size);
    }

    void* data() const override
    {
        DCHECK(m_isLocked);
        return m_data;
    }

    void DiscardForTesting() override
    {
    }

    void releaseMem()
    {
        if (m_isLocked || !m_data)
            return;
        m_allocator->onDiscardableMemoryRelease(m_size * kDebugSizeExt);
#ifdef _DEBUG
        for (size_t i = m_size; i < 2 * kDebugSizeExt; ++i) {
            if (m_data[i] != 0xf4)
                DebugBreak();
        }
#endif // _DEBUG
        delete[] m_data;
        m_data = nullptr;
    }

    int64_t getId() const
    {
        return m_id;
    }

    base::trace_event::MemoryAllocatorDump* CreateMemoryAllocatorDump(const char* name, base::trace_event::ProcessMemoryDump* pmd) const override
    {
        return nullptr;
    }

private:
    bool m_isLocked = true;
    uint8_t* m_data = nullptr;
    size_t m_size = 0;
    int64_t m_id = 0;
    TestDiscardableMemoryAllocator* m_allocator = nullptr;
};

std::unique_ptr<base::DiscardableMemory> TestDiscardableMemoryAllocator::AllocateLockedDiscardableMemory(size_t size)
{
    if (size > cc::kMemoryThresholdTSoftwareImageDecodeCache)
        return nullptr;

    if (m_discardableMemorySize > cc::kMemoryThresholdTSoftwareImageDecodeCache) {
        char output[100] = { 0 };
        sprintf(output, "AllocateLockedDiscardableMemory fail: %d\n", size);
        OutputDebugStringA(output);

        m_lock.lock();
        for (std::set<int64_t>::iterator it = m_ids.begin(); it != m_ids.end(); ++it) {
            int64_t id = *it;
            DiscardableMemoryImpl* item = (DiscardableMemoryImpl*)common::LiveIdDetect::get()->getPtr(id);
            item->releaseMem();
        }
        m_lock.unlock();
        return nullptr;
    }

    std::unique_ptr<DiscardableMemoryImpl> ret = std::make_unique<DiscardableMemoryImpl>(this, size);
    WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
    m_ids.insert(ret->getId());
    return std::move(ret);
}

void TestDiscardableMemoryAllocator::onDiscardableMemoryDestroy(int64_t id)
{
    WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
    m_ids.erase(id);
}

void TestDiscardableMemoryAllocator::onDiscardableMemoryAllocator(size_t size)
{
    WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
    m_discardableMemorySize += size;
}

void TestDiscardableMemoryAllocator::onDiscardableMemoryRelease(size_t size)
{
    WTF::Locker<WTF::RecursiveMutex> locker(m_lock);
    m_discardableMemorySize -= size;
}

size_t TestDiscardableMemoryAllocator::GetBytesAllocated() const
{
    return 0U;
}

static std::unique_ptr<base::MessagePump> createMainThreadMessagePump()
{
#if BUILDFLAG(IS_MAC)
    // Blink uses Cocoa timers for scrolling and text input on macOS.
    return base::MessagePump::Create(base::MessagePumpType::NS_RUNLOOP);
#elif BUILDFLAG(IS_FUCHSIA)
    return base::MessagePump::Create(base::MessagePumpType::IO);
#else
    return base::MessagePump::Create(base::MessagePumpType::UI);
#endif
}

#if defined(OS_WIN)
void SkiaPreCacheFont(const LOGFONT& logfont)
{
    //     RenderThread* render_thread = RenderThread::Get();
    //     if (render_thread) {
    //         render_thread->PreCacheFont(logfont);
    //     }
    //DebugBreak();
}
#endif

class EmptyBrowserInterfaceBroker : public ::blink::mojom::BrowserInterfaceBroker {
public:
    EmptyBrowserInterfaceBroker()
    //: m_contentSecurityNotifierImplReceiver(new ContentSecurityNotifierImpl(0))
    {
    }

private:
    void GetInterface(::mojo::GenericPendingReceiver receiver) override // �ο�WebLocalFrameClientImpl::GetInterface
    {
        std::string output("EmptyBrowserInterfaceBroker::GetInterface: ");
        std::string name = receiver.interface_name().has_value() ? receiver.interface_name().value() : "no-name";
        output += name;
        output += "\n";
        OutputDebugStringA(output.c_str());

        if ("blink.mojom.ContentSecurityNotifier" == name) {
            //mojo::PendingReceiver<::blink::mojom::blink::ContentSecurityNotifier> pendingReceiver(receiver.PassPipe());
            //m_contentSecurityNotifierImplReceiver.Bind(std::move(pendingReceiver));
            createAndBindBrokerProxy<::blink::mojom::blink::ContentSecurityNotifier, ContentSecurityNotifierImpl>(receiver.PassPipe(), NULL_WEBVIEW);
        } else
            DebugBreak();
    }

    //mojo::Receiver<::blink::mojom::blink::ContentSecurityNotifier> m_contentSecurityNotifierImplReceiver;
};

class EmptyAssociatedInterfaceProvider {
public:
    EmptyAssociatedInterfaceProvider()
        : m_emptyLocalFrameHostReceiver(new LocalFrameHostImpl(nullptr))
        //, m_backForwardCacheControllerHostReceiver(new BackForwardCacheControllerHostImpl())
        //, m_emptyLocalMainFrameHostReceiver(new LocalMainFrameHostImpl(0))
        //, m_emptyBrowserInterfaceBroker(new EmptyBrowserInterfaceBroker())
    {
    }

    void onBindLocalMainFrameHost(mojo::ScopedInterfaceEndpointHandle handle)
    {
        //mojo::PendingAssociatedReceiver<::blink::mojom::blink::LocalMainFrameHost> pendingReceiver(std::move(handle));
        //m_emptyLocalMainFrameHostReceiver.Bind(std::move(pendingReceiver));
        createAndBindInterface<::blink::mojom::blink::LocalMainFrameHost, LocalMainFrameHostImpl>(std::move(handle), nullptr);
    }

    void onBindLocalFrameHost(mojo::ScopedInterfaceEndpointHandle handle)
    {
        createAndBindInterface<::blink::mojom::blink::LocalFrameHost, LocalFrameHostImpl>(std::move(handle), nullptr);
    }

    void onBindBackForwardCacheControllerHost(mojo::ScopedInterfaceEndpointHandle handle)
    {
        //mojo::PendingAssociatedReceiver<::blink::mojom::blink::LocalFrameHost> pendingReceiver(std::move(handle));
        //m_emptyLocalFrameHostReceiver.Bind(std::move(pendingReceiver));
        createAndBindInterface<::blink::mojom::blink::BackForwardCacheControllerHost, BackForwardCacheControllerHostImpl>(std::move(handle));
    }

    mojo::AssociatedReceiver<::blink::mojom::blink::LocalFrameHost> m_emptyLocalFrameHostReceiver;
    //mojo::AssociatedReceiver<::blink::mojom::blink::LocalMainFrameHost> m_emptyLocalMainFrameHostReceiver;

    //mojo::Receiver<::blink::mojom::BrowserInterfaceBroker> m_emptyBrowserInterfaceBroker;
};

// class EmptyBrowserInterfaceBrokerProxyImpl : public blink::TestableBrowserInterfaceBrokerProxy {
// public:
//     explicit EmptyBrowserInterfaceBrokerProxyImpl();
// 
//     EmptyBrowserInterfaceBrokerProxyImpl(const EmptyBrowserInterfaceBrokerProxyImpl&) = delete;
//     EmptyBrowserInterfaceBrokerProxyImpl& operator=(const EmptyBrowserInterfaceBrokerProxyImpl&) = delete;
// 
//     void Bind(CrossVariantMojoRemote<mojom::BrowserInterfaceBrokerInterfaceBase> broker, scoped_refptr<base::SingleThreadTaskRunner> task_runner);
// 
//     CrossVariantMojoReceiver<mojom::BrowserInterfaceBrokerInterfaceBase> Reset(scoped_refptr<base::SingleThreadTaskRunner> task_runner) override;
// 
//     // Asks the browser to bind the given receiver. If a non-null testing override
//     // was set by |SetBinderForTesting()|, the request will be intercepted by that
//     // binder instead of going to the browser.
//     void GetInterface(mojo::GenericPendingReceiver) const override;
// 
//     bool is_bound() const;
// 
//     void Trace(Visitor*) const;
// 
// private:
//     // TODO(https://crbug.com/352165586): Stop using
//     // `kForceWithoutContextObserver`.
//     HeapMojoRemote<mojom::blink::BrowserInterfaceBroker, HeapMojoWrapperMode::kForceWithoutContextObserver> broker_;
// };

// blink::BrowserInterfaceBrokerProxy& getEmptyBrowserInterfaceBroker()
// {
//     static base::SequenceLocalStorageSlot<blink::BrowserInterfaceBrokerProxyImpl> proxySlot;
//     if (!proxySlot.GetValuePointer()) {
//         blink::BrowserInterfaceBrokerProxyImpl& proxy = proxySlot.GetOrCreateValue();
//         mojo::PendingRemote<blink::mojom::BrowserInterfaceBroker> remote;
//         std::ignore = remote.InitWithNewPipeAndPassReceiver();
//         proxy.Bind(std::move(remote), base::SingleThreadTaskRunner::GetCurrentDefault());
//     }
// 
//     return proxySlot.GetOrCreateValue();
// }

void RenderThreadImpl::OverrideAssociatedInterfaceProvider(blink::AssociatedInterfaceProvider* associatedInterfaceProvider)
{
    if (!m_emptyAssociatedInterfaceProvider)
        m_emptyAssociatedInterfaceProvider = new EmptyAssociatedInterfaceProvider();

    // ��W:\mycode\mb108\content\renderer\AssociatedInterfaceProviderImpl.cpp��
    associatedInterfaceProvider->OverrideBinderForTesting("blink.mojom.LocalMainFrameHost",
        base::BindRepeating(&EmptyAssociatedInterfaceProvider::onBindLocalMainFrameHost, base::Unretained(m_emptyAssociatedInterfaceProvider)));

    associatedInterfaceProvider->OverrideBinderForTesting("blink.mojom.LocalFrameHost",
        base::BindRepeating(&EmptyAssociatedInterfaceProvider::onBindLocalFrameHost, base::Unretained(m_emptyAssociatedInterfaceProvider)));

    associatedInterfaceProvider->OverrideBinderForTesting("blink.mojom.BackForwardCacheControllerHost",
        base::BindRepeating(&EmptyAssociatedInterfaceProvider::onBindBackForwardCacheControllerHost, base::Unretained(m_emptyAssociatedInterfaceProvider)));

    //     if ("blink.mojom.BackForwardCacheControllerHost" == name) {
    //         mojo::PendingAssociatedReceiver<::blink::mojom::blink::BackForwardCacheControllerHost> pendingReceiver(receiver.PassHandle());
    //         m_backForwardCacheControllerHostReceiver.Bind(std::move(pendingReceiver));
    //     } else if ("blink.mojom.LocalFrameHost" == name) {
    //         mojo::PendingAssociatedReceiver<::blink::mojom::blink::LocalFrameHost> pendingReceiver(receiver.PassHandle());
    //         m_localFrameHostReceiver.Bind(std::move(pendingReceiver));
    //     } else if ("blink.mojom.LocalMainFrameHost" == name) {
    //         mojo::PendingAssociatedReceiver<::blink::mojom::blink::LocalMainFrameHost> pendingReceiver(receiver.PassHandle());
    //         m_localMainFrameHostReceiver.Bind(std::move(pendingReceiver));
    //     } else
    //         DebugBreak();


//     //blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting("blink.mojom.ContentSecurityNotifier", );
//     mojo::PendingReceiver<blink::mojom::BrowserInterfaceBroker> pendingBroker
//         = getEmptyBrowserInterfaceBroker().Reset(base::SingleThreadTaskRunner::GetCurrentDefault());
//     m_emptyAssociatedInterfaceProvider->m_emptyBrowserInterfaceBroker.Bind(std::move(pendingBroker));
//     //     blink::GetEmptyBrowserInterfaceBroker().Bind(
//     //         m_emptyAssociatedInterfaceProvider->m_emptyBrowserInterfaceBroker.BindNewPipeAndPassRemote(),
//     //         base::ThreadTaskRunnerHandle::Get());
}

void RenderThreadImpl::OverrideEmptyAssociatedInterfaceProvider()
{
    blink::AssociatedInterfaceProvider* associatedInterfaceProvider = blink::AssociatedInterfaceProvider::GetEmptyAssociatedInterfaceProvider();
    OverrideAssociatedInterfaceProvider(associatedInterfaceProvider);
}

void* genV8EmbeddedData(const void* data, size_t size)
{
    void* result = (void*)data;
#if defined(OS_WIN)
    DWORD oldProtect = 0;
    ::VirtualProtect((LPVOID)data, size, PAGE_EXECUTE_READWRITE, &oldProtect);
#else
    // mprotect(pushAllRegistersPtr, sizeof(pushAllRegistersShellCode), PROT_READ | PROT_EXEC);
    result = mmap(0, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    memcpy(result, data, size);
#endif
    return result;
}

// Maximum allocation size allowed for image scaling filters that
// require pre-scaling. Skia will fallback to a filter that doesn't
// require pre-scaling if the default filter would require an
// allocation that exceeds this limit.
const size_t kImageCacheSingleAllocationByteLimit = 64 * 1024 * 1024;

void initializeSkia()
{
    // Make sure that any switches used here are propagated to the renderer and GPU processes.
    //const base::CommandLine& cmd = *base::CommandLine::ForCurrentProcess();
    SkGraphics::Init();

    const int kMB = 1024 * 1024;
    //size_t font_cache_limit;
#if BUILDFLAG(IS_ANDROID)
    font_cache_limit = base::SysInfo::IsLowEndDevice() ? kMB : 8 * kMB;
    SkGraphics::SetFontCacheLimit(font_cache_limit);
#else
//     if (cmd.HasSwitch(switches::kSkiaFontCacheLimitMb)) {
//         if (base::StringToSizeT(cmd.GetSwitchValueASCII(switches::kSkiaFontCacheLimitMb), &font_cache_limit)) {
//             SkGraphics::SetFontCacheLimit(font_cache_limit * kMB);
//         }
//     }
//
//     size_t resource_cache_limit;
//     if (cmd.HasSwitch(switches::kSkiaResourceCacheLimitMb)) {
//         if (base::StringToSizeT(
//             cmd.GetSwitchValueASCII(switches::kSkiaResourceCacheLimitMb),
//             &resource_cache_limit)) {
//             SkGraphics::SetResourceCacheTotalByteLimit(resource_cache_limit * kMB);
//         }
//     }
#endif

    //     if (base::FeatureList::IsEnabled(kSmallerFontCache)) {
    //         // Could also reduce the maximum number of cached strikes, but the intent
    //         // being to reduce memory usage, only control cache memory usage.
    //         SkGraphics::SetFontCacheLimit(kMB);
    //     }

    //InitSkiaEventTracer();
    //base::trace_event::MemoryDumpManager::GetInstance()->RegisterDumpProvider(skia::SkiaMemoryDumpProvider::GetInstance(), "Skia", nullptr);

    SkGraphics::SetResourceCacheSingleAllocationByteLimit(kImageCacheSingleAllocationByteLimit);
}

const uint8_t* find_string(const uint8_t* str, int str_len, const uint8_t* target, int target_len)
{
    for (int i = 0; i <= str_len - target_len; i++) {
        int j;
        for (j = 0; j < target_len; j++) {
            if (str[i + j] != target[j]) {
                break;
            }
        }
        if (j == target_len) {
            return &str[i];
        }
    }
    return NULL;
}

void initV8Data()
{
    static bool s_init = false;
    if (s_init)
        return;
    s_init = true;
    initializeICUData();
    v8::StartupData snapshotBlob;

#if !defined(OS_WIN)
#if BUILDFLAG(IS_MAC)
    // The macOS GN target links the architecture-specific snapshot generated
    // by mksnapshot. The bundled non-Windows resource is Linux-only.
    return;
#endif
#if defined(__AARCH64EL__) || defined(_M_ARM64)
    snapshotBlob.data = (const char*)(&SnapshotBlobBinArm64Linux);
    snapshotBlob.raw_size = sizeof(SnapshotBlobBinArm64Linux);
#else
    snapshotBlob.data = (const char*)(&SnapshotBlobBinX64Linux);
    snapshotBlob.raw_size = sizeof(SnapshotBlobBinX64Linux);
#endif
#else
#if defined(__LP64__) || __SIZEOF_LONG__ == 8 || defined(_WIN64)
    snapshotBlob.data = (const char*)(&SnapshotBlobBinX64);
    snapshotBlob.raw_size = sizeof(SnapshotBlobBinX64);
#else
    snapshotBlob.data = (const char*)(&SnapshotBlobBinX86);
    snapshotBlob.raw_size = sizeof(SnapshotBlobBinX86);
#endif
#endif // OS_LINUX

    //     snapshotBlob.data = (const char*)(&SnapshotBlobBinArm64Linux); // ����
    //     snapshotBlob.raw_size = sizeof(SnapshotBlobBinArm64Linux);
    //
    //std::vector<char>* buffer = new std::vector<char>();
    //readFileToBuf("W:\\mycode\\C132\\out\\r32\\snapshot_blob.bin", buffer);
    //readFileToBuf("W:\\mycode\\C132\\out\\r32\\v8_context_snapshot.bin", buffer);
    //snapshotBlob.data = (const char*)buffer->data();
    //snapshotBlob.raw_size = buffer->size();

    v8::V8::SetSnapshotDataBlob(&snapshotBlob);

    char output[120] = { 0 };
    sprintf(output, "embedded_blob: %p, %p, %d\n", v8_Default_embedded_blob_data_, v8_Default_embedded_blob_code_, snapshotBlob.raw_size);
    OutputDebugStringA(output);

    const uint8_t target[] = { 0x0F, 0xB6, 0x0C, 0x17, 0x8B, 0x0C, 0x8E };
    find_string(v8_Default_embedded_blob_data_, v8_Default_embedded_blob_data_size_, target, sizeof(target));
    find_string(v8_Default_embedded_blob_code_, v8_Default_embedded_blob_code_size_, target, sizeof(target));
    find_string((const uint8_t*)snapshotBlob.data, snapshotBlob.raw_size, target, sizeof(target));

//     v8_Default_embedded_blob_data_ = (const uint8_t*)genV8EmbeddedData((const void*)v8_Default_embedded_blob_data_stub, v8_Default_embedded_blob_data_size_);
//     v8_Default_embedded_blob_code_ = (const uint8_t*)genV8EmbeddedData((const void*)v8_Default_embedded_blob_code_stub, v8_Default_embedded_blob_code_size_);


    //     std::vector<char>* buffer = new std::vector<char>();
    //     readFileToBuf("W:\\mycode\\mb108\\content\\resources\\EmbeddedBlobDataX64Win.bin", buffer);
    //     v8_Default_embedded_blob_data_ = (const uint8_t*)genV8EmbeddedData(buffer->data(), buffer->size());
    //     v8_Default_embedded_blob_data_size_ = buffer->size();
    //
    //     char* output = (char*)malloc(400);
    //     sprintf_s(output, 399, "EmbeddedBlobDataX64Win hash: %d, %d\n",
    //         v8::internal::RSHash(buffer->data(), buffer->size()),
    //         v8::internal::RSHash((const char*)v8_Default_embedded_blob_data_, buffer->size())
    //         );
    //     OutputDebugStringA(output);
    //     free(output);
    //
    //     buffer = new std::vector<char>();
    //     readFileToBuf("W:\\mycode\\mb108\\content\\resources\\EmbeddedBlobCodeX64Win.bin", buffer);
    //     v8_Default_embedded_blob_code_ = (const uint8_t*)genV8EmbeddedData(buffer->data(), buffer->size());
    //     v8_Default_embedded_blob_code_size_ = buffer->size();
}

void RenderThreadImpl::initializeWebKitOnThread(/*mojo::BinderMap* binders*/)
{
    ThreadCall::initializeWebKit();
    initializeSkia();

#if defined(OS_WIN)
    // blink::WebFontRendering::SetUseDirectWrite(base::win::GetVersion() >= base::win::Version::WIN7);
    blink::FontCache::SetFontManager(skia::DefaultFontMgr());
#elif BUILDFLAG(IS_MAC)
    blink::FontCache::SetFontManager(SkFontMgr_New_CoreText(nullptr));
#endif

    initV8Data();
    base::ThreadPoolInstance::CreateAndStartWithDefaultParams("WebKitThread");
    blink::Platform::InitializeBlink();

    TestDiscardableMemoryAllocator* discardable_memory_allocator = new TestDiscardableMemoryAllocator(); // TODO: �ڴ�й¶
    base::DiscardableMemoryAllocator::SetInstance(discardable_memory_allocator);

    m_mainThreadScheduler = blink::scheduler::WebThreadScheduler::CreateMainThreadScheduler(createMainThreadMessagePump());

    mojo::BinderMap binders;
    RendererBlinkPlatformImpl* blinkPlatformImpl = new RendererBlinkPlatformImpl();
    blink::Initialize(blinkPlatformImpl, &binders, m_mainThreadScheduler.get());

#if defined(OS_WIN)
    SkTypeface_SetEnsureLOGFONTAccessibleProc(SkiaPreCacheFont);
#elif !BUILDFLAG(IS_MAC)
    std::string fonts;
    fonts = "/usr/share/fonts";

    sk_sp<SkFontMgr> fontMgr(SkFontMgr_New_Custom_Directory(fonts.c_str())); // FontCacheSkia.cpp, FontCache::getLastResortFallbackFont
#endif
    std::unique_ptr<blink::scheduler::WebAgentGroupScheduler> agentGroupScheduler
        = blink::scheduler::WebThreadScheduler::MainThreadScheduler().CreateWebAgentGroupScheduler();
    m_agentGroupScheduler = agentGroupScheduler.release();

    m_blinkPlatformImpl = blinkPlatformImpl;

    initializeCompositorThread();

    OverrideEmptyAssociatedInterfaceProvider();
    MojoInitialize(nullptr);

    blink::WebRuntimeFeaturesBase::EnableSharedArrayBuffer(true);
    blink::WebRuntimeFeaturesBase::EnableSharedArrayBufferOnDesktop(true);
    blink::WebRuntimeFeaturesBase::EnableAdTagging(true);

    blink::GetNetworkStateNotifier().SetOnLine(true);
    blink::GetNetworkStateNotifier().SetWebConnection(blink::kWebConnectionTypeWifi, 0.0 /*max_bandwidth_mbps*/);

    url::EnableNonStandardSchemesForAndroidWebView();
}

void RenderThreadImpl::initializeCompositorThread()
{
    m_blinkPlatformImpl->CreateAndSetCompositorThread();
    m_compositorTaskRunner = m_blinkPlatformImpl->CompositorThreadTaskRunner();
    //m_compositorTaskRunner->PostTask(FROM_HERE, base::BindOnce(&base::DisallowBlocking));
}

void RenderThreadImpl::initializeDemo()
{
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
    m_service = std::make_unique<VizService>(std::move(frameSinkManagerReceiver), std::move(frameSinkManagerClient));

    //     m_host = std::make_unique<demo::DemoHost>(
    //         m_widget,
    //         gfx::Size(g_wndSize.cx, g_wndSize.cy),
    //         std::move(frameSinkManagerClientReceiver),
    //         std::move(frameSinkManager),
    //         m_service->GetCompositorThreadRunner());
    DebugBreak();
}

bool RenderThreadImpl::isCurrentThread() const
{
    return m_thread.GetThreadId() == base::PlatformThread::CurrentId();
}

base::PlatformThreadId RenderThreadImpl::GetBlinkThreadId() const
{
    return m_thread.GetThreadId();
}

// static void loadUrl(const char* urlStr, blink::WebNavigationControl* navigationControl)
// {
//     blink::KURL url(WTF::String::FromUTF8(urlStr));
// 
//     blink::WebNavigationInfo info;
//     info.url_request.SetUrl(url);
//     if (!navigationControl->WillStartNavigation(info))
//         DebugBreak();
// 
//     blink::WebURLRequest urlRequest;
//     urlRequest.SetUrl(url);
//     navigationControl->Load(urlRequest, blink::WebFrameLoadType::kStandard, blink::WebHistoryItem());
// }

void RenderThreadImpl::createTestView()
{
    DebugBreak();
    //     blink::WebView* webWiew = blink::WebView::Create(new WebViewClientImpl(), false, false, false, absl::nullopt,
    //         /*compositing_enabled=*/true, false, nullptr, blink::CrossVariantMojoAssociatedReceiver<blink::mojom::PageBroadcastInterfaceBase>(),
    //         *(m_agentGroupScheduler), "session_storage_namespace_id", absl::optional<SkColor>(0xffffffff));
    //
    //     mojo::PendingAssociatedRemote<blink::mojom::blink::PolicyContainerHost> policyContainerRemote;
    //     mojo::PendingAssociatedReceiver<blink::mojom::blink::PolicyContainerHost> policyContainerReceiver
    //         = policyContainerRemote.InitWithNewEndpointAndPassReceiver();
    //
    //     mojo::AssociatedReceiver<blink::mojom::blink::PolicyContainerHost>* blinkPolicyContainerHostReceiver
    //         = new mojo::AssociatedReceiver<blink::mojom::blink::PolicyContainerHost>(new PolicyContainerHostImpl());
    //     blinkPolicyContainerHostReceiver->Bind(std::move(policyContainerReceiver)); // TODO: �ڴ�й¶
    //
    //     std::unique_ptr<blink::WebPolicyContainer> policyContainer
    //         = std::make_unique<blink::WebPolicyContainer>(blink::WebPolicyContainerPolicies(), blink::ToCrossVariantMojoType(std::move(policyContainerRemote)));
    //
    //     WebLocalFrameClientImpl* frameClient = new WebLocalFrameClientImpl();
    //     blink::WebLocalFrame* webFrame = blink::WebLocalFrame::CreateMainFrame(webWiew, frameClient, new BlinkInterfaceRegistryImpl(), blink::LocalFrameToken(),
    //         blink::DocumentToken(), std::move(policyContainer), nullptr, blink::WebString::FromUTF8("hahaha"), network::mojom::WebSandboxFlags::kNone);
    //
    //     m_renderWidgetHostImpl = new RenderWidgetHostImpl(base::ThreadTaskRunnerHandle::Get());
    // //     if (m_testHost.get())
    // //         m_renderWidgetHostImpl->setHostFrameSinkManager(m_testHost->host_frame_sink_manager());
    // //     else
    //     m_renderWidgetHostImpl->setHostFrameSinkManager(m_host->host_frame_sink_manager());
    //
    //     demo::g_demo_client->set_widget_host(m_renderWidgetHostImpl);
    //
    //     CreateFrameWidgetParams* createFrameWidgetParams = m_renderWidgetHostImpl->bindAndGenerateCreateFrameWidgetParams();
    //     m_renderWidgetHostImpl->m_mainFrame = webFrame;
    //     m_renderWidgetHostImpl->m_webWiew = webWiew;
    //
    //     blink::WebSettings* websettings = webWiew->GetSettings();
    //     websettings->SetDefaultFontSize(13);
    //     websettings->SetDefaultFixedFontSize(13);
    //     websettings->SetJavaScriptEnabled(true);
    //
    //     blink::DocumentLoader::DisableCodeCacheForTesting();
    //
    //     //constexpr viz::FrameSinkId root_frame_sink_id(0xdead, 0xbeef);
    //     //m_renderWidgetHostImpl->setFrameSinkId(root_frame_sink_id);
    //
    //     blink::WebFrameWidget* webFrameWidget
    //         = webFrame->InitializeFrameWidget(
    //             blink::ToCrossVariantMojoType(std::move(createFrameWidgetParams->frameWidgetHost)),
    //             blink::ToCrossVariantMojoType(std::move(createFrameWidgetParams->frameWidget)),
    //             blink::ToCrossVariantMojoType(std::move(createFrameWidgetParams->widgetHost)),
    //             blink::ToCrossVariantMojoType(std::move(createFrameWidgetParams->widget)),
    //             viz::FrameSinkId(/*RenderThread::Get()->GetClientId()*/ 1, /*params->widget_params->routing_id*/ 2),
    //             /*is_for_nested_main_frame*/ false, /*is_for_scalable_page*/ true,
    //             /*hidden=*/true);
    //     webFrameWidget->InitializeCompositing(
    //         *m_renderWidgetHostImpl->m_agentGroupScheduler, createFrameWidgetParams->visualProperties.screen_infos, /*settings=*/nullptr);
    //
    //     webWiew->DidAttachLocalMainFrame();
    //
    //     const char* urlStr =
    //         //"file:///G:/test/web_test/monaco-editor/m3u8-downloader-master/index.html";
    //         //"file:///G:/mycode/mb108/content/demo/res/jiqimao2.htm";
    //         //"file:///G:/mycode/mb108/content/demo/test.htm";
    //         //"http://windbg.org/";
    //         //"https://mycaht.top/";
    //         //"http://192.168.0.102:8091/index.html";
    //         "https://foundation.mozilla.org/en/";
    //         //"https://vuejs.org/";
    //         //"https://bxm0927.github.io/canvas-special/zhihu/index.html";
    //         //"file:///M:/doc/fingerprint/offcanvase.html";
    //         //"file:///W:/test/web_test/message_test/1.htm";
    //
    //     loadUrl(urlStr, frameClient->m_navigationControl);
}

void RenderThreadImpl::loadTestUrl(const char* urlStr, blink::WebNavigationControl* navigationControl)
{
    DebugBreak();

}

void RenderThreadImpl::garbageCollectionDelay(size_t millisecond)
{
    content::ThreadCall::callBlinkThreadDelayed(
        FROM_HERE, []() { content::RenderThreadImpl::get()->onRendererBackgrounded(); }, millisecond);
}

void RenderThreadImpl::onRendererBackgrounded()
{
    v8::Isolate::GetCurrent()->LowMemoryNotification();
    m_mainThreadScheduler->SetRendererBackgrounded(true);
    //discardable_memory_allocator_->OnBackgrounded();
    //internal::PartitionAllocSupport::Get()->OnBackgrounded();

    //blink::MainThreadIsolate()->MemoryPressureNotification(v8::MemoryPressureLevel::kCritical);
    //blink::MemoryPressureNotificationToWorkerThreadIsolates(v8::MemoryPressureLevel::kCritical);
}

void RenderThreadImpl::onRendererForegrounded()
{
}

}

namespace blink {

void TraceEventNamePrint(const char* name)
{
    //     std::string output("TraceEventNamePrint:");
    //     output += name;
    //     output += "\n";
    //     OutputDebugStringA(output.c_str());
}

}