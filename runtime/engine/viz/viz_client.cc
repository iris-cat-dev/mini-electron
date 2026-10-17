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
#include "runtime/engine/renderer/render_thread_impl.h"
#include "runtime/engine/common/thread_call.h"
#include "runtime/engine/common/live_id_detect.h"

namespace content {

static base::Thread* s_vizClientThread = nullptr;

VizClient::VizClient(const viz::FrameSinkId& frameSinkId, const viz::LocalSurfaceId& localSurfaceId, const gfx::Rect& bounds,
    scoped_refptr<base::SingleThreadTaskRunner> servicRrunner)
    //: m_thread(frameSinkId.ToString()) // ��m_vizCompositorRunner�߳���
    : m_frameSinkId(frameSinkId)
    , m_hostLocalSurfaceId(localSurfaceId)
    , m_bounds(bounds)
    , m_vizCompositorRunner(servicRrunner)
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
    m_canCommitFrame = false;

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

// void VizClient::initialize(
//     mojo::PendingReceiver<viz::mojom::CompositorFrameSinkClient> receiver, mojo::PendingRemote<viz::mojom::CompositorFrameSink> sink_remote)
// {
//     m_thread.task_runner()->PostTask(FROM_HERE,
//         base::BindOnce(&VizClient::initializeOnThread, base::Unretained(this), std::move(receiver), mojo::NullAssociatedRemote(), std::move(sink_remote)));
// }

viz::LocalSurfaceId VizClient::embedOnBlinkThread(const viz::FrameSinkId& frameSinkId, const gfx::Rect& bounds, const viz::LocalSurfaceId& newLocalSurfaceId)
{
    // |m_embeds| is used on the client-thread in CreateFrame(). So this needs to
    // be mutated under a lock.
    base::AutoLock lock(m_lock);
    EmbedInfo& embed = m_embeds[frameSinkId];
    embed.lsid = newLocalSurfaceId;
    embed.bounds = bounds;

    //     char* output = (char*)malloc(0x500);
    //     sprintf_s(output, 0x499, "VizClient::Embed: %p (newLocalSurfaceId:%d)(m_hostLocalSurfaceId:%d)(width:%d)(tid:%d)\n",
    //         this,
    //         newLocalSurfaceId.hash(),
    //         m_hostLocalSurfaceId.hash(),
    //         m_bounds.width(),
    //         ::GetCurrentThreadId()
    //     );
    //     OutputDebugStringA(output);
    //     free(output);

    return newLocalSurfaceId;
}

void VizClient::onChildSurfaceActivated(const viz::SurfaceId& surfaceId)
{
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
            && activated.parent_sequence_number() < embed.active_lsid.parent_sequence_number())
            return;
        embed.active_lsid = activated;
        if (activated.embed_token() == embed.lsid.embed_token()
            && activated.parent_sequence_number() == embed.lsid.parent_sequence_number()
            && activated.child_sequence_number() >= embed.lsid.child_sequence_number())
            embed.lsid = activated;
        ++m_frameRevision;
    }
    m_vizCompositorRunner->PostTask(FROM_HERE, base::BindOnce([](int64_t id) {
        VizClient* self = (VizClient*)common::LiveIdDetect::get()->getPtr(id);
        if (self && self->getPtrImpl())
            self->getPtrImpl()->SetNeedsBeginFrame(true);
    }, m_id));
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

bool g_canSetNeedsBeginFrame = true;

void VizClient::resizeOnHostThread(const gfx::Size& size, const viz::LocalSurfaceId& localSurfaceId, float device_scale_factor)
{
    // |m_bounds| and |m_hostLocalSurfaceId| are used on the client-thread in
    // createFrame(). So these need to be mutated under a lock.
    base::AutoLock lock(m_lock);
    m_bounds.set_size(size);
    m_hostLocalSurfaceId = localSurfaceId; // ����Ǹ�surfaceid����ΪSubmitCompositorFrame�Ĳ���
    m_deviceScaleFactor = device_scale_factor > 0.f ? device_scale_factor : 1.f;
    ++m_frameRevision;
    //m_forbidResize = true;
    //m_canCommitFrame = false;// ���ڸĳ�һֱ����
    scoped_refptr<base::SequencedTaskRunner> runner = base::SequencedTaskRunner::GetCurrentDefault();

    // Request for begin-frames.
    m_vizCompositorRunner->PostTask(FROM_HERE,
        base::BindOnce(
            [](scoped_refptr<base::SequencedTaskRunner> runner, int64_t id) {
                VizClient* self = (VizClient*)common::LiveIdDetect::get()->getPtr(id);
                if (!self)
                    return;
                if (!self->getPtr())
                    return;

                if (g_canSetNeedsBeginFrame)
                    self->getPtr()->SetNeedsBeginFrame(true);
                else
                    self->allowResize();

                runner->PostTask(FROM_HERE,
                    base::BindOnce(
                        [](int64_t id) {
                            VizClient* self = (VizClient*)common::LiveIdDetect::get()->getPtr(id);
                            if (!self)
                                return;
                            self->m_canCommitFrame = true;
                        },
                        id));
            },
            std::move(runner), m_id));
}

viz::mojom::CompositorFrameSink* VizClient::getPtrImpl()
{
    if (m_associatedSink.is_bound())
        return m_associatedSink.TryGet();
    if (m_sink.is_bound())
        return m_sink.TryGet();
    return nullptr;
}

viz::mojom::CompositorFrameSink* VizClient::getPtr()
{
    viz::mojom::CompositorFrameSink* ret = getPtrImpl();
    for (int count = 0; !ret; ret = getPtrImpl(), ++count) {
        if (1 == (count % 11)) {
            void* webview = common::LiveIdDetect::getWebViewIds()->getPtr(m_webviewId);
            if (!webview)
                return nullptr;
        }
        base::PlatformThread::Sleep(base::Milliseconds(5));
    }
    return ret;
}

void VizClient::initializeOnThread(mojo::PendingReceiver<viz::mojom::CompositorFrameSinkClient> receiver,
    mojo::PendingAssociatedRemote<viz::mojom::CompositorFrameSink> associated_sink_remote, mojo::PendingRemote<viz::mojom::CompositorFrameSink> sink_remote)
{
    m_receiver.Bind(std::move(receiver));

    if (associated_sink_remote)
        m_associatedSink.Bind(std::move(associated_sink_remote));
    else
        m_sink.Bind(std::move(sink_remote));

    // ��resize�ٵ���SetNeedsBeginFrame(true);
}

void VizClient::DidReceiveCompositorFrameAck(std::vector<viz::ReturnedResource> resources)
{
    // See documentation in mojom for how this can be used.
    //     char* output = (char*)malloc(0x500);
    //     sprintf_s(output, 0x499, "VizClient::DidReceiveCompositorFrameAck: %p (%d), tid:%d\n",
    //         this, m_hostLocalSurfaceId.hash(), ::GetCurrentThreadId());
    //     OutputDebugStringA(output);
    //     free(output);

    RenderThreadImpl::get()->getCompositorTaskRunner()->PostTask(FROM_HERE,
        base::BindOnce(
            [](int64_t id) {
                VizClient* self = (VizClient*)common::LiveIdDetect::get()->getPtr(id);
                if (self)
                    self->allowResize();

                //         char* output = (char*)malloc(0x500);
                //         sprintf_s(output, 0x499, "VizClient::DidReceiveCompositorFrameAck on Compositor: %p,tid:%d\n",
                //             self.get(), ::GetCurrentThreadId());
                //         OutputDebugStringA(output);
                //         free(output);
            },
            m_id));

    //     m_vizCompositorRunner->PostTask(FROM_HERE, base::BindOnce([](int64_t id) {
    //         VizClient* self = (VizClient*)common::LiveIdDetect::get()->getPtr(id);
    //         if (!self)
    //             return;
    //         self->getPtr()->SetNeedsBeginFrame(false);
    //     }, m_id));
}

bool g_canCommitFrame = true;

viz::CompositorFrame VizClient::createFrame(const viz::BeginFrameArgs& args)
{
    viz::CompositorFrame frame;

    frame.metadata.begin_frame_ack = viz::BeginFrameAck(args, true);
    frame.metadata.device_scale_factor = m_deviceScaleFactor;
    frame.metadata.frame_token = ++m_nextFrameToken;

    //     char* output = (char*)malloc(0x500);
    //     sprintf_s(output, 0x499, "VizClient::createFrame on viz compos thread: %p (%d), w:%d tid:%d\n",
    //         this, m_hostLocalSurfaceId.hash(), m_bounds.width(), ::GetCurrentThreadId());
    //     OutputDebugStringA(output);
    //     free(output);

    const viz::CompositorRenderPassId kRenderPassId { 10010 };
    const gfx::Rect& outputRect = m_bounds;
    const gfx::Rect& damageRect = gfx::Rect(0, 0, 0, 0); // gfx::Rect(0, 0, 0, 0);//outputRect
    std::unique_ptr<viz::CompositorRenderPass> renderPass = viz::CompositorRenderPass::Create();
    renderPass->SetNew(kRenderPassId, outputRect, damageRect, gfx::Transform());

    //     renderPass->SetAll(kRenderPassId, outputRect, damageRect, gfx::Transform(),
    //         cc::FilterOperations(), cc::FilterOperations(), absl::optional<gfx::RRectF>(),
    //         viz::SubtreeCaptureId(), gfx::Size(), viz::SharedElementResourceId(), true /*has_transparent_background*/,
    //         false /*cache_render_pass*/, false /*has_damage_from_contributing_content*/, false /*generate_mipmap*/, false /*per_quad_damage*/);

    // Keep child geometry at Viz's uniform device-scale ratio. For a smaller
    // fallback, SurfaceAggregator fills only the uncovered gutters from the
    // active child's root_background_color, preserving translucent backgrounds.
    for (std::map<viz::FrameSinkId, EmbedInfo>::const_iterator iter = m_embeds.begin(); iter != m_embeds.end(); ++iter) {
        if (!g_canCommitFrame)
            continue;

        const gfx::Rect& childBounds = iter->second.bounds;
        const gfx::Vector2dF center(childBounds.width() / 2, childBounds.height() / 2);

        // Apply a rotation so there's visual-update every frame in the demo.
        gfx::Transform transform;
        //         transform.Translate(center + childBounds.OffsetFromOrigin());
        //         transform.Rotate(iter.second.degrees);
        //         iter.second.degrees += 0.3;
        //         transform.Translate(-center);

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

DWORD g_lastBeginFrameTime = 0;


void VizClient::OnBeginFrame(const ::viz::BeginFrameArgs& args, const base::flat_map<uint32_t, ::viz::FrameTimingDetails>& details, bool frame_ack,
    std::vector<::viz::ReturnedResource> resources)
{
    base::AutoLock lock(m_lock);

    if (!m_canCommitFrame)
        return;

    // Generate a new compositor-frame for each begin-frame. This demo client
    // generates and submits the compositor-frame immediately. But it is possible
    // for the client to delay sending the compositor-frame. |args| includes the
    // deadline for the client before it needs to submit the compositor-frame.
    viz::CompositorFrame frame = createFrame(args);
    viz::LocalSurfaceId localSurfaceId = m_hostLocalSurfaceId;
    const uint64_t frameRevision = m_frameRevision;


    m_vizCompositorRunner->PostTask(FROM_HERE,
        base::BindOnce(
            [](VizClient* self, viz::CompositorFrame frame, viz::LocalSurfaceId localSurfaceId, uint64_t frameRevision) {
                viz::mojom::CompositorFrameSink* ptr = self->getPtrImpl();
                // ���ߵ�SurfaceManager::GetOrCreateAllocationGroupForSurfaceId
                if (self->m_canCommitFrame && ptr) {
                    // Activation notifications can still be queued on Blink,
                    // which is waiting for this root acknowledgment. Query the
                    // service's actual active child before choosing fallback.
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
                                frame.metadata.referenced_surfaces[rangeIndex++] = quad->surface_range;
                            }
                        }
                    }
                    ptr->SubmitCompositorFrame(localSurfaceId, std::move(frame), absl::optional<viz::HitTestRegionList>(), /*trace_time=*/0);
                }
                else
                    self->allowResize();


                if (!self->getPtr())
                    return;

                // Child activation explicitly requests the cutover frame. Root
                // acknowledgments still release resize while fallback is drawn.
                base::AutoLock lock(self->m_lock);
                self->getPtr()->SetNeedsBeginFrame(self->m_frameRevision != frameRevision);
            },
            base::Unretained(this), std::move(frame), localSurfaceId, frameRevision));
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
