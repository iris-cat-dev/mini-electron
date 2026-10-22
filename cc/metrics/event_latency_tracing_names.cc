// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/metrics/event_latency_tracing_recorder.h"

#include "base/notreached.h"

namespace cc {
// static
const char* EventLatencyTracingRecorder::GetDispatchBreakdownName(EventMetrics::DispatchStage start_stage, EventMetrics::DispatchStage end_stage)
{
    switch (start_stage) {
    case EventMetrics::DispatchStage::kGenerated:
        switch (end_stage) {
        case EventMetrics::DispatchStage::kScrollsBlockingTouchDispatchedToRenderer:
        case EventMetrics::DispatchStage::kArrivedInBrowserMain:
            return "GenerationToBrowserMain";
        case EventMetrics::DispatchStage::kArrivedInRendererCompositor:
            return "GenerationToRendererCompositor";
        default:
            NOTREACHED() << static_cast<int>(end_stage);
        }
    case EventMetrics::DispatchStage::kScrollsBlockingTouchDispatchedToRenderer:
        switch (end_stage) {
        case EventMetrics::DispatchStage::kArrivedInBrowserMain:
            // This stage can only be in a Scroll EventLatency. It means a path of
            // a corresponding blocking TouchMove from BrowserMain To Renderer To
            // BrowserMain. Look at the corresponding TouchMove EventLatency for
            // a more detailed breakdown of this stage.
            return "TouchRendererHandlingToBrowserMain";
        default:
            NOTREACHED() << static_cast<int>(end_stage);
        }
    case EventMetrics::DispatchStage::kArrivedInBrowserMain:
        DCHECK_EQ(end_stage, EventMetrics::DispatchStage::kArrivedInRendererCompositor);
        return "BrowserMainToRendererCompositor";
    case EventMetrics::DispatchStage::kArrivedInRendererCompositor:
        switch (end_stage) {
        case EventMetrics::DispatchStage::kRendererCompositorStarted:
            return "RendererCompositorQueueingDelay";
        case EventMetrics::DispatchStage::kRendererMainStarted:
            return "RendererCompositorToMain";
        default:
            NOTREACHED() << static_cast<int>(end_stage);
        }
    case EventMetrics::DispatchStage::kRendererCompositorStarted:
        DCHECK_EQ(end_stage, EventMetrics::DispatchStage::kRendererCompositorFinished);
        return "RendererCompositorProcessing";
    case EventMetrics::DispatchStage::kRendererCompositorFinished:
        DCHECK_EQ(end_stage, EventMetrics::DispatchStage::kRendererMainStarted);
        return "RendererCompositorToMain";
    case EventMetrics::DispatchStage::kRendererMainStarted:
        DCHECK_EQ(end_stage, EventMetrics::DispatchStage::kRendererMainFinished);
        return "RendererMainProcessing";
    case EventMetrics::DispatchStage::kRendererMainFinished:
        NOTREACHED();
    }
}

// static
const char* EventLatencyTracingRecorder::GetDispatchToCompositorBreakdownName(
    EventMetrics::DispatchStage dispatch_stage, CompositorFrameReporter::StageType compositor_stage)
{
    switch (dispatch_stage) {
    case EventMetrics::DispatchStage::kRendererCompositorFinished:
        switch (compositor_stage) {
        case CompositorFrameReporter::StageType::kBeginImplFrameToSendBeginMainFrame:
            return "RendererCompositorFinishedToBeginImplFrame";
        case CompositorFrameReporter::StageType::kSendBeginMainFrameToCommit:
            return "RendererCompositorFinishedToSendBeginMainFrame";
        case CompositorFrameReporter::StageType::kCommit:
            return "RendererCompositorFinishedToCommit";
        case CompositorFrameReporter::StageType::kEndCommitToActivation:
            return "RendererCompositorFinishedToEndCommit";
        case CompositorFrameReporter::StageType::kActivation:
            return "RendererCompositorFinishedToActivation";
        case CompositorFrameReporter::StageType::kEndActivateToSubmitCompositorFrame:
            return "RendererCompositorFinishedToEndActivate";
        case CompositorFrameReporter::StageType::kSubmitCompositorFrameToPresentationCompositorFrame:
            return "RendererCompositorFinishedToSubmitCompositorFrame";
        default:
            NOTREACHED() << "Invalid CC stage after compositor thread: " << static_cast<int>(compositor_stage);
        }
    case EventMetrics::DispatchStage::kRendererMainFinished:
        switch (compositor_stage) {
        case CompositorFrameReporter::StageType::kBeginImplFrameToSendBeginMainFrame:
            return "RendererMainFinishedToBeginImplFrame";
        case CompositorFrameReporter::StageType::kSendBeginMainFrameToCommit:
            return "RendererMainFinishedToSendBeginMainFrame";
        case CompositorFrameReporter::StageType::kCommit:
            return "RendererMainFinishedToCommit";
        case CompositorFrameReporter::StageType::kEndCommitToActivation:
            return "RendererMainFinishedToEndCommit";
        case CompositorFrameReporter::StageType::kActivation:
            return "RendererMainFinishedToActivation";
        case CompositorFrameReporter::StageType::kEndActivateToSubmitCompositorFrame:
            return "RendererMainFinishedToEndActivate";
        case CompositorFrameReporter::StageType::kSubmitCompositorFrameToPresentationCompositorFrame:
            return "RendererMainFinishedToSubmitCompositorFrame";
        default:
            NOTREACHED() << "Invalid CC stage after main thread: " << static_cast<int>(compositor_stage);
        }
    default:
        NOTREACHED();
    }
}

// static
const char* EventLatencyTracingRecorder::GetDispatchToTerminationBreakdownName(EventMetrics::DispatchStage dispatch_stage)
{
    switch (dispatch_stage) {
    case EventMetrics::DispatchStage::kArrivedInRendererCompositor:
        return "ArrivedInRendererCompositorToTermination";
    case EventMetrics::DispatchStage::kRendererCompositorStarted:
        return "RendererCompositorStartedToTermination";
    case EventMetrics::DispatchStage::kRendererCompositorFinished:
        return "RendererCompositorFinishedToTermination";
    case EventMetrics::DispatchStage::kRendererMainStarted:
        return "RendererMainStartedToTermination";
    case EventMetrics::DispatchStage::kRendererMainFinished:
        return "RendererMainFinishedToTermination";
    default:
        NOTREACHED();
    }
}

} // namespace cc
