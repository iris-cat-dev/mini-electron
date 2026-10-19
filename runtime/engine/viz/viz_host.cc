// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "runtime/engine/viz/viz_host.h"

#include <utility>

#include "base/rand_util.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "runtime/engine/viz/viz_client.h"
#include "runtime/engine/viz/offscreen_display_client.h"
#if BUILDFLAG(IS_MAC)
#include "runtime/engine/browser/web_view_host.h"
#endif
#include "runtime/engine/common/thread_call.h"
#include "components/viz/host/renderer_settings_creation.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"

namespace content {

VizHost::VizHost(WebViewHost* mbwebview, bool isTransparent,
    //gfx::AcceleratedWidget widget,
    const gfx::Size& size, mojo::PendingReceiver<viz::mojom::FrameSinkManagerClient> clientReceiver,
    mojo::PendingRemote<viz::mojom::FrameSinkManager> frameSinkManagerRemote, scoped_refptr<base::SingleThreadTaskRunner> workRunner,
#if BUILDFLAG(IS_MAC)
    int64_t vsyncDisplayId,
#endif
    scoped_refptr<base::SingleThreadTaskRunner> serviceRunner)
    : m_widget(gfx::kNullAcceleratedWidget)
    , m_engineViewHost(mbwebview)
    , m_isTransparent(isTransparent)
    , m_size(size)
#if BUILDFLAG(IS_MAC)
    , m_vsyncDisplayId(vsyncDisplayId)
#endif
    //, m_thread("VizHost")
    , m_workRunner(workRunner)
    , m_serviceRunner(serviceRunner)
{
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
    //m_hostFrameSinkManager.InvalidateFrameSinkId(m_frameSinkId);
    //m_hostFrameSinkManager.SetConnectionLostCallback();
    VizHost* self = this;
    if (m_rootClient) {
        m_rootClient->destroy([self, callback] {
            self->m_workRunner->PostTask(FROM_HERE,
                base::BindOnce(
                    [](VizHost* self, std::function<void(void)>&& callback) {
                        OutputDebugStringA("VizHost::destroy\n");
                        self->m_rootClient.release(); // TODO
                        (callback)();

                        ThreadCall::delayDestroySelf(self, self->m_workRunner, 2000);
                    },
                    base::Unretained(self), std::move(callback)));
        });
    }
}

void VizHost::resizeOnBlinkThread(const gfx::Size& size, float device_scale_factor)
{
    if (m_rootClient)
        m_rootClient->disallowResize();
    OutputDebugStringA("VizHost::resizeOnBlinkThread\n");

    if (device_scale_factor <= 0.f)
        device_scale_factor = 1.f;
    base::WaitableEvent waitEvt;
    m_workRunner->PostTask(FROM_HERE, base::BindOnce(&VizHost::resizeOnHostThread, base::Unretained(this), size, device_scale_factor, &waitEvt));
    waitEvt.Wait();
}

#if BUILDFLAG(IS_MAC)
void VizHost::setVSyncDisplayIdOnBlinkThread(int64_t display_id)
{
    DCHECK(ThreadCall::isBlinkThread());
    // Do not obtain or dereference a host-sequence WeakPtr on Blink.
    m_workRunner->PostTask(FROM_HERE,
        base::BindOnce(&VizHost::setVSyncDisplayIdOnHostThread, m_hostWeakPtr, display_id));
}

void VizHost::setVSyncDisplayIdOnHostThread(int64_t display_id)
{
    DCHECK(m_workRunner->RunsTasksInCurrentSequence());
    if (m_vsyncDisplayId == display_id)
        return;
    m_vsyncDisplayId = display_id;
    // This runtime's Mojo Remote exposes the receiver directly, rather than a
    // proxy which marshals calls. Use the same runner that creates the root
    // sink and its DisplayScheduler, not the host runner owning this Remote.
    if (m_serviceRunner->RunsTasksInCurrentSequence()) {
        if (auto* display = m_displayPrivate.TryGet())
            display->SetVSyncDisplayID(display_id);
        return;
    }

    // Completion bounds the raw captures' lifetime: host destruction runs on
    // m_workRunner and cannot overtake this task while it is waiting. Resolve
    // the receiver only on Viz so a preceding teardown cannot leave a raw sink.
    // initialize() queues root creation on this same runner before publishing
    // completion, so this update cannot overtake initial receiver binding.
    base::WaitableEvent updated;
    if (m_serviceRunner->PostTask(FROM_HERE,
            base::BindOnce(
                [](VizHost* self, int64_t display_id, base::WaitableEvent* updated) {
                    DCHECK(self->m_serviceRunner->RunsTasksInCurrentSequence());
                    if (auto* display = self->m_displayPrivate.TryGet())
                        display->SetVSyncDisplayID(display_id);
                    updated->Signal();
                },
                base::Unretained(this), display_id, &updated))) {
        updated.Wait();
    }
}
#endif

void VizHost::resizeOnHostThread(const gfx::Size& size, float device_scale_factor, base::WaitableEvent* waitEvt)
{
//     if (m_size == size) {
//         if (m_rootClient)
//             m_rootClient->allowResize();
// 
//         if (waitEvt)
//             waitEvt->Signal();
//         return;
//     }
    m_size = size;

    // Every size change for a client needs a new LocalSurfaceId.
    m_allocator.GenerateId();
    viz::LocalSurfaceId localSurfaceId = m_allocator.GetCurrentLocalSurfaceId();

    char* output = (char*)malloc(400);
    sprintf(output, "VizHost::resizeOnHostThread: %d, size:%d, tid:%d\n", localSurfaceId.hash(), size.width(), ::GetCurrentThreadId());
    OutputDebugStringA(output);
    free(output);

    m_rootClient->resizeOnHostThread(m_size, localSurfaceId, device_scale_factor);
    delayResizeDisplayOnVizThread(waitEvt);
}

void VizHost::delayResizeDisplayOnVizThread(base::WaitableEvent* waitEvt)
{
    gfx::Size size = m_size;
    m_serviceRunner->PostTask(FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<VizHost> self, const gfx::Size& size, base::WaitableEvent* waitEvt) {
                if (!self->isAllowResize()) {
                    self->delayResizeDisplayOnVizThread(waitEvt);
                    return;
                }
                self->m_displayPrivate->Resize(size);
                if (waitEvt)
                    waitEvt->Signal();
            },
            m_weakPtrFactory.GetWeakPtr(), size, waitEvt));
}

bool VizHost::isAllowResize() const
{
    if (!m_rootClient.get())
        return true;
    return m_rootClient->isAllowResize();
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
    rootParams->display_private = m_displayPrivate.BindNewEndpointAndPassReceiver();
    rootParams->display_client = m_displayClient->GetBoundRemote(nullptr);

    constexpr viz::FrameSinkId rootFrameSinkId(0xdead, 0xbeef);
    rootParams->frame_sink_id = rootFrameSinkId;
    rootParams->widget = /*m_widget*/ 0;
#if BUILDFLAG(IS_MAC)
    // This port's output device uses the engine view identity, not an NSView*
    // in the renderer process. cc resources remain shared CPU-raster tiles.
    rootParams->widget = static_cast<gfx::AcceleratedWidget>(m_engineViewHost->getId());
#endif
    rootParams->gpu_compositing = false;
    rootParams->renderer_settings = viz::CreateRendererSettings();
    rootParams->renderer_settings.should_clear_root_render_pass = false;
#if BUILDFLAG(IS_MAC)
    rootParams->renderer_settings.display_id = m_vsyncDisplayId;
    m_hostWeakPtr = m_hostWeakPtrFactory.GetWeakPtr();
#endif

    if (m_isTransparent) {
        rootParams->renderer_settings.should_clear_root_render_pass = true;
        rootParams->renderer_settings.requires_alpha_channel = true;
    }

    m_allocator.GenerateId();
    m_rootClient = std::make_unique<VizClient>(rootFrameSinkId, m_allocator.GetCurrentLocalSurfaceId(), gfx::Rect(m_size), m_serviceRunner);
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

    m_serviceRunner->PostTask(FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<VizHost> self, const gfx::Size& size) {
                self->m_displayPrivate->Resize(size);
                self->m_displayPrivate->SetDisplayVisible(true);
            },
            m_weakPtrFactory.GetWeakPtr(), m_size));

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
