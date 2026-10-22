// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/metrics/event_latency_tracing_recorder.h"

#include "base/feature_list.h"
#include "base/notreached.h"
#include "base/ranges/algorithm.h"
#include "base/time/time.h"
#include "base/trace_event/trace_id_helper.h"
#include "base/trace_event/typed_macros.h"
#include "base/tracing/protos/chrome_track_event.pbzero.h"
#include "cc/base/features.h"
#include "cc/metrics/event_metrics.h"
#include "third_party/perfetto/include/perfetto/tracing/track.h"

namespace cc {
namespace {

constexpr char kTracingCategory[] = "cc,benchmark,input,input.scrolling";

bool IsTracingEnabled()
{
    bool enabled;
    TRACE_EVENT_CATEGORY_GROUP_ENABLED(kTracingCategory, &enabled);
    return enabled;
}

constexpr base::TimeDelta high_latency_threshold = base::Milliseconds(90);

constexpr perfetto::protos::pbzero::EventLatency::EventType ToProtoEnum(EventMetrics::EventType event_type)
{
#define CASE(event_type, proto_event_type)                                                                                                                     \
    case EventMetrics::EventType::event_type:                                                                                                                  \
        return perfetto::protos::pbzero::EventLatency::proto_event_type
    switch (event_type) {
        CASE(kMousePressed, MOUSE_PRESSED);
        CASE(kMouseReleased, MOUSE_RELEASED);
        CASE(kMouseWheel, MOUSE_WHEEL);
        CASE(kKeyPressed, KEY_PRESSED);
        CASE(kKeyReleased, KEY_RELEASED);
        CASE(kTouchPressed, TOUCH_PRESSED);
        CASE(kTouchReleased, TOUCH_RELEASED);
        CASE(kTouchMoved, TOUCH_MOVED);
        CASE(kGestureScrollBegin, GESTURE_SCROLL_BEGIN);
        CASE(kGestureScrollUpdate, GESTURE_SCROLL_UPDATE);
        CASE(kGestureScrollEnd, GESTURE_SCROLL_END);
        CASE(kGestureDoubleTap, GESTURE_DOUBLE_TAP);
        CASE(kGestureLongPress, GESTURE_LONG_PRESS);
        CASE(kGestureLongTap, GESTURE_LONG_TAP);
        CASE(kGestureShowPress, GESTURE_SHOW_PRESS);
        CASE(kGestureTap, GESTURE_TAP);
        CASE(kGestureTapCancel, GESTURE_TAP_CANCEL);
        CASE(kGestureTapDown, GESTURE_TAP_DOWN);
        CASE(kGestureTapUnconfirmed, GESTURE_TAP_UNCONFIRMED);
        CASE(kGestureTwoFingerTap, GESTURE_TWO_FINGER_TAP);
        CASE(kFirstGestureScrollUpdate, FIRST_GESTURE_SCROLL_UPDATE);
        CASE(kMouseDragged, MOUSE_DRAGGED);
        CASE(kGesturePinchBegin, GESTURE_PINCH_BEGIN);
        CASE(kGesturePinchEnd, GESTURE_PINCH_END);
        CASE(kGesturePinchUpdate, GESTURE_PINCH_UPDATE);
        CASE(kInertialGestureScrollUpdate, INERTIAL_GESTURE_SCROLL_UPDATE);
        CASE(kMouseMoved, MOUSE_MOVED_EVENT);
    }
}

const char* GetVizBreakdownToPresentationName(CompositorFrameReporter::VizBreakdown breakdown)
{
    switch (breakdown) {
    case CompositorFrameReporter::VizBreakdown::kSwapStartToSwapEnd:
        return "SwapStartToPresentation";
    case CompositorFrameReporter::VizBreakdown::kLatchToSwapEnd:
        return "LatchToPresentation";
    default:
        base::UmaHistogramEnumeration("Compositing.VizBreakdownToPresentationUnexpected", breakdown);
        return "Unknown";
    }
}

} // namespace


// static
void EventLatencyTracingRecorder::RecordEventLatencyTraceEvent(EventMetrics* event_metrics, base::TimeTicks termination_time, base::TimeDelta vsync_interval,
    const std::vector<CompositorFrameReporter::StageData>* stage_history, const CompositorFrameReporter::ProcessedVizBreakdown* viz_breakdown)
{
    // As there are multiple teardown paths for EventMetrics, we want to denote
    // the attempt to trace, even if tracing is currently disabled.
    if (IsTracingEnabled()) {
        RecordEventLatencyTraceEventInternal(event_metrics, termination_time, vsync_interval, stage_history, viz_breakdown);
    }
    event_metrics->tracing_recorded();
}

// static
bool EventLatencyTracingRecorder::IsEventLatencyTracingEnabled()
{
    return IsTracingEnabled() || !base::FeatureList::IsEnabled(::features::kMetricsTracingCalculationReduction);
}

void EventLatencyTracingRecorder::RecordEventLatencyTraceEventInternal(const EventMetrics* event_metrics, base::TimeTicks termination_time,
    base::TimeDelta vsync_interval, const std::vector<CompositorFrameReporter::StageData>* stage_history,
    const CompositorFrameReporter::ProcessedVizBreakdown* viz_breakdown)
{
    DCHECK(event_metrics);
    DCHECK(event_metrics->should_record_tracing());

    const base::TimeTicks generated_timestamp = event_metrics->GetDispatchStageTimestamp(EventMetrics::DispatchStage::kGenerated);

    const auto trace_track = perfetto::Track(base::trace_event::GetNextGlobalTraceId());
    TRACE_EVENT_BEGIN(kTracingCategory, "EventLatency", trace_track, generated_timestamp, [&](perfetto::EventContext context) {
        auto* event = context.event<perfetto::protos::pbzero::ChromeTrackEvent>();
        auto* event_latency = event->set_event_latency();
        event_latency->set_event_type(ToProtoEnum(event_metrics->type()));
        bool has_high_latency = (termination_time - generated_timestamp) > high_latency_threshold;
        event_latency->set_has_high_latency(has_high_latency);
        for (auto stage : event_metrics->GetHighLatencyStages()) {
            // TODO(crbug.com/40228308): Consider changing the high_latency_stage
            // type from a string to enum type in chrome_track_event.proto,
            // similar to event_type.
            event_latency->add_high_latency_stage(stage);
        }
        if (event_metrics->trace_id().has_value()) {
            event_latency->set_event_latency_id(event_metrics->trace_id()->value());
        }

        const ScrollUpdateEventMetrics* scroll_update = event_metrics->AsScrollUpdate();
        if (scroll_update && scroll_update->is_janky_scrolled_frame().has_value()) {
            event_latency->set_is_janky_scrolled_frame(scroll_update->is_janky_scrolled_frame().value());
        }
        event_latency->set_vsync_interval_ms(vsync_interval.InMillisecondsF());
    });

    // Event dispatch stages.
    EventMetrics::DispatchStage dispatch_stage = EventMetrics::DispatchStage::kGenerated;
    base::TimeTicks dispatch_timestamp = event_metrics->GetDispatchStageTimestamp(dispatch_stage);
    while (dispatch_stage != EventMetrics::DispatchStage::kMaxValue) {
        DCHECK(!dispatch_timestamp.is_null());

        // Find the end dispatch stage.
        auto end_stage = static_cast<EventMetrics::DispatchStage>(static_cast<int>(dispatch_stage) + 1);
        base::TimeTicks end_timestamp = event_metrics->GetDispatchStageTimestamp(end_stage);
        while (end_timestamp.is_null() && end_stage != EventMetrics::DispatchStage::kMaxValue) {
            end_stage = static_cast<EventMetrics::DispatchStage>(static_cast<int>(end_stage) + 1);
            end_timestamp = event_metrics->GetDispatchStageTimestamp(end_stage);
        }
        if (end_timestamp.is_null())
            break;

        const char* breakdown_name = GetDispatchBreakdownName(dispatch_stage, end_stage);
        TRACE_EVENT_BEGIN(kTracingCategory, perfetto::StaticString { breakdown_name }, trace_track, dispatch_timestamp);
        TRACE_EVENT_END(kTracingCategory, trace_track, end_timestamp);

        dispatch_stage = end_stage;
        dispatch_timestamp = end_timestamp;
    }
    if (stage_history) {
        DCHECK(viz_breakdown);
        // Find the first compositor stage that starts at the same time or after the
        // end of the final event dispatch stage.
        auto stage_it = base::ranges::lower_bound(*stage_history, dispatch_timestamp, {}, &CompositorFrameReporter::StageData::start_time);
        // TODO(crbug.com/40843545): Ideally, at least the start time of
        // SubmitCompositorFrameToPresentationCompositorFrame stage should be
        // greater than or equal to the final event dispatch timestamp, but
        // apparently, this is not always the case (see crbug.com/1330903). Skip
        // recording compositor stages for now until we investigate the issue.
        if (stage_it != stage_history->end()) {
            DCHECK(dispatch_stage == EventMetrics::DispatchStage::kRendererCompositorFinished
                || dispatch_stage == EventMetrics::DispatchStage::kRendererMainFinished);

            // Record dispatch-to-compositor stage only if it has non-zero duration.
            if (dispatch_timestamp < stage_it->start_time) {
                const char* d2c_breakdown_name = GetDispatchToCompositorBreakdownName(dispatch_stage, stage_it->stage_type);
                TRACE_EVENT_BEGIN(kTracingCategory, perfetto::StaticString { d2c_breakdown_name }, trace_track, dispatch_timestamp);
                TRACE_EVENT_END(kTracingCategory, trace_track, stage_it->start_time);
            }

            // Compositor stages.
            for (; stage_it != stage_history->end(); ++stage_it) {
                if (stage_it->start_time >= termination_time)
                    break;
                DCHECK_GE(stage_it->end_time, stage_it->start_time);
                if (stage_it->start_time == stage_it->end_time)
                    continue;
                const char* stage_name = CompositorFrameReporter::GetStageName(stage_it->stage_type);

                TRACE_EVENT_BEGIN(kTracingCategory, perfetto::StaticString { stage_name }, trace_track, stage_it->start_time);

                if (stage_it->stage_type == CompositorFrameReporter::StageType::kSubmitCompositorFrameToPresentationCompositorFrame) {
                    DCHECK(viz_breakdown);
                    for (auto it = viz_breakdown->CreateIterator(true); it.IsValid(); it.Advance()) {
                        base::TimeTicks start_time = it.GetStartTime();
                        base::TimeTicks end_time = it.GetEndTime();

                        // Only record events with positive duration that start before
                        // termination.
                        // For example, in WebView, swap start time is the same as
                        // presentation time, and it wouldn't make sense to have a
                        // zero-duration `SwapStartToPresentation` event. As a result, the
                        // last stage for WebView is `StartDrawToSwapStart`.
                        //
                        // http://b/337195538 tracks a feature request for receiving
                        // presentation time in WebView, which should make it consistent
                        // with Chrome.
                        if (start_time >= end_time || start_time >= termination_time) {
                            continue;
                        }

                        CompositorFrameReporter::VizBreakdown breakdown = it.GetBreakdown();
                        const char* breakdown_name = nullptr;

                        if (end_time > termination_time) {
                            end_time = termination_time;
                            // A breakdown ending in swap-end can end after termination time
                            // (because swap-end is actually the time the post swap end
                            // callback is run, which can happen after presentation). In this
                            // case we truncate the breakdown to presentation.
                            DCHECK(breakdown == CompositorFrameReporter::VizBreakdown::kSwapStartToSwapEnd
                                || breakdown == CompositorFrameReporter::VizBreakdown::kLatchToSwapEnd);
                            breakdown_name = GetVizBreakdownToPresentationName(breakdown);
                        } else {
                            breakdown_name = CompositorFrameReporter::GetVizBreakdownName(breakdown);
                        }
                        TRACE_EVENT_BEGIN(kTracingCategory, perfetto::StaticString { breakdown_name }, trace_track, start_time);
                        TRACE_EVENT_END(kTracingCategory, trace_track, end_time);
                    }
                }

                TRACE_EVENT_END(kTracingCategory, trace_track, stage_it->end_time);
            }
        }
    } else {
        DCHECK(!viz_breakdown);
        const char* d2t_breakdown_name = GetDispatchToTerminationBreakdownName(dispatch_stage);
        TRACE_EVENT_BEGIN(kTracingCategory, perfetto::StaticString { d2t_breakdown_name }, trace_track, dispatch_timestamp);
        TRACE_EVENT_END(kTracingCategory, trace_track, termination_time);
    }
    TRACE_EVENT_END(kTracingCategory, trace_track, termination_time);
}

} // namespace cc
