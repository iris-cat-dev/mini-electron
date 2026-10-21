
#ifndef content_renderer_RenderThreadImpl_h
#define content_renderer_RenderThreadImpl_h

#include <atomic>

#include "base/threading/thread.h"
#include "third_party/blink/public/platform/scheduler/web_thread_scheduler.h"
#include "ui/gfx/native_widget_types.h"
#include "base/task/single_thread_task_executor.h"

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

        //         m_thread.task_runner()->PostTask(
        //             FROM_HERE, base::BindOnce(&RenderThreadImpl::createTestView, base::Unretained(this)));
    }

    void initializeWebKitOnThread(/*mojo::BinderMap* binders*/);
    void initializeCompositorThread();
    void initializeDemo();
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

    void createTestView();
    void loadTestUrl(const char* urlStr, blink::WebNavigationControl* navigationControl);

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
    base::Thread m_thread;
    base::Thread m_hostThread;

    float m_zoom = 1.0;

    std::unique_ptr<base::SingleThreadTaskExecutor> m_uiThreadTask;

    EmptyAssociatedInterfaceProvider* m_emptyAssociatedInterfaceProvider = nullptr;

    static RenderThreadImpl* s_inst;
};

}

#endif // content_renderer_RenderThreadImpl_h