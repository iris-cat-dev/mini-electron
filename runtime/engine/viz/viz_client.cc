// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "runtime/engine/viz/viz_client.h"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "components/viz/common/quads/compositor_frame.h"
#include "components/viz/common/quads/solid_color_draw_quad.h"
#include "components/viz/common/quads/surface_draw_quad.h"
#include "components/viz/service/frame_sinks/compositor_frame_sink_support.h"
#include "components/viz/service/frame_sinks/frame_sink_manager_impl.h"
#include "runtime/engine/browser/render_widget_host_impl.h"
#include "runtime/engine/common/thread_call.h"
#include "runtime/engine/common/live_id_detect.h"

namespace content {

static base::Thread* s_vizClientThread = nullptr;

VizClient::VizClient(const viz::FrameSinkId& frameSinkId, const viz::LocalSurfaceId& localSurfaceId, const gfx::Rect& bounds,
    scoped_refptr<base::SingleThreadTaskRunner> serviceRunner,
    base::RepeatingCallback<void(uint64_t)> frameCompletionCallback)
    : m_frameSinkId(frameSinkId)
    , m_hostLocalSurfaceId(localSurfaceId)
    , m_bounds(bounds)
    , m_frameCompletionCallback(std::move(frameCompletionCallback))
    , m_vizCompositorRunner(std::move(serviceRunner))
{
    if (!s_vizClientThread) {
        s_vizClientThread = new base::Thread("VizClient");
        CHECK(s_vizClientThread->Start());
    }

    m_id = common::LiveIdDetect::get()->constructed(this);

    //*(int*)1 = 1; //SetServer\OnServerDestroy not impl!
}

VizClient::~VizClient()
{
    common::LiveIdDetect::get()->deconstructed(m_id);
}

void VizClient::destroy(std::function<void(void)>&& callback)
{
    CHECK(!m_sink.is_bound());
    {
        base::AutoLock lock(m_lock);
        m_canCommitFrame = false;
    }

    viz::CompositorFrameSinkSupport* sink = (viz::CompositorFrameSinkSupport*)m_vizServer;
    m_vizServer = nullptr;
    if (sink)
        sink->OnClientDestroy();
    s_vizClientThread->task_runner()->PostTask(FROM_HERE,
        base::BindOnce(
            [](VizClient* self, std::function<void(void)>&& callback) {
                //         int waitCount = 0;
                //
                //         self->m_receiver.set_disconnect_handler(base::BindOnce([](int* waitCount) {
                //             *waitCount = 1; // ���Ͷ�Ҳ�ر��ˣ����ߵ�����
                //         }, &waitCount));
                //         self->m_receiver.set_connection_error_handler_is_sync();
                //
                //         while (0 == waitCount) {
                //             base::PlatformThread::Sleep(base::Milliseconds(1));
                //         }
                self->m_receiver.reset();

                self->m_associatedSink.reset();
                self->m_sink.reset();
                //delete self; // TODO

                ThreadCall::delayDestroySelf(self, s_vizClientThread->task_runner(), 2000);

                (callback)();
                OutputDebugStringA("VizClient::destroy\n");
            },
            base::Unretained(this), std::move(callback)));
}

void VizClient::initialize(
    mojo::PendingReceiver<viz::mojom::CompositorFrameSinkClient> receiver, mojo::PendingAssociatedRemote<viz::mojom::CompositorFrameSink> sink_remote)
{
    s_vizClientThread->task_runner()->PostTask(
        FROM_HERE, base::BindOnce(&VizClient::initializeOnThread, base::Unretained(this), std::move(receiver), std::move(sink_remote), mojo::NullRemote()));
}


viz::LocalSurfaceId VizClient::embedOnBlinkThread(const viz::FrameSinkId& frameSinkId, const gfx::Rect& bounds, const viz::LocalSurfaceId& newLocalSurfaceId)
{
    bool changed = false;
    {
        // createFrame() snapshots the complete root state under this lock.
        base::AutoLock lock(m_lock);
        EmbedInfo& embed = m_embeds[frameSinkId];
        changed = embed.lsid != newLocalSurfaceId || embed.bounds != bounds;
        if (changed) {
            embed.lsid = newLocalSurfaceId;
            embed.bounds = bounds;
            ++m_frameRevision;
        }
    }
    if (changed)
        requestBeginFrame();
    return newLocalSurfaceId;
}

void VizClient::onChildSurfaceActivated(const viz::SurfaceId& surfaceId)
{
    bool rootChanged = false;
    {
        base::AutoLock lock(m_lock);
        auto iter = m_embeds.find(surfaceId.frame_sink_id());
        if (iter == m_embeds.end())
            return;
        EmbedInfo& embed = iter->second;
        const viz::LocalSurfaceId& activated = surfaceId.local_surface_id();
        // A delayed notification must not retire a newer complete fallback.
        if (embed.active_lsid.is_valid()
            && activated.embed_token() == embed.active_lsid.embed_token()
            && embed.active_lsid.IsNewerThan(activated))
            return;

        const viz::LocalSurfaceId oldRequested = embed.lsid;
        const viz::LocalSurfaceId oldActive = embed.active_lsid;
        const bool hadFallback = oldActive.is_valid() && oldActive != oldRequested;
        embed.active_lsid = activated;
        if (activated.embed_token() == embed.lsid.embed_token()
            && activated.parent_sequence_number() == embed.lsid.parent_sequence_number()
            && activated.child_sequence_number() >= embed.lsid.child_sequence_number())
            embed.lsid = activated;

        const bool hasFallback = embed.active_lsid.is_valid()
            && embed.active_lsid != embed.lsid;
        rootChanged = oldRequested != embed.lsid
            || hadFallback != hasFallback
            || (hadFallback && oldActive != embed.active_lsid);
        if (rootChanged)
            ++m_frameRevision;
    }
    if (rootChanged)
        requestBeginFrame();
}

viz::LocalSurfaceId VizClient::GetLocalSurfaceId()
{
    DebugBreak();
    base::AutoLock lock(m_lock);
    return m_hostLocalSurfaceId;
}

viz::LocalSurfaceId VizClient::GenLocalSurfaceId()
{
    DebugBreak();
    base::AutoLock lock(m_lock);
    m_allocator.GenerateId();
    return m_allocator.GetCurrentLocalSurfaceId();
}


uint64_t VizClient::resizeOnHostThread(const gfx::Size& size, const viz::LocalSurfaceId& localSurfaceId, float device_scale_factor)
{
    uint64_t frameRevision;
    {
        // createFrame() snapshots these values under the same lock.
        base::AutoLock lock(m_lock);
        m_bounds.set_size(size);
        m_hostLocalSurfaceId = localSurfaceId;
        m_deviceScaleFactor = device_scale_factor > 0.f ? device_scale_factor : 1.f;
        frameRevision = ++m_frameRevision;
    }
    requestBeginFrame();
    return frameRevision;
}

void VizClient::requestBeginFrame()
{
    {
        base::AutoLock lock(m_lock);
        if (!m_canCommitFrame || m_queuedFrameRevision == m_frameRevision)
            return;
    }
    m_vizCompositorRunner->PostTask(FROM_HERE, base::BindOnce([](int64_t id) {
        VizClient* self = (VizClient*)common::LiveIdDetect::get()->getPtr(id);
        if (!self)
            return;
        bool needsBeginFrame;
        {
            base::AutoLock lock(self->m_lock);
            needsBeginFrame = self->m_canCommitFrame
                && self->m_queuedFrameRevision != self->m_frameRevision;
        }
        if (needsBeginFrame) {
            if (viz::mojom::CompositorFrameSink* ptr = self->getPtrImpl())
                ptr->SetNeedsBeginFrame(true);
        }
    }, m_id));
}

void VizClient::didNotProduceFrame(viz::BeginFrameAck ack)
{
    m_vizCompositorRunner->PostTask(FROM_HERE,
        base::BindOnce([](int64_t id, viz::BeginFrameAck ack) {
            VizClient* self = (VizClient*)common::LiveIdDetect::get()->getPtr(id);
            if (self) {
                if (viz::mojom::CompositorFrameSink* ptr = self->getPtrImpl())
                    ptr->DidNotProduceFrame(ack);
            }
        }, m_id, std::move(ack)));
}

void VizClient::notifyFrameCompletion(uint64_t frame_revision)
{
    if (m_frameCompletionCallback)
        m_frameCompletionCallback.Run(frame_revision);
}

viz::mojom::CompositorFrameSink* VizClient::getPtrImpl()
{
    if (m_associatedSink.is_bound())
        return m_associatedSink.TryGet();
    if (m_sink.is_bound())
        return m_sink.TryGet();
    return nullptr;
}


void VizClient::initializeOnThread(mojo::PendingReceiver<viz::mojom::CompositorFrameSinkClient> receiver,
    mojo::PendingAssociatedRemote<viz::mojom::CompositorFrameSink> associated_sink_remote, mojo::PendingRemote<viz::mojom::CompositorFrameSink> sink_remote)
{
    m_receiver.Bind(std::move(receiver));

    if (associated_sink_remote)
        m_associatedSink.Bind(std::move(associated_sink_remote));
    else
        m_sink.Bind(std::move(sink_remote));

    requestBeginFrame();
}

void VizClient::DidReceiveCompositorFrameAck(std::vector<viz::ReturnedResource> resources)
{
    uint64_t frameRevision = 0;
    {
        base::AutoLock lock(m_lock);
        if (!m_submittedFrameRevisions.empty()) {
            frameRevision = m_submittedFrameRevisions.front();
            m_submittedFrameRevisions.pop_front();
        }
    }
    if (frameRevision)
        notifyFrameCompletion(frameRevision);
}


viz::CompositorFrame VizClient::createFrame(const viz::BeginFrameArgs& args)
{
    viz::CompositorFrame frame;

    frame.metadata.begin_frame_ack = viz::BeginFrameAck(args, true);
    frame.metadata.device_scale_factor = m_deviceScaleFactor;
    frame.metadata.frame_token = ++m_nextFrameToken;


    const viz::CompositorRenderPassId kRenderPassId { 10010 };
    const gfx::Rect& outputRect = m_bounds;
    const gfx::Rect& damageRect = outputRect;
    std::unique_ptr<viz::CompositorRenderPass> renderPass = viz::CompositorRenderPass::Create();
    renderPass->SetNew(kRenderPassId, outputRect, damageRect, gfx::Transform());


    // Child geometry normally follows Viz's uniform device-scale ratio.
    // Windows temporarily stretches an older active surface below until the
    // requested resize activates, rather than exposing incomplete gutters.
    for (std::map<viz::FrameSinkId, EmbedInfo>::const_iterator iter = m_embeds.begin(); iter != m_embeds.end(); ++iter) {

        const gfx::Rect& childBounds = iter->second.bounds;
        gfx::Transform transform;
        viz::SharedQuadState* quadState1 = renderPass->CreateAndAppendSharedQuadState();
        quadState1->SetAll(transform,
            /*quad_layer_rect=*/childBounds,
            /*visible_layer_rect=*/childBounds,
            /*mask_filter_info=*/gfx::MaskFilterInfo(),
            /*clip_rect=*/absl::nullopt,
            /*are_contents_opaque=*/false,
            /*opacity=*/1.f,
            /*blend_mode=*/SkBlendMode::kSrcOver,
            /*sorting_context_id=*/0,
            /*layer_id*/0,
            /*fast_rounded_corner*/false);

        viz::SurfaceDrawQuad* embedSurfaceDrawquad = renderPass->CreateAndAppendDrawQuad<viz::SurfaceDrawQuad>();

        viz::SurfaceId surfaceId(iter->first, iter->second.lsid);
        std::optional<viz::SurfaceId> fallback;
        if (iter->second.active_lsid.is_valid() && iter->second.active_lsid != iter->second.lsid)
            fallback = viz::SurfaceId(iter->first, iter->second.active_lsid);
        const viz::SurfaceRange surfaceRange(fallback, surfaceId);
        frame.metadata.referenced_surfaces.push_back(surfaceRange);
        // |rect| and |visible_rect| needs to be in the quad's coord-space, so to draw the whole quad, it needs to use origin (0, 0).
        embedSurfaceDrawquad->SetNew(quadState1,
            /*rect=*/gfx::Rect(childBounds.size()),
            /*visible_rect=*/gfx::Rect(childBounds.size()), surfaceRange,
            SkColors::kTransparent,
            /*stretch_content_to_fill_bounds=*/false);
    }

    frame.render_pass_list.push_back(std::move(renderPass));

    return frame;
}


void VizClient::OnBeginFrame(const ::viz::BeginFrameArgs& args, const base::flat_map<uint32_t, ::viz::FrameTimingDetails>& details, bool frame_ack,
    std::vector<::viz::ReturnedResource> resources)
{
    // With OnBeginFrame ACK merging enabled, resource returns and the previous
    // submission ACK arrive on this callback even when no new frame is wanted.
    if (frame_ack)
        DidReceiveCompositorFrameAck(std::move(resources));
    else if (!resources.empty())
        ReclaimResources(std::move(resources));

    std::optional<viz::CompositorFrame> frame;
    viz::LocalSurfaceId localSurfaceId;
    uint64_t frameRevision = 0;
    {
        base::AutoLock lock(m_lock);
        if (m_canCommitFrame && m_queuedFrameRevision != m_frameRevision) {
            m_queuedFrameRevision = m_frameRevision;
            frameRevision = m_queuedFrameRevision;
            localSurfaceId = m_hostLocalSurfaceId;
            frame.emplace(createFrame(args));
        }
    }

    if (!frame) {
        // SetNeedsBeginFrame(false) can race with an already-issued BeginFrame,
        // and Viz also uses OnBeginFrame to deliver ACKs and presentation
        // feedback. Every such callback still requires a protocol response.
        didNotProduceFrame(viz::BeginFrameAck(args, false));
        return;
    }

    m_vizCompositorRunner->PostTask(FROM_HERE,
        base::BindOnce(
            [](int64_t id, viz::CompositorFrame frame,
                viz::LocalSurfaceId localSurfaceId, uint64_t frameRevision,
                viz::BeginFrameAck noDamageAck) {
                VizClient* self = (VizClient*)common::LiveIdDetect::get()->getPtr(id);
                if (!self)
                    return;
                viz::mojom::CompositorFrameSink* ptr = self->getPtrImpl();
                bool canCommitFrame;
                {
                    base::AutoLock lock(self->m_lock);
                    canCommitFrame = self->m_canCommitFrame;
                }
                if (!canCommitFrame || !ptr) {
                    self->notifyFrameCompletion(frameRevision);
                    if (ptr)
                        ptr->DidNotProduceFrame(noDamageAck);
                    return;
                }

                // Activation notifications can still be queued while this root
                // submission is prepared. Query the service's actual active
                // child before choosing fallback.
                auto* support = static_cast<viz::CompositorFrameSinkSupport*>(self->m_vizServer);
                if (support) {
                    size_t rangeIndex = 0;
                    for (auto& pass : frame.render_pass_list) {
                        for (auto iter = pass->quad_list.begin(); iter != pass->quad_list.end(); ++iter) {
                            if ((*iter)->material != viz::DrawQuad::Material::kSurfaceContent)
                                continue;
                            auto* quad = static_cast<viz::SurfaceDrawQuad*>(*iter);
                            const viz::SurfaceId requested = quad->surface_range.end();
                            auto* child = support->frame_sink_manager()->GetFrameSinkForId(requested.frame_sink_id());
                            if (child && child->last_activated_surface_id().is_valid()) {
                                const viz::SurfaceId& active = child->last_activated_surface_id();
                                const auto& activeId = active.local_surface_id();
                                const auto& requestedId = requested.local_surface_id();
                                const bool reachedRequest = activeId.embed_token() == requestedId.embed_token()
                                    && activeId.parent_sequence_number() >= requestedId.parent_sequence_number()
                                    && activeId.child_sequence_number() >= requestedId.child_sequence_number();
                                quad->surface_range = reachedRequest
                                    ? viz::SurfaceRange(active) : viz::SurfaceRange(active, requested);
                            }
#if BUILDFLAG(IS_WIN)
                            quad->stretch_content_to_fill_bounds = quad->surface_range.start().has_value();
#endif
                            frame.metadata.referenced_surfaces[rangeIndex++] = quad->surface_range;
                        }
                    }
                }
                // Record the exact submitted state before calling into Viz so
                // even a synchronously delivered ACK observes its revision.
                if (self->m_frameCompletionCallback) {
                    base::AutoLock lock(self->m_lock);
                    self->m_submittedFrameRevisions.push_back(frameRevision);
                }
                ptr->SubmitCompositorFrame(localSurfaceId, std::move(frame),
                    absl::optional<viz::HitTestRegionList>(), /*trace_time=*/0);

                // If a mutation raced this submission and has not already been
                // captured by another queued frame, keep BeginFrames enabled.
                // Otherwise the submitted/queued root state is complete and
                // feedback-only callbacks must not keep the source active.
                bool needsBeginFrame;
                {
                    base::AutoLock lock(self->m_lock);
                    needsBeginFrame = self->m_canCommitFrame
                        && self->m_queuedFrameRevision != self->m_frameRevision;
                }
                ptr->SetNeedsBeginFrame(needsBeginFrame);
            },
            m_id, std::move(*frame), localSurfaceId, frameRevision,
            viz::BeginFrameAck(args, false)));
}

void VizClient::OnBeginFramePausedChanged(bool paused)
{
}

void VizClient::ReclaimResources(std::vector<viz::ReturnedResource> resources)
{
}

void VizClient::OnCompositorFrameTransitionDirectiveProcessed(uint32_t sequence_id)
{
}

void VizClient::OnSurfaceEvicted(const ::viz::LocalSurfaceId& local_surface_id)
{

}

bool VizClient::SetServer(void* self)
{
    m_vizServer = self;
    return true;
}

void VizClient::OnServerDestroy()
{
    m_vizServer = nullptr;
}

} // namespace demo
