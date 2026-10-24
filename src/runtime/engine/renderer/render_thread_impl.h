
#ifndef content_renderer_RenderThreadImpl_h
#define content_renderer_RenderThreadImpl_h

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "base/memory/discardable_memory_allocator.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_executor.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "third_party/blink/public/platform/scheduler/web_thread_scheduler.h"
#include "ui/gfx/native_widget_types.h"

namespace node {
class MultiIsolatePlatform;
}

namespace v8 {
class Isolate;
}

namespace blink {
class WebNavigationControl;
class AssociatedInterfaceProvider;
}

namespace content {

// Applies hysteresis and notification cooldowns to renderer memory samples.
// Kept separate from the OS sampler so its transition behavior is deterministic.
class RendererMemoryPressurePolicy {
public:
    enum class Level {
        kNone,
        kModerate,
        kCritical,
    };

    enum class Action {
        kNone,
        kModerate,
        kCritical,
    };

    Action Evaluate(uint64_t committed_bytes, uint64_t job_limit_bytes,
        bool system_low_memory, base::TimeTicks now);
    Level level() const { return m_level; }

private:
    Level m_level = Level::kNone;
    base::TimeTicks m_lastModerateNotification;
    base::TimeTicks m_lastCriticalNotification;
    bool m_moderateNotificationPending = false;
};

// Heap-backed discardable memory for renderer caches. Unlocked allocations can
// be reclaimed explicitly, while locked allocations remain valid. Accounting
// and memory dumps report retained payload bytes, excluding allocation metadata.
class RendererDiscardableMemoryAllocator final
    : public base::DiscardableMemoryAllocator {
public:
    explicit RendererDiscardableMemoryAllocator(size_t max_bytes);
    ~RendererDiscardableMemoryAllocator() override;

    std::unique_ptr<base::DiscardableMemory>
    AllocateLockedDiscardableMemory(size_t size) override;
    size_t GetBytesAllocated() const override;
    void ReleaseFreeMemory() override;

private:
    class Allocation;
    friend class Allocation;

    void DiscardUnlockedAllocationsLocked()
        EXCLUSIVE_LOCKS_REQUIRED(m_lock);
    void DiscardAllocationLocked(Allocation* allocation)
        EXCLUSIVE_LOCKS_REQUIRED(m_lock);
    void DestroyAllocation(Allocation* allocation);

    const size_t m_maxBytes;
    mutable base::Lock m_lock;
    Allocation* m_allocations GUARDED_BY(m_lock) = nullptr;
    size_t m_bytesAllocated GUARDED_BY(m_lock) = 0;
};

#if BUILDFLAG(IS_WIN)
class RendererMemoryPressureMonitor;
#endif

class TestHost;
class RendererBlinkPlatformImpl;
class RenderWidgetHostImpl;

class VizService;
class VizHost;

class EmptyAssociatedInterfaceProvider;

class RenderThreadImpl {
public:
    class ThreadDelegate : public base::Thread::Delegate {
    public:
        ThreadDelegate(RenderThreadImpl* parent);
        ~ThreadDelegate() override;
        scoped_refptr<base::SingleThreadTaskRunner> GetDefaultTaskRunner() override;
        void BindToCurrentThread() override;

        RenderThreadImpl* m_parent;
    };

    RenderThreadImpl();
    ~RenderThreadImpl();

    static RenderThreadImpl* get();
    // Selects an already initialized Node V8 platform for Blink isolates.
    // Must be called before get().
    static void setV8Platform(node::MultiIsolatePlatform* platform);
    // Tears down Blink's isolate before the process-wide V8 platform is
    // disposed. This is a one-way process shutdown operation and must run on
    // the engine owner sequence after every WebView has been destroyed.
    static void shutdown();

    void initializeWebKit(/*mojo::BinderMap* binders*/)
    {
        //         m_thread.task_runner()->PostTask(
        //             FROM_HERE, base::BindOnce(&RenderThreadImpl::initializeWebKitOnThread, base::Unretained(this)));

    }

    void initializeWebKitOnThread(/*mojo::BinderMap* binders*/);
    void initializeCompositorThread();
    void OverrideEmptyAssociatedInterfaceProvider();

    void onRendererBackgrounded();
    void onRendererForegrounded();
    void garbageCollectionDelay(size_t millisecond);

    bool isCurrentThread() const;
    base::PlatformThreadId GetBlinkThreadId() const;

    float getZoom() const
    {
        return m_zoom;
    }
    void setZoom(float f)
    {
        m_zoom = f;
    }

    scoped_refptr<base::SingleThreadTaskRunner> getTaskRunner() const
    {
        return m_blinkTaskRunner;
    }
    bool usesHostUiSequence() const { return m_usesHostUiSequence; }

    scoped_refptr<base::SingleThreadTaskRunner> getCompositorTaskRunner() const
    {
        return m_compositorTaskRunner;
    }

    void OverrideAssociatedInterfaceProvider(blink::AssociatedInterfaceProvider* associatedInterfaceProvider);


    RendererBlinkPlatformImpl* m_blinkPlatformImpl;
    std::unique_ptr<blink::scheduler::WebThreadScheduler> m_mainThreadScheduler;

    scoped_refptr<base::SingleThreadTaskRunner> m_compositorTaskRunner;

    gfx::AcceleratedWidget m_widget;
    std::unique_ptr<VizService> m_service;
    std::unique_ptr<VizHost> m_host;
    // std::unique_ptr<TestHost> m_testHost;

    RenderWidgetHostImpl* m_renderWidgetHostImpl = nullptr;
    blink::scheduler::WebAgentGroupScheduler* m_agentGroupScheduler = nullptr;

    std::atomic<bool> m_inited{false};
    bool m_usesHostUiSequence = false;
    bool m_shutdown = false;
    base::PlatformThreadId m_blinkThreadId = 0;
    scoped_refptr<base::SingleThreadTaskRunner> m_blinkTaskRunner;
    v8::Isolate* m_blinkIsolate = nullptr;
#if BUILDFLAG(IS_WIN)
    std::unique_ptr<RendererMemoryPressureMonitor> m_memoryPressureMonitor;
#endif
    base::Thread m_thread;
    base::Thread m_hostThread;

    float m_zoom = 1.0;

    std::unique_ptr<base::SingleThreadTaskExecutor> m_uiThreadTask;

    EmptyAssociatedInterfaceProvider* m_emptyAssociatedInterfaceProvider = nullptr;

    static RenderThreadImpl* s_inst;
};

}

#endif // content_renderer_RenderThreadImpl_h