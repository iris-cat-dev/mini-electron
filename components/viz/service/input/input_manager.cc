// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/input/input_manager.h"

#include <utility>

#include "base/memory/scoped_refptr.h"
#include "base/metrics/histogram_macros.h"
#include "components/viz/service/input/render_input_router_delegate_impl.h"
#include "components/viz/service/input/render_input_router_iterator_impl.h"
#include "components/viz/service/input/render_input_router_support_child_frame.h"

namespace viz {

FrameSinkMetadata::FrameSinkMetadata(
    uint32_t grouping_id, std::unique_ptr<RenderInputRouterSupportBase> support, std::unique_ptr<RenderInputRouterDelegateImpl> delegate)
    : grouping_id(grouping_id)
    , rir_support(std::move(support))
    , rir_delegate(std::move(delegate))
{
}

FrameSinkMetadata::~FrameSinkMetadata() = default;

FrameSinkMetadata::FrameSinkMetadata(FrameSinkMetadata&& other) = default;
FrameSinkMetadata& FrameSinkMetadata::operator=(FrameSinkMetadata&& other) = default;

InputManager::~InputManager()
{
    frame_sink_manager_->RemoveObserver(this);
}

InputManager::InputManager(FrameSinkManagerImpl* frame_sink_manager)
    : frame_sink_manager_(frame_sink_manager)
{
    TRACE_EVENT("viz", "InputManager::InputManager");
    DCHECK(frame_sink_manager_);
    frame_sink_manager_->AddObserver(this);
}

void InputManager::OnCreateCompositorFrameSink(const FrameSinkId& frame_sink_id, bool is_root,
    input::mojom::RenderInputRouterConfigPtr render_input_router_config, bool /*create_input_receiver*/, gpu::SurfaceHandle /*surface_handle*/)
{
    TRACE_EVENT("viz", "InputManager::OnCreateCompositorFrameSink", "config_is_null", !render_input_router_config, "frame_sink_id", frame_sink_id);

    // `render_input_router_config` is non null only when layer tree frame sinks
    // for renderer are being requested.
    if (!render_input_router_config) {
        return;
    }

    DCHECK(render_input_router_config->rir_client.is_valid());
    DCHECK(input::IsTransferInputToVizSupported() && !is_root);

    uint32_t grouping_id = render_input_router_config->grouping_id;

    auto [it, inserted] = rwhier_map_.try_emplace(grouping_id, base::MakeRefCounted<input::RenderWidgetHostInputEventRouter>(frame_sink_manager_, this));

    if (inserted) {
        TRACE_EVENT_INSTANT("viz", "RenderWidgetHostInputEventRouterCreated", "grouping_id", grouping_id);
    }

    // |rir_delegate| should outlive |render_input_router|.
    auto rir_delegate = std::make_unique<RenderInputRouterDelegateImpl>(it->second, *this, frame_sink_id, grouping_id);

    auto render_input_router = std::make_unique<input::RenderInputRouter>(
        /* host */ nullptr,
        /* fling_scheduler */ nullptr,
        /* delegate */ rir_delegate.get(), base::SingleThreadTaskRunner::GetCurrentDefault());

    frame_sink_metadata_map_.emplace(std::make_pair(
        frame_sink_id, FrameSinkMetadata { grouping_id, MakeRenderInputRouterSupport(render_input_router.get(), frame_sink_id), std::move(rir_delegate) }));

    rir_map_.emplace(std::make_pair(frame_sink_id, std::move(render_input_router)));
}

void InputManager::OnDestroyedCompositorFrameSink(const FrameSinkId& frame_sink_id)
{
    TRACE_EVENT("viz", "InputManager::OnDestroyedCompositorFrameSink", "frame_sink_id", frame_sink_id);
    auto rir_iter = rir_map_.find(frame_sink_id);
    // Return early if |frame_sink_id| is associated with a non layer tree frame
    // sink.
    if (rir_iter == rir_map_.end()) {
        return;
    }

    rir_map_.erase(rir_iter);

    uint32_t grouping_id = frame_sink_metadata_map_.find(frame_sink_id)->second.grouping_id;
    // Deleting FrameSinkMetadata for |frame_sink_id| decreases the refcount for
    // RenderWidgetHostInputEventRouter in |rwhier_map_|(associated with the
    // RenderInputRouterDelegateImpl), for this |frame_sink_id|.
    frame_sink_metadata_map_.erase(frame_sink_id);

    auto it = rwhier_map_.find(grouping_id);
    if (it != rwhier_map_.end()) {
        if (it->second->HasOneRef()) {
            // There are no CompositorFrameSinks associated with this
            // RenderWidgetHostInputEventRouter, delete it.
            rwhier_map_.erase(it);
        }
    }
}

input::TouchEmulator* InputManager::GetTouchEmulator(bool create_if_necessary)
{
    return nullptr;
}

const DisplayHitTestQueryMap& InputManager::GetDisplayHitTestQuery() const
{
    return frame_sink_manager_->GetDisplayHitTestQuery();
}

float InputManager::GetDeviceScaleFactorForId(const FrameSinkId& frame_sink_id)
{
    auto* support = frame_sink_manager_->GetFrameSinkForId(frame_sink_id);
    CHECK(support);
    CHECK(support->GetLastActivatedFrameMetadata());
    return support->GetLastActivatedFrameMetadata()->device_scale_factor;
}

FrameSinkId InputManager::GetRootCompositorFrameSinkId(const FrameSinkId& child_frame_sink_id)
{
    return frame_sink_manager_->GetOldestRootCompositorFrameSinkId(child_frame_sink_id);
}

RenderInputRouterSupportBase* InputManager::GetParentRenderInputRouterSupport(const FrameSinkId& frame_sink_id)
{
    auto parent_id = frame_sink_manager_->GetOldestParentByChildFrameId(frame_sink_id);

    CHECK(!frame_sink_manager_->IsFrameSinkIdInRootSinkMap(parent_id));

    auto it = frame_sink_metadata_map_.find(parent_id);
    if (it != frame_sink_metadata_map_.end()) {
        return it->second.rir_support.get();
    }
    DUMP_WILL_BE_NOTREACHED();
    return nullptr;
}

RenderInputRouterSupportBase* InputManager::GetRootRenderInputRouterSupport(const FrameSinkId& frame_sink_id)
{
    auto parent_frame_sink_id = frame_sink_manager_->GetOldestParentByChildFrameId(frame_sink_id);
    FrameSinkId current_id = frame_sink_id;

    while (!frame_sink_manager_->IsFrameSinkIdInRootSinkMap(parent_frame_sink_id)) {
        current_id = parent_frame_sink_id;
        parent_frame_sink_id = frame_sink_manager_->GetOldestParentByChildFrameId(parent_frame_sink_id);
    }

    auto it = frame_sink_metadata_map_.find(current_id);
    if (it != frame_sink_metadata_map_.end()) {
        return it->second.rir_support.get();
    }

    DUMP_WILL_BE_NOTREACHED();
    return nullptr;
}

std::unique_ptr<input::RenderInputRouterIterator> InputManager::GetEmbeddedRenderInputRouters(const FrameSinkId& id)
{
    auto rirs = std::make_unique<RenderInputRouterIteratorImpl>(*this, frame_sink_manager_->GetChildrenByParent(id));
    return std::move(rirs);
}

void InputManager::NotifyObserversOfInputEvent(const FrameSinkId& frame_sink_id, uint32_t grouping_id, std::unique_ptr<blink::WebCoalescedInputEvent> event)
{
    rir_delegate_remote_map_.at(grouping_id)->NotifyObserversOfInputEvent(frame_sink_id, std::move(event));
}

void InputManager::NotifyObserversOfInputEventAcks(const FrameSinkId& frame_sink_id, uint32_t grouping_id, blink::mojom::InputEventResultSource ack_source,
    blink::mojom::InputEventResultState ack_result, std::unique_ptr<blink::WebCoalescedInputEvent> event)
{
    rir_delegate_remote_map_.at(grouping_id)->NotifyObserversOfInputEventAcks(frame_sink_id, ack_source, ack_result, std::move(event));
}

void InputManager::OnInvalidInputEventSource(const FrameSinkId& frame_sink_id, uint32_t grouping_id)
{
    rir_delegate_remote_map_.at(grouping_id)->OnInvalidInputEventSource(frame_sink_id);
}

void InputManager::SetupRenderInputRouterDelegateConnection(
    uint32_t grouping_id, mojo::PendingRemote<input::mojom::RenderInputRouterDelegateClient> rir_delegate_remote)
{
    rir_delegate_remote_map_[grouping_id].Bind(std::move(rir_delegate_remote));
    rir_delegate_remote_map_[grouping_id].set_disconnect_handler(
        base::BindOnce(&InputManager::OnRIRDelegateClientDisconnected, base::Unretained(this), grouping_id));
}

input::RenderInputRouter* InputManager::GetRenderInputRouterFromFrameSinkId(const FrameSinkId& id)
{
    return rir_map_[id].get();
}

std::unique_ptr<RenderInputRouterSupportBase> InputManager::MakeRenderInputRouterSupport(input::RenderInputRouter* rir, const FrameSinkId& frame_sink_id)
{
    TRACE_EVENT_INSTANT("input", "InputManager::MakeRenderInputRouterSupport");
    auto parent_id = frame_sink_manager_->GetOldestParentByChildFrameId(frame_sink_id);
    if (frame_sink_manager_->IsFrameSinkIdInRootSinkMap(parent_id)) {
        // InputVizard has no root-frame implementation on desktop.
        NOTREACHED();
    }
    return std::make_unique<RenderInputRouterSupportChildFrame>(rir, this, frame_sink_id);
}

void InputManager::OnRIRDelegateClientDisconnected(uint32_t grouping_id)
{
    rir_delegate_remote_map_.erase(grouping_id);
}

} // namespace viz
