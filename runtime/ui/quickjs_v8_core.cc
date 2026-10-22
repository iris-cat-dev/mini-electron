// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "runtime/ui/quickjs_v8_internal.h"
#include "runtime/ui/quickjs_v8_values.h"
#include "runtime/ui/quickjs_v8_objects.h"
#include "runtime/ui/quickjs_v8_scripts.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <thread>
#include <unordered_map>
#include <vector>

namespace v8 {
namespace detail {
struct WeakPersistent {
    Isolate* isolate = nullptr;
    JSRuntime* runtime = nullptr;
    JSWeakContext* context_observer = nullptr;
    JSValue weak_reference = JS_UNDEFINED;
    uint64_t observed_gc_epoch = 0;
    std::weak_ptr<DataPayload> payload;
    ConstructView construct = nullptr;
    DestroyView destroy = nullptr;
    unsigned type_flags = 0;
    void* parameter = nullptr;
    void* fields[2] = {nullptr, nullptr};
    WeakCallback callback;
    bool dead = false;
    bool callback_ready = false;
    bool notified = false;
    bool cancelled = false;
    bool internal_fields = false;
    ~WeakPersistent() {
        JS_FreeValueRT(runtime, weak_reference);
        if (context_observer) JS_FreeWeakContext(runtime, context_observer);
    }
};
}
namespace {
struct CoreThreadState { std::vector<Isolate*> entered; };
struct ContextRegistration {
    Isolate* isolate;
    std::weak_ptr<detail::DataPayload> payload;
};
struct CoreRegistry {
    const std::thread::id owner_thread = std::this_thread::get_id();
    std::unordered_map<JSRuntime*, Isolate*> runtimes;
    std::unordered_map<JSContext*, ContextRegistration> contexts;
    std::unordered_map<std::thread::id, CoreThreadState> threads;
};
CoreRegistry& Registry() {
    // Initialized by CreateForQuickJS before the product owner's static
    // construction completes; consequently outlives that owner's destructor.
    static CoreRegistry registry;
    // NativeHeap and the QuickJS runtime are owned by this single UI thread.
    if (registry.owner_thread != std::this_thread::get_id()) std::abort();
    return registry;
}
std::vector<Isolate*>& EnteredIsolates() {
    thread_local CoreThreadState* thread_state = nullptr;
    if (!thread_state) {
        auto& registry = Registry();
        thread_state = &registry.threads[std::this_thread::get_id()];
    }
    return thread_state->entered;
}
constexpr size_t kArenaBlockSize = 256;
struct ExceptionFrame {
    detail::ExceptionSink* sink = nullptr;
    TryCatch* try_catch = nullptr;
};
struct MessageListener {
    MessageCallback callback = nullptr;
    int levels = 0;
    Global<Value> data;
};
struct NativeJob {
    MicrotaskCallback callback;
    void* data;
};
JSClassID native_job_class = 0;
void FreeNativeJob(JSRuntime*, JSValue value) {
    delete static_cast<NativeJob*>(JS_GetOpaque(value, native_job_class));
}
JSValue RunNativeJob(JSContext* context, int argc, JSValueConst* argv) {
    if (argc != 1) std::abort();
    auto* job = static_cast<NativeJob*>(JS_GetOpaque(argv[0], native_job_class));
    if (!job || !job->callback) std::abort();
    job->callback(job->data);
    auto* isolate = detail::IsolateFor(context);
    if (isolate && detail::HasPendingException(isolate))
        return JS_Throw(context, detail::TakePendingException(isolate));
    return JS_UNDEFINED;
}
JSValue RunFunctionJob(JSContext* context, int argc, JSValueConst* argv) {
    if (argc != 1) std::abort();
    return JS_Call(context, argv[0], JS_UNDEFINED, 0, nullptr);
}
struct FixedArrayPayload final : detail::DataPayload {
    explicit FixedArrayPayload(size_t length) : cells(length, nullptr) {}
    ~FixedArrayPayload() override { for (auto* cell : cells) if (cell) detail::Release(cell); }
    std::vector<detail::Cell*> cells;
};
}

struct Isolate::Impl {
    JSRuntime* runtime = nullptr;
    JSContext* primitive_context = nullptr;
    JSContext* pending_context = nullptr;
    std::thread::id thread = std::this_thread::get_id();
    std::array<void*, 64> data{};
    HandleScope* scope = nullptr;
    std::vector<Context*> contexts;
    std::vector<std::unique_ptr<detail::Cell[]>> cell_blocks;
    std::vector<std::unique_ptr<detail::ApiView[]>> view_blocks;
    detail::Cell* free_cells = nullptr;
    detail::ApiView* free_views = nullptr;
    size_t live_cells = 0;
    std::array<detail::Cell*, 4> primitive_roots{};
    JSContext* exception_context = nullptr;
    JSValue exception = JS_UNDEFINED;
    bool has_exception = false;
    bool exception_is_termination = false;
    bool reporting_exception = false;
    detail::ExceptionMetadata exception_metadata;
    std::vector<ExceptionFrame> exception_frames;
    MicrotasksPolicy microtasks_policy = MicrotasksPolicy::kAuto;
    std::unique_ptr<MicrotaskQueue> default_microtask_queue;
    int microtasks_depth = 0;
    int microtasks_suppressed = 0;
    int execution_depth = 0;
    int native_callback_depth = 0;
    const Context::BackupIncumbentScope* backup_incumbent_scope = nullptr;
    bool running_microtasks = false;
    std::vector<std::pair<MicrotasksCompletedCallbackWithData, void*>> completed_callbacks;
    std::vector<std::shared_ptr<MessageListener>> message_listeners;
    int64_t external_memory = 0;
    std::atomic<bool> terminating{false};
    bool disposing = false;
    std::vector<std::weak_ptr<detail::WeakPersistent>> weak_persistents;
    std::unordered_map<void*, std::vector<std::weak_ptr<detail::WeakPersistent>>> weak_internal_fields;
    bool collecting_weak = false;
    bool weak_notifications_pending = false;
    uint64_t completed_gc_epoch = 0;
};

namespace detail {
struct CoreAccess {
    static Isolate::Impl& Get(Isolate* isolate) { CheckIsolate(isolate); return *isolate->impl_; }
    static void PushBackupIncumbent(Context::BackupIncumbentScope* scope, Context* context) {
        if (!scope || !context) std::abort();
        auto* cell = CellOf(context);
        if (!cell) std::abort();
        auto& state = Get(cell->isolate);
        if (scope->isolate_ || state.disposing) std::abort();
        Retain(cell);
        scope->isolate_ = cell->isolate;
        scope->context_cell_ = cell;
        scope->previous_ = state.backup_incumbent_scope;
        scope->execution_depth_ = state.execution_depth;
        state.backup_incumbent_scope = scope;
    }
    static void PopBackupIncumbent(Context::BackupIncumbentScope* scope) {
        if (!scope || !scope->isolate_) std::abort();
        auto& state = Get(scope->isolate_);
        if (state.backup_incumbent_scope != scope) std::abort();
        state.backup_incumbent_scope = scope->previous_;
        auto* cell = std::exchange(scope->context_cell_, nullptr);
        scope->previous_ = nullptr;
        scope->execution_depth_ = 0;
        scope->isolate_ = nullptr;
        Release(cell);
    }
    static Cell* BackupIncumbentCell(Isolate* isolate) {
        auto& state = Get(isolate);
        const auto* scope = state.backup_incumbent_scope;
        // A QuickJS execution already active when the scope was pushed is
        // older than the backup entry. Only a nested execution can supersede it.
        if (!scope || state.execution_depth > scope->execution_depth_) return nullptr;
        return scope->context_cell_;
    }
    static ApiView* AllocateView(Isolate* isolate) {
        auto& state = Get(isolate);
        if (!state.free_views) {
            auto block = std::make_unique<ApiView[]>(kArenaBlockSize);
            for (size_t i = 0; i < kArenaBlockSize; ++i) {
                block[i].next = state.free_views;
                state.free_views = &block[i];
            }
            state.view_blocks.push_back(std::move(block));
        }
        auto* view = state.free_views;
        state.free_views = view->next;
        view->next = nullptr;
        return view;
    }
    static void RecycleView(Isolate* isolate, ApiView* view) {
        auto& state = Get(isolate);
        view->destroy(view->view);
        view->view = nullptr;
        view->construct = nullptr;
        view->destroy = nullptr;
        view->next = state.free_views;
        state.free_views = view;
    }
};

static void WeakTargetDied(JSRuntime*, void* opaque) {
    auto* weak = static_cast<WeakPersistent*>(opaque);
    weak->dead = true;
    weak->callback_ready = true;
    CoreAccess::Get(weak->isolate).weak_notifications_pending = true;
}

ExecutionScope::ExecutionScope(Isolate* isolate) : isolate_(isolate) {
    auto& state = CoreAccess::Get(isolate);
    isolate->Enter();
    ++state.execution_depth;
}
ExecutionScope::~ExecutionScope() {
    auto& state = CoreAccess::Get(isolate_);
    if (state.execution_depth <= 0) std::abort();
    --state.execution_depth;
    if (!state.execution_depth && !state.has_exception && state.microtasks_policy == MicrotasksPolicy::kAuto)
        isolate_->PerformMicrotaskCheckpoint();
    isolate_->Exit();
}
std::shared_ptr<WeakPersistent> MakeWeakPersistent(Cell* cell, void* parameter,
    WeakCallback callback, bool internal_fields) {
    if (!cell) std::abort();
    auto& state = CoreAccess::Get(cell->isolate);
    auto weak = std::make_shared<WeakPersistent>();
    weak->isolate = cell->isolate;
    weak->runtime = state.runtime;
    weak->payload = cell->payload;
    weak->construct = cell->primary.construct;
    weak->destroy = cell->primary.destroy;
    weak->type_flags = cell->type_flags;
    weak->parameter = parameter;
    weak->callback = std::move(callback);
    weak->internal_fields = internal_fields;
    if (cell->type_flags & 64) {
        // The observer owns no JSContext reference and no metadata payload.
        weak->context_observer = JS_NewWeakContext(cell->context, WeakTargetDied, weak.get());
        if (!weak->context_observer) { CaptureException(cell->isolate, cell->context); return {}; }
    } else {
        if (!(cell->type_flags & 1)) std::abort();
        if (internal_fields && JS_IsObject(cell->value)) {
            auto* object = RebindView<Object>(cell);
            for (int i = 0; i < std::min(2, object->InternalFieldCount()); ++i)
                weak->fields[i] = object->GetAlignedPointerFromInternalField(i);
        }
        if (JS_IsObject(cell->value)) {
            // A weak token's creation-context edge must never pin its target
            // realm. This private scratch context has no DOM/global bindings.
            weak->weak_reference = JS_NewWeakRef(state.primitive_context, cell->value);
            if (JS_IsException(weak->weak_reference)) {
                weak->weak_reference = JS_UNDEFINED;
                CaptureException(cell->isolate, state.primitive_context);
                return {};
            }
            if (!JS_GetCompletedGCEpoch(state.runtime, &weak->observed_gc_epoch)) std::abort();
        } else {
            JS_ThrowTypeError(state.primitive_context, "Unsupported weak reference target");
            CaptureException(cell->isolate, state.primitive_context);
            return {};
        }
    }
    state.weak_persistents.push_back(weak);
    if (internal_fields && (cell->type_flags & 1) && JS_IsObject(cell->value))
        state.weak_internal_fields[JS_VALUE_GET_PTR(cell->value)].push_back(weak);
    return weak;
}
bool WeakPersistentEmpty(const std::shared_ptr<WeakPersistent>& weak) {
    if (!weak || weak->dead) return true;
    if (weak->context_observer && !JS_GetWeakContext(weak->context_observer))
        WeakTargetDied(weak->runtime, weak.get());
    if (JS_IsObject(weak->weak_reference)) {
        auto& state = CoreAccess::Get(weak->isolate);
        JSValue target = JS_GetWeakRefValue(state.primitive_context, weak->weak_reference);
        if (JS_IsException(target)) { CaptureException(weak->isolate, state.primitive_context); std::abort(); }
        // Reads can observe death immediately; notification still waits for a
        // completed collection epoch in CollectWeakPersistents.
        if (JS_IsUndefined(target)) weak->dead = true;
        JS_FreeValueRT(state.runtime, target);
    }
    return weak->dead;
}
void CancelWeakPersistent(const std::shared_ptr<WeakPersistent>& weak) {
    if (!weak || weak->cancelled) return;
    auto& state = CoreAccess::Get(weak->isolate);
    weak->cancelled = true;
    weak->dead = true;
    weak->callback = {};
    JS_FreeValueRT(weak->runtime, std::exchange(weak->weak_reference, JS_UNDEFINED));
    if (auto* observer = std::exchange(weak->context_observer, nullptr))
        JS_FreeWeakContext(weak->runtime, observer);
    state.weak_notifications_pending = true;
}
Local<Data> LocalizeWeak(Isolate* isolate, const std::shared_ptr<WeakPersistent>& weak) {
    CheckIsolate(isolate);
    if (!weak || weak->isolate != isolate) std::abort();
    if (weak->dead) return {};
    if (weak->type_flags & 64) {
        auto* context = JS_GetWeakContext(weak->context_observer);
        if (!context) { WeakTargetDied(weak->runtime, weak.get()); return {}; }
        // Lock metadata only while producing a strong Local. NativeData, not
        // this observation, owns it for the actual engine context lifetime.
        if (auto* canonical = LookupContext(context)) return Local<Data>(canonical);
        auto payload = weak->payload.lock();
        if (!payload) std::abort();
        return MakeLocal<Context>(isolate, context, JS_UNDEFINED, std::move(payload));
    }
    auto payload = weak->payload.lock();
    // The owned target carries its own VM-visible creation-context edge.
    auto* context = CoreAccess::Get(isolate).primitive_context;
    JSValue owned = JS_GetWeakRefValue(context, weak->weak_reference);
    if (JS_IsException(owned)) { CaptureException(isolate, context); return {}; }
    if (weak->dead || JS_IsUndefined(owned)) {
        JS_FreeValue(context, owned);
        weak->dead = true;
        return {};
    }
    return Local<Data>(ViewOf(AllocateCell(isolate, context, owned, std::move(payload),
        weak->construct, weak->destroy, weak->type_flags)));
}
Cell* StrengthenWeak(const std::shared_ptr<WeakPersistent>& weak) {
    if (!weak) return nullptr;
    HandleScope scope(weak->isolate);
    auto local = LocalizeWeak(weak->isolate, weak);
    auto* cell = CellOf(*local);
    if (cell) Retain(cell);
    return cell;
}
std::shared_ptr<WeakPersistent> CopyWeakPersistent(const std::shared_ptr<WeakPersistent>& weak) {
    if (!weak) return {};
    HandleScope scope(weak->isolate);
    auto local = LocalizeWeak(weak->isolate, weak);
    if (local.IsEmpty()) return {};
    auto copy = MakeWeakPersistent(CellOf(*local), weak->parameter, weak->callback, weak->internal_fields);
    if (copy) { copy->fields[0] = weak->fields[0]; copy->fields[1] = weak->fields[1]; }
    return copy;
}
void* WeakParameter(const std::shared_ptr<WeakPersistent>& weak) { return weak ? weak->parameter : nullptr; }
Isolate* WeakIsolate(const std::shared_ptr<WeakPersistent>& weak) { return weak ? weak->isolate : nullptr; }
void UpdateWeakInternalField(Isolate* isolate, JSValueConst object, int index, void* pointer) {
    if (index < 0) std::abort();
    if (index >= kEmbedderFieldsInWeakCallback) return;
    auto& state = CoreAccess::Get(isolate);
    if (!JS_IsObject(object)) std::abort();
    auto found = state.weak_internal_fields.find(JS_VALUE_GET_PTR(object));
    if (found == state.weak_internal_fields.end()) return;
    auto& slots = found->second;
    slots.erase(std::remove_if(slots.begin(), slots.end(), [&](const auto& entry) {
        auto weak = entry.lock();
        if (!weak || weak->dead) return true;
        // An object address can be reused before the next completed GC polls
        // its old weak token. Never overwrite that dead target's field snapshot.
        JSValue target = JS_GetWeakRefValue(state.primitive_context, weak->weak_reference);
        if (JS_IsException(target)) { CaptureException(isolate, state.primitive_context); std::abort(); }
        const bool matches = JS_IsObject(target) && JS_VALUE_GET_PTR(target) == JS_VALUE_GET_PTR(object);
        JS_FreeValueRT(state.runtime, target);
        if (!matches) { weak->dead = true; return true; }
        weak->fields[index] = pointer;
        return false;
    }), slots.end());
    if (slots.empty()) state.weak_internal_fields.erase(found);
}
void CollectWeakPersistents(Isolate* isolate) {
    auto& state = CoreAccess::Get(isolate);
    if (state.collecting_weak) return;
    uint64_t completed_epoch = 0;
    if (!JS_GetCompletedGCEpoch(state.runtime, &completed_epoch)) return;
    if (completed_epoch == state.completed_gc_epoch && !state.weak_notifications_pending) return;
    state.collecting_weak = true;
    struct CollectionGuard {
        bool& collecting;
        ~CollectionGuard() { collecting = false; }
    } collection_guard{state.collecting_weak};
    Isolate::Scope isolate_scope(isolate);
    if (completed_epoch != state.completed_gc_epoch) {
        state.completed_gc_epoch = completed_epoch;
        for (const auto& entry : state.weak_persistents) {
            auto weak = entry.lock();
            if (!weak || weak->cancelled || !JS_IsObject(weak->weak_reference)
                || weak->observed_gc_epoch == completed_epoch) continue;
            weak->observed_gc_epoch = completed_epoch;
            if (weak->dead) {
                WeakTargetDied(state.runtime, weak.get());
                continue;
            }
            JSValue target = JS_GetWeakRefValue(state.primitive_context, weak->weak_reference);
            if (JS_IsException(target)) { CaptureException(isolate, state.primitive_context); return; }
            if (JS_IsUndefined(target)) WeakTargetDied(state.runtime, weak.get());
            JS_FreeValueRT(state.runtime, target);
        }
    }
    if (!state.weak_notifications_pending) return;
    state.weak_notifications_pending = false;
    std::vector<std::shared_ptr<WeakPersistent>> dead;
    auto& slots = state.weak_persistents;
    slots.erase(std::remove_if(slots.begin(), slots.end(), [&](const auto& entry) {
        auto weak = entry.lock();
        if (!weak) return true;
        if (weak->dead && weak->callback_ready && !weak->notified) {
            weak->notified = true;
            dead.push_back(std::move(weak));
        }
        return false;
    }), slots.end());
    for (auto it = state.weak_internal_fields.begin(); it != state.weak_internal_fields.end();) {
        auto& entries = it->second;
        entries.erase(std::remove_if(entries.begin(), entries.end(), [](const auto& entry) {
            auto weak = entry.lock(); return !weak || weak->dead;
        }), entries.end());
        if (entries.empty()) it = state.weak_internal_fields.erase(it);
        else ++it;
    }
    std::vector<WeakSecondPass> second_pass;
    for (const auto& weak : dead) {
        if (!weak->cancelled && weak->callback) {
            // A first-pass callback must Reset its own handle. Moving the
            // callable out prevents that Reset from destroying an active call.
            auto callback = std::move(weak->callback);
            auto second = callback(isolate, weak->parameter, weak->fields);
            if (second) second_pass.push_back(std::move(second));
        }
    }
    for (auto& callback : second_pass) callback();
}

void EnterNativeCallback(Isolate* isolate) {
    ++CoreAccess::Get(isolate).native_callback_depth;
}
void LeaveNativeCallback(Isolate* isolate) {
    auto& depth = CoreAccess::Get(isolate).native_callback_depth;
    if (depth <= 0) std::abort();
    --depth;
}

bool CanCollectAtHostIdle(Isolate* isolate) {
    auto& state = CoreAccess::Get(isolate);
    return !state.disposing && !state.execution_depth && !state.native_callback_depth && !state.running_microtasks
        && !state.microtasks_depth && !state.microtasks_suppressed
        && !state.collecting_weak && !state.has_exception
        && !JS_IsJobPending(state.runtime) && !JS_IsJobRunning(state.runtime);
}

std::size_t HandleArenaAllocatedBytes(Isolate* isolate) {
    const auto& state = CoreAccess::Get(isolate);
    return state.cell_blocks.size() * kArenaBlockSize * sizeof(Cell)
        + state.view_blocks.size() * kArenaBlockSize * sizeof(ApiView);
}
HandleArenaStats GetHandleArenaStats(Isolate* isolate) {
    const auto& state = CoreAccess::Get(isolate);
    return {
        state.cell_blocks.size() * kArenaBlockSize * sizeof(Cell)
            + state.view_blocks.size() * kArenaBlockSize * sizeof(ApiView),
        state.cell_blocks.size() * kArenaBlockSize,
        state.view_blocks.size() * kArenaBlockSize,
        state.live_cells
    };
}
void TrimHandleArenas(Isolate* isolate) {
    auto& state = CoreAccess::Get(isolate);
    if (!CanCollectAtHostIdle(isolate)) return;
    // Only completely free arrays can move out of ownership. Live Cells and
    // ApiViews keep their exact addresses; retain one empty array per pool.
    if (state.cell_blocks.size() > 1) {
        bool spare = false;
        const auto removed = std::erase_if(state.cell_blocks, [&](const auto& block) {
            for (std::size_t i = 0; i < kArenaBlockSize; ++i)
                if (block[i].isolate) return false;
            return std::exchange(spare, true);
        });
        if (removed) {
            state.free_cells = nullptr;
            for (const auto& block : state.cell_blocks) {
                for (std::size_t i = 0; i < kArenaBlockSize; ++i) {
                    if (block[i].isolate) continue;
                    block[i].free_next = state.free_cells;
                    state.free_cells = &block[i];
                }
            }
        }
    }
    if (state.view_blocks.size() > 1) {
        bool spare = false;
        const auto removed = std::erase_if(state.view_blocks, [&](const auto& block) {
            for (std::size_t i = 0; i < kArenaBlockSize; ++i)
                if (block[i].view) return false;
            return std::exchange(spare, true);
        });
        if (removed) {
            state.free_views = nullptr;
            for (const auto& block : state.view_blocks) {
                for (std::size_t i = 0; i < kArenaBlockSize; ++i) {
                    if (block[i].view) continue;
                    block[i].next = state.free_views;
                    state.free_views = &block[i];
                }
            }
        }
    }
}

void CheckIsolate(Isolate* isolate) {
    if (!isolate || isolate->impl_->thread != std::this_thread::get_id()) std::abort();
}
Cell* CellOf(const Data* data) { return data ? data->cell_ : nullptr; }
Data* ViewOf(Cell* cell) { return cell ? cell->primary.view : nullptr; }
JSValueConst Borrow(const Data* data) { auto* cell = CellOf(data); return cell ? cell->value : JS_UNDEFINED; }
JSContext* ContextOf(const Data* data) {
    auto* cell = CellOf(data);
    if (!cell) return nullptr;
    if (cell->context) return cell->context;
    if (!(cell->type_flags & kIsolateData) && JS_IsObject(cell->value)) {
        if (auto* origin = JS_GetObjectContext(cell->value)) return origin;
    }
    return NeutralJSContext(cell->isolate);
}
Isolate* IsolateOf(const Data* data) { auto* cell = CellOf(data); return cell ? cell->isolate : nullptr; }
std::shared_ptr<DataPayload> Payload(const Data* data) { auto* cell = CellOf(data); return cell ? cell->payload : nullptr; }
void SetPayload(Data* data, std::shared_ptr<DataPayload> payload) {
    auto* cell = CellOf(data); if (!cell) std::abort(); CheckIsolate(cell->isolate); cell->payload = std::move(payload);
}
JSContext* CurrentJSContext(Isolate* isolate) {
    auto& state = CoreAccess::Get(isolate);
    return state.contexts.empty() ? state.primitive_context : ContextOf(state.contexts.back());
}
JSContext* NeutralJSContext(Isolate* isolate) {
    auto& state = CoreAccess::Get(isolate);
    if (!state.primitive_context) std::abort();
    return state.primitive_context;
}
Isolate* IsolateFor(JSContext* context) {
    auto& registry = Registry();
    auto& runtime_isolates = registry.runtimes;
    if (!context) return nullptr;
    auto it = runtime_isolates.find(JS_GetRuntime(context));
    return it == runtime_isolates.end() ? nullptr : it->second;
}
Local<Value> Adopt(JSContext* context, JSValue owned) {
    auto* isolate = IsolateFor(context);
    if (!isolate) { if (context) JS_FreeValue(context, owned); std::abort(); }
    if (JS_IsException(owned)) { CaptureException(isolate, context); return {}; }
    return ClassifyAndAdopt(isolate, context, owned);
}
Cell* AllocateCell(Isolate* isolate, JSContext* context, JSValue owned,
    std::shared_ptr<DataPayload> payload, ConstructView construct, DestroyView destroy, unsigned flags) {
    auto& state = CoreAccess::Get(isolate);
    if (!context || JS_GetRuntime(context) != state.runtime || !state.scope || state.disposing) {
        if (context) JS_FreeValue(context, owned);
        std::abort();
    }
    if (!state.free_cells) {
        auto block = std::make_unique<Cell[]>(kArenaBlockSize);
        for (size_t i = 0; i < kArenaBlockSize; ++i) {
            block[i].free_next = state.free_cells;
            state.free_cells = &block[i];
        }
        state.cell_blocks.push_back(std::move(block));
    }
    auto* cell = state.free_cells;
    state.free_cells = cell->free_next;
    cell->free_next = nullptr;
    cell->isolate = isolate;
    // Only real Context/compiled metadata owns a context reference. Objects
    // already have a GC-visible creation-context edge; primitives are neutral.
    cell->context = (flags & kPinnedContext) ? JS_DupContext(context) : nullptr;
    cell->value = owned;
    cell->payload = std::move(payload);
    cell->owners = 1;
    cell->type_flags = flags;
    cell->read_only_root = false;
    cell->scope = state.scope;
    cell->previous = nullptr;
    cell->next = state.scope->head_;
    if (cell->next) cell->next->previous = cell;
    state.scope->head_ = cell;
    cell->primary.construct = construct;
    cell->primary.destroy = destroy;
    cell->primary.view = construct(cell->primary.storage, cell);
    ++state.live_cells;
    return cell;
}
Data* RebindCellView(Cell* cell, ConstructView construct, DestroyView destroy) {
    if (!cell) return nullptr;
    CheckIsolate(cell->isolate);
    if (cell->primary.construct == construct) return cell->primary.view;
    for (auto* view = cell->extra_views; view; view = view->next)
        if (view->construct == construct) return view->view;
    auto* view = CoreAccess::AllocateView(cell->isolate);
    view->construct = construct;
    view->destroy = destroy;
    view->view = construct(view->storage, cell);
    view->next = cell->extra_views;
    cell->extra_views = view;
    return view->view;
}
void Retain(Cell* cell) { if (!cell) return; CheckIsolate(cell->isolate); if (!cell->owners) std::abort(); ++cell->owners; }
void Release(Cell* cell) {
    if (!cell) return;
    CheckIsolate(cell->isolate);
    if (!cell->owners) std::abort();
    if (--cell->owners) return;
    if (cell->scope) std::abort();
    auto* isolate = cell->isolate;
    auto& state = CoreAccess::Get(isolate);
    auto payload = std::move(cell->payload);
    cell->primary.destroy(cell->primary.view);
    cell->primary.view = nullptr;
    while (cell->extra_views) {
        auto* view = cell->extra_views;
        cell->extra_views = view->next;
        CoreAccess::RecycleView(isolate, view);
    }
    JS_FreeValueRT(state.runtime, cell->value);
    cell->value = JS_UNDEFINED;
    // Metadata can own other Cells. Release it before the context and arena slot.
    payload.reset();
    if (cell->context) JS_FreeContext(cell->context);
    cell->context = nullptr;
    cell->isolate = nullptr;
    cell->type_flags = 0;
    cell->read_only_root = false;
    cell->free_next = state.free_cells;
    state.free_cells = cell;
    --state.live_cells;
}
Local<Data> Localize(Isolate* isolate, Cell* cell) {
    if (!cell) return {};
    auto& state = CoreAccess::Get(isolate);
    if (cell->isolate != isolate) std::abort();
    if (cell->read_only_root) return Local<Data>(ViewOf(cell));
    if (!state.scope) std::abort();
    for (auto* scope = state.scope; scope; scope = scope->parent_)
        if (cell->scope == scope) return Local<Data>(ViewOf(cell));
    // A persistent-only Cell needs a scope-owned clone; a live ancestor Local
    // already has the required lifetime and is reused without allocation.
    auto* clone = AllocateCell(isolate, ContextOf(ViewOf(cell)), JS_DupValueRT(state.runtime, cell->value),
        cell->payload, cell->primary.construct, cell->primary.destroy, cell->type_flags);
    return Local<Data>(ViewOf(clone));
}
Data* ReadOnlyPrimitiveRoot(Isolate* isolate, PrimitiveRoot root) {
    auto& state = CoreAccess::Get(isolate);
    const auto index = static_cast<unsigned>(root);
    if (index >= state.primitive_roots.size() || !state.primitive_roots[index]) std::abort();
    return ViewOf(state.primitive_roots[index]);
}
Local<Value> LocalizeEdge(Isolate* isolate, mini_ui::QuickJSEdgeId edge) {
    CheckIsolate(isolate);
    if (!edge || !mini_ui::IsQuickJSStrongEdgeAlive(isolate, edge)) return {};
    auto* context = CurrentJSContext(isolate);
    return Adopt(context, JS_DupValue(context, mini_ui::GetQuickJSStrongEdge(isolate, edge)));
}
bool Equal(const Data* left, const Data* right) {
    if (!left || !right) return left == right;
    auto* a = CellOf(left); auto* b = CellOf(right);
    if (a == b) return true;
    if (a->isolate != b->isolate) return false;
    if (!(a->type_flags & 1) || !(b->type_flags & 1)) return a->payload && a->payload == b->payload;
    return JS_IsValueEq(ContextOf(left), a->value, b->value, true);
}
void RegisterContext(Isolate* isolate, JSContext* context, Context* view) {
    auto& registry = Registry();
    auto& context_cells = registry.contexts;
    CheckIsolate(isolate);
    auto* cell = CellOf(view);
    if (!context || !cell || cell->isolate != isolate || cell->context != context || !cell->payload) std::abort();
    auto it = context_cells.find(context);
    if (it != context_cells.end()) {
        auto existing = it->second.payload.lock();
        if (existing && existing != cell->payload) std::abort();
        it->second = {isolate, cell->payload};
        return;
    }
    context_cells.emplace(context, ContextRegistration{isolate, cell->payload});
}
void UnregisterContext(JSContext* context) {
    auto& registry = Registry();
    auto& context_cells = registry.contexts;
    auto it = context_cells.find(context);
    if (it == context_cells.end()) return;
    context_cells.erase(it);
    ForgetScriptContext(context);
}
Context* LookupContext(JSContext* context) {
    auto& registry = Registry();
    auto& context_cells = registry.contexts;
    auto it = context_cells.find(context);
    if (it == context_cells.end()) return nullptr;
    auto payload = it->second.payload.lock();
    if (!payload) {
        context_cells.erase(it);
        ForgetScriptContext(context);
        return nullptr;
    }
    return *MakeLocal<Context>(it->second.isolate, context, JS_UNDEFINED, std::move(payload));
}
void EnterContext(Isolate* isolate, Context* context) {
    auto& state = CoreAccess::Get(isolate);
    if (!context || IsolateOf(context) != isolate) std::abort();
    Retain(CellOf(context));
    state.contexts.push_back(context);
}
void LeaveContext(Isolate* isolate, Context* context) {
    auto& state = CoreAccess::Get(isolate);
    if (state.contexts.empty() || state.contexts.back() != context) std::abort();
    state.contexts.pop_back();
    Release(CellOf(context));
}
JSContext* ConsumePendingContext(Isolate* isolate) { return std::exchange(CoreAccess::Get(isolate).pending_context, nullptr); }
void PushExceptionSink(Isolate* isolate, ExceptionSink* sink) {
    if (!sink) std::abort(); CoreAccess::Get(isolate).exception_frames.push_back({sink, nullptr});
}
void PopExceptionSink(Isolate* isolate, ExceptionSink* sink) {
    auto& frames = CoreAccess::Get(isolate).exception_frames;
    if (frames.empty() || frames.back().sink != sink) std::abort(); frames.pop_back();
}
void PushTryCatch(Isolate* isolate, TryCatch* frame) {
    if (!frame) std::abort(); CoreAccess::Get(isolate).exception_frames.push_back({nullptr, frame});
}
void PopTryCatch(Isolate* isolate, TryCatch* frame) {
    auto& frames = CoreAccess::Get(isolate).exception_frames;
    if (frames.empty() || frames.back().try_catch != frame) std::abort(); frames.pop_back();
}
bool HasPendingException(Isolate* isolate) { return CoreAccess::Get(isolate).has_exception; }
JSValueConst PeekPendingException(Isolate* isolate) { return CoreAccess::Get(isolate).exception; }
JSValue TakePendingException(Isolate* isolate) {
    auto& state = CoreAccess::Get(isolate);
    if (!state.has_exception) return JS_UNDEFINED;
    auto value = state.exception;
    state.exception = JS_UNDEFINED;
    state.has_exception = false;
    state.exception_is_termination = false;
    // JS_Throw and native return paths may also have installed a QuickJS pending value.
    JS_FreeValue(state.exception_context, JS_GetException(state.exception_context));
    JS_FreeContext(state.exception_context);
    state.exception_context = nullptr;
    state.exception_metadata = {};
    return value;
}
void ClearPendingException(Isolate* isolate) {
    auto& state = CoreAccess::Get(isolate);
    if (!state.has_exception) return;
    auto value = TakePendingException(isolate);
    JS_FreeValueRT(state.runtime, value);
}
const ExceptionMetadata& PendingExceptionMetadata(Isolate* isolate) { return CoreAccess::Get(isolate).exception_metadata; }
void SetPendingExceptionMetadata(Isolate* isolate, ExceptionMetadata metadata) { CoreAccess::Get(isolate).exception_metadata = std::move(metadata); }
static void StoreException(Isolate* isolate, JSContext* context, JSValue value) {
    auto& state = CoreAccess::Get(isolate);
    if (!context || JS_GetRuntime(context) != state.runtime || JS_IsException(value)) std::abort();
    auto* owned_context = JS_DupContext(context);
    if (state.has_exception) {
        JS_FreeValue(state.exception_context, state.exception);
        JS_FreeContext(state.exception_context);
    }
    state.exception_context = owned_context;
    state.exception = value;
    state.has_exception = true;
    state.exception_is_termination = state.terminating.load(std::memory_order_relaxed);
    // The QuickJS error object (including its stack) remains the source of truth.
    if (!state.exception_frames.empty()) {
        auto frame = state.exception_frames.back();
        if (frame.sink) frame.sink->OnException(context, value, state.exception_metadata);
        else frame.try_catch->Capture(context, value, state.terminating.load(std::memory_order_relaxed));
        if (!frame.try_catch || !frame.try_catch->IsVerbose()) return;
    }
    if (!state.message_listeners.empty() && !state.reporting_exception) {
        state.reporting_exception = true;
        // Listener callbacks may replace registrations or throw. Snapshot ownership.
        auto listeners = state.message_listeners;
        JSValue reported = JS_DupValue(context, value);
        HandleScope handles(isolate);
        auto message = Exception::CreateMessage(isolate, Adopt(context, JS_DupValue(context, reported)));
        for (const auto& listener : listeners) {
            if (!(listener->levels & Isolate::kMessageError)) continue;
            auto data = listener->data.IsEmpty() ? Adopt(context, JS_DupValue(context, reported)) : listener->data.Get(isolate);
            listener->callback(message, data);
        }
        JS_FreeValue(context, reported);
        state.reporting_exception = false;
    }
}
void CaptureException(Isolate* isolate, JSContext* context) { StoreException(isolate, context, JS_GetException(context)); }
void Throw(Isolate* isolate, JSContext* context, JSValue owned) {
    // Install both the facade state and the actual pending QuickJS exception.
    JS_Throw(context, JS_DupValue(context, owned));
    StoreException(isolate, context, owned);
}
} // namespace detail
Context::BackupIncumbentScope::BackupIncumbentScope(Local<Context> context) {
    if (context.IsEmpty()) std::abort();
    detail::CoreAccess::PushBackupIncumbent(this, *context);
}
Context::BackupIncumbentScope::~BackupIncumbentScope() {
    detail::CoreAccess::PopBackupIncumbent(this);
}


bool Data::IsValue() const { return detail::CellOf(this)->type_flags & 1; }
bool Data::IsModule() const { return detail::CellOf(this)->type_flags & 2; }
bool Data::IsFixedArray() const { return detail::CellOf(this)->type_flags & 4; }
bool Data::IsPrivate() const { return detail::CellOf(this)->type_flags & 8; }
bool Data::IsObjectTemplate() const { return detail::CellOf(this)->type_flags & 16; }
bool Data::IsFunctionTemplate() const { return detail::CellOf(this)->type_flags & 32; }
bool Data::IsContext() const { return detail::CellOf(this)->type_flags & 64; }

HandleScope::HandleScope(Isolate* isolate) : isolate_(isolate) {
    detail::CheckIsolate(isolate);
    parent_ = isolate->impl_->scope;
    isolate->impl_->scope = this;
}
HandleScope::~HandleScope() {
    auto& state = *isolate_->impl_;
    if (state.scope != this) std::abort();
    state.scope = parent_;
    while (head_) {
        auto* cell = head_;
        head_ = cell->next;
        if (head_) head_->previous = nullptr;
        cell->scope = nullptr;
        cell->previous = nullptr;
        cell->next = nullptr;
        detail::Release(cell);
    }
}
int HandleScope::NumberOfHandles(Isolate* isolate) {
    auto& state = detail::CoreAccess::Get(isolate);
    size_t count = 0;
    for (auto* scope = state.scope; scope; scope = scope->parent_)
        for (auto* cell = scope->head_; cell; cell = cell->next) ++count;
    if (count > static_cast<size_t>(std::numeric_limits<int>::max())) std::abort();
    return static_cast<int>(count);
}
detail::Cell* HandleScope::EscapeCell(detail::Cell* cell) {
    if (isolate_->impl_->scope != this || cell->isolate != isolate_) std::abort();
    if (cell->read_only_root) return cell;
    if (!parent_) std::abort();
    if (cell->scope != this) {
        for (auto* scope = parent_; scope; scope = scope->parent_) if (cell->scope == scope) return cell;
        std::abort();
    }
    if (cell->previous) cell->previous->next = cell->next; else head_ = cell->next;
    if (cell->next) cell->next->previous = cell->previous;
    cell->scope = parent_;
    cell->previous = nullptr;
    cell->next = parent_->head_;
    if (cell->next) cell->next->previous = cell;
    parent_->head_ = cell;
    return cell;
}

Local<FixedArray> FixedArray::New(Isolate* isolate, int length) {
    if (length < 0) std::abort();
    return detail::MakeLocal<FixedArray>(isolate, detail::CurrentJSContext(isolate), JS_UNDEFINED,
        std::make_shared<FixedArrayPayload>(static_cast<size_t>(length)));
}
int FixedArray::Length() const { return static_cast<int>(std::static_pointer_cast<FixedArrayPayload>(detail::Payload(this))->cells.size()); }
Local<Data> FixedArray::Get(Local<Context> context, int index) const {
    auto payload = std::static_pointer_cast<FixedArrayPayload>(detail::Payload(this));
    if (context.IsEmpty() || detail::IsolateOf(*context) != detail::IsolateOf(this) || index < 0 || static_cast<size_t>(index) >= payload->cells.size()) std::abort();
    return detail::Localize(detail::IsolateOf(this), payload->cells[index]);
}
void FixedArray::Set(int index, Local<Data> value) {
    auto payload = std::static_pointer_cast<FixedArrayPayload>(detail::Payload(this));
    if (index < 0 || static_cast<size_t>(index) >= payload->cells.size()) std::abort();
    auto* next = detail::CellOf(*value);
    if (next && detail::IsolateOf(*value) != detail::IsolateOf(this)) std::abort();
    if (next) detail::Retain(next);
    auto* old = std::exchange(payload->cells[index], next);
    if (old) detail::Release(old);
}

namespace {
bool EqualTracedData(JSContext* context, JSValueConst left, JSValueConst right) {
    if (JS_VALUE_GET_TAG(left) == JS_TAG_MODULE || JS_VALUE_GET_TAG(right) == JS_TAG_MODULE)
        return JS_VALUE_GET_TAG(left) == JS_VALUE_GET_TAG(right) && JS_VALUE_GET_PTR(left) == JS_VALUE_GET_PTR(right);
    return JS_IsValueEq(context,left,right,true);
}
}

bool TracedReferenceBase::IsEmpty() const {
    if (edge_) detail::CheckIsolate(isolate_);
    return edge_ == mini_ui::kInvalidQuickJSEdge || !mini_ui::IsQuickJSStrongEdgeAlive(isolate_, edge_);
}
bool TracedReferenceBase::Equals(const Data* value) const {
    const bool empty = IsEmpty();
    if (empty || !value) return empty && !value;
    return detail::IsolateOf(value) == isolate_ && EqualTracedData(detail::ContextOf(value),
        mini_ui::GetQuickJSStrongEdge(isolate_, edge_), detail::Borrow(value));
}
void TracedReferenceBase::Reset() {
    if (!edge_) { isolate_ = nullptr; return; }
    detail::CheckIsolate(isolate_);
    mini_ui::ResetQuickJSStrongEdge(isolate_, edge_);
    edge_ = mini_ui::kInvalidQuickJSEdge;
    isolate_ = nullptr;
}
void TracedReferenceBase::Set(Isolate* isolate, const Data* value) {
    detail::CheckIsolate(isolate);
    if (!value) { Reset(); return; }
    if (detail::IsolateOf(value) != isolate || (!value->IsValue() && !value->IsModule())) std::abort();
    JSValueConst raw = detail::Borrow(value);
    auto edge = value->IsModule()
        ? detail::RegisterModuleEdge(isolate, detail::ContextOf(value),
            static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(raw)))
        : mini_ui::RegisterQuickJSStrongEdge(isolate, raw);
    Reset(); isolate_ = isolate; edge_ = edge;
}
void TracedReferenceBase::Copy(const TracedReferenceBase& rhs) {
    if (rhs.IsEmpty()) { Reset(); return; }
    detail::CheckIsolate(rhs.isolate_);
    JSValueConst raw = mini_ui::GetQuickJSStrongEdge(rhs.isolate_, rhs.edge_);
    auto edge = JS_VALUE_GET_TAG(raw) == JS_TAG_MODULE
        ? detail::RegisterModuleEdge(rhs.isolate_, detail::ModuleEdgeContext(rhs.isolate_, rhs.edge_),
            static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(raw)))
        : mini_ui::RegisterQuickJSStrongEdge(rhs.isolate_, raw);
    Reset(); isolate_ = rhs.isolate_; edge_ = edge;
}
void TracedReferenceBase::Move(TracedReferenceBase& rhs) {
    Reset(); isolate_ = std::exchange(rhs.isolate_, nullptr); edge_ = std::exchange(rhs.edge_, mini_ui::kInvalidQuickJSEdge);
}
Local<Data> TracedReferenceBase::Get(Isolate* isolate) const {
    if (IsEmpty()) return {};
    if (isolate != isolate_) std::abort();
    JSValueConst raw = mini_ui::GetQuickJSStrongEdge(isolate, edge_);
    if (JS_VALUE_GET_TAG(raw) == JS_TAG_MODULE)
        return detail::ModuleView(detail::ModuleEdgeContext(isolate, edge_),
            static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(raw)));
    return detail::LocalizeEdge(isolate, edge_);
}
void TracedReferenceBase::Trace() const {
    if (edge_) {
        detail::CheckIsolate(isolate_);
        mini_ui::TraceQuickJSStrongEdge(isolate_, edge_);
    }
}
bool TracedReferenceBase::operator==(const TracedReferenceBase& rhs) const {
    if (IsEmpty() || rhs.IsEmpty()) return IsEmpty() && rhs.IsEmpty();
    if (isolate_ != rhs.isolate_) return false;
    auto* context = detail::CurrentJSContext(isolate_);
    return EqualTracedData(context, mini_ui::GetQuickJSStrongEdge(isolate_, edge_), mini_ui::GetQuickJSStrongEdge(rhs.isolate_, rhs.edge_));
}

Isolate::Isolate() : impl_(std::make_unique<Impl>()) {}
Isolate::~Isolate() = default;
Isolate* Isolate::CreateForQuickJS(JSRuntime* runtime) {
    auto& registry = Registry();
    auto& runtime_isolates = registry.runtimes;
    EnteredIsolates();
    if (!runtime || runtime_isolates.count(runtime)) return nullptr;
    std::unique_ptr<Isolate, void (*)(Isolate*)> isolate(new Isolate, [](Isolate* value) { delete value; });
    isolate->impl_->runtime = runtime;
    isolate->impl_->default_microtask_queue = MicrotaskQueue::New(isolate.get(), MicrotasksPolicy::kAuto);
    std::unique_ptr<JSContext, void (*)(JSContext*)> primitive(JS_NewContext(runtime), JS_FreeContext);
    if (!primitive) return nullptr;
    isolate->impl_->primitive_context = primitive.get();
    if (!native_job_class) JS_NewClassID(&native_job_class);
    if (!JS_IsRegisteredClass(runtime, native_job_class)) {
        JSClassDef definition{};
        definition.class_name = "QuickJSFacadeMicrotask";
        definition.finalizer = FreeNativeJob;
        if (JS_NewClass(runtime, native_job_class, &definition) < 0) {
            return nullptr;
        }
    }
    if (!JS_GetCompletedGCEpoch(runtime, &isolate->impl_->completed_gc_epoch)) std::abort();
    {
        // Like V8's read-only roots, these scalars outlive HandleScopes and do
        // not pin a realm. Script task cleanup may request them without a scope.
        HandleScope scope(isolate.get());
        auto cache = [&](detail::PrimitiveRoot root, const Data* value) {
            auto* cell = detail::CellOf(value);
            cell->read_only_root = true;
            detail::Retain(cell);
            isolate->impl_->primitive_roots[static_cast<unsigned>(root)] = cell;
        };
        cache(detail::PrimitiveRoot::kUndefined,
            *detail::MakeLocal<Primitive>(isolate.get(), primitive.get(), JS_UNDEFINED));
        cache(detail::PrimitiveRoot::kNull,
            *detail::MakeLocal<Primitive>(isolate.get(), primitive.get(), JS_NULL));
        cache(detail::PrimitiveRoot::kTrue,
            *detail::MakeLocal<Boolean>(isolate.get(), primitive.get(), JS_TRUE));
        cache(detail::PrimitiveRoot::kFalse,
            *detail::MakeLocal<Boolean>(isolate.get(), primitive.get(), JS_FALSE));
    }
    runtime_isolates.emplace(runtime, isolate.get());
    JS_SetInterruptHandler(runtime, [](JSRuntime*, void* opaque) -> int {
        return static_cast<Isolate*>(opaque)->impl_->terminating.load(std::memory_order_relaxed) ? 1 : 0;
    }, isolate.get());
    primitive.release();
    return isolate.release();
}
void Isolate::Dispose() {
    auto& registry = Registry();
    auto& context_cells = registry.contexts;
    auto& runtime_isolates = registry.runtimes;
    auto& entered_isolates = EnteredIsolates();
    detail::CheckIsolate(this);
    if (impl_->scope || !impl_->contexts.empty() || impl_->backup_incumbent_scope
        || !impl_->exception_frames.empty()
        || std::find(entered_isolates.begin(), entered_isolates.end(), this) != entered_isolates.end()) std::abort();
    detail::CollectWeakPersistents(this);
    impl_->disposing = true;
    detail::DisposeValuesState(this);
    impl_->message_listeners.clear();
    detail::ClearPendingException(this);
    if (impl_->pending_context) JS_FreeContext(std::exchange(impl_->pending_context, nullptr));
    // Weak registration never extends a realm or a Cell's lifetime.
    for (auto it = context_cells.begin(); it != context_cells.end();) {
        if (it->second.isolate == this) {
            detail::ForgetScriptContext(it->first);
            it = context_cells.erase(it);
        }
        else ++it;
    }
    for (auto*& root : impl_->primitive_roots)
        detail::Release(std::exchange(root, nullptr));
    // Persistent owners must release their handles before their isolate dies.
    if (impl_->live_cells) std::abort();
    for (const auto& entry : impl_->weak_persistents) if (!entry.expired()) std::abort();
    JS_SetContinuationPreservedEmbedderData(impl_->runtime, JS_UNDEFINED);
    impl_->default_microtask_queue.reset();
    JS_FreeContext(impl_->primitive_context);
    impl_->primitive_context = nullptr;
    JS_SetInterruptHandler(impl_->runtime, nullptr, nullptr);
    runtime_isolates.erase(impl_->runtime);
    delete this;
}
JSRuntime* Isolate::GetQuickJSRuntime() const { detail::CheckIsolate(const_cast<Isolate*>(this)); return impl_->runtime; }
void Isolate::SetPendingContextForQuickJS(JSContext* context) {
    detail::CheckIsolate(this);
    if (context && JS_GetRuntime(context) != impl_->runtime) std::abort();
    auto* next = context ? JS_DupContext(context) : nullptr;
    if (impl_->pending_context) JS_FreeContext(impl_->pending_context);
    impl_->pending_context = next;
}
void Isolate::Enter() { detail::CheckIsolate(this); EnteredIsolates().push_back(this); }
void Isolate::Exit() {
    auto& entered_isolates = EnteredIsolates();
    detail::CheckIsolate(this);
    if (entered_isolates.empty() || entered_isolates.back() != this) std::abort();
    entered_isolates.pop_back();
}
Isolate* Isolate::GetCurrent() { auto* isolate = TryGetCurrent(); if (!isolate) std::abort(); return isolate; }
Isolate* Isolate::TryGetCurrent() { auto& entered = EnteredIsolates(); return entered.empty() ? nullptr : entered.back(); }
bool Isolate::IsCurrent() const { return TryGetCurrent() == this; }
void* Isolate::GetData(uint32_t slot) const { detail::CheckIsolate(const_cast<Isolate*>(this)); if (slot >= impl_->data.size()) std::abort(); return impl_->data[slot]; }
void Isolate::SetData(uint32_t slot, void* value) { detail::CheckIsolate(this); if (slot >= impl_->data.size()) std::abort(); impl_->data[slot] = value; }
Local<Context> Isolate::GetCurrentContext() {
    detail::CheckIsolate(this);
    return impl_->contexts.empty() ? Local<Context>() : detail::Localize(this, detail::CellOf(impl_->contexts.back())).As<Context>();
}
Local<Context> Isolate::GetEnteredOrMicrotaskContext() { return GetCurrentContext(); }
Local<Context> Isolate::GetIncumbentContext() {
    if (auto* cell = detail::CoreAccess::BackupIncumbentCell(this))
        return detail::Localize(this, cell).As<Context>();
    return GetCurrentContext();
}
Local<Value> Isolate::GetContinuationPreservedEmbedderData() {
    detail::CheckIsolate(this);
    return detail::Adopt(detail::CurrentJSContext(this), JS_GetContinuationPreservedEmbedderData(impl_->runtime));
}
void Isolate::SetContinuationPreservedEmbedderData(Local<Value> value) {
    detail::CheckIsolate(this);
    if (!value.IsEmpty() && detail::IsolateOf(*value) != this) std::abort();
    JS_SetContinuationPreservedEmbedderData(impl_->runtime, detail::Borrow(*value));
}
bool Isolate::InContext() const { detail::CheckIsolate(const_cast<Isolate*>(this)); return !impl_->contexts.empty(); }
Local<Value> Isolate::ThrowException(Local<Value> value) {
    if (value.IsEmpty() || detail::IsolateOf(*value) != this) std::abort();
    auto* context = detail::CurrentJSContext(this);
    detail::Throw(this, context, JS_DupValue(context, detail::Borrow(*value)));
    return Undefined(this);
}
bool Isolate::HasPendingException() const { return detail::HasPendingException(const_cast<Isolate*>(this)); }
bool Isolate::AddMessageListener(MessageCallback callback, Local<Value> data) {
    return AddMessageListenerWithErrorLevel(callback, kMessageError, data);
}
bool Isolate::AddMessageListenerWithErrorLevel(MessageCallback callback, int levels, Local<Value> data) {
    detail::CheckIsolate(this);
    if (!callback || !(levels & kMessageAll) || (levels & ~kMessageAll)) return false;
    auto listener = std::make_shared<MessageListener>();
    listener->callback = callback;
    listener->levels = levels;
    listener->data.Reset(this, data);
    impl_->message_listeners.push_back(std::move(listener));
    return true;
}
void Isolate::RemoveMessageListeners(MessageCallback callback) {
    detail::CheckIsolate(this);
    auto& listeners = impl_->message_listeners;
    listeners.erase(std::remove_if(listeners.begin(), listeners.end(),
        [callback](const auto& listener) { return listener->callback == callback; }), listeners.end());
}
int64_t Isolate::AdjustAmountOfExternalAllocatedMemory(int64_t delta) {
    detail::CheckIsolate(this);
    if ((delta > 0 && impl_->external_memory > std::numeric_limits<int64_t>::max() - delta)
        || (delta < 0 && impl_->external_memory < std::numeric_limits<int64_t>::min() - delta)) std::abort();
    impl_->external_memory += delta;
    return impl_->external_memory;
}
void Isolate::TerminateExecution() { impl_->terminating.store(true, std::memory_order_relaxed); }
void Isolate::CancelTerminateExecution() {
    detail::CheckIsolate(this);
    if (!impl_->terminating.exchange(false, std::memory_order_relaxed)) return;
    JS_ResetUncatchableError(detail::CurrentJSContext(this));
    if (impl_->exception_is_termination) detail::ClearPendingException(this);
}
bool Isolate::IsExecutionTerminating() const { return impl_->terminating.load(std::memory_order_relaxed); }
void Isolate::PerformMicrotaskCheckpoint() {
    detail::CheckIsolate(this);
    if (impl_->running_microtasks || impl_->microtasks_depth || impl_->microtasks_suppressed) return;
    impl_->running_microtasks = true;
    Isolate::Scope isolate_scope(this);
    HandleScope handles(this);
    JSContext* job_context = nullptr;
    struct JobEntryState {
        Isolate* isolate;
        Context* entered = nullptr;
        bool executing = false;
    } entry{this};
    while (JS_IsJobPending(impl_->runtime)) {
        HandleScope job_handles(this);
        int result = JS_ExecutePendingJob(impl_->runtime, &job_context,
            [](JSContext* context, JS_BOOL before, void* opaque) {
                auto& entry = *static_cast<JobEntryState*>(opaque);
                auto& state = detail::CoreAccess::Get(entry.isolate);
                if (before) {
                    if (entry.executing) std::abort();
                    ++state.execution_depth;
                    entry.executing = true;
                    entry.entered = detail::LookupContext(context);
                    if (entry.entered) detail::EnterContext(entry.isolate, entry.entered);
                } else {
                    if (!entry.executing || state.execution_depth <= 0) std::abort();
                    if (entry.entered) {
                        detail::LeaveContext(entry.isolate, entry.entered);
                        entry.entered = nullptr;
                    }
                    entry.executing = false;
                    --state.execution_depth;
                }
            }, &entry);
        if (result < 0) {
            detail::CaptureException(this, job_context ? job_context : impl_->primitive_context);
            break;
        }
        if (!result) break;
    }
    const auto callbacks = impl_->completed_callbacks;
    for (const auto& callback : callbacks) {
        if (std::find(impl_->completed_callbacks.begin(), impl_->completed_callbacks.end(), callback) != impl_->completed_callbacks.end())
            callback.first(this, callback.second);
    }
    impl_->running_microtasks = false;
    detail::CollectWeakPersistents(this);
}
void Isolate::EnqueueMicrotask(Local<Function> function) {
    detail::CheckIsolate(this);
    if (function.IsEmpty() || detail::IsolateOf(*function) != this) std::abort();
    auto* context = detail::ContextOf(*function);
    JSValue value = detail::Borrow(*function);
    if (JS_EnqueueJob(context, RunFunctionJob, 1, &value) < 0) detail::CaptureException(this, context);
}
void Isolate::EnqueueMicrotask(MicrotaskCallback callback, void* data) {
    detail::CheckIsolate(this);
    if (!callback) std::abort();
    auto* context = detail::CurrentJSContext(this);
    auto token = JS_NewObjectClass(context, native_job_class);
    if (JS_IsException(token)) { detail::CaptureException(this, context); return; }
    auto job = std::make_unique<NativeJob>();
    job->callback = callback; job->data = data;
    JS_SetOpaque(token, job.release());
    int result = JS_EnqueueJob(context, RunNativeJob, 1, &token);
    JS_FreeValue(context, token);
    if (result < 0) detail::CaptureException(this, context);
}
void Isolate::SetMicrotasksPolicy(MicrotasksPolicy policy) { detail::CheckIsolate(this); impl_->microtasks_policy = policy; }
MicrotasksPolicy Isolate::GetMicrotasksPolicy() const { detail::CheckIsolate(const_cast<Isolate*>(this)); return impl_->microtasks_policy; }
MicrotaskQueue* Isolate::GetMicrotaskQueue() const {
    detail::CheckIsolate(const_cast<Isolate*>(this));
    if (!impl_->default_microtask_queue) std::abort();
    return impl_->default_microtask_queue.get();
}
void Isolate::AddMicrotasksCompletedCallback(MicrotasksCompletedCallbackWithData callback, void* data) {
    detail::CheckIsolate(this); if (!callback) std::abort();
    auto pair = std::make_pair(callback, data);
    if (std::find(impl_->completed_callbacks.begin(), impl_->completed_callbacks.end(), pair) == impl_->completed_callbacks.end()) impl_->completed_callbacks.push_back(pair);
}
void Isolate::RemoveMicrotasksCompletedCallback(MicrotasksCompletedCallbackWithData callback, void* data) {
    detail::CheckIsolate(this);
    auto& callbacks = impl_->completed_callbacks;
    callbacks.erase(std::remove(callbacks.begin(), callbacks.end(), std::make_pair(callback, data)), callbacks.end());
}
void Isolate::RequestGarbageCollectionForTesting(int type) { if (type != kFullGarbageCollection && type != kMinorGarbageCollection) std::abort(); LowMemoryNotification(); }
void Isolate::LowMemoryNotification() {
    detail::CheckIsolate(this);
    JS_RunGC(impl_->runtime);
    detail::CollectWeakPersistents(this);
}

std::unique_ptr<MicrotaskQueue> MicrotaskQueue::New(Isolate* isolate, MicrotasksPolicy policy) {
    detail::CheckIsolate(isolate); isolate->SetMicrotasksPolicy(policy);
    return std::unique_ptr<MicrotaskQueue>(new MicrotaskQueue(isolate));
}
void MicrotaskQueue::EnqueueMicrotask(Isolate* isolate, Local<Function> function) { if (isolate != isolate_) std::abort(); isolate_->EnqueueMicrotask(function); }
void MicrotaskQueue::EnqueueMicrotask(Isolate* isolate, MicrotaskCallback callback, void* data) { if (isolate != isolate_) std::abort(); isolate_->EnqueueMicrotask(callback, data); }
void MicrotaskQueue::PerformCheckpoint(Isolate* isolate) { if (isolate != isolate_) std::abort(); isolate_->PerformMicrotaskCheckpoint(); }
bool MicrotaskQueue::IsRunningMicrotasks() const { return detail::CoreAccess::Get(isolate_).running_microtasks; }
int MicrotaskQueue::GetMicrotasksScopeDepth() const { return detail::CoreAccess::Get(isolate_).microtasks_depth; }
Isolate* MicrotaskQueue::GetIsolate() const { detail::CheckIsolate(isolate_); return isolate_; }
MicrotaskQueue::~MicrotaskQueue() {
    if (!completed_callbacks_.empty()) isolate_->RemoveMicrotasksCompletedCallback(DispatchCompleted, this);
}
void MicrotaskQueue::AddMicrotasksCompletedCallback(MicrotasksCompletedCallbackWithData callback, void* data) {
    detail::CheckIsolate(isolate_); if (!callback) std::abort();
    auto pair = std::make_pair(callback, data);
    if (std::find(completed_callbacks_.begin(), completed_callbacks_.end(), pair) != completed_callbacks_.end()) return;
    if (completed_callbacks_.empty()) isolate_->AddMicrotasksCompletedCallback(DispatchCompleted, this);
    completed_callbacks_.push_back(pair);
}
void MicrotaskQueue::RemoveMicrotasksCompletedCallback(MicrotasksCompletedCallbackWithData callback, void* data) {
    detail::CheckIsolate(isolate_);
    completed_callbacks_.erase(std::remove(completed_callbacks_.begin(), completed_callbacks_.end(),
        std::make_pair(callback, data)), completed_callbacks_.end());
    if (completed_callbacks_.empty()) isolate_->RemoveMicrotasksCompletedCallback(DispatchCompleted, this);
}
void MicrotaskQueue::DispatchCompleted(Isolate* isolate, void* opaque) {
    auto* queue = static_cast<MicrotaskQueue*>(opaque);
    auto callbacks = queue->completed_callbacks_;
    auto& registered = detail::CoreAccess::Get(isolate).completed_callbacks;
    const auto dispatch = std::make_pair(DispatchCompleted, opaque);
    for (const auto& callback : callbacks) {
        // A completed callback can destroy this adapter or remove a later
        // callback. Never dereference a destroyed queue or invoke a removed one.
        if (std::find(registered.begin(), registered.end(), dispatch) == registered.end()) return;
        if (std::find(queue->completed_callbacks_.begin(), queue->completed_callbacks_.end(), callback)
            != queue->completed_callbacks_.end()) callback.first(isolate, callback.second);
    }
}
MicrotasksScope::MicrotasksScope(Isolate* isolate, Type type) : isolate_(isolate), type_(type) {
    auto& state = detail::CoreAccess::Get(isolate);
    if (type == kRunMicrotasks) ++state.microtasks_depth;
    else ++state.microtasks_suppressed;
}
MicrotasksScope::MicrotasksScope(Local<Context> context, Type type)
    : MicrotasksScope(detail::IsolateOf(*context), context->GetMicrotaskQueue(), type) {}
MicrotasksScope::MicrotasksScope(Isolate* isolate, MicrotaskQueue* queue, Type type)
    : MicrotasksScope(isolate, type) {
    if (queue && queue->GetIsolate() != isolate) std::abort();
}
MicrotasksScope::~MicrotasksScope() {
    auto& state = detail::CoreAccess::Get(isolate_);
    auto& depth = type_ == kRunMicrotasks ? state.microtasks_depth : state.microtasks_suppressed;
    if (depth <= 0) std::abort();
    --depth;
    if (!state.microtasks_depth && !state.microtasks_suppressed && type_ == kRunMicrotasks
        && state.microtasks_policy != MicrotasksPolicy::kExplicit) isolate_->PerformMicrotaskCheckpoint();
}
void MicrotasksScope::PerformCheckpoint(Isolate* isolate) { isolate->PerformMicrotaskCheckpoint(); }
bool MicrotasksScope::IsRunningMicrotasks(Isolate* isolate) { return detail::CoreAccess::Get(isolate).running_microtasks; }
int MicrotasksScope::GetCurrentDepth(Isolate* isolate) { return detail::CoreAccess::Get(isolate).microtasks_depth; }
} // namespace v8
