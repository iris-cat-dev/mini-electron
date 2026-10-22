// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "runtime/ui/native_heap.h"

#include <cstdlib>
#include <unordered_set>
#include <utility>

#include "runtime/ui/cppgc_platform.h"
#include "runtime/ui/quickjs_bindings.h"
#include "third_party/blink/renderer/platform/heap/custom_spaces.h"
#include "v8/src/heap/base/stack.h"
#include "v8/src/heap/cppgc/heap-base.h"
#include "v8/src/heap/cppgc/raw-heap.h"
#include "v8/src/heap/cppgc/visitor.h"
#include "v8/src/heap/cppgc/stats-collector.h"

namespace mini_ui {

namespace {

class NativeStackVisitor final : public cppgc::internal::ConservativeTracingVisitor,
                                 public ::heap::base::StackVisitor {
public:
    NativeStackVisitor(cppgc::internal::HeapBase& heap, cppgc::Visitor& visitor)
        : ConservativeTracingVisitor(heap, *heap.page_backend(), visitor)
    {
    }

private:
    void VisitPointer(const void* pointer) override
    {
        TraceConservativelyIfNeeded(pointer);
    }

    void VisitInConstructionConservatively(cppgc::internal::HeapObjectHeader& header,
        TraceConservativelyCallback trace) override
    {
        if (in_construction_.insert(&header).second)
            trace(this, header);
    }

    std::unordered_set<const void*> in_construction_;
};

} // namespace

void VisitNativeHeapRoots(cppgc::HeapHandle& handle,
    cppgc::internal::RootVisitorBase& roots, cppgc::Visitor& visitor,
    cppgc::EmbedderStackState stack_state)
{
    auto& heap = cppgc::internal::HeapBase::From(handle);
    heap.GetStrongPersistentRegion().Iterate(roots);
    {
        cppgc::internal::PersistentRegionLock lock;
        heap.GetStrongCrossThreadPersistentRegion().Iterate(roots);
    }
    if (stack_state == cppgc::EmbedderStackState::kNoHeapPointers)
        return;
    NativeStackVisitor stack_visitor(heap, visitor);
    auto* stack = heap.stack();
    stack->SetMarkerAndCallback([&] { stack->IteratePointers(&stack_visitor); });
}

NativeHeap::NativeHeap()
{
    InitializeCppgcProcess();
    cppgc::Heap::HeapOptions options;
    options.custom_spaces = blink::CustomSpaces::CreateCustomSpaces();
    options.marking_support = cppgc::Heap::MarkingType::kAtomic;
    options.sweeping_support = cppgc::Heap::SweepingType::kAtomic;
    heap_ = cppgc::Heap::Create(GetCppgcPlatform(), std::move(options));
}

NativeHeap::~NativeHeap()
{
    Terminate();
}

cppgc::AllocationHandle& NativeHeap::GetAllocationHandle()
{
    return heap_->GetAllocationHandle();
}

cppgc::HeapHandle& NativeHeap::GetHeapHandle()
{
    return heap_->GetHeapHandle();
}

cppgc::Heap& NativeHeap::heap()
{
    return *heap_;
}

void NativeHeap::Collect(cppgc::EmbedderStackState stack_state)
{
    RecomputeQuickJSActiveWrappables(heap_->GetHeapHandle());
    heap_->ForceGarbageCollectionSlow("mini-ui", "native heap collection", stack_state);
}

std::size_t NativeHeap::EstimatedAllocatedBytes() const
{
    return cppgc::internal::HeapBase::From(heap_->GetHeapHandle())
        .stats_collector()->allocated_object_size();
}

cppgc::HeapStatistics NativeHeap::CollectStatistics(
    cppgc::HeapStatistics::DetailLevel detail)
{
    return cppgc::internal::HeapBase::From(heap_->GetHeapHandle()).CollectStatistics(detail);
}

void NativeHeap::CustomSpaceBytes(
    std::span<const cppgc::CustomSpaceIndex> indices,
    std::span<std::size_t> bytes)
{
    if (indices.size() != bytes.size())
        std::abort();
    if (indices.empty())
        return;
    const auto statistics = CollectStatistics(cppgc::HeapStatistics::kDetailed);
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const auto space = cppgc::internal::RawHeap::kNumberOfRegularSpaces + indices[i].value;
        if (space >= statistics.space_stats.size())
            std::abort();
        bytes[i] = statistics.space_stats[space].used_size_bytes;
    }
}

void NativeHeap::Terminate()
{
    if (!terminated_) {
        DetachQuickJSCppgcHeap(heap_->GetHeapHandle());
        cppgc::internal::HeapBase::From(heap_->GetHeapHandle()).Terminate();
        terminated_ = true;
    }
}

} // namespace mini_ui
