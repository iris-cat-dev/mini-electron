// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "runtime/ui/quickjs_bindings.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "base/check.h"
#include "base/auto_reset.h"
#include "runtime/ui/native_heap.h"
#include "runtime/ui/quickjs_v8_internal.h"

#include "third_party/blink/renderer/platform/bindings/active_script_wrappable_manager.h"
#include "third_party/blink/renderer/platform/bindings/v8_per_isolate_data.h"
#include "third_party/blink/renderer/platform/bindings/script_wrappable.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/trace-trait.h"
#include "v8/include/cppgc/visitor.h"
#include "v8/include/cppgc/internal/persistent-node.h"
#include "v8/src/heap/cppgc/visitor.h"

namespace mini_ui {
namespace {

struct EdgeRecord {
    JSValue value = JS_UNDEFINED;
    // A module is context-owned, not a GC object. Its genuine import.meta
    // object is the traced value that keeps the owning context alive.
    JSModuleDef* module = nullptr;
    std::uint32_t generation = 1;
    std::uint32_t next_free = 0;
    bool active = false;
    bool live = false;
    bool participates_in_trial_deletion = false;
};

struct WrapperRecord {
    blink::ScriptWrappable* native = nullptr;
    cppgc::TraceDescriptor descriptor { nullptr, nullptr };
    bool live = false;
    cppgc::Persistent<blink::ScriptWrappable> root;
    std::uint16_t tag = 0;
};

bool ParticipatesInTrialDeletion(JSValueConst value)
{
    const int tag = JS_VALUE_GET_TAG(value);
    return tag == JS_TAG_OBJECT || tag == JS_TAG_FUNCTION_BYTECODE;
}

struct WorkKey {
    const void* object = nullptr;
    cppgc::TraceCallback callback = nullptr;

    bool operator==(const WorkKey& other) const
    {
        return object == other.object && callback == other.callback;
    }
};

struct WorkKeyHash {
    std::size_t operator()(const WorkKey& key) const
    {
        const auto object = reinterpret_cast<std::uintptr_t>(key.object);
        const auto callback = std::hash<cppgc::TraceCallback> {}(key.callback);
        return static_cast<std::size_t>(object ^ (callback + 0x9e3779b9U
            + (object << 6) + (object >> 2)));
    }
};


class JointVisitor final : public cppgc::internal::VisitorBase {
public:
    JointVisitor() = default;

    void Reset();
    void AddRoot(const void*, cppgc::TraceDescriptor);
    void Drain();
    bool IsLive(const void* key) const;

private:
    struct WorkItem {
        const void* object;
        cppgc::TraceDescriptor descriptor;
    };
    struct Ephemeron {
        const void* key;
        const void* value;
        cppgc::TraceDescriptor descriptor;
        bool processed = false;
    };

    void Visit(const void*, cppgc::TraceDescriptor) override;
    void VisitWeak(const void*, cppgc::TraceDescriptor,
        cppgc::WeakCallback, const void*) override;
    void VisitEphemeron(const void*, const void*,
        cppgc::TraceDescriptor) override;
    void VisitWeakContainer(const void*, cppgc::TraceDescriptor,
        cppgc::TraceDescriptor, cppgc::WeakCallback, const void*) override;

    void Enqueue(const void*, cppgc::TraceDescriptor);
    bool ResolveEphemerons();

    std::vector<WorkItem> worklist_;
    std::size_t worklist_index_ = 0;
    std::vector<Ephemeron> ephemerons_;
    std::unordered_set<const void*> live_native_;
    std::unordered_set<WorkKey, WorkKeyHash> visited_callbacks_;
};

class JointRootVisitor final : public cppgc::internal::RootVisitorBase {
public:
    explicit JointRootVisitor(JointVisitor& visitor)
        : visitor_(visitor)
    {
    }

private:
    void VisitRoot(const void* object, cppgc::TraceDescriptor descriptor,
        const cppgc::SourceLocation&) override
    {
        visitor_.AddRoot(object, descriptor);
    }

    void VisitWeakRoot(const void*, cppgc::TraceDescriptor,
        cppgc::WeakCallback, const void*,
        const cppgc::SourceLocation&) override
    {
        // Weak persistents do not contribute to joint reachability.
    }

    JointVisitor& visitor_;
};

class CrossHeapState final {
public:
    explicit CrossHeapState(v8::Isolate* isolate)
        : isolate_(isolate)
    {
    }

    ~CrossHeapState()
    {
        if (runtime_)
            JS_SetExternalGCRootTracer(runtime_, nullptr, nullptr);
        ClearAllEdges(/*already_decremented=*/false);
    }

    void SetRuntime(JSRuntime* runtime)
    {
        CHECK(!runtime_ || runtime_ == runtime);
        if (runtime_ == runtime)
            return;
        runtime_ = runtime;
        if (runtime_)
            JS_SetExternalGCRootTracer(runtime_, &ExternalTrace, this);
    }

    void Attach(cppgc::HeapHandle& heap_handle)
    {
        CHECK(!heap_handle_ || heap_handle_ == &heap_handle);
        heap_handle_ = &heap_handle;
    }

    bool IsAttachedTo(const cppgc::HeapHandle& heap_handle) const
    {
        return heap_handle_ == &heap_handle;
    }

    void RecomputeActiveWrappables()
    {
        if (!heap_handle_ || !isolate_->GetData(gin::kEmbedderBlink))
            return;
        auto* manager = blink::V8PerIsolateData::From(isolate_)
                            ->GetActiveScriptWrappableManager();
        if (manager) {
            manager->RecomputeActiveScriptWrappables(
                blink::ActiveScriptWrappableManager::RecomputeMode::kRequired);
        }
    }

    void Collect(cppgc::EmbedderStackState stack_state)
    {
        CHECK(runtime_);
        base::AutoReset<cppgc::EmbedderStackState> scope(&stack_state_, stack_state);
        JS_RunGC(runtime_);
    }

    void Detach()
    {
        heap_handle_ = nullptr;
        wrappers_.clear();
        visitor_.Reset();
        ClearAllEdges(/*already_decremented=*/false);
    }

    QuickJSEdgeId RegisterEdge(JSValueConst value)
    {
        if (!runtime_)
            return kInvalidQuickJSEdge;
        std::uint32_t slot;
        if (free_edge_) {
            slot = free_edge_;
            free_edge_ = edges_[slot - 1].next_free;
        } else {
            CHECK_LT(edges_.size(), std::numeric_limits<std::uint32_t>::max());
            // Grow before retaining the JS value so allocation failure leaks
            // neither a reference nor an active slot.
            edges_.emplace_back();
            slot = static_cast<std::uint32_t>(edges_.size());
        }
        EdgeRecord& edge = edges_[slot - 1];
        edge.next_free = 0;
        edge.module = nullptr;
        JS_DupValueRT(runtime_, value);
        edge.value = value;
        edge.active = true;
        edge.live = false;
        edge.participates_in_trial_deletion = ParticipatesInTrialDeletion(value);
        ++active_edges_;
        return (static_cast<QuickJSEdgeId>(edge.generation) << 32) | slot;
    }

    QuickJSEdgeId RegisterModuleEdge(JSContext* context, JSModuleDef* module)
    {
        CHECK_EQ(JS_GetRuntime(context), runtime_);
        JSValue anchor = JS_GetImportMeta(context, module);
        CHECK(!JS_IsException(anchor));
        QuickJSEdgeId id;
        try {
            id = RegisterEdge(anchor);
        } catch (...) {
            JS_FreeValue(context, anchor);
            throw;
        }
        JS_FreeValue(context, anchor);
        if (EdgeRecord* edge = Edge(id))
            edge->module = module;
        return id;
    }

    JSContext* ModuleContext(QuickJSEdgeId id) const
    {
        const EdgeRecord* edge = Edge(id);
        return edge && edge->active && edge->module
            ? JS_GetObjectContext(edge->value) : nullptr;
    }

    void ResetEdge(QuickJSEdgeId id)
    {
        EdgeRecord* edge = Edge(id);
        if (!edge || !edge->active)
            return;
        ReleaseEdgeSlot(static_cast<std::uint32_t>(id), /*free_value=*/true);
    }

    bool IsEdgeAlive(QuickJSEdgeId id) const
    {
        const EdgeRecord* edge = Edge(id);
        return edge && edge->active;
    }

    JSValueConst GetEdge(QuickJSEdgeId id) const
    {
        const EdgeRecord* edge = Edge(id);
        if (!edge || !edge->active)
            return JS_UNDEFINED;
        return edge->module ? JS_MKPTR(JS_TAG_MODULE, edge->module) : edge->value;
    }

    void TraceEdge(QuickJSEdgeId id)
    {
        EdgeRecord* edge = Edge(id);
        if (!edge || !edge->active || phase_ != JS_EXTERNAL_GC_PHASE_SCAN
            || edge->live) {
            return;
        }
        edge->live = true;
        ++stats_.live_edge_count;
        JS_MarkValue(runtime_, edge->value, mark_func_, nullptr);
    }

    void RegisterWrapper(JSValueConst wrapper,
        blink::ScriptWrappable* native, std::uint16_t tag = 0)
    {
        if (!native || !JS_IsObject(wrapper))
            return;
        wrappers_[JS_VALUE_GET_PTR(wrapper)] = {
            native,
            cppgc::TraceTrait<blink::ScriptWrappable>::GetTraceDescriptor(native),
            false,
            cppgc::Persistent<blink::ScriptWrappable>(native),
            tag,
        };
    }

    void ClearWrapper(JSValueConst wrapper)
    {
        if (JS_IsObject(wrapper))
            wrappers_.erase(JS_VALUE_GET_PTR(wrapper));
    }

    const void* GetWrapperTypeInfo(JSValueConst wrapper) const
    {
        if (!JS_IsObject(wrapper))
            return nullptr;
        auto it = wrappers_.find(JS_VALUE_GET_PTR(wrapper));
        return it != wrappers_.end() && it->second.native
            ? it->second.native->GetWrapperTypeInfo() : nullptr;
    }

    void* GetBuiltinWrapper(JSValueConst wrapper,
        v8::CppHeapPointerTagRange range) const
    {
        if (!JS_IsObject(wrapper))
            return nullptr;
        const auto lower = static_cast<std::uint16_t>(range.lower_bound);
        const auto upper = static_cast<std::uint16_t>(range.upper_bound);
        if (lower > upper
            || upper > static_cast<std::uint16_t>(v8::CppHeapPointerTag::kLastTag))
            return nullptr;
        auto found = wrappers_.find(JS_VALUE_GET_PTR(wrapper));
        return found != wrappers_.end() && found->second.tag >= lower
                && found->second.tag <= upper
            ? found->second.native : nullptr;
    }

    void MarkBuiltinWrapper(JSValueConst wrapper)
    {
        MarkWrapper(wrapper, mark_func_);
    }

    void MarkWrapper(JSValueConst wrapper, JS_MarkFunc* mark_func)
    {
        // Dead-cycle refcount restoration invokes host markers too. It must
        // never enter native tracing or move nodes out of the dead-object list.
        if (phase_ != JS_EXTERNAL_GC_PHASE_SCAN || mark_func != mark_func_
            || !JS_IsObject(wrapper)) {
            return;
        }
        auto it = wrappers_.find(JS_VALUE_GET_PTR(wrapper));
        if (it == wrappers_.end() || it->second.live)
            return;

        WrapperRecord& record = it->second;
        record.live = true;
        ++stats_.live_wrapper_count;
        visitor_.AddRoot(record.native, record.descriptor);
        visitor_.Drain();
    }

    QuickJSGCStats Stats() const
    {
        QuickJSGCStats result = stats_;
        result.wrapper_count = wrappers_.size();
        result.edge_count = active_edges_;
        return result;
    }

private:

    static void ExternalTrace(JSRuntime* runtime, JSExternalGCPhase phase,
        JS_MarkFunc* mark_func, void* opaque)
    {
        auto* self = static_cast<CrossHeapState*>(opaque);
        if (self && self->runtime_ == runtime)
            self->OnExternalTrace(phase, mark_func);
    }

    void OnExternalTrace(JSExternalGCPhase phase, JS_MarkFunc* mark_func)
    {
        phase_ = phase;
        mark_func_ = mark_func;
        switch (phase) {
        case JS_EXTERNAL_GC_PHASE_DECREF:
            BeginEpoch(mark_func);
            break;
        case JS_EXTERNAL_GC_PHASE_SCAN:
            ScanNativeRoots();
            break;
        case JS_EXTERNAL_GC_PHASE_SWEEP:
            SweepJointlyUnreachableEdges();
            mark_func_ = nullptr;
            break;
        }
    }

    void BeginEpoch(JS_MarkFunc* mark_func)
    {
        // Bridge-owned roots protect wrappers between collections, but cannot
        // prove reachability in this collection. Native GC is atomic on this
        // same UI thread, so none can run during the rootless joint scan.
        for (auto& [key, wrapper] : wrappers_) {
            wrapper.root.Clear();
            wrapper.live = false;
        }
        // Refresh real pending-activity roots before either JS trial marking or
        // native root traversal. No V8 GC callback runs in this profile.
        RecomputeActiveWrappables();
        ++stats_.epoch;
        stats_.live_edge_count = 0;
        stats_.live_wrapper_count = 0;
        visitor_.Reset();
        for (EdgeRecord& edge : edges_) {
            edge.live = false;
            if (edge.active)
                JS_MarkValue(runtime_, edge.value, mark_func, nullptr);
        }
    }

    void ScanNativeRoots()
    {
        if (!heap_handle_)
            return;
        JointRootVisitor root_visitor(visitor_);
        VisitNativeHeapRoots(*heap_handle_, root_visitor, visitor_, stack_state_);
        visitor_.Drain();
    }

    void SweepJointlyUnreachableEdges()
    {
        for (std::size_t index = 0; index < edges_.size(); ++index) {
            const EdgeRecord& edge = edges_[index];
            if (!edge.active || edge.live)
                continue;
            // Object trial references were removed in DECREF. Primitive and
            // other ref-counted values still need their ordinary release.
            const bool free_value = !edge.participates_in_trial_deletion;
            ReleaseEdgeSlot(static_cast<std::uint32_t>(index + 1), free_value);
        }
        for (auto it = wrappers_.begin(); it != wrappers_.end();) {
            if (!it->second.live) {
                it = wrappers_.erase(it);
            } else {
                // Restore only proven-live wrappers for independent native GC.
                it->second.root = it->second.native;
                ++it;
            }
        }
    }

    void ReleaseEdgeSlot(std::uint32_t slot, bool free_value)
    {
        EdgeRecord& edge = edges_[slot - 1];
        DCHECK(edge.active);
        const JSValue value = edge.value;
        edge.value = JS_UNDEFINED;
        edge.module = nullptr;
        edge.active = false;
        edge.live = false;
        --active_edges_;
        // Reentrant class finalizers must see the old id invalidated before
        // releasing its value, even if registration immediately reuses this slot.
        if (edge.generation != std::numeric_limits<std::uint32_t>::max()) {
            ++edge.generation;
            edge.next_free = free_edge_;
            free_edge_ = slot;
        }
        if (runtime_ && free_value)
            JS_FreeValueRT(runtime_, value);
    }

    void ClearAllEdges(bool already_decremented)
    {
        for (std::size_t index = 0; index < edges_.size(); ++index) {
            if (edges_[index].active) {
                ReleaseEdgeSlot(static_cast<std::uint32_t>(index + 1),
                    !already_decremented);
            }
        }
    }

    EdgeRecord* Edge(QuickJSEdgeId id)
    {
        return const_cast<EdgeRecord*>(
            static_cast<const CrossHeapState*>(this)->Edge(id));
    }

    const EdgeRecord* Edge(QuickJSEdgeId id) const
    {
        const auto slot = static_cast<std::uint32_t>(id);
        const auto generation = static_cast<std::uint32_t>(id >> 32);
        if (!slot || !generation || slot > edges_.size())
            return nullptr;
        const EdgeRecord& edge = edges_[slot - 1];
        return edge.generation == generation ? &edge : nullptr;
    }

    v8::Isolate* const isolate_;
    JSRuntime* runtime_ = nullptr;
    cppgc::HeapHandle* heap_handle_ = nullptr;
    cppgc::EmbedderStackState stack_state_ = cppgc::EmbedderStackState::kMayContainHeapPointers;
    JSExternalGCPhase phase_ = JS_EXTERNAL_GC_PHASE_SWEEP;
    JS_MarkFunc* mark_func_ = nullptr;
    std::vector<EdgeRecord> edges_;
    std::uint32_t free_edge_ = 0;
    std::size_t active_edges_ = 0;
    std::unordered_map<const void*, WrapperRecord> wrappers_;
    JointVisitor visitor_;
    QuickJSGCStats stats_;
};

void JointVisitor::Reset()
{
    worklist_.clear();
    worklist_index_ = 0;
    ephemerons_.clear();
    live_native_.clear();
    visited_callbacks_.clear();
}

void JointVisitor::AddRoot(
    const void* object, cppgc::TraceDescriptor descriptor)
{
    Enqueue(object, descriptor);
}

void JointVisitor::Visit(
    const void* object, cppgc::TraceDescriptor descriptor)
{
    Enqueue(object, descriptor);
}

void JointVisitor::VisitWeak(const void*, cppgc::TraceDescriptor,
    cppgc::WeakCallback, const void*)
{
    // WeakMember does not retain its target in the joint graph.
}

void JointVisitor::VisitEphemeron(const void* key, const void* value,
    cppgc::TraceDescriptor descriptor)
{
    ephemerons_.push_back({ key, value, descriptor, false });
}

void JointVisitor::VisitWeakContainer(const void* object,
    cppgc::TraceDescriptor, cppgc::TraceDescriptor weak_descriptor,
    cppgc::WeakCallback, const void*)
{
    if (object && weak_descriptor.callback)
        Enqueue(object, { nullptr, weak_descriptor.callback });
}

void JointVisitor::Enqueue(
    const void* object, cppgc::TraceDescriptor descriptor)
{
    if (!object || !descriptor.callback)
        return;

    const void* trace_object = descriptor.base_object_payload
        ? descriptor.base_object_payload
        : object;
    const WorkKey key { trace_object, descriptor.callback };
    if (!visited_callbacks_.insert(key).second)
        return;

    if (descriptor.base_object_payload)
        live_native_.insert(descriptor.base_object_payload);
    worklist_.push_back({ trace_object, descriptor });
}

bool JointVisitor::ResolveEphemerons()
{
    bool added = false;
    for (Ephemeron& ephemeron : ephemerons_) {
        if (ephemeron.processed || !IsLive(ephemeron.key))
            continue;
        ephemeron.processed = true;
        const std::size_t old_size = worklist_.size();
        Enqueue(ephemeron.value, ephemeron.descriptor);
        added |= worklist_.size() != old_size;
    }
    return added;
}

void JointVisitor::Drain()
{
    // A VM scan can add native roots in several drains within one epoch.
    // Preserve the cursor: replaying the entire prefix for each wrapper makes
    // collection quadratic even though Enqueue deduplicates the work items.
    if (worklist_index_ == worklist_.size())
        return;
    for (;;) {
        while (worklist_index_ < worklist_.size()) {
            const WorkItem item = worklist_[worklist_index_++];
            item.descriptor.callback(this, item.object);
        }
        if (!ResolveEphemerons())
            break;
    }
}

bool JointVisitor::IsLive(const void* key) const
{
    return key && live_native_.find(key) != live_native_.end();
}

using StateMap = std::unordered_map<v8::Isolate*, std::unique_ptr<CrossHeapState>>;

StateMap& States()
{
    // Constructed while BlinkProcess initializes its isolate; its static
    // destructor is registered first and therefore runs after process teardown.
    // TLS destruction would run before BlinkProcess and invalidate live heaps.
    static StateMap states;
    return states;
}

CrossHeapState* Find(v8::Isolate* isolate)
{
    auto it = States().find(isolate);
    return it == States().end() ? nullptr : it->second.get();
}

CrossHeapState& Ensure(v8::Isolate* isolate)
{
    auto [it, inserted] = States().try_emplace(isolate);
    if (inserted)
        it->second = std::make_unique<CrossHeapState>(isolate);
    return *it->second;
}

} // namespace

void RegisterQuickJSIsolate(v8::Isolate* isolate, JSRuntime* runtime)
{
    if (!isolate || !runtime)
        return;
    Ensure(isolate).SetRuntime(runtime);
}

void UnregisterQuickJSIsolate(v8::Isolate* isolate)
{
    States().erase(isolate);
}

void AttachQuickJSCppgcHeap(
    v8::Isolate* isolate, cppgc::HeapHandle& heap_handle)
{
    if (isolate)
        Ensure(isolate).Attach(heap_handle);
}

void DetachQuickJSCppgcHeap(v8::Isolate* isolate)
{
    if (CrossHeapState* state = Find(isolate))
        state->Detach();
}

void DetachQuickJSCppgcHeap(cppgc::HeapHandle& heap_handle)
{
    for (const auto& [isolate, state] : States()) {
        if (state->IsAttachedTo(heap_handle)) {
            state->Detach();
            return;
        }
    }
}

void RecomputeQuickJSActiveWrappables(cppgc::HeapHandle& heap_handle)
{
    for (const auto& [isolate, state] : States()) {
        if (state->IsAttachedTo(heap_handle)) {
            state->RecomputeActiveWrappables();
            return;
        }
    }
}

void CollectQuickJSGarbage(v8::Isolate* isolate, cppgc::EmbedderStackState stack_state)
{
    auto* state = Find(isolate);
    CHECK(state);
    state->Collect(stack_state);
    v8::detail::CollectWeakPersistents(isolate);
}

QuickJSEdgeId RegisterQuickJSStrongEdge(
    v8::Isolate* isolate, JSValueConst value)
{
    CrossHeapState* state = Find(isolate);
    return state ? state->RegisterEdge(value) : kInvalidQuickJSEdge;
}

void ResetQuickJSStrongEdge(v8::Isolate* isolate, QuickJSEdgeId edge)
{
    if (CrossHeapState* state = Find(isolate))
        state->ResetEdge(edge);
}

bool IsQuickJSStrongEdgeAlive(v8::Isolate* isolate, QuickJSEdgeId edge)
{
    CrossHeapState* state = Find(isolate);
    return state && state->IsEdgeAlive(edge);
}

JSValueConst GetQuickJSStrongEdge(
    v8::Isolate* isolate, QuickJSEdgeId edge)
{
    CrossHeapState* state = Find(isolate);
    return state ? state->GetEdge(edge) : JS_UNDEFINED;
}

void TraceQuickJSStrongEdge(v8::Isolate* isolate, QuickJSEdgeId edge)
{
    if (CrossHeapState* state = Find(isolate))
        state->TraceEdge(edge);
}

void RegisterQuickJSWrapper(v8::Isolate* isolate, JSValueConst wrapper,
    blink::ScriptWrappable* native)
{
    if (CrossHeapState* state = Find(isolate))
        state->RegisterWrapper(wrapper, native);
}

const void* GetQuickJSWrapperTypeInfo(v8::Isolate* isolate, JSValueConst wrapper)
{
    CrossHeapState* state = Find(isolate);
    return state ? state->GetWrapperTypeInfo(wrapper) : nullptr;
}

void ClearQuickJSWrapper(v8::Isolate* isolate, JSValueConst wrapper)
{
    if (CrossHeapState* state = Find(isolate))
        state->ClearWrapper(wrapper);
}

void MarkQuickJSWrapper(v8::Isolate* isolate, JSValueConst wrapper,
    JS_MarkFunc* mark_func)
{
    if (CrossHeapState* state = Find(isolate))
        state->MarkWrapper(wrapper, mark_func);
}

QuickJSGCStats GetQuickJSGCStats(v8::Isolate* isolate)
{
    CrossHeapState* state = Find(isolate);
    return state ? state->Stats() : QuickJSGCStats {};
}

} // namespace mini_ui

namespace v8::detail {
mini_ui::QuickJSEdgeId RegisterModuleEdge(
    Isolate* isolate, JSContext* context, JSModuleDef* module)
{
    auto* state = mini_ui::Find(isolate);
    return state ? state->RegisterModuleEdge(context, module) : mini_ui::kInvalidQuickJSEdge;
}

JSContext* ModuleEdgeContext(Isolate* isolate, mini_ui::QuickJSEdgeId id)
{
    auto* state = mini_ui::Find(isolate);
    return state ? state->ModuleContext(id) : nullptr;
}

namespace {
void BuiltinWrapperEvent(JSValue wrapper, void* owner, JS_USER_DATA_WEAK_STEP step)
{
    auto* state = mini_ui::Find(static_cast<Isolate*>(owner));
    if (!state)
        return;
    if (step == JS_USER_DATA_WEAK_FREE)
        state->ClearWrapper(wrapper);
    else if (step == JS_USER_DATA_WEAK_SCAN)
        state->MarkBuiltinWrapper(wrapper);
}
}

void SetBuiltinWrapper(Isolate* isolate, JSValueConst wrapper,
    void* native, CppHeapPointerTag tag)
{
    auto* state = mini_ui::Find(isolate);
    if (!state || !JS_IsObject(wrapper))
        std::abort();
    void* owner = JS_GetUserdata(wrapper);
    if (owner && owner != isolate)
        std::abort();
    if (!native) {
        state->ClearWrapper(wrapper);
        return;
    }
    state->RegisterWrapper(wrapper, static_cast<blink::ScriptWrappable*>(native),
        static_cast<std::uint16_t>(tag));
    if (!owner)
        JS_SetUserdata(JS_GetObjectContext(wrapper), wrapper,
            &BuiltinWrapperEvent, isolate);
}

void* GetBuiltinWrapper(Isolate* isolate, JSValueConst wrapper,
    CppHeapPointerTagRange range)
{
    auto* state = mini_ui::Find(isolate);
    return state ? state->GetBuiltinWrapper(wrapper, range) : nullptr;
}
} // namespace v8::detail
