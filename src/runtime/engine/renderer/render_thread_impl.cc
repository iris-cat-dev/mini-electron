
#include "src/runtime/engine/renderer/render_thread_impl.h"

#include "src/runtime/engine/renderer/renderer_blink_platform_impl.h"
#include "src/runtime/engine/renderer/web_local_frame_client_impl.h"
#include "src/runtime/engine/renderer/blink_interface_registry_impl.h"
#include "src/runtime/engine/renderer/policy_container_host_impl.h"
#include "src/runtime/engine/renderer/web_view_client_impl.h"
#include "src/runtime/engine/renderer/content_security_notifier_impl.h"
#include "src/runtime/engine/browser/local_frame_host_impl.h"
#include "src/runtime/engine/browser/local_main_frame_host_impl.h"
#include "src/runtime/engine/browser/back_forward_cache_controller_host_impl.h"
#include "src/runtime/engine/browser/render_widget_host_impl.h"
#include "src/runtime/engine/common/thread_call.h"
#include "gin/public/isolate_holder.h"
#include "third_party/libnode/src/node.h"
#include "src/runtime/engine/common/create_and_bind_templ.h"
#include "src/runtime/electron/common/renderer_crash_diagnostics.h"
#include "src/runtime/engine/viz/viz_service.h"
#include "src/runtime/engine/viz/viz_host.h"
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
#include "third_party/blink/public/platform/web_memory_pressure_listener.h"
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
#include "third_party/blink/renderer/platform/bindings/v8_per_isolate_data.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread_scheduler.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread.h"
#include "third_party/blink/public/platform/weak_wrapper_resource_load_info_notifier.h"
#include "services/network/public/cpp/resource_request.h"
#include "src/runtime/network/loader/web_url_loader_impl_curl.h"
#include "ui/gfx/native_widget_types.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "base/check.h"
#include "base/memory/memory_pressure_listener.h"
#include "base/synchronization/lock.h"
#include "base/memory/discardable_memory_allocator.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/power_monitor/power_monitor.h"
#include "base/power_monitor/power_monitor_device_source.h"
#include "base/timer/timer.h"
#include "base/trace_event/process_memory_dump.h"
#include "third_party/icu/source/common/unicode/uclean.h"
#include "third_party/icu/source/common/unicode/udata.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/ports/SkFontMgr_directory.h"
#include "third_party/skia/include/core/SkGraphics.h"
#include "third_party/blink/renderer/platform/fonts/font_cache.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#if !defined(OS_WIN)
#include <sys/mman.h>
#else
#include <windows.h>
#include <psapi.h>

#include "third_party/blink/public/web/win/web_font_rendering.h"
#include "skia/ext/font_utils.h"
#include "base/win/windows_version.h"
#include "base/win/scoped_handle.h"
#include "base/allocator/partition_alloc_support.h"
#include "base/threading/sequence_local_storage_slot.h"
#include <Shlwapi.h>
#endif
#include "url/url_util.h"
#include <map>
#include <new>
#include <utility>

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
namespace {

node::MultiIsolatePlatform* g_externalV8Platform = nullptr;

class NodeIsolatePlatformDelegate final
    : public node::IsolatePlatformDelegate {
public:
    explicit NodeIsolatePlatformDelegate(
        std::shared_ptr<v8::TaskRunner> taskRunner)
        : taskRunner_(std::move(taskRunner))
    {
    }

    std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner() override
    {
        return taskRunner_;
    }

    bool IdleTasksEnabled() override
    {
        return taskRunner_->IdleTasksEnabled();
    }

private:
    std::shared_ptr<v8::TaskRunner> taskRunner_;
};

base::Lock& IsolateDelegatesLock()
{
    static auto* lock = new base::Lock();
    return *lock;
}

std::map<v8::Isolate*, std::unique_ptr<NodeIsolatePlatformDelegate>>&
IsolateDelegates()
{
    static auto* delegates =
        new std::map<v8::Isolate*,
            std::unique_ptr<NodeIsolatePlatformDelegate>>();
    return *delegates;
}

void RegisterExternalIsolate(
    v8::Isolate* isolate, std::shared_ptr<v8::TaskRunner> taskRunner)
{
    auto delegate =
        std::make_unique<NodeIsolatePlatformDelegate>(std::move(taskRunner));
    NodeIsolatePlatformDelegate* delegatePtr = delegate.get();
    {
        base::AutoLock lock(IsolateDelegatesLock());
        CHECK(IsolateDelegates().emplace(isolate, std::move(delegate)).second);
    }
    g_externalV8Platform->RegisterIsolate(isolate, delegatePtr);
}

void UnregisterExternalIsolate(v8::Isolate* isolate)
{
    g_externalV8Platform->UnregisterIsolate(isolate);
    base::AutoLock lock(IsolateDelegatesLock());
    CHECK_EQ(IsolateDelegates().erase(isolate), 1u);
}

} // namespace



RenderThreadImpl* RenderThreadImpl::get()
{
    if (s_inst)
        return s_inst;
    s_inst = new RenderThreadImpl();
    return s_inst;
}
void RenderThreadImpl::setV8Platform(
    node::MultiIsolatePlatform* platform)
{
    CHECK(!s_inst);
    CHECK(platform);
    CHECK(!g_externalV8Platform);
    g_externalV8Platform = platform;
    gin::IsolateHolder::UseExternalV8Platform(
        platform, RegisterExternalIsolate, UnregisterExternalIsolate);
}

void RenderThreadImpl::shutdown()
{
    if (!s_inst || s_inst->m_shutdown)
        return;

    RenderThreadImpl* self = s_inst;
    CHECK(!self->isCurrentThread());
    self->m_shutdown = true;

    // Every WebView must already be gone. Stop the auxiliary host first, then
    // stop and join the compositor on Blink's thread.
    self->m_hostThread.Stop();
    ThreadCall::callBlinkThreadSync(FROM_HERE, [self] {
        self->m_compositorTaskRunner = nullptr;
        blink::Thread::ShutdownCompositorThread();
    });


    self->m_thread.Stop();
    self->m_blinkTaskRunner = nullptr;
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

namespace {

constexpr int kModerateEnterPercent = 70;
constexpr int kModerateExitPercent = 60;
constexpr int kCriticalEnterPercent = 85;
constexpr int kCriticalExitPercent = 75;
constexpr base::TimeDelta kModerateNotificationCooldown = base::Seconds(30);
constexpr base::TimeDelta kCriticalNotificationCooldown = base::Minutes(1);

uint64_t PercentageOf(uint64_t value, int percent)
{
    DCHECK_GE(percent, 0);
    DCHECK_LE(percent, 100);
    return (value / 100) * percent + ((value % 100) * percent) / 100;
}

RendererMemoryPressurePolicy::Level PressureForUsage(
    RendererMemoryPressurePolicy::Level previous_level,
    uint64_t used_bytes, uint64_t limit_bytes)
{
    using Level = RendererMemoryPressurePolicy::Level;
    if (!limit_bytes)
        return Level::kNone;
    if (used_bytes >= PercentageOf(limit_bytes, kCriticalEnterPercent))
        return Level::kCritical;
    if (previous_level == Level::kCritical
        && used_bytes >= PercentageOf(limit_bytes, kCriticalExitPercent))
        return Level::kCritical;
    if (used_bytes >= PercentageOf(limit_bytes, kModerateEnterPercent))
        return Level::kModerate;
    if (previous_level != Level::kNone
        && used_bytes >= PercentageOf(limit_bytes, kModerateExitPercent))
        return Level::kModerate;
    return Level::kNone;
}

} // namespace

RendererMemoryPressurePolicy::Action RendererMemoryPressurePolicy::Evaluate(
    uint64_t committed_bytes, uint64_t job_limit_bytes,
    uint64_t used_heap_bytes, uint64_t heap_limit_bytes,
    bool system_low_memory, base::TimeTicks now)
{
    const Level previous_level = m_level;
    m_jobLevel = PressureForUsage(m_jobLevel, committed_bytes, job_limit_bytes);
    m_heapLevel = PressureForUsage(m_heapLevel, used_heap_bytes, heap_limit_bytes);
    m_level = system_low_memory ? Level::kCritical
        : std::max(m_jobLevel, m_heapLevel);
    if (m_level != Level::kModerate)
        m_moderateNotificationPending = false;
    else if (previous_level == Level::kNone)
        m_moderateNotificationPending = true;


    if (m_level == Level::kCritical
        && (m_lastCriticalNotification.is_null()
            || now - m_lastCriticalNotification
                >= kCriticalNotificationCooldown)) {
        m_lastCriticalNotification = now;
        return Action::kCritical;
    }
    if (m_level == Level::kModerate && m_moderateNotificationPending
        && (m_lastModerateNotification.is_null()
            || now - m_lastModerateNotification
                >= kModerateNotificationCooldown)) {
        m_lastModerateNotification = now;
        m_moderateNotificationPending = false;
        return Action::kModerate;
    }
    return Action::kNone;
}

class RendererDiscardableMemoryAllocator::Allocation final
    : public base::DiscardableMemory {
public:
    ~Allocation() override
    {
        m_allocator->DestroyAllocation(this);
    }

    bool Lock() override
    {
        base::AutoLock lock(m_allocator->m_lock);
        DCHECK(!m_isLocked);
        if (m_isLocked || !m_data)
            return false;
        m_isLocked = true;
        return true;
    }

    void Unlock() override
    {
        base::AutoLock lock(m_allocator->m_lock);
        DCHECK(m_isLocked);
        if (!m_isLocked)
            return;
        m_isLocked = false;
    }

    void* data() const override
    {
        base::AutoLock lock(m_allocator->m_lock);
        DCHECK(m_isLocked);
        return m_isLocked ? m_data.get() : nullptr;
    }

    void DiscardForTesting() override
    {
        base::AutoLock lock(m_allocator->m_lock);
        DCHECK(!m_isLocked);
        m_allocator->DiscardAllocationLocked(this);
    }

    base::trace_event::MemoryAllocatorDump* CreateMemoryAllocatorDump(
        const char* name, base::trace_event::ProcessMemoryDump* pmd) const override
    {
        base::AutoLock lock(m_allocator->m_lock);
        auto* dump = pmd->CreateAllocatorDump(name);
        dump->AddScalar(base::trace_event::MemoryAllocatorDump::kNameSize,
            base::trace_event::MemoryAllocatorDump::kUnitsBytes,
            m_data ? m_size : 0);
        return dump;
    }

private:
    friend class RendererDiscardableMemoryAllocator;

    Allocation(RendererDiscardableMemoryAllocator* allocator, size_t size,
        std::unique_ptr<uint8_t[]> data)
        : m_allocator(allocator)
        , m_size(size)
        , m_data(std::move(data))
    {
    }

    RendererDiscardableMemoryAllocator* const m_allocator;
    const size_t m_size;
    std::unique_ptr<uint8_t[]> m_data;
    bool m_isLocked = true;
    Allocation* m_previous = nullptr;
    Allocation* m_next = nullptr;
};

RendererDiscardableMemoryAllocator::RendererDiscardableMemoryAllocator(
    size_t max_bytes)
    : m_maxBytes(max_bytes)
{
}

RendererDiscardableMemoryAllocator::~RendererDiscardableMemoryAllocator()
{
    base::AutoLock lock(m_lock);
    DCHECK(!m_allocations);
    DCHECK_EQ(m_bytesAllocated, 0u);
}

std::unique_ptr<base::DiscardableMemory>
RendererDiscardableMemoryAllocator::AllocateLockedDiscardableMemory(size_t size)
{
    base::AutoLock lock(m_lock);
    if (size > m_maxBytes)
        return nullptr;

    if (m_bytesAllocated > m_maxBytes - size)
        DiscardUnlockedAllocationsLocked();
    if (m_bytesAllocated > m_maxBytes - size)
        return nullptr;

    std::unique_ptr<uint8_t[]> data(new (std::nothrow) uint8_t[size]);
    if (!data)
        return nullptr;
    std::unique_ptr<Allocation> allocation(new (std::nothrow)
            Allocation(this, size, std::move(data)));
    if (!allocation)
        return nullptr;

    allocation->m_next = m_allocations;
    if (m_allocations)
        m_allocations->m_previous = allocation.get();
    m_allocations = allocation.get();
    m_bytesAllocated += size;
    return allocation;
}

size_t RendererDiscardableMemoryAllocator::GetBytesAllocated() const
{
    base::AutoLock lock(m_lock);
    return m_bytesAllocated;
}

void RendererDiscardableMemoryAllocator::ReleaseFreeMemory()
{
    base::AutoLock lock(m_lock);
    DiscardUnlockedAllocationsLocked();
}

void RendererDiscardableMemoryAllocator::DiscardUnlockedAllocationsLocked()
{
    for (Allocation* allocation = m_allocations; allocation;
         allocation = allocation->m_next) {
        if (!allocation->m_isLocked)
            DiscardAllocationLocked(allocation);
    }
}

void RendererDiscardableMemoryAllocator::DiscardAllocationLocked(
    Allocation* allocation)
{
    if (allocation->m_isLocked || !allocation->m_data)
        return;
    DCHECK_GE(m_bytesAllocated, allocation->m_size);
    m_bytesAllocated -= allocation->m_size;
    allocation->m_data.reset();
}

void RendererDiscardableMemoryAllocator::DestroyAllocation(
    Allocation* allocation)
{
    base::AutoLock lock(m_lock);
    if (allocation->m_previous)
        allocation->m_previous->m_next = allocation->m_next;
    else
        m_allocations = allocation->m_next;
    if (allocation->m_next)
        allocation->m_next->m_previous = allocation->m_previous;
    if (allocation->m_data) {
        DCHECK_GE(m_bytesAllocated, allocation->m_size);
        m_bytesAllocated -= allocation->m_size;
        allocation->m_data.reset();
    }
}

#if BUILDFLAG(IS_WIN)

namespace {

constexpr base::TimeDelta kMemoryPressurePollInterval = base::Seconds(1);

bool ReadRendererJobMemory(uint64_t* committed_bytes, uint64_t* limit_bytes)
{
    BOOL in_job = FALSE;
    if (!::IsProcessInJob(::GetCurrentProcess(), nullptr, &in_job) || !in_job)
        return false;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    if (!::QueryInformationJobObject(nullptr,
            JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr)) {
        return false;
    }
    SIZE_T enforced_limit = 0;
    if ((limits.BasicLimitInformation.LimitFlags
            & JOB_OBJECT_LIMIT_PROCESS_MEMORY)
        && limits.ProcessMemoryLimit) {
        enforced_limit = limits.ProcessMemoryLimit;
    }
    if ((limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY)
        && limits.JobMemoryLimit
        && (!enforced_limit || limits.JobMemoryLimit < enforced_limit)) {
        enforced_limit = limits.JobMemoryLimit;
    }
    if (!enforced_limit)
        return false;

    PROCESS_MEMORY_COUNTERS_EX counters {};
    counters.cb = sizeof(counters);
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        return false;
    }

    *committed_bytes = static_cast<uint64_t>(counters.PrivateUsage);
    *limit_bytes = static_cast<uint64_t>(enforced_limit);
    return true;
}

} // namespace

class RendererMemoryPressureMonitor {
public:
    explicit RendererMemoryPressureMonitor(v8::Isolate* isolate)
        : m_isolate(isolate)
    {
        HANDLE notification =
            ::CreateMemoryResourceNotification(LowMemoryResourceNotification);
        if (notification)
            m_lowMemoryNotification.Set(notification);
    }

    ~RendererMemoryPressureMonitor()
    {
        m_timer.Stop();
    }

    void Start()
    {
        m_timer.Start(FROM_HERE, kMemoryPressurePollInterval, this,
            &RendererMemoryPressureMonitor::Poll);
    }

private:
    bool IsSystemLowOnMemory() const
    {
        if (!m_lowMemoryNotification.is_valid())
            return false;
        BOOL low_memory = FALSE;
        return ::QueryMemoryResourceNotification(
                   m_lowMemoryNotification.get(), &low_memory)
            && low_memory;
    }

    void Poll()
    {
        DCHECK_EQ(v8::Isolate::GetCurrent(), m_isolate);
        v8::HeapStatistics heap;
        m_isolate->GetHeapStatistics(&heap);

        uint64_t committed_bytes = 0;
        uint64_t limit_bytes = 0;
        ReadRendererJobMemory(&committed_bytes, &limit_bytes);
        const RendererMemoryPressurePolicy::Level previous_level =
            m_policy.level();
        const RendererMemoryPressurePolicy::Action action = m_policy.Evaluate(
            committed_bytes, limit_bytes, heap.used_heap_size(),
            heap.heap_size_limit(), IsSystemLowOnMemory(), base::TimeTicks::Now());
        const RendererMemoryPressurePolicy::Level current_level =
            m_policy.level();
        if (action == RendererMemoryPressurePolicy::Action::kNone) {
            if (previous_level
                    == RendererMemoryPressurePolicy::Level::kCritical
                && current_level
                    == RendererMemoryPressurePolicy::Level::kModerate) {
                blink::MemoryPressureNotificationToAllIsolates(
                    v8::MemoryPressureLevel::kModerate);
            } else if (previous_level
                    != RendererMemoryPressurePolicy::Level::kNone
                && current_level
                    == RendererMemoryPressurePolicy::Level::kNone) {
                blink::MemoryPressureNotificationToAllIsolates(
                    v8::MemoryPressureLevel::kNone);
            }
            atom::RecordRendererV8Memory(m_isolate);
            return;
        }

        const bool critical =
            action == RendererMemoryPressurePolicy::Action::kCritical;
        const auto base_level = critical
            ? base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_CRITICAL
            : base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_MODERATE;
        const auto v8_level = critical ? v8::MemoryPressureLevel::kCritical
                                       : v8::MemoryPressureLevel::kModerate;
        // V8 coalesces identical pressure levels. Reset only when a sustained
        // critical state reaches our cooldown, so the retry can request work
        // without forcing a collection on every monitor tick.
        if (critical
            && previous_level
                == RendererMemoryPressurePolicy::Level::kCritical) {
            blink::MemoryPressureNotificationToAllIsolates(
                v8::MemoryPressureLevel::kNone);
        }

        base::MemoryPressureListener::NotifyMemoryPressure(base_level);
        blink::WebMemoryPressureListener::OnMemoryPressure(base_level);
        if (critical)
            blink::WebMemoryPressureListener::OnPurgeMemory();
        base::DiscardableMemoryAllocator::GetInstance()->ReleaseFreeMemory();
        SkGraphics::PurgeAllCaches();
        blink::MemoryPressureNotificationToAllIsolates(v8_level);
        atom::RecordRendererV8Memory(m_isolate);
    }

    v8::Isolate* const m_isolate;
    base::win::ScopedHandle m_lowMemoryNotification;
    RendererMemoryPressurePolicy m_policy;
    base::RepeatingTimer m_timer;
};

#endif // BUILDFLAG(IS_WIN)

RenderThreadImpl::ThreadDelegate::~ThreadDelegate()
{
    // ThreadMain has unwound its RunLoop before releasing the delegate. The
    // scheduler owns that loop's SequenceManager and cannot be destroyed from
    // one of its own tasks.
    CHECK_EQ(v8::Isolate::GetCurrent(), m_parent->m_blinkIsolate);
#if BUILDFLAG(IS_WIN)
    m_parent->m_memoryPressureMonitor.reset();
#endif
    delete m_parent->m_agentGroupScheduler;
    m_parent->m_agentGroupScheduler = nullptr;
    blink::ThreadScheduler::Current()->SetV8Isolate(nullptr);
    blink::V8PerIsolateData::WillBeDestroyed(m_parent->m_blinkIsolate);
    blink::V8PerIsolateData::Destroy(m_parent->m_blinkIsolate);
    m_parent->m_blinkIsolate = nullptr;
    m_parent->m_mainThreadScheduler->Shutdown();
}

RenderThreadImpl::RenderThreadImpl()
    : m_thread("RenderThreadImpl")
    , m_hostThread("HostThread")
{
    m_usesHostUiSequence =
        base::SingleThreadTaskRunner::HasCurrentDefault();
    if (!m_usesHostUiSequence) {
#if BUILDFLAG(IS_MAC)
        // Standalone hosts need their own CFRunLoop-backed UI executor.
        m_uiThreadTask = std::make_unique<base::SingleThreadTaskExecutor>(
            base::MessagePumpType::NS_RUNLOOP);
#else
        m_uiThreadTask = std::make_unique<base::SingleThreadTaskExecutor>(
            base::MessagePumpType::UI);
#endif
        base::PlatformThread::SetName("Mb132UiThread");
    }

    if (!base::ThreadPoolInstance::Get())
        base::ThreadPoolInstance::CreateAndStartWithDefaultParams("MiniElectron");
    // Construct the OS power source on the owning UI loop before starting
    // Blink. PowerMonitor owns it for the lifetime of the process.
    if (!base::PowerMonitor::GetInstance()->IsInitialized())
        base::PowerMonitor::GetInstance()->Initialize(
            std::make_unique<base::PowerMonitorDeviceSource>());

    // The Electron host has already registered its UI runner. Standalone
    // renderer processes reach this idempotent initialization for the first
    // time.
    ThreadCall::init(nullptr);

    base::Thread::Options opt;
    opt.stack_size = 3000000;
    opt.delegate =
        std::make_unique<RenderThreadImpl::ThreadDelegate>(this);
    m_thread.StartWithOptions(std::move(opt));
    while (!m_inited)
        ::Sleep(1);

    m_hostThread.Start();
}

RenderThreadImpl::~RenderThreadImpl() = default;

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
        }
        // An unsupported interface is rejected by closing its receiver.
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
    m_blinkThreadId = base::PlatformThread::CurrentId();
    ThreadCall::initializeWebKit();
    initializeSkia();

#if defined(OS_WIN)
    // blink::WebFontRendering::SetUseDirectWrite(base::win::GetVersion() >= base::win::Version::WIN7);
    blink::FontCache::SetFontManager(skia::DefaultFontMgr());
#elif BUILDFLAG(IS_MAC)
    blink::FontCache::SetFontManager(SkFontMgr_New_CoreText(nullptr));
#endif

    initV8Data();
    blink::Platform::InitializeBlink();

    base::DiscardableMemoryAllocator::SetInstance(
        new RendererDiscardableMemoryAllocator(
            cc::kMemoryThresholdTSoftwareImageDecodeCache));

    m_mainThreadScheduler =
        blink::scheduler::WebThreadScheduler::CreateMainThreadScheduler(
            createMainThreadMessagePump());

    mojo::BinderMap binders;
    RendererBlinkPlatformImpl* blinkPlatformImpl =
        new RendererBlinkPlatformImpl();
    blink::Initialize(
        blinkPlatformImpl, &binders, m_mainThreadScheduler.get());

#if defined(OS_WIN)
    SkTypeface_SetEnsureLOGFONTAccessibleProc(SkiaPreCacheFont);
#elif !BUILDFLAG(IS_MAC)
    std::string fonts;
    fonts = "/usr/share/fonts";

    sk_sp<SkFontMgr> fontMgr(SkFontMgr_New_Custom_Directory(fonts.c_str()));
#endif
    std::unique_ptr<blink::scheduler::WebAgentGroupScheduler>
        agentGroupScheduler =
            blink::scheduler::WebThreadScheduler::MainThreadScheduler()
                .CreateWebAgentGroupScheduler();
    m_agentGroupScheduler = agentGroupScheduler.release();
    m_blinkTaskRunner = m_agentGroupScheduler->DefaultTaskRunner();
    m_blinkIsolate = m_agentGroupScheduler->Isolate();

    m_blinkPlatformImpl = blinkPlatformImpl;

    initializeCompositorThread();

    OverrideEmptyAssociatedInterfaceProvider();
    MojoInitialize(nullptr);

    blink::WebRuntimeFeaturesBase::EnableSharedArrayBuffer(true);
    blink::WebRuntimeFeaturesBase::EnableSharedArrayBufferOnDesktop(true);
    blink::WebRuntimeFeaturesBase::EnableAdTagging(true);

    blink::GetNetworkStateNotifier().SetOnLine(true);
    blink::GetNetworkStateNotifier().SetWebConnection(
        blink::kWebConnectionTypeWifi, 0.0 /*max_bandwidth_mbps*/);

    url::EnableNonStandardSchemesForAndroidWebView();

#if BUILDFLAG(IS_WIN)
    m_memoryPressureMonitor =
        std::make_unique<RendererMemoryPressureMonitor>(m_blinkIsolate);
    m_memoryPressureMonitor->Start();
#endif
}

void RenderThreadImpl::initializeCompositorThread()
{
    m_blinkPlatformImpl->CreateAndSetCompositorThread();
    m_compositorTaskRunner = m_blinkPlatformImpl->CompositorThreadTaskRunner();
    //m_compositorTaskRunner->PostTask(FROM_HERE, base::BindOnce(&base::DisallowBlocking));
}


bool RenderThreadImpl::isCurrentThread() const
{
    return m_blinkThreadId == base::PlatformThread::CurrentId();
}

base::PlatformThreadId RenderThreadImpl::GetBlinkThreadId() const
{
    return m_blinkThreadId;
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