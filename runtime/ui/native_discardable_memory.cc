// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "runtime/ui/native_discardable_memory.h"

#include <inttypes.h>

#include <limits>
#include <memory>
#include <utility>

#include "base/check.h"
#include "base/format_macros.h"
#include "base/memory/discardable_memory.h"
#include "base/memory/discardable_memory_allocator.h"
#include "base/memory/discardable_shared_memory.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "base/trace_event/memory_allocator_dump.h"
#include "base/trace_event/process_memory_dump.h"

namespace mini_ui {
namespace {

class NativeDiscardableMemory;

struct NativeDiscardableMemoryState {
    base::Lock lock;
    NativeDiscardableMemory* first = nullptr;
    size_t bytes_allocated = 0;
};

class NativeDiscardableMemory final : public base::DiscardableMemory {
public:
    static std::unique_ptr<NativeDiscardableMemory> Create(
        std::shared_ptr<NativeDiscardableMemoryState> state, size_t size)
    {
        if (!size)
            return nullptr;

        auto memory = std::unique_ptr<NativeDiscardableMemory>(
            new NativeDiscardableMemory(std::move(state)));
        if (!memory->shared_memory_.CreateAndMap(size))
            return nullptr;

        memory->mapped_size_ = memory->shared_memory_.mapped_size();
        memory->locked_ = true;
        memory->accounted_ = true;

        base::AutoLock lock(memory->state_->lock);
        CHECK_LE(memory->mapped_size_,
            std::numeric_limits<size_t>::max()
                - memory->state_->bytes_allocated);
        memory->state_->bytes_allocated += memory->mapped_size_;
        memory->next_ = memory->state_->first;
        if (memory->next_)
            memory->next_->previous_ = memory.get();
        memory->state_->first = memory.get();
        memory->registered_ = true;
        return memory;
    }

    ~NativeDiscardableMemory() override
    {
        base::AutoLock lock(state_->lock);
        if (!registered_)
            return;

        if (previous_)
            previous_->next_ = next_;
        else
            state_->first = next_;
        if (next_)
            next_->previous_ = previous_;

        ReleaseAccountingLocked();
        registered_ = false;
    }

    bool Lock() override
    {
        base::AutoLock lock(state_->lock);
        DCHECK(!locked_);
        if (!accounted_)
            return false;

        if (shared_memory_.Lock(0, 0)
            == base::DiscardableSharedMemory::SUCCESS) {
            locked_ = true;
            return true;
        }

        // A failed lock means the contents have been discarded. Retaining the
        // virtual mapping cannot make this object usable again.
        CHECK(shared_memory_.Unmap());
        shared_memory_.Close();
        ReleaseAccountingLocked();
        return false;
    }

    void Unlock() override
    {
        base::AutoLock lock(state_->lock);
        DCHECK(locked_);
        DCHECK(accounted_);
        shared_memory_.Unlock(0, 0);
        locked_ = false;
    }

    void* data() const override
    {
        base::AutoLock lock(state_->lock);
        DCHECK(locked_);
        DCHECK(accounted_);
        return shared_memory_.memory().data();
    }

    void DiscardForTesting() override
    {
        base::AutoLock lock(state_->lock);
        DCHECK(!locked_);
        CHECK(PurgeUnlockedBackingLocked());
    }

    base::trace_event::MemoryAllocatorDump* CreateMemoryAllocatorDump(
        const char* name,
        base::trace_event::ProcessMemoryDump* pmd) const override
    {
        base::AutoLock lock(state_->lock);
        auto* dump = pmd->CreateAllocatorDump(name);
        dump->AddScalar(base::trace_event::MemoryAllocatorDump::kNameSize,
            base::trace_event::MemoryAllocatorDump::kUnitsBytes,
            accounted_ ? static_cast<uint64_t>(mapped_size_) : 0U);

        if (accounted_) {
            std::string segment_name = base::StringPrintf(
                "discardable/mini_ui_segment_0x%" PRIXPTR,
                reinterpret_cast<uintptr_t>(this));
            auto* segment_dump = pmd->CreateAllocatorDump(segment_name);
            shared_memory_.CreateSharedMemoryOwnershipEdge(
                segment_dump, pmd, true);
            pmd->AddSuballocation(dump->guid(), segment_name);
        }
        return dump;
    }

private:
    friend class NativeDiscardableMemoryAllocator;

    explicit NativeDiscardableMemory(
        std::shared_ptr<NativeDiscardableMemoryState> state)
        : state_(std::move(state))
    {
    }

    bool PurgeUnlockedBackingLocked()
    {
        if (!accounted_)
            return true;
        DCHECK(!locked_);

        // A stale observed timestamp can make the first purge refresh its view
        // rather than discard. No external copy is handed out, so a retry is
        // sufficient unless the backing is unexpectedly locked.
        if (!shared_memory_.Purge(base::Time::Now())
            && !shared_memory_.Purge(base::Time::Now())) {
            return false;
        }

        CHECK(shared_memory_.Unmap());
        shared_memory_.Close();
        ReleaseAccountingLocked();
        return true;
    }

    void ReleaseAccountingLocked()
    {
        if (!accounted_)
            return;
        CHECK_GE(state_->bytes_allocated, mapped_size_);
        state_->bytes_allocated -= mapped_size_;
        accounted_ = false;
    }

    const std::shared_ptr<NativeDiscardableMemoryState> state_;
    base::DiscardableSharedMemory shared_memory_;
    NativeDiscardableMemory* previous_ = nullptr;
    NativeDiscardableMemory* next_ = nullptr;
    size_t mapped_size_ = 0;
    bool registered_ = false;
    bool locked_ = false;
    bool accounted_ = false;
};

class NativeDiscardableMemoryAllocator final
    : public base::DiscardableMemoryAllocator {
public:
    NativeDiscardableMemoryAllocator()
        : state_(std::make_shared<NativeDiscardableMemoryState>())
    {
    }

    ~NativeDiscardableMemoryAllocator() override
    {
        if (base::DiscardableMemoryAllocator::HasInstance()
            && base::DiscardableMemoryAllocator::GetInstance() == this) {
            base::DiscardableMemoryAllocator::SetInstance(nullptr);
        }
    }

    std::unique_ptr<base::DiscardableMemory>
    AllocateLockedDiscardableMemory(size_t size) override
    {
        return NativeDiscardableMemory::Create(state_, size);
    }

    size_t GetBytesAllocated() const override
    {
        base::AutoLock lock(state_->lock);
        return state_->bytes_allocated;
    }

    void ReleaseFreeMemory() override
    {
        base::AutoLock lock(state_->lock);
        for (NativeDiscardableMemory* memory = state_->first; memory;
             memory = memory->next_) {
            if (!memory->locked_)
                memory->PurgeUnlockedBackingLocked();
        }
    }

private:
    const std::shared_ptr<NativeDiscardableMemoryState> state_;
};

} // namespace

void InitializeNativeDiscardableMemory()
{
    // The host owns any allocator installed before this QuickJS UI runtime.
    if (base::DiscardableMemoryAllocator::HasInstance())
        return;

    static NativeDiscardableMemoryAllocator allocator;
    base::DiscardableMemoryAllocator::SetInstance(&allocator);
}

} // namespace mini_ui
