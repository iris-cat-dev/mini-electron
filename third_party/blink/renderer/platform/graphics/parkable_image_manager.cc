// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/graphics/parkable_image_manager.h"

#include "base/metrics/histogram_functions.h"
#include "base/not_fatal_until.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/trace_event/process_memory_dump.h"
#include "third_party/blink/renderer/platform/graphics/parkable_image.h"
#include "third_party/blink/renderer/platform/scheduler/public/main_thread.h"
#include "third_party/blink/renderer/platform/wtf/wtf.h"

namespace blink {

struct ParkableImageManager::Statistics {
    size_t unparked_size = 0;
#if !defined(MINI_UI_QUICKJS)
    size_t on_disk_size = 0;
#endif
    size_t total_size = 0;
};

constexpr const char* ParkableImageManager::kAllocatorDumpName;

#if !defined(MINI_UI_QUICKJS)
constexpr base::TimeDelta ParkableImageManager::kDelayedParkingInterval;
#endif

// static
ParkableImageManager& ParkableImageManager::Instance()
{
    static base::NoDestructor<ParkableImageManager> instance;
    return *instance;
}

ParkableImageManager::ParkableImageManager()
    : task_runner_(Thread::MainThread()->GetTaskRunner(MainThreadTaskRunnerRestricted()))
{
}

void ParkableImageManager::SetTaskRunnerForTesting(scoped_refptr<base::SingleThreadTaskRunner> task_runner)
{
    DCHECK(task_runner);
    task_runner_ = std::move(task_runner);
}

bool ParkableImageManager::OnMemoryDump(const base::trace_event::MemoryDumpArgs&, base::trace_event::ProcessMemoryDump* pmd)
{
    auto* dump = pmd->CreateAllocatorDump(kAllocatorDumpName);

    base::AutoLock lock(lock_);
    Statistics stats = ComputeStatistics();

    dump->AddScalar("total_size", "bytes", stats.total_size);
    dump->AddScalar("unparked_size", "bytes", stats.unparked_size);
#if !defined(MINI_UI_QUICKJS)
    dump->AddScalar("on_disk_size", "bytes", stats.on_disk_size);
#endif

    return true;
}

ParkableImageManager::Statistics ParkableImageManager::ComputeStatistics() const
{
    Statistics stats;

    for (auto* unparked : unparked_images_)
        stats.unparked_size += unparked->size();

#if !defined(MINI_UI_QUICKJS)
    for (auto* on_disk : on_disk_images_)
        stats.on_disk_size += on_disk->size();

    stats.total_size = stats.on_disk_size + stats.unparked_size;
#else
    stats.total_size = stats.unparked_size;
#endif

    return stats;
}

size_t ParkableImageManager::Size() const
{
    base::AutoLock lock(lock_);

#if defined(MINI_UI_QUICKJS)
    return unparked_images_.size();
#else
    return on_disk_images_.size() + unparked_images_.size();
#endif
}

#if !defined(MINI_UI_QUICKJS)
DiskDataAllocator& ParkableImageManager::data_allocator() const
{
    if (allocator_for_testing_)
        return *allocator_for_testing_;

    return DiskDataAllocator::Instance();
}
#endif

void ParkableImageManager::ResetForTesting()
{
    base::AutoLock lock(lock_);

#if !defined(MINI_UI_QUICKJS)
    has_pending_parking_task_ = false;
#endif
    has_posted_accounting_task_ = false;
    unparked_images_.clear();
#if !defined(MINI_UI_QUICKJS)
    on_disk_images_.clear();
    allocator_for_testing_ = nullptr;
    total_disk_read_time_ = base::TimeDelta();
    total_disk_write_time_ = base::TimeDelta();
#endif
}

void ParkableImageManager::Add(ParkableImageImpl* impl)
{
    DCHECK(IsMainThread());
#if DCHECK_IS_ON()
    {
        base::AutoLock lock(impl->lock_);
        DCHECK(!IsRegistered(impl));
    }
#endif // DCHECK_IS_ON()

    base::AutoLock lock(lock_);

#if !defined(MINI_UI_QUICKJS)
    ScheduleDelayedParkingTaskIfNeeded();
#endif

    if (!has_posted_accounting_task_) {
        // |base::Unretained(this)| is fine because |this| is a NoDestructor
        // singleton.
        task_runner_->PostDelayedTask(
            FROM_HERE, base::BindOnce(&ParkableImageManager::RecordStatisticsAfter5Minutes, base::Unretained(this)), base::Minutes(5));
        has_posted_accounting_task_ = true;
    }

    unparked_images_.insert(impl);
}

void ParkableImageManager::RecordStatisticsAfter5Minutes() const
{
    DCHECK(IsMainThread());

    base::AutoLock lock(lock_);

    Statistics stats = ComputeStatistics();

    // In KiB
    base::UmaHistogramCounts100000("Memory.ParkableImage.TotalSize.5min", static_cast<int>(stats.total_size / 1024));
#if !defined(MINI_UI_QUICKJS)
    base::UmaHistogramCounts100000("Memory.ParkableImage.OnDiskSize.5min", static_cast<int>(stats.on_disk_size / 1024));
#endif
    base::UmaHistogramCounts100000("Memory.ParkableImage.UnparkedSize.5min", static_cast<int>(stats.unparked_size / 1024));

#if !defined(MINI_UI_QUICKJS)
    // Metrics related to disk parking should only be recorded if the feature is
    // enabled and the allocator can write.
    if (IsParkableImagesToDiskEnabled() && data_allocator().may_write()) {
        base::UmaHistogramTimes("Memory.ParkableImage.TotalWriteTime.5min", total_disk_write_time_);
        base::UmaHistogramTimes("Memory.ParkableImage.TotalReadTime.5min", total_disk_read_time_);
    }
#endif
}

scoped_refptr<ParkableImageImpl> ParkableImageManager::CreateParkableImage(size_t offset)
{
    base::AutoLock lock(lock_);
    scoped_refptr<ParkableImageImpl> impl = ParkableImageImpl::Create(offset);
    return impl;
}

void ParkableImageManager::DestroyParkableImageOnMainThread(scoped_refptr<ParkableImageImpl> image)
{
    DCHECK(IsMainThread());
}

void ParkableImageManager::DestroyParkableImage(scoped_refptr<ParkableImageImpl> image)
{
    if (IsMainThread()) {
        DestroyParkableImageOnMainThread(std::move(image));
    } else {
        task_runner_->PostTask(FROM_HERE, base::BindOnce(&ParkableImageManager::DestroyParkableImageOnMainThread, base::Unretained(this), std::move(image)));
    }
}

void ParkableImageManager::Remove(ParkableImageImpl* image)
{
    base::AutoLock lock(lock_);

#if defined(MINI_UI_QUICKJS)
    auto it = unparked_images_.find(image);
    CHECK(it != unparked_images_.end(), base::NotFatalUntil::M130);
    unparked_images_.erase(it);
#else
    // Image could be on disk or unparked. Remove it in either case.
    auto* map = image->is_on_disk() ? &on_disk_images_ : &unparked_images_;
    auto it = map->find(image);
    CHECK(it != map->end(), base::NotFatalUntil::M130);
    map->erase(it);
#endif
}

#if !defined(MINI_UI_QUICKJS)
void ParkableImageManager::MoveImage(ParkableImageImpl* image, WTF::HashSet<ParkableImageImpl*>* from, WTF::HashSet<ParkableImageImpl*>* to)
{
    auto it = from->find(image);
    CHECK(it != from->end());
    CHECK(!to->Contains(image));
    from->erase(it);
    to->insert(image);
}
#endif

bool ParkableImageManager::IsRegistered(ParkableImageImpl* image)
{
    base::AutoLock lock(lock_);

#if defined(MINI_UI_QUICKJS)
    return unparked_images_.Contains(image);
#else
    auto* map = image->is_on_disk() ? &on_disk_images_ : &unparked_images_;
    auto it = map->find(image);

    return it != map->end();
#endif
}

#if !defined(MINI_UI_QUICKJS)
void ParkableImageManager::OnWrittenToDisk(ParkableImageImpl* image)
{
    base::AutoLock lock(lock_);
    MoveImage(image, &unparked_images_, &on_disk_images_);
}

void ParkableImageManager::OnReadFromDisk(ParkableImageImpl* image)
{
    base::AutoLock lock(lock_);
    MoveImage(image, &on_disk_images_, &unparked_images_);
    ScheduleDelayedParkingTaskIfNeeded();
}

void ParkableImageManager::ScheduleDelayedParkingTaskIfNeeded()
{
    if (!ParkableImageManager::IsParkableImagesToDiskEnabled())
        return;

    if (has_pending_parking_task_)
        return;

    task_runner_->PostDelayedTask(
        FROM_HERE, base::BindOnce(&ParkableImageManager::MaybeParkImages, base::Unretained(this)), ParkableImageManager::kDelayedParkingInterval);
    has_pending_parking_task_ = true;
}

void ParkableImageManager::MaybeParkImages()
{
    // Because we only have a raw pointer to the ParkableImageImpl, we need to be
    // very careful here to avoid a UAF.
    // To avoid this, we make sure that ParkableImageImpl is always destroyed on
    // the main thread, using |ParkableImageManager::DestroyParkableImage|.
    DCHECK(ParkableImageManager::IsParkableImagesToDiskEnabled());
    DCHECK(IsMainThread());

    base::AutoLock lock(lock_);

    // This makes a copy of the pointers stored in |unparked_images_|. We iterate
    // over this copy in |MaybeParkImages|, instead of |unparked_images_|
    // directly, to avoid deadlock when we need to park synchronously (i.e. if we
    // have already written to disk and don't need to post a background task), as
    // synchronous parking calls |ParkableImageManager::OnWrittenToDisk()|;
    WTF::Vector<ParkableImageImpl*> unparked_images(unparked_images_);

    // We unlock here so that we can avoid a deadlock, since if the data for the
    // image is already written to disk, we can discard our copy of the data
    // synchronously, which calls back into the manager.
    lock_.Release();

    bool should_reschedule = false;
    for (auto* image : unparked_images) {
        if (image->ShouldReschedule())
            should_reschedule = true;
        image->MaybePark(task_runner_);
    }

    lock_.Acquire();

    has_pending_parking_task_ = false;

    if (should_reschedule)
        ScheduleDelayedParkingTaskIfNeeded();
}
#endif // !defined(MINI_UI_QUICKJS)

} // namespace blink
