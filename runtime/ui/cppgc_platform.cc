// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "runtime/ui/cppgc_platform.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "v8/include/cppgc/platform.h"
#include "v8/src/base/page-allocator.h"

namespace mini_ui {
namespace {

using Clock = std::chrono::steady_clock;

double NowSeconds()
{
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

class ForegroundTasks final : public cppgc::TaskRunner {
public:
    ForegroundTasks() : thread_(std::this_thread::get_id()) {}

    bool IdleTasksEnabled() override { return false; }
    bool NonNestableTasksEnabled() const override { return true; }
    bool NonNestableDelayedTasksEnabled() const override { return true; }

    void RunDueTasks()
    {
        if (std::this_thread::get_id() != thread_)
            std::abort();
        if (running_)
            return;
        running_ = true;
        for (;;) {
            std::unique_ptr<cppgc::Task> task;
            {
                std::lock_guard guard(mutex_);
                if (tasks_.empty() || tasks_.front().due > NowSeconds())
                    break;
                std::pop_heap(tasks_.begin(), tasks_.end(), Later());
                task = std::move(tasks_.back().task);
                tasks_.pop_back();
            }
            task->Run();
        }
        running_ = false;
    }

protected:
    void PostTaskImpl(std::unique_ptr<cppgc::Task> task,
        const v8::SourceLocation&) override
    {
        Post(std::move(task), 0);
    }

    void PostNonNestableTaskImpl(std::unique_ptr<cppgc::Task> task,
        const v8::SourceLocation&) override
    {
        Post(std::move(task), 0);
    }

    void PostDelayedTaskImpl(std::unique_ptr<cppgc::Task> task, double delay,
        const v8::SourceLocation&) override
    {
        Post(std::move(task), delay);
    }

    void PostNonNestableDelayedTaskImpl(std::unique_ptr<cppgc::Task> task,
        double delay, const v8::SourceLocation&) override
    {
        Post(std::move(task), delay);
    }

private:
    struct ScheduledTask {
        double due;
        std::uint64_t sequence;
        std::unique_ptr<cppgc::Task> task;
    };

    struct Later {
        bool operator()(const ScheduledTask& a, const ScheduledTask& b) const
        {
            return a.due > b.due || (a.due == b.due && a.sequence > b.sequence);
        }
    };

    void Post(std::unique_ptr<cppgc::Task> task, double delay)
    {
        if (!task)
            std::abort();
        const double due = NowSeconds() + (delay > 0 ? delay : 0);
        std::lock_guard guard(mutex_);
        tasks_.push_back({ due, next_sequence_++, std::move(task) });
        std::push_heap(tasks_.begin(), tasks_.end(), Later());
    }

    const std::thread::id thread_;
    std::mutex mutex_;
    std::vector<ScheduledTask> tasks_;
    std::uint64_t next_sequence_ = 0;
    bool running_ = false;
};

class NativeGCPlatform final : public cppgc::Platform {
public:
    explicit NativeGCPlatform(cppgc::PageAllocator& page_allocator)
        : page_allocator_(page_allocator)
        , foreground_(std::make_shared<ForegroundTasks>())
    {
    }

    cppgc::PageAllocator* GetPageAllocator() override
    {
        return &page_allocator_;
    }

    double MonotonicallyIncreasingTime() override { return NowSeconds(); }

    std::shared_ptr<cppgc::TaskRunner> GetForegroundTaskRunner() override
    {
        return foreground_;
    }

    std::shared_ptr<cppgc::TaskRunner> GetForegroundTaskRunner(
        cppgc::TaskPriority) override
    {
        return foreground_;
    }

    void RunDueTasks() { foreground_->RunDueTasks(); }

private:
    cppgc::PageAllocator& page_allocator_;
    std::shared_ptr<ForegroundTasks> foreground_;
};

struct ProcessState {
    ProcessState()
    {
        cppgc::InitializeProcess(&page_allocator);
        platform = std::make_shared<NativeGCPlatform>(page_allocator);
    }

    ~ProcessState()
    {
        platform.reset();
        cppgc::ShutdownProcess();
    }

    v8::base::PageAllocator page_allocator;
    std::shared_ptr<NativeGCPlatform> platform;
};

ProcessState& Process()
{
    static ProcessState state;
    return state;
}

} // namespace

void InitializeCppgcProcess()
{
    static_cast<void>(Process());
}

std::shared_ptr<cppgc::Platform> GetCppgcPlatform()
{
    return Process().platform;
}

void RunCppgcTasks()
{
    Process().platform->RunDueTasks();
}

} // namespace mini_ui
