// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/heap/thread_state.h"

#include <array>

#include <fstream>
#include <iostream>

#include "base/functional/callback.h"
#include "base/notreached.h"
#if defined(MINI_UI_QUICKJS)
#include "runtime/ui/quickjs_bindings.h"
#else
#include "gin/public/isolate_holder.h"
#endif
#include "third_party/blink/renderer/platform/bindings/dom_data_store.h"
#include "third_party/blink/renderer/platform/bindings/dom_wrapper_world.h"
#include "third_party/blink/renderer/platform/bindings/script_wrappable.h"
#include "third_party/blink/renderer/platform/bindings/wrapper_type_info.h"
#include "third_party/blink/renderer/platform/heap/custom_spaces.h"
#include "third_party/blink/renderer/platform/heap/thread_state_storage.h"
#include "third_party/blink/renderer/platform/wtf/hash_set.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"
#include "v8/include/cppgc/heap-consistency.h"
#if !defined(MINI_UI_QUICKJS)
#include "v8/include/v8-cppgc.h"
#include "v8/include/v8-embedder-heap.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-profiler.h"
#include "v8/include/v8-traced-handle.h"
#endif

namespace blink {

#if !defined(MINI_UI_QUICKJS)
namespace {

// Handler allowing for dropping V8 wrapper objects that can be recreated
// lazily.
class BlinkRootsHandler final : public v8::EmbedderRootsHandler {
public:
    explicit BlinkRootsHandler(v8::Isolate* isolate)
        : isolate_(isolate)
    {
    }

    // ResetRoot() clears references to V8 wrapper objects in all worlds. It is
    // invoked for references where IsRoot() returned false during young
    // generation garbage collections.
    void ResetRoot(const v8::TracedReference<v8::Value>& handle) final
    {
        const v8::TracedReference<v8::Object>& traced = handle.As<v8::Object>();
        const bool success = DOMDataStore::ClearWrapperInAnyWorldIfEqualTo(ToAnyScriptWrappable(isolate_, traced), traced);
        // Since V8 found a handle, Blink needs to find it as well when trying to
        // remove it. Note that this is even true for the case where a
        // DOMWrapperWorld and DOMDataStore are already unreachable as the internal
        // worldmap contains a weak ref that remains valid until the next full GC
        // call. The weak ref is guaranteed to still be valid because it is only
        // cleared on full GCs and the `BlinkRootsHandler` is used on minor V8 GCs.
        CHECK(success);
    }

    bool TryResetRoot(const v8::TracedReference<v8::Value>& handle) final
    {
        const v8::TracedReference<v8::Object>& traced = handle.As<v8::Object>();
        return DOMDataStore::ClearInlineStorageWrapperIfEqualTo(ToAnyScriptWrappable(isolate_, traced), traced);
    }

private:
    v8::Isolate* isolate_;
};

} // namespace
#endif

// static
ThreadState* ThreadState::AttachMainThread()
{
#if defined(MINI_UI_QUICKJS)
    static std::unique_ptr<ThreadState, void (*)(ThreadState*)> owner(
        new ThreadState(), [](ThreadState* state) { delete state; });
    CHECK(owner->IsCreationThread());
    if (!ThreadStateStorage::Current()) {
        ThreadStateStorage::AttachMainThread(
            *owner, owner->cpp_heap().GetAllocationHandle(), owner->heap_handle());
    } else {
        CHECK_EQ(owner.get(), &ThreadStateStorage::Current()->thread_state());
    }
    return owner.get();
#else
    auto* thread_state = new ThreadState(gin::IsolateHolder::GetPlatform());
    ThreadStateStorage::AttachMainThread(*thread_state, thread_state->cpp_heap().GetAllocationHandle(), thread_state->cpp_heap().GetHeapHandle());
    return thread_state;
#endif
}

#if !defined(MINI_UI_QUICKJS)
// static
ThreadState* ThreadState::AttachMainThreadForTesting(v8::Platform* platform)
{
    auto* thread_state = new ThreadState(platform);
    ThreadStateStorage::AttachMainThread(*thread_state, thread_state->cpp_heap().GetAllocationHandle(), thread_state->cpp_heap().GetHeapHandle());
    thread_state->EnableDetachedGarbageCollectionsForTesting();
    return thread_state;
}
#endif

// static
ThreadState* ThreadState::AttachCurrentThread()
{
#if defined(MINI_UI_QUICKJS)
    auto* thread_state = new ThreadState();
#else
    auto* thread_state = new ThreadState(gin::IsolateHolder::GetPlatform());
#endif
    ThreadStateStorage::AttachNonMainThread(*thread_state, thread_state->cpp_heap().GetAllocationHandle(), thread_state->cpp_heap().GetHeapHandle());
    return thread_state;
}

#if !defined(MINI_UI_QUICKJS)
// static
ThreadState* ThreadState::AttachCurrentThreadForTesting(v8::Platform* platform)
{
    ThreadState* thread_state = new ThreadState(platform);
    ThreadStateStorage::AttachNonMainThread(*thread_state, thread_state->cpp_heap().GetAllocationHandle(), thread_state->cpp_heap().GetHeapHandle());
    thread_state->EnableDetachedGarbageCollectionsForTesting();
    return thread_state;
}
#endif

// static
void ThreadState::DetachCurrentThread()
{
    auto* state = ThreadState::Current();
    DCHECK(state);
    delete state;
}

void ThreadState::AttachToIsolate(v8::Isolate* isolate, V8BuildEmbedderGraphCallback)
{
#if defined(MINI_UI_QUICKJS)
    CHECK(isolate);
    CHECK(!isolate_);
    mini_ui::AttachQuickJSCppgcHeap(isolate, heap_handle_);
    isolate_ = isolate;
#else
    isolate->AttachCppHeap(cpp_heap_.get());
    CHECK_EQ(cpp_heap_.get(), isolate->GetCppHeap());
    isolate_ = isolate;
    embedder_roots_handler_ = std::make_unique<BlinkRootsHandler>(isolate);
    isolate_->SetEmbedderRootsHandler(embedder_roots_handler_.get());
#endif
}

void ThreadState::DetachFromIsolate()
{
#if defined(MINI_UI_QUICKJS)
    CHECK(isolate_);
    mini_ui::DetachQuickJSCppgcHeap(isolate_);
#else
    CHECK_EQ(cpp_heap_.get(), isolate_->GetCppHeap());
    isolate_->DetachCppHeap();
    isolate_->SetEmbedderRootsHandler(nullptr);
#endif
    isolate_ = nullptr;
}

#if defined(MINI_UI_QUICKJS)
ThreadState::ThreadState()
    : cpp_heap_(std::make_unique<mini_ui::NativeHeap>())
#else
ThreadState::ThreadState(v8::Platform* platform)
    : cpp_heap_(v8::CppHeap::Create(platform, v8::CppHeapCreateParams(CustomSpaces::CreateCustomSpaces())))
#endif
    , heap_handle_(cpp_heap_->GetHeapHandle())
    , thread_id_(CurrentThread())
{
}

ThreadState::~ThreadState()
{
    DCHECK(IsCreationThread());
#if defined(MINI_UI_QUICKJS)
    if (isolate_)
        DetachFromIsolate();
#endif
    cpp_heap_->Terminate();
#if defined(MINI_UI_QUICKJS)
    if (IsMainThread()) {
        auto* storage = ThreadStateStorage::Current();
        storage->allocation_handle_ = nullptr;
        storage->heap_handle_ = nullptr;
        storage->thread_state_ = nullptr;
        g_thread_specific_ = nullptr;
    } else {
        ThreadStateStorage::DetachNonMainThread(*ThreadStateStorage::Current());
    }
#else
    ThreadStateStorage::DetachNonMainThread(*ThreadStateStorage::Current());
#endif
}

void ThreadState::CollectAllGarbageForTesting(StackState stack_state)
{
    size_t previous_live_bytes = 0;
    for (size_t i = 0; i < 5; i++) {
#if defined(MINI_UI_QUICKJS)
        cpp_heap().Collect(stack_state);
#else
        cpp_heap().CollectGarbageForTesting(stack_state);
#endif
        const size_t live_bytes = cpp_heap().CollectStatistics(cppgc::HeapStatistics::kBrief).used_size_bytes;
        if (previous_live_bytes == live_bytes) {
            break;
        }
        previous_live_bytes = live_bytes;
    }
}

#if !defined(MINI_UI_QUICKJS)
void ThreadState::CollectGarbageInYoungGenerationForTesting(StackState stack_state)
{
    cpp_heap().CollectGarbageInYoungGenerationForTesting(stack_state);
}

namespace {

class CustomSpaceStatisticsReceiverImpl final : public v8::CustomSpaceStatisticsReceiver {
public:
    explicit CustomSpaceStatisticsReceiverImpl(base::OnceCallback<void(size_t allocated_node_bytes, size_t allocated_css_bytes)> callback)
        : callback_(std::move(callback))
    {
    }

    ~CustomSpaceStatisticsReceiverImpl() final
    {
        DCHECK(node_bytes_.has_value());
        DCHECK(css_bytes_.has_value());
        std::move(callback_).Run(*node_bytes_, *css_bytes_);
    }

    void AllocatedBytes(cppgc::CustomSpaceIndex space_index, size_t bytes) final
    {
        if (space_index.value == NodeSpace::kSpaceIndex.value) {
            node_bytes_ = bytes;
        } else {
            DCHECK_EQ(space_index.value, CSSValueSpace::kSpaceIndex.value);
            css_bytes_ = bytes;
        }
    }

private:
    base::OnceCallback<void(size_t allocated_node_bytes, size_t allocated_css_bytes)> callback_;
    std::optional<size_t> node_bytes_;
    std::optional<size_t> css_bytes_;
};

} // anonymous namespace
#endif

void ThreadState::CollectNodeAndCssStatistics(base::OnceCallback<void(size_t allocated_node_bytes, size_t allocated_css_bytes)> callback)
{
#if defined(MINI_UI_QUICKJS)
    constexpr std::array spaces { NodeSpace::kSpaceIndex, CSSValueSpace::kSpaceIndex };
    std::array<size_t, spaces.size()> bytes;
    cpp_heap().CustomSpaceBytes(spaces, bytes);
    std::move(callback).Run(bytes[0], bytes[1]);
#else
    std::vector<cppgc::CustomSpaceIndex> spaces { NodeSpace::kSpaceIndex, CSSValueSpace::kSpaceIndex };
    cpp_heap().CollectCustomSpaceStatisticsAtLastGC(std::move(spaces), std::make_unique<CustomSpaceStatisticsReceiverImpl>(std::move(callback)));
#endif
}

#if !defined(MINI_UI_QUICKJS)
void ThreadState::EnableDetachedGarbageCollectionsForTesting()
{
    cpp_heap().EnableDetachedGarbageCollectionsForTesting();
}
#endif

bool ThreadState::IsIncrementalMarking()
{
    return cppgc::subtle::HeapState::IsMarking(ThreadState::Current()->heap_handle())
        && !cppgc::subtle::HeapState::IsInAtomicPause(ThreadState::Current()->heap_handle());
}

#if !defined(MINI_UI_QUICKJS)
namespace {

class BufferedStream final : public v8::OutputStream {
public:
    explicit BufferedStream(std::streambuf* stream_buffer)
        : out_stream_(stream_buffer)
    {
    }

    WriteResult WriteAsciiChunk(char* data, int size) override
    {
        out_stream_.write(data, size);
        return kContinue;
    }

    void EndOfStream() override
    {
    }

private:
    std::ostream out_stream_;
};

} // namespace

void ThreadState::TakeHeapSnapshotForTesting(const char* filename) const
{
    CHECK(isolate_);
    v8::HeapProfiler* profiler = isolate_->GetHeapProfiler();
    CHECK(profiler);

    v8::HeapProfiler::HeapSnapshotOptions options;
    options.snapshot_mode = v8::HeapProfiler::HeapSnapshotMode::kExposeInternals;
    const v8::HeapSnapshot* snapshot = profiler->TakeHeapSnapshot(options);

    {
        std::ofstream file_stream;
        if (filename) {
            file_stream.open(filename, std::ios_base::out | std::ios_base::trunc);
        }
        BufferedStream stream(filename ? file_stream.rdbuf() : std::cout.rdbuf());
        snapshot->Serialize(&stream);
    }

    const_cast<v8::HeapSnapshot*>(snapshot)->Delete();
}

bool ThreadState::IsTakingHeapSnapshot() const
{
    if (!isolate_) {
        return false;
    }
    v8::HeapProfiler* profiler = isolate_->GetHeapProfiler();
    return profiler && profiler->IsTakingSnapshot();
}

const char* ThreadState::CopyNameForHeapSnapshot(const char* name) const
{
    CHECK(isolate_);
    v8::HeapProfiler* profiler = isolate_->GetHeapProfiler();
    CHECK(profiler);
    return profiler->CopyNameForHeapSnapshot(name);
}
#endif

} // namespace blink
