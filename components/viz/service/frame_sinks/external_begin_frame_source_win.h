// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_VIZ_SERVICE_FRAME_SINKS_EXTERNAL_BEGIN_FRAME_SOURCE_WIN_H_
#define COMPONENTS_VIZ_SERVICE_FRAME_SINKS_EXTERNAL_BEGIN_FRAME_SOURCE_WIN_H_

#include <cstdint>

#include "base/containers/flat_set.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "components/viz/common/display/update_vsync_parameters_callback.h"
#include "components/viz/common/frame_sinks/begin_frame_source.h"
#include "components/viz/service/viz_service_export.h"

namespace viz {

// Generates begin frames from a validated vertical-blank signal for the
// selected monitor. Blocking DXGI calls run on a dedicated worker sequence.
// DWM timing (for the primary monitor) or a calibrated timer is used when the
// selected DXGI output does not provide a reliable blocking signal.
class VIZ_SERVICE_EXPORT ExternalBeginFrameSourceWin : public ExternalBeginFrameSource,
                                                       public ExternalBeginFrameSourceClient {
public:
    enum class WindowsVSyncClock {
        kUnknown,
        kDxgi,
        kDwm,
        kTimer,
    };

    struct WindowsVSyncInfo {
        WindowsVSyncClock source = WindowsVSyncClock::kUnknown;
        int64_t interval_us = 0;
        uint64_t ticks = 0;
        bool active = false;
        int64_t monitor_id = 0;
    };

    static WindowsVSyncInfo GetWindowsVSyncInfo(int64_t display_id);

    ExternalBeginFrameSourceWin(uint32_t restart_id, int64_t display_id,
        scoped_refptr<base::SequencedTaskRunner> task_runner);

    ExternalBeginFrameSourceWin(const ExternalBeginFrameSourceWin&) = delete;
    ExternalBeginFrameSourceWin& operator=(const ExternalBeginFrameSourceWin&) = delete;

    ~ExternalBeginFrameSourceWin() override;

    // BeginFrameSource implementation.
    BeginFrameArgs GetMissedBeginFrameArgs(BeginFrameObserver* obs) override;
    void SetPreferredInterval(base::TimeDelta interval) override;
    base::TimeDelta GetMinimumFrameInterval() override;
    base::flat_set<base::TimeDelta> GetSupportedFrameIntervals(
        base::TimeDelta interval) override;
    void SetVSyncDisplayID(int64_t display_id) override;
    void SetUpdateVSyncParametersCallback(
        UpdateVSyncParametersCallback callback) override;

    // ExternalBeginFrameSourceClient implementation.
    void OnNeedsBeginFrames(bool needs_begin_frames) override;

private:
    struct WorkerState;
    void RequestDisplayProbe();

    void OnDisplayProbeOnSequence(uint64_t generation,
        base::TimeDelta interval, bool output_available, long error,
        base::TimeTicks dwm_timebase, base::TimeDelta dwm_interval);

    void ScheduleNextVSync();
    void ScheduleFallbackTick(base::TimeTicks now);
    void OnVSyncOnSequence(uint64_t generation, base::TimeTicks vsync_time,
        base::TimeDelta interval, bool hardware, long error,
        base::TimeTicks dwm_timebase, base::TimeDelta dwm_interval);
    void OnFallbackTimer();
    void EnterFallback(base::TimeTicks dwm_timebase,
        base::TimeDelta dwm_interval);
    void DispatchVSync(base::TimeTicks vsync_time,
        base::TimeDelta vsync_interval, WindowsVSyncClock source);
    void UpdateNominalInterval(base::TimeTicks timebase,
        base::TimeDelta interval);
    void UpdateSubsamplingFactor();
    void PublishDiagnostics();

    scoped_refptr<base::SequencedTaskRunner> task_runner_;
    scoped_refptr<base::SequencedTaskRunner> worker_task_runner_;
    scoped_refptr<WorkerState> worker_state_;
    base::DeadlineTimer fallback_timer_;
    UpdateVSyncParametersCallback update_vsync_params_callback_;

    BeginFrameArgsGenerator begin_frame_args_generator_;

    int64_t display_id_;
    uint64_t display_generation_ = 1;
    uint64_t pending_wait_generation_ = 0;
    uint64_t published_generation_ = 0;
    bool needs_begin_frames_ = false;
    bool wait_pending_ = false;
    bool probe_pending_ = false;
    bool using_fallback_clock_ = false;
    WindowsVSyncClock fallback_clock_ = WindowsVSyncClock::kTimer;
    WindowsVSyncClock logged_clock_ = WindowsVSyncClock::kUnknown;
    WindowsVSyncClock current_clock_ = WindowsVSyncClock::kUnknown;
    uint64_t clock_ticks_ = 0;
    int vsync_subsampling_factor_ = 1;
    int vsyncs_to_skip_ = 0;
    base::TimeDelta nominal_refresh_period_ = BeginFrameArgs::DefaultInterval();
    base::TimeDelta preferred_interval_;
    base::TimeTicks fallback_timebase_;
    base::TimeTicks fallback_tick_time_;
    base::TimeTicks retry_hardware_after_;

    base::WeakPtrFactory<ExternalBeginFrameSourceWin> weak_factory_ { this };
};

} // namespace viz

#endif // COMPONENTS_VIZ_SERVICE_FRAME_SINKS_EXTERNAL_BEGIN_FRAME_SOURCE_WIN_H_
