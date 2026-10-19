// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/frame_sinks/external_begin_frame_source_win.h"

#include <windows.h>

#include <dwmapi.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/task/thread_pool.h"
#include "base/trace_event/trace_event.h"
#include "ui/display/types/display_constants.h"
#include "ui/display/win/display_config_helper.h"

namespace viz {

namespace {

constexpr base::TimeDelta kHardwareRetryDelay = base::Seconds(2);
constexpr base::TimeDelta kMaxSupportedFrameInterval = base::Hertz(14);
constexpr base::TimeDelta kIntervalTolerance = base::Microseconds(10);
constexpr int kDxgiValidationWaitCount = 3;
constexpr int kDxgiRequiredReliableWaits = 2;
constexpr base::TimeDelta kMinimumReliableWait = base::Microseconds(500);
constexpr base::TimeDelta kMaximumReliableWait = base::Milliseconds(2);
using WindowsVSyncInfo = ExternalBeginFrameSourceWin::WindowsVSyncInfo;

struct WindowsVSyncRegistry {
    base::Lock lock;
    std::map<const ExternalBeginFrameSourceWin*, WindowsVSyncInfo> entries;
};

WindowsVSyncRegistry& GetWindowsVSyncRegistry()
{
    static base::NoDestructor<WindowsVSyncRegistry> registry;
    return *registry;
}

void RemoveWindowsVSyncInfo(const ExternalBeginFrameSourceWin* source)
{
    WindowsVSyncRegistry& registry = GetWindowsVSyncRegistry();
    base::AutoLock lock(registry.lock);
    registry.entries.erase(source);
}


HMONITOR MonitorFromDisplayId(int64_t display_id)
{
    if (display_id == display::kInvalidDisplayId || display_id == 0) {
        return ::MonitorFromPoint(POINT { 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    }

    HMONITOR monitor = reinterpret_cast<HMONITOR>(
        static_cast<intptr_t>(display_id));
    MONITORINFO monitor_info = {};
    monitor_info.cbSize = sizeof(monitor_info);
    return ::GetMonitorInfoW(monitor, &monitor_info) ? monitor : nullptr;
}

base::TimeDelta GetMonitorRefreshInterval(HMONITOR monitor)
{
    if (!monitor)
        return BeginFrameArgs::DefaultInterval();

    if (auto path = display::win::GetDisplayConfigPathInfo(monitor)) {
        const DISPLAYCONFIG_RATIONAL& rate = path->targetInfo.refreshRate;
        if (rate.Numerator && rate.Denominator) {
            return base::Hertz(
                static_cast<double>(rate.Numerator) / rate.Denominator);
        }
    }

    MONITORINFOEXW monitor_info = {};
    monitor_info.cbSize = sizeof(monitor_info);
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (::GetMonitorInfoW(monitor, &monitor_info)
        && ::EnumDisplaySettingsW(
            monitor_info.szDevice, ENUM_CURRENT_SETTINGS, &mode)
        && mode.dmDisplayFrequency > 1) {
        return base::Hertz(static_cast<double>(mode.dmDisplayFrequency));
    }

    return BeginFrameArgs::DefaultInterval();
}

struct DwmTiming {
    base::TimeTicks timebase;
    base::TimeDelta interval;
};

base::TimeDelta MinimumReliableDxgiWait(base::TimeDelta interval)
{
    return std::clamp(interval / 4, kMinimumReliableWait,
        kMaximumReliableWait);
}

base::TimeDelta MinimumReliableDxgiCadence(base::TimeDelta interval)
{
    return std::max(interval * 3 / 4, kMinimumReliableWait);
}

DwmTiming GetDwmTiming(HMONITOR monitor,
    base::TimeDelta selected_monitor_interval)
{
    // DwmGetCompositionTimingInfo is global starting with Windows 8.1. Its
    // timing describes the primary-monitor composition clock on Windows 10,
    // so do not attribute it to another selected monitor.
    if (!monitor || monitor != ::MonitorFromPoint(
            POINT { 0, 0 }, MONITOR_DEFAULTTOPRIMARY)
        || !base::TimeTicks::IsHighResolution()) {
        return {};
    }

    DWM_TIMING_INFO timing_info = {};
    timing_info.cbSize = sizeof(timing_info);
    if (::DwmGetCompositionTimingInfo(nullptr, &timing_info) != S_OK
        || timing_info.qpcVBlank == 0
        || timing_info.qpcVBlank
            > static_cast<ULONGLONG>(std::numeric_limits<LONGLONG>::max())
        || timing_info.qpcRefreshPeriod
            > static_cast<ULONGLONG>(std::numeric_limits<LONGLONG>::max())) {
        return {};
    }

    base::TimeDelta rate_interval;
    if (timing_info.rateRefresh.uiNumerator
        && timing_info.rateRefresh.uiDenominator) {
        rate_interval = base::Hertz(
            static_cast<double>(timing_info.rateRefresh.uiNumerator)
            / timing_info.rateRefresh.uiDenominator);
    }

    base::TimeDelta interval;
    if (timing_info.qpcRefreshPeriod) {
        interval = base::TimeDelta::FromQPCValue(
            static_cast<LONGLONG>(timing_info.qpcRefreshPeriod));
    }
    if (interval < base::Milliseconds(1)
        || (!rate_interval.is_zero() && interval < rate_interval / 2)) {
        interval = rate_interval;
    }
    if (!interval.is_positive() || interval > base::Seconds(1))
        return {};

    // On newer mixed-refresh systems DWM can run at the fastest display
    // rather than the primary display. A mismatched clock is not a selected
    // monitor signal, even when the DWM query itself succeeds.
    if (selected_monitor_interval.is_positive()
        && (interval - selected_monitor_interval).magnitude()
            > std::max(base::Milliseconds(1),
                selected_monitor_interval / 5)) {
        return {};
    }

    const base::TimeTicks timebase = base::TimeTicks::FromQPCValue(
        static_cast<LONGLONG>(timing_info.qpcVBlank));
    const base::TimeTicks now = base::TimeTicks::Now();
    if (timebase.is_null() || timebase > now + interval
        || timebase < now - base::Seconds(1)) {
        return {};
    }

    return { timebase, interval };
}

} // namespace

struct ExternalBeginFrameSourceWin::WorkerState
    : public base::RefCountedThreadSafe<WorkerState> {
    WorkerState() = default;

    void Probe(scoped_refptr<base::SequencedTaskRunner> result_runner,
        base::WeakPtr<ExternalBeginFrameSourceWin> source,
        uint64_t requested_generation, int64_t display_id)
    {
        base::TimeDelta refresh_interval;
        const HRESULT result =
            Configure(requested_generation, display_id, &refresh_interval);
        const DwmTiming dwm_timing =
            GetDwmTiming(monitor, refresh_interval);
        result_runner->PostTask(
            FROM_HERE,
            base::BindOnce(
                &ExternalBeginFrameSourceWin::OnDisplayProbeOnSequence,
                std::move(source), requested_generation, refresh_interval,
                SUCCEEDED(result), static_cast<long>(result),
                dwm_timing.timebase, dwm_timing.interval));
    }

    void WaitForVSync(
        scoped_refptr<base::SequencedTaskRunner> result_runner,
        base::WeakPtr<ExternalBeginFrameSourceWin> source,
        uint64_t requested_generation, int64_t display_id)
    {
        base::TimeDelta refresh_interval;
        HRESULT result =
            Configure(requested_generation, display_id, &refresh_interval);
        base::TimeTicks now;
        if (SUCCEEDED(result)) {
            const base::TimeDelta minimum_reliable_wait =
                MinimumReliableDxgiWait(refresh_interval);
            const base::TimeDelta minimum_reliable_cadence =
                MinimumReliableDxgiCadence(refresh_interval);
            if (!output_validated) {
                int reliable_waits = 0;
                for (int wait = 0; wait < kDxgiValidationWaitCount; ++wait) {
                    const base::TimeTicks before_wait =
                        base::TimeTicks::Now();
                    result = output->WaitForVBlank();
                    now = base::TimeTicks::Now();
                    if (FAILED(result))
                        break;
                    if (now - before_wait >= minimum_reliable_wait)
                        ++reliable_waits;
                }
                if (SUCCEEDED(result)
                    && reliable_waits < kDxgiRequiredReliableWaits) {
                    result = DXGI_ERROR_UNSUPPORTED;
                } else if (SUCCEEDED(result)) {
                    output_validated = true;
                }
            } else {
                result = output->WaitForVBlank();
                now = base::TimeTicks::Now();

                // A successful return that is too close to the previous
                // signal may be the same vblank. Confirm it with one more
                // wait. Two immediate returns prove that this output is not
                // providing the blocking semantics promised by DXGI.
                if (SUCCEEDED(result) && !last_vblank_time.is_null()
                    && now - last_vblank_time
                        < minimum_reliable_cadence) {
                    result = output->WaitForVBlank();
                    now = base::TimeTicks::Now();
                    if (SUCCEEDED(result)
                        && now - last_vblank_time
                            < minimum_reliable_cadence) {
                        result = DXGI_ERROR_UNSUPPORTED;
                    }
                }
            }
        }

        if (now.is_null())
            now = base::TimeTicks::Now();
        if (FAILED(result)) {
            output.Reset();
            output_validated = false;
            last_vblank_time = base::TimeTicks();
        } else {
            last_vblank_time = now;
        }

        const DwmTiming dwm_timing =
            GetDwmTiming(monitor, refresh_interval);
        result_runner->PostTask(
            FROM_HERE,
            base::BindOnce(
                &ExternalBeginFrameSourceWin::OnVSyncOnSequence,
                std::move(source), requested_generation, now,
                refresh_interval, SUCCEEDED(result),
                static_cast<long>(result), dwm_timing.timebase,
                dwm_timing.interval));
    }

private:
    friend class base::RefCountedThreadSafe<WorkerState>;
    ~WorkerState() = default;

    HRESULT Configure(uint64_t requested_generation, int64_t display_id,
        base::TimeDelta* refresh_interval)
    {
        if (generation != requested_generation) {
            generation = requested_generation;
            monitor = MonitorFromDisplayId(display_id);
            interval = GetMonitorRefreshInterval(monitor);
            output.Reset();
            factory.Reset();
            output_validated = false;
            last_vblank_time = base::TimeTicks();
        }
        *refresh_interval = interval;

        if (output)
            return S_OK;
        if (!monitor)
            return E_HANDLE;
        factory.Reset();

        HRESULT result = ::CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(result))
            return result;

        for (UINT adapter_index = 0;; ++adapter_index) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            result = factory->EnumAdapters1(adapter_index, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(result))
                return result;

            for (UINT output_index = 0;; ++output_index) {
                Microsoft::WRL::ComPtr<IDXGIOutput> candidate;
                result = adapter->EnumOutputs(output_index, &candidate);
                if (result == DXGI_ERROR_NOT_FOUND)
                    break;
                if (FAILED(result))
                    return result;

                DXGI_OUTPUT_DESC description = {};
                result = candidate->GetDesc(&description);
                if (FAILED(result))
                    return result;
                if (description.AttachedToDesktop
                    && description.Monitor == monitor) {
                    output = std::move(candidate);
                    return S_OK;
                }
            }
        }
        return DXGI_ERROR_NOT_FOUND;
    }

    uint64_t generation = 0;
    HMONITOR monitor = nullptr;
    base::TimeDelta interval = BeginFrameArgs::DefaultInterval();
    bool output_validated = false;
    base::TimeTicks last_vblank_time;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
};
// static
ExternalBeginFrameSourceWin::WindowsVSyncInfo
ExternalBeginFrameSourceWin::GetWindowsVSyncInfo(int64_t display_id)
{
    WindowsVSyncInfo result;
    result.monitor_id = display_id;
    bool found = false;

    WindowsVSyncRegistry& registry = GetWindowsVSyncRegistry();
    base::AutoLock lock(registry.lock);
    for (const auto& entry : registry.entries) {
        const WindowsVSyncInfo& info = entry.second;
        if (info.monitor_id != display_id)
            continue;
        if (!found || (info.active && !result.active)
            || (info.active == result.active && info.ticks > result.ticks)) {
            result = info;
            found = true;
        }
    }
    return result;
}


ExternalBeginFrameSourceWin::ExternalBeginFrameSourceWin(
    uint32_t restart_id, int64_t display_id,
    scoped_refptr<base::SequencedTaskRunner> task_runner)
    : ExternalBeginFrameSource(this, restart_id)
    , task_runner_(std::move(task_runner))
    , worker_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          { base::MayBlock(), base::TaskPriority::USER_BLOCKING,
              base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN }))
    , worker_state_(base::MakeRefCounted<WorkerState>())
    , display_id_(display_id)
{
    DCHECK(task_runner_);
    DCHECK(worker_task_runner_);
    PublishDiagnostics();
}

ExternalBeginFrameSourceWin::~ExternalBeginFrameSourceWin()
{
    DCHECK(task_runner_->RunsTasksInCurrentSequence());
    needs_begin_frames_ = false;
    fallback_timer_.Stop();
    RemoveWindowsVSyncInfo(this);
    weak_factory_.InvalidateWeakPtrs();
}


void ExternalBeginFrameSourceWin::RequestDisplayProbe()
{
    DCHECK(task_runner_->RunsTasksInCurrentSequence());
    if (probe_pending_)
        return;

    probe_pending_ = true;
    const uint64_t generation = display_generation_;
    if (!worker_task_runner_->PostTask(
            FROM_HERE,
            base::BindOnce(&WorkerState::Probe, worker_state_, task_runner_,
                weak_factory_.GetWeakPtr(), generation, display_id_))) {
        probe_pending_ = false;
        EnterFallback(base::TimeTicks(), base::TimeDelta());
        retry_hardware_after_ = base::TimeTicks::Max();
        ScheduleNextVSync();
    }
}

void ExternalBeginFrameSourceWin::OnDisplayProbeOnSequence(
    uint64_t generation, base::TimeDelta interval, bool output_available,
    long error, base::TimeTicks dwm_timebase, base::TimeDelta dwm_interval)
{
    DCHECK(task_runner_->RunsTasksInCurrentSequence());
    probe_pending_ = false;
    if (generation != display_generation_) {
        RequestDisplayProbe();
        return;
    }

    UpdateNominalInterval(base::TimeTicks::Now(), interval);
    VLOG(1) << "Windows VSync monitor=" << display_id_
            << " interval=" << interval.InMicroseconds()
            << "us DXGI-output=" << output_available
            << " status=" << error;
    if (!output_available) {
        const bool was_using_fallback = using_fallback_clock_;
        EnterFallback(dwm_timebase, dwm_interval);
        retry_hardware_after_ = base::TimeTicks::Now() + kHardwareRetryDelay;
        if (!was_using_fallback) {
            LOG(WARNING) << "Windows VSync monitor=" << display_id_
                         << " has no available DXGI output (status=" << error
                         << "); using "
                         << (fallback_clock_ == WindowsVSyncClock::kDwm
                                 ? "DWM"
                                 : "calibrated timer")
                         << " interval="
                         << nominal_refresh_period_.InMicroseconds() << "us";
        }
    } else {
        using_fallback_clock_ = false;
        retry_hardware_after_ = base::TimeTicks();
    }
    PublishDiagnostics();

    ScheduleNextVSync();
}

void ExternalBeginFrameSourceWin::ScheduleFallbackTick(
    base::TimeTicks now)
{
    DCHECK(task_runner_->RunsTasksInCurrentSequence());
    if (fallback_timebase_.is_null())
        fallback_timebase_ = now;

    fallback_tick_time_ =
        now.SnappedToNextTick(fallback_timebase_, nominal_refresh_period_);
    if (fallback_tick_time_ <= now)
        fallback_tick_time_ += nominal_refresh_period_;

    fallback_timer_.Start(
        FROM_HERE, fallback_tick_time_,
        base::BindOnce(&ExternalBeginFrameSourceWin::OnFallbackTimer,
            weak_factory_.GetWeakPtr()),
        base::subtle::DelayPolicy::kPrecise);
}

void ExternalBeginFrameSourceWin::ScheduleNextVSync()
{
    DCHECK(task_runner_->RunsTasksInCurrentSequence());
    if (!needs_begin_frames_ || wait_pending_
        || fallback_timer_.IsRunning()) {
        return;
    }

    const base::TimeTicks now = base::TimeTicks::Now();
    if (using_fallback_clock_ && now < retry_hardware_after_) {
        ScheduleFallbackTick(now);
        return;
    }

    wait_pending_ = true;
    pending_wait_generation_ = display_generation_;
    if (!worker_task_runner_->PostTask(
            FROM_HERE,
            base::BindOnce(&WorkerState::WaitForVSync, worker_state_,
                task_runner_, weak_factory_.GetWeakPtr(),
                pending_wait_generation_, display_id_))) {
        wait_pending_ = false;
        EnterFallback(base::TimeTicks(), base::TimeDelta());
        retry_hardware_after_ = base::TimeTicks::Max();
        ScheduleNextVSync();
    }
}

void ExternalBeginFrameSourceWin::OnVSyncOnSequence(
    uint64_t generation, base::TimeTicks vsync_time,
    base::TimeDelta interval, bool hardware, long error,
    base::TimeTicks dwm_timebase, base::TimeDelta dwm_interval)
{
    DCHECK(task_runner_->RunsTasksInCurrentSequence());
    if (pending_wait_generation_ == generation)
        wait_pending_ = false;
    if (generation != display_generation_) {
        ScheduleNextVSync();
        return;
    }
    if (!needs_begin_frames_)
        return;

    if (hardware) {
        if (current_clock_ != WindowsVSyncClock::kDxgi)
            published_generation_ = 0;
        UpdateNominalInterval(vsync_time, interval);
        using_fallback_clock_ = false;
        retry_hardware_after_ = base::TimeTicks();
        DispatchVSync(vsync_time, nominal_refresh_period_,
            WindowsVSyncClock::kDxgi);
    } else {
        const bool was_using_fallback = using_fallback_clock_;
        EnterFallback(dwm_timebase, dwm_interval);
        retry_hardware_after_ = base::TimeTicks::Now() + kHardwareRetryDelay;
        if (!was_using_fallback) {
            LOG(WARNING) << "Windows VSync monitor=" << display_id_
                         << " DXGI wait is unavailable or non-blocking"
                         << " (status=" << error << "); using "
                         << (fallback_clock_ == WindowsVSyncClock::kDwm
                                 ? "DWM"
                                 : "calibrated timer")
                         << " interval="
                         << nominal_refresh_period_.InMicroseconds() << "us";
        }
    }

    ScheduleNextVSync();
}

void ExternalBeginFrameSourceWin::EnterFallback(
    base::TimeTicks dwm_timebase, base::TimeDelta dwm_interval)
{
    DCHECK(task_runner_->RunsTasksInCurrentSequence());
    fallback_timer_.Stop();
    fallback_tick_time_ = base::TimeTicks();
    using_fallback_clock_ = true;

    if (!dwm_timebase.is_null() && dwm_interval.is_positive()) {
        fallback_clock_ = WindowsVSyncClock::kDwm;
        fallback_timebase_ = dwm_timebase;
        if (current_clock_ != WindowsVSyncClock::kDwm)
            published_generation_ = 0;
        UpdateNominalInterval(dwm_timebase, dwm_interval);
    } else {
        fallback_clock_ = WindowsVSyncClock::kTimer;
        fallback_timebase_ = base::TimeTicks::Now();
        if (current_clock_ != WindowsVSyncClock::kTimer)
            published_generation_ = 0;
        UpdateNominalInterval(
            fallback_timebase_, nominal_refresh_period_);
    }
    current_clock_ = fallback_clock_;
    PublishDiagnostics();
}

void ExternalBeginFrameSourceWin::OnFallbackTimer()
{
    DCHECK(task_runner_->RunsTasksInCurrentSequence());
    if (!needs_begin_frames_)
        return;
    if (fallback_tick_time_.is_null()) {
        ScheduleNextVSync();
        return;
    }

    const base::TimeTicks now = base::TimeTicks::Now();
    if (now < fallback_tick_time_) {
        ScheduleFallbackTick(now);
        return;
    }

    base::TimeTicks tick_time = fallback_tick_time_;
    if (tick_time + nominal_refresh_period_ <= now) {
        tick_time =
            now.SnappedToNextTick(
                fallback_timebase_, nominal_refresh_period_)
            - nominal_refresh_period_;
    }
    fallback_tick_time_ = base::TimeTicks();
    DispatchVSync(tick_time, nominal_refresh_period_, fallback_clock_);
    ScheduleNextVSync();
}

void ExternalBeginFrameSourceWin::DispatchVSync(
    base::TimeTicks vsync_time, base::TimeDelta vsync_interval,
    WindowsVSyncClock source)
{
    if (logged_clock_ != source) {
        const char* source_name = "timer";
        if (source == WindowsVSyncClock::kDxgi)
            source_name = "DXGI";
        else if (source == WindowsVSyncClock::kDwm)
            source_name = "DWM";

        if (source == WindowsVSyncClock::kTimer) {
            LOG(WARNING) << "Windows VSync monitor=" << display_id_
                         << " clock=" << source_name << " interval="
                         << vsync_interval.InMicroseconds() << "us";
        } else {
            LOG(INFO) << "Windows VSync monitor=" << display_id_
                      << " clock=" << source_name << " interval="
                      << vsync_interval.InMicroseconds() << "us";
        }
        logged_clock_ = source;
    }
    TRACE_EVENT2("viz", "ExternalBeginFrameSourceWin::VSync",
        "source", static_cast<int>(source), "interval_us",
        vsync_interval.InMicroseconds());
    current_clock_ = source;
    ++clock_ticks_;
    PublishDiagnostics();

    if (last_begin_frame_args_.IsValid()
        && vsync_time <= last_begin_frame_args_.frame_time) {
        return;
    }
    if (vsyncs_to_skip_ > 0) {
        --vsyncs_to_skip_;
        return;
    }

    vsyncs_to_skip_ = vsync_subsampling_factor_ - 1;
    const base::TimeDelta begin_frame_interval =
        vsync_interval * vsync_subsampling_factor_;
    ExternalBeginFrameSource::OnBeginFrame(
        begin_frame_args_generator_.GenerateBeginFrameArgs(
            source_id(), vsync_time, vsync_time + begin_frame_interval,
            begin_frame_interval));
}

void ExternalBeginFrameSourceWin::UpdateNominalInterval(
    base::TimeTicks timebase, base::TimeDelta interval)
{
    if (!interval.is_positive() || interval > base::Seconds(1))
        interval = BeginFrameArgs::DefaultInterval();

    const bool changed = nominal_refresh_period_ != interval;
    nominal_refresh_period_ = interval;
    if (changed)
        UpdateSubsamplingFactor();

    if (update_vsync_params_callback_
        && (changed || published_generation_ != display_generation_)) {
        published_generation_ = display_generation_;
        update_vsync_params_callback_.Run(timebase, interval);
    }
    if (changed)
        PublishDiagnostics();
}

BeginFrameArgs ExternalBeginFrameSourceWin::GetMissedBeginFrameArgs(
    BeginFrameObserver* obs)
{
    auto frame_time = last_begin_frame_args_.frame_time;
    auto interval = last_begin_frame_args_.interval;
    const base::TimeTicks now = base::TimeTicks::Now();

    if (last_begin_frame_args_.IsValid()) {
        frame_time =
            now.SnappedToNextTick(frame_time, interval) - interval;
    } else {
        frame_time = now;
        interval =
            nominal_refresh_period_ * vsync_subsampling_factor_;
    }

    if (!last_begin_frame_args_.IsValid()
        || frame_time > last_begin_frame_args_.frame_time) {
        last_begin_frame_args_ =
            begin_frame_args_generator_.GenerateBeginFrameArgs(
                source_id(), frame_time, frame_time + interval, interval);
    }

    return ExternalBeginFrameSource::GetMissedBeginFrameArgs(obs);
}

void ExternalBeginFrameSourceWin::SetPreferredInterval(
    base::TimeDelta interval)
{
    DCHECK(task_runner_->RunsTasksInCurrentSequence());
    preferred_interval_ = interval;
    UpdateSubsamplingFactor();
}

void ExternalBeginFrameSourceWin::UpdateSubsamplingFactor()
{
    int factor = 1;
    if (preferred_interval_.is_positive()
        && preferred_interval_ > nominal_refresh_period_) {
        const base::TimeDelta adjusted =
            std::min(preferred_interval_, kMaxSupportedFrameInterval);
        const base::TimeDelta divisor =
            nominal_refresh_period_ > kIntervalTolerance * 2
            ? nominal_refresh_period_ - kIntervalTolerance
            : nominal_refresh_period_;
        factor = static_cast<int>(
            std::max<int64_t>(1, adjusted.IntDiv(divisor)));
    }
    if (vsync_subsampling_factor_ == factor)
        return;

    vsync_subsampling_factor_ = factor;
    vsyncs_to_skip_ = 0;
    TRACE_EVENT1("viz",
        "ExternalBeginFrameSourceWin::SetPreferredInterval",
        "subsampling_factor", factor);
}
void ExternalBeginFrameSourceWin::PublishDiagnostics()
{
    WindowsVSyncInfo info;
    info.source = current_clock_;
    info.interval_us = nominal_refresh_period_.InMicroseconds();
    info.ticks = clock_ticks_;
    info.active = needs_begin_frames_;
    info.monitor_id = display_id_;

    WindowsVSyncRegistry& registry = GetWindowsVSyncRegistry();
    base::AutoLock lock(registry.lock);
    registry.entries[this] = info;
}


base::TimeDelta ExternalBeginFrameSourceWin::GetMinimumFrameInterval()
{
    return nominal_refresh_period_;
}

base::flat_set<base::TimeDelta>
ExternalBeginFrameSourceWin::GetSupportedFrameIntervals(
    base::TimeDelta /*interval*/)
{
    base::flat_set<base::TimeDelta> supported_intervals;
    base::TimeDelta supported = nominal_refresh_period_;
    while (supported <= kMaxSupportedFrameInterval) {
        supported_intervals.insert(supported);
        supported *= 2;
    }
    if (supported_intervals.empty())
        supported_intervals.insert(nominal_refresh_period_);
    return supported_intervals;
}

void ExternalBeginFrameSourceWin::OnNeedsBeginFrames(
    bool needs_begin_frames)
{
    DCHECK(task_runner_->RunsTasksInCurrentSequence());
    if (needs_begin_frames_ == needs_begin_frames)
        return;

    needs_begin_frames_ = needs_begin_frames;
    vsyncs_to_skip_ = 0;
    if (!needs_begin_frames_) {
        fallback_timer_.Stop();
        fallback_tick_time_ = base::TimeTicks();
    }
    PublishDiagnostics();
    if (needs_begin_frames_)
        ScheduleNextVSync();
}

void ExternalBeginFrameSourceWin::SetVSyncDisplayID(int64_t display_id)
{
    DCHECK(task_runner_->RunsTasksInCurrentSequence());
    display_id_ = display_id;
    ++display_generation_;
    published_generation_ = 0;
    using_fallback_clock_ = false;
    fallback_clock_ = WindowsVSyncClock::kTimer;
    logged_clock_ = WindowsVSyncClock::kUnknown;
    current_clock_ = WindowsVSyncClock::kUnknown;
    clock_ticks_ = 0;
    retry_hardware_after_ = base::TimeTicks();
    fallback_timebase_ = base::TimeTicks();
    fallback_tick_time_ = base::TimeTicks();
    vsyncs_to_skip_ = 0;
    fallback_timer_.Stop();
    PublishDiagnostics();

    VLOG(1) << "Windows VSync selecting monitor=" << display_id_;
    RequestDisplayProbe();
    ScheduleNextVSync();
}

void ExternalBeginFrameSourceWin::SetUpdateVSyncParametersCallback(
    UpdateVSyncParametersCallback callback)
{
    DCHECK(task_runner_->RunsTasksInCurrentSequence());
    update_vsync_params_callback_ = std::move(callback);
    RequestDisplayProbe();
}

} // namespace viz
