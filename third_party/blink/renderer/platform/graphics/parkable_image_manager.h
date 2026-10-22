// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_GRAPHICS_PARKABLE_IMAGE_MANAGER_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_GRAPHICS_PARKABLE_IMAGE_MANAGER_H_

#if !defined(MINI_UI_QUICKJS)
#include "base/feature_list.h"
#endif
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/trace_event/memory_dump_provider.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/renderer/platform/disk_data_allocator.h"
#endif
#include "third_party/blink/renderer/platform/graphics/parkable_image.h"
#include "third_party/blink/renderer/platform/platform_export.h"
#include "third_party/blink/renderer/platform/wtf/hash_set.h"

namespace blink {

class ParkableImageImpl;
class ParkableImage;

// Manages parkable images, which are used in blink::BitmapImage. Currently,
// only records metrics for this. In the future we will park eligible images
// to disk.
// Main Thread only.
class PLATFORM_EXPORT ParkableImageManager : public base::trace_event::MemoryDumpProvider {
public:
    static ParkableImageManager& Instance();
    ~ParkableImageManager() override = default;

    bool OnMemoryDump(const base::trace_event::MemoryDumpArgs&, base::trace_event::ProcessMemoryDump*) override;

    // Number of parked and unparked images.
    size_t Size() const LOCKS_EXCLUDED(lock_);

#if !defined(MINI_UI_QUICKJS)
    static bool IsParkableImagesToDiskEnabled()
    {
        return base::FeatureList::IsEnabled(features::kParkableImagesToDisk);
    }
#endif

#if !defined(MINI_UI_QUICKJS)
    void MaybeParkImagesForTesting()
    {
        MaybeParkImages();
    }
#endif

private:
    struct Statistics;

    friend class ParkableImage;
    friend class ParkableImageImpl;
    friend class base::NoDestructor<ParkableImageManager>;
    friend class ParkableImageBaseTest;

    ParkableImageManager();

#if !defined(MINI_UI_QUICKJS)
    DiskDataAllocator& data_allocator() const;
#endif

    // Register and unregister a ParkableImage with the manager. ParkableImage
    // should call these when created/destructed.
    void Add(ParkableImageImpl* image) LOCKS_EXCLUDED(lock_);
    void Remove(ParkableImageImpl* image) LOCKS_EXCLUDED(lock_) EXCLUSIVE_LOCKS_REQUIRED(image->lock_);

    scoped_refptr<ParkableImageImpl> CreateParkableImage(size_t offset);
    void DestroyParkableImageOnMainThread(scoped_refptr<ParkableImageImpl> image) LOCKS_EXCLUDED(lock_);
    void DestroyParkableImage(scoped_refptr<ParkableImageImpl> image) LOCKS_EXCLUDED(lock_);

    bool IsRegistered(ParkableImageImpl* image) LOCKS_EXCLUDED(lock_) EXCLUSIVE_LOCKS_REQUIRED(image->lock_);
    // bool IsRegistered(ParkableImage* image) LOCKS_EXCLUDED(lock_)
    //     EXCLUSIVE_LOCKS_REQUIRED(image->impl_->lock_);

#if !defined(MINI_UI_QUICKJS)
    void ScheduleDelayedParkingTaskIfNeeded() EXCLUSIVE_LOCKS_REQUIRED(lock_);
    void MaybeParkImages() LOCKS_EXCLUDED(lock_);
#endif

    Statistics ComputeStatistics() const EXCLUSIVE_LOCKS_REQUIRED(lock_);

    void RecordStatisticsAfter5Minutes() const LOCKS_EXCLUDED(lock_);

#if !defined(MINI_UI_QUICKJS)
    void MoveImage(ParkableImageImpl* image, WTF::HashSet<ParkableImageImpl*>* from, WTF::HashSet<ParkableImageImpl*>* to) EXCLUSIVE_LOCKS_REQUIRED(lock_);
#endif

#if !defined(MINI_UI_QUICKJS)
    void RecordDiskWriteTime(base::TimeDelta write_time) LOCKS_EXCLUDED(lock_)
    {
        base::AutoLock lock(lock_);
        total_disk_write_time_ += write_time;
    }

    void RecordDiskReadTime(base::TimeDelta read_time) LOCKS_EXCLUDED(lock_)
    {
        base::AutoLock lock(lock_);
        total_disk_read_time_ += read_time;
    }

    // Keeps track of whether the image is unparked or on disk. ParkableImage
    // calls these when written to or read from disk.
    void OnWrittenToDisk(ParkableImageImpl* image) LOCKS_EXCLUDED(lock_);
    void OnReadFromDisk(ParkableImageImpl* image) LOCKS_EXCLUDED(lock_);

    void SetDataAllocatorForTesting(std::unique_ptr<DiskDataAllocator> allocator)
    {
        allocator_for_testing_ = std::move(allocator);
    }
#endif

    void SetTaskRunnerForTesting(scoped_refptr<base::SingleThreadTaskRunner> task_runner);

    void ResetForTesting();
#if !defined(MINI_UI_QUICKJS)
    constexpr static auto kDelayedParkingInterval = base::Seconds(2);
#endif
    constexpr static const char* kAllocatorDumpName = "parkable_images";

    mutable base::Lock lock_;

    // Tracks every created ParkableImage until destruction.
    WTF::HashSet<ParkableImageImpl*> unparked_images_ GUARDED_BY(lock_);
#if !defined(MINI_UI_QUICKJS)
    // Images with only a disk representation are moved here until read back.
    WTF::HashSet<ParkableImageImpl*> on_disk_images_ GUARDED_BY(lock_);
#endif

#if !defined(MINI_UI_QUICKJS)
    bool has_pending_parking_task_ GUARDED_BY(lock_) = false;
#endif
    bool has_posted_accounting_task_ GUARDED_BY(lock_) = false;

#if !defined(MINI_UI_QUICKJS)
    base::TimeDelta total_disk_read_time_ GUARDED_BY(lock_) = base::TimeDelta();
    base::TimeDelta total_disk_write_time_ GUARDED_BY(lock_) = base::TimeDelta();
#endif

    scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
#if !defined(MINI_UI_QUICKJS)
    std::unique_ptr<DiskDataAllocator> allocator_for_testing_;
#endif
};

} // namespace blink

#endif // THIRD_PARTY_BLINK_RENDERER_PLATFORM_GRAPHICS_PARKABLE_IMAGE_MANAGER_H_
