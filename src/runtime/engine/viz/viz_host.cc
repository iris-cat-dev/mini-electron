// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/runtime/engine/viz/viz_host.h"

#include <utility>

#include "base/memory/ref_counted_delete_on_sequence.h"
#include "base/rand_util.h"
#include "base/task/bind_post_task.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "src/runtime/engine/viz/viz_client.h"
#include "src/runtime/engine/viz/offscreen_display_client.h"
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
#include "src/runtime/engine/browser/web_view_host.h"
#endif
#include "src/runtime/engine/common/thread_call.h"
#include "components/viz/host/renderer_settings_creation.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"

namespace content {

// mojo_lite resolves Remote calls to the receiver object directly. Keep the
// endpoint and every callable operation on the Viz service sequence; posted
// operations retain this state rather than the owning VizHost.
class VizHost::DisplayPrivateState
    : public base::RefCountedDeleteOnSequence<VizHost::DisplayPrivateState> {
public:
    explicit DisplayPrivateState(scoped_refptr<base::SingleThreadTaskRunner> service_runner)
        : base::RefCountedDeleteOnSequence<DisplayPrivateState>(service_runner)
        , m_serviceRunner(std::move(service_runner))
    {
    }

    mojo::PendingReceiver<viz::mojom::DisplayPrivate> BindNewEndpointAndPassReceiver()
    {
        return m_displayPrivate.BindNewEndpointAndPassReceiver(m_serviceRunner);
    }

    void Resize(const gfx::Size& size)
    {
        DCHECK(m_serviceRunner->RunsTasksInCurrentSequence());
        if (auto* display = m_displayPrivate.TryGet())
            display->Resize(size);
    }

    void SetDisplayVisible(bool visible)
    {
        DCHECK(m_serviceRunner->RunsTasksInCurrentSequence());
        if (auto* display = m_displayPrivate.TryGet())
            display->SetDisplayVisible(visible);
    }

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
    void SetVSyncDisplayID(int64_t display_id)
    {
        DCHECK(m_serviceRunner->RunsTasksInCurrentSequence());
        if (auto* display = m_displayPrivate.TryGet())
            display->SetVSyncDisplayID(display_id);
    }
#endif

private:
    friend class base::RefCountedDeleteOnSequence<DisplayPrivateState>;
    friend class base::DeleteHelper<DisplayPrivateState>;

    ~DisplayPrivateState() = default;

    const scoped_refptr<base::SingleThreadTaskRunner> m_serviceRunner;
    mojo::AssociatedRemote<viz::mojom::DisplayPrivate> m_displayPrivate;
};

VizHost::VizHost(WebViewHost* mbwebview, bool isTransparent,
    //gfx::AcceleratedWidget widget,
    const gfx::Size& size, mojo::PendingReceiver<viz::mojom::FrameSinkManagerClient> clientReceiver,
    mojo::PendingRemote<viz::mojom::FrameSinkManager> frameSinkManagerRemote, scoped_refptr<base::SingleThreadTaskRunner> workRunner,
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
    int64_t vsyncDisplayId,
#endif
    scoped_refptr<base::SingleThreadTaskRunner> serviceRunner)
    : m_widget(gfx::kNullAcceleratedWidget)
    , m_engineViewHost(mbwebview)
    , m_isTransparent(isTransparent)
    , m_size(size)
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
    , m_vsyncDisplayId(vsyncDisplayId)
#endif
    //, m_thread("VizHost")
    , m_workRunner(workRunner)
    , m_serviceRunner(serviceRunner)
{
    m_displayPrivateState = base::MakeRefCounted<DisplayPrivateState>(m_serviceRunner);
    //CHECK(m_thread.Start());

    m_hostFrameSinkManager.SetServiceRunner(serviceRunner);
    CHECK(m_workRunner->PostTask(
        FROM_HERE, base::BindOnce(&VizHost::initialize, base::Unretained(this), std::move(clientReceiver), std::move(frameSinkManagerRemote))));
    m_initialized.Wait();
}

VizHost::~VizHost()
{
    char* output = (char*)malloc(0x100);
    sprintf(output, "~VizHost: %p\n", this);
    OutputDebugStringA(output);
    free(output);
}

// ������������blink�߳�
void VizHost::destroy(std::function<void(void)>&& callback)
{
    if (!m_rootClient)
        return;

    const base::WeakPtr<VizHost> host = m_hostWeakPtr;
    const scoped_refptr<base::SingleThreadTaskRunner> hostRunner = m_workRunner;
    m_rootClient->destroy(
        [host, hostRunner, callback = std::move(callback)]() mutable {
            hostRunner->PostTask(
                FROM_HERE,
                base::BindOnce(
                    [](base::WeakPtr<VizHost> self,
                        std::function<void(void)>&& callback) {
                        if (!self)
                            return;
                        VizHost* host = self.get();
                        const scoped_refptr<base::SingleThreadTaskRunner> hostRunner
                            = host->m_workRunner;
                        OutputDebugStringA("VizHost::destroy\n");
                        host->m_hostWeakPtrFactory.InvalidateWeakPtrs();
                        host->m_rootClient.release(); // Deleted by VizClient::destroy.
                        callback();
                        ThreadCall::delayDestroySelf(host, hostRunner, 2000);
                    },
                    host, std::move(callback)));
        });
}

void VizHost::resizeOnBlinkThread(const gfx::Size& size, float device_scale_factor)
{
    DCHECK(ThreadCall::isBlinkThread());
    OutputDebugStringA("VizHost::resizeOnBlinkThread\n");

    if (device_scale_factor <= 0.f)
        device_scale_factor = 1.f;
    m_workRunner->PostTask(
        FROM_HERE,
        base::BindOnce(&VizHost::resizeOnHostThread, m_hostWeakPtr, size, device_scale_factor));
}

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
void VizHost::setVSyncDisplayIdOnBlinkThread(int64_t display_id)
{
    DCHECK(ThreadCall::isBlinkThread());
    // Do not obtain or dereference a host-sequence WeakPtr on Blink.
    m_workRunner->PostTask(FROM_HERE,
        base::BindOnce(&VizHost::setVSyncDisplayIdOnHostThread, m_hostWeakPtr, display_id));
}

void VizHost::setVisibleOnBlinkThread(bool visible)
{
    DCHECK(ThreadCall::isBlinkThread());
    m_workRunner->PostTask(FROM_HERE,
        base::BindOnce(&VizHost::setVisibleOnHostThread, m_hostWeakPtr, visible));
}

void VizHost::setVisibleOnHostThread(bool visible)
{
    DCHECK(m_workRunner->RunsTasksInCurrentSequence());
    if (m_visible == visible)
        return;
    m_visible = visible;
    if (m_serviceRunner->RunsTasksInCurrentSequence()) {
        m_displayPrivateState->SetDisplayVisible(visible);
        return;
    }
    base::WaitableEvent updated;
    if (m_serviceRunner->PostTask(
            FROM_HERE,
            base::BindOnce(
                [](scoped_refptr<DisplayPrivateState> display_state, bool visible,
                    base::WaitableEvent* updated) {
                    display_state->SetDisplayVisible(visible);
                    updated->Signal();
                },
                m_displayPrivateState, visible, &updated))) {
        updated.Wait();
    }
}

void VizHost::setVSyncDisplayIdOnHostThread(int64_t display_id)
{
    DCHECK(m_workRunner->RunsTasksInCurrentSequence());
#if BUILDFLAG(IS_MAC)
    if (m_vsyncDisplayId == display_id)
        return;
#endif
    m_vsyncDisplayId = display_id;
    // mojo_lite exposes the receiver directly, so all DisplayPrivate calls
    // must run on the sequence that owns the receiver.
    if (m_serviceRunner->RunsTasksInCurrentSequence()) {
        m_displayPrivateState->SetVSyncDisplayID(display_id);
        return;
    }

    base::WaitableEvent updated;
    if (m_serviceRunner->PostTask(
            FROM_HERE,
            base::BindOnce(
                [](scoped_refptr<DisplayPrivateState> display_state,
                    int64_t display_id, base::WaitableEvent* updated) {
                    display_state->SetVSyncDisplayID(display_id);
                    updated->Signal();
                },
                m_displayPrivateState, display_id, &updated))) {
        updated.Wait();
    }
}
#endif

void VizHost::resizeOnHostThread(const gfx::Size& size, float device_scale_factor)
{
    DCHECK(m_workRunner->RunsTasksInCurrentSequence());
    if (!m_rootClient)
        return;

    m_size = size;

    // Publish the new geometry and LocalSurfaceId before requesting the root
    // frame whose ACK will permit the corresponding display resize.
    m_allocator.GenerateId();
    viz::LocalSurfaceId localSurfaceId = m_allocator.GetCurrentLocalSurfaceId();

    char* output = (char*)malloc(400);
    sprintf(output, "VizHost::resizeOnHostThread: %d, size:%d, tid:%d\n", localSurfaceId.hash(), size.width(), ::GetCurrentThreadId());
    OutputDebugStringA(output);
    free(output);

    const uint64_t frameRevision
        = m_rootClient->resizeOnHostThread(m_size, localSurfaceId, device_scale_factor);
    m_pendingDisplayResizeSize = size;
    m_pendingDisplayResizeRevision = frameRevision;
}

void VizHost::onRootFrameCompletedOnHostThread(uint64_t frame_revision)
{
    DCHECK(m_workRunner->RunsTasksInCurrentSequence());
    if (!m_pendingDisplayResizeRevision
        || frame_revision < m_pendingDisplayResizeRevision) {
        return;
    }

    const gfx::Size size = m_pendingDisplayResizeSize;
    m_pendingDisplayResizeRevision = 0;
    m_serviceRunner->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](scoped_refptr<DisplayPrivateState> display_state,
                const gfx::Size& size) {
                display_state->Resize(size);
            },
            m_displayPrivateState, size));
}

// ����������Ƕ���Ǹ�������
// void VizHost::embedClients(VizClient* embedderClient, const gfx::Rect& childBounds)
// {
//     // Generate a FrameSinkId for the client. Each client can have any number of
//     // frame-sinks, and these frame-sinks should share the same |client_id|
//     // component for the FrameSinkId. In this demo however, each client has a
//     // single FrameSink, so the client-id can just be randomly generated, and it
//     // doesn't make a difference.
//     uint64_t rand = base::RandUint64();
//     viz::FrameSinkId frameSinkId(rand >> 32, rand & 0xffffffff);
//
//     // Register the frame sink and its hierarchy.
//     m_hostFrameSinkManager.RegisterFrameSinkId(frameSinkId, this, viz::ReportFirstSurfaceActivation::kNo);
//     m_hostFrameSinkManager.RegisterFrameSinkHierarchy(embedderClient->frameSinkId(), frameSinkId);
//
//     // Next, create a mojom::CompositorFrameSink for the client, so that the
//     // client is able to submit visual (and hit-test) content to the viz service.
//     // Note that in this demo app, the host is setting up the message-pipes, and
//     // then sending the end-points to the embedded-client and the viz-service.
//     // However, it is possible for the embedded-client to initiate the creation of
//     // the message-pipes, in which case, the client would need to send the
//     // service-end-points to the host (via a non-viz API), so that the host can in
//     // turn send them to the service.
//     mojo::PendingRemote<viz::mojom::CompositorFrameSinkClient> clientRemote;
//     auto clientReceiver = clientRemote.InitWithNewPipeAndPassReceiver();
//
//     mojo::PendingRemote<viz::mojom::CompositorFrameSink> sinkRemote;
//     auto sinkReceiver = sinkRemote.InitWithNewPipeAndPassReceiver();
//
//     m_hostFrameSinkManager.CreateCompositorFrameSink(frameSinkId, std::move(sinkReceiver), std::move(clientRemote));
//
//     // At this point, the host is done setting everything up. Now it is up to the
//     // new client to take over the communication (i.e. the mojo message pipes)
//     // with the service for the frame-sink. The embedder (i.e. the parent client)
//     // also needs to know about the new client's FrameSinkId, so that it is able
//     // to embed it. Both the embedder and the embedded client also need to use
//     // the same LocalSurfaceId for the embedding. Typically, the embedder is the
//     // one that generates the LocalSurfaceId for the embedded client. However, it
//     // is possible for another source (e.g. the viz-host) to generate the
//     // LocalSurfaceId, and dispatch that separately to both the embedder and the
//     // embedded clients. There is no specific viz-API for communicating these
//     // FrameSinkId and LocalSurfaceId between these clients. In chrome, these
//     // happen through the content API (or through the window-service API in
//     // ChromeOS).
//     // In this demo app, the embedder-client is assigning the LocalSurfaceId
//     // (through VizClient).
//
//     viz::LocalSurfaceId lsidAllocation = embedderClient->embed(frameSinkId, childBounds);
//     std::unique_ptr<VizClient> embeddedClient = std::make_unique<VizClient>(frameSinkId, lsidAllocation, childBounds, m_serviceRunner);
//     embeddedClient->initialize(std::move(clientReceiver), std::move(sinkRemote));
// #if 1
//     if (embedderClient == m_rootClient.get()) {
//         // embed another client after a second. This could embed the client
//         // immediately here too if desired. The delay is to demonstrate asynchronous
//         // usage of the API.
//         base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
//             FROM_HERE, base::BindOnce(&VizHost::embedClients, base::Unretained(this), embeddedClient.get(), gfx::Rect(125, 125, 150, 150)), base::Seconds(1));
//     }
// #endif
//     m_embeddedClients.push_back(std::move(embeddedClient));
// }

void VizHost::initialize(mojo::PendingReceiver<viz::mojom::FrameSinkManagerClient> receiver, mojo::PendingRemote<viz::mojom::FrameSinkManager> remote)
{
    //m_displayClient = std::make_unique<viz::HostDisplayClient>(m_widget);
    m_displayClient = std::make_unique<OffscreenDisplayClient /*viz::HostDisplayClient*/>(m_engineViewHost);

    viz::mojom::RootCompositorFrameSinkParamsPtr rootParams = viz::mojom::RootCompositorFrameSinkParams::New();

    // Create interfaces for a root CompositorFrameSink.
    ::mojo::PendingAssociatedRemote<viz::mojom::CompositorFrameSink> sink_remote;
    rootParams->compositor_frame_sink = sink_remote.InitWithNewEndpointAndPassReceiver();

    ::mojo::PendingReceiver<::viz::mojom::CompositorFrameSinkClient> client_receiver
        = rootParams->compositor_frame_sink_client.InitWithNewPipeAndPassReceiver();
    rootParams->display_private = m_displayPrivateState->BindNewEndpointAndPassReceiver();
    rootParams->display_client = m_displayClient->GetBoundRemote(nullptr);

    constexpr viz::FrameSinkId rootFrameSinkId(0xdead, 0xbeef);
    rootParams->frame_sink_id = rootFrameSinkId;
    rootParams->widget = /*m_widget*/ 0;
#if BUILDFLAG(IS_MAC)
    // This port's output device uses the engine view identity, not an NSView*
    // in the renderer process. cc resources remain shared CPU-raster tiles.
    rootParams->widget = static_cast<gfx::AcceleratedWidget>(m_engineViewHost->getId());
#endif
#if BUILDFLAG(IS_WIN)
    rootParams->widget = reinterpret_cast<gfx::AcceleratedWidget>(
        static_cast<intptr_t>(m_engineViewHost->getId()));
#endif
    rootParams->gpu_compositing = false;
    rootParams->renderer_settings = viz::CreateRendererSettings();
    rootParams->renderer_settings.should_clear_root_render_pass = false;
#if BUILDFLAG(IS_MAC)
    rootParams->renderer_settings.display_id = m_vsyncDisplayId;
#endif
    m_hostWeakPtr = m_hostWeakPtrFactory.GetWeakPtr();

    if (m_isTransparent) {
        rootParams->renderer_settings.should_clear_root_render_pass = true;
        rootParams->renderer_settings.requires_alpha_channel = true;
    }

    m_allocator.GenerateId();
    m_rootClient = std::make_unique<VizClient>(
        rootFrameSinkId, m_allocator.GetCurrentLocalSurfaceId(), gfx::Rect(m_size),
        m_serviceRunner,
        base::BindPostTask(
            m_workRunner,
            base::BindRepeating(
                &VizHost::onRootFrameCompletedOnHostThread, m_hostWeakPtr)));
    m_rootClient->setIsRootClient();

    //     char* output = (char*)malloc(0x500);
    //     sprintf(output, "VizHost::Initialize: %p (%d %d %s)\n",
    //         this,
    //         m_allocator.GetCurrentLocalSurfaceId().parent_sequence_number(),
    //         m_allocator.GetCurrentLocalSurfaceId().child_sequence_number(),
    //         m_allocator.GetCurrentLocalSurfaceId().embed_token().ToString().c_str()
    //     );
    //     OutputDebugStringA(output);
    //     free(output);

    CHECK(!m_frameSinkId.is_valid());
    m_frameSinkId = rootParams->frame_sink_id;

    m_hostFrameSinkManager.BindAndSetManager(std::move(receiver), nullptr, std::move(remote));
    m_hostFrameSinkManager.RegisterFrameSinkId(rootParams->frame_sink_id, this, viz::ReportFirstSurfaceActivation::kNo);
    m_hostFrameSinkManager.CreateRootCompositorFrameSink(std::move(rootParams));

    m_serviceRunner->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](scoped_refptr<DisplayPrivateState> display_state,
                const gfx::Size& size, int64_t vsync_display_id) {
                display_state->Resize(size);
                display_state->SetDisplayVisible(true);
#if BUILDFLAG(IS_WIN)
                display_state->SetVSyncDisplayID(vsync_display_id);
#endif
            },
            m_displayPrivateState, m_size,
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
            m_vsyncDisplayId
#else
            0
#endif
            ));

    // Initialize as a client now, since the host has to submit compositor frames
    // like any other clients.
    // The 'root' is not embedded by anything else. However, it still needs to
    // have a valid LocalSurfaceId, which changes when root changes size (or
    // device-scale-factor etc.).
    m_rootClient->initialize(std::move(client_receiver), std::move(sink_remote));

    // Publish initialized Mojo endpoints and the host-sequence WeakPtr to Blink.
    m_initialized.Signal();
}

void VizHost::OnFirstSurfaceActivation(const viz::SurfaceInfo& surface_info)
{
}

void VizHost::OnFrameTokenChanged(uint32_t frame_token, base::TimeTicks activation_time)
{
}

} // namespace demo
