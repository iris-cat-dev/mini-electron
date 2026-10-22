// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef MINI_UI_NATIVE_HEAP_H_
#define MINI_UI_NATIVE_HEAP_H_

#include <memory>
#include <span>

#include "v8/include/cppgc/common.h"
#include "v8/include/cppgc/custom-space.h"
#include "v8/include/cppgc/heap-statistics.h"
#include "v8/include/cppgc/heap.h"

namespace cppgc {
class Visitor;
}

namespace cppgc::internal {
class RootVisitorBase;
}

namespace mini_ui {

void VisitNativeHeapRoots(cppgc::HeapHandle&, cppgc::internal::RootVisitorBase&,
    cppgc::Visitor&, cppgc::EmbedderStackState);

// Blink's Oilpan heap, without v8::CppHeap, an Isolate, or the V8 JS runtime.
// Collection and sweeping run atomically on the owning UI thread. Script/native
// reachability is installed separately by the QuickJS binding backend.
class NativeHeap final {
public:
    NativeHeap();
    ~NativeHeap();
    NativeHeap(const NativeHeap&) = delete;
    NativeHeap& operator=(const NativeHeap&) = delete;

    cppgc::AllocationHandle& GetAllocationHandle();
    cppgc::HeapHandle& GetHeapHandle();
    cppgc::Heap& heap();
    void Collect(cppgc::EmbedderStackState);
    std::size_t EstimatedAllocatedBytes() const;
    cppgc::HeapStatistics CollectStatistics(cppgc::HeapStatistics::DetailLevel);
    void CustomSpaceBytes(std::span<const cppgc::CustomSpaceIndex>,
        std::span<std::size_t>);
    void Terminate();

private:
    std::unique_ptr<cppgc::Heap> heap_;
    bool terminated_ = false;
};

} // namespace mini_ui

#endif // MINI_UI_NATIVE_HEAP_H_
