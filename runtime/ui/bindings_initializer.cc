// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "runtime/ui/bindings_initializer.h"

#include "base/check.h"
#include "runtime/ui/local_resources.h"
#include "runtime/ui/native_heap.h"
#include "runtime/ui/native_discardable_memory.h"
#include "runtime/ui/quickjs_bindings.h"
#include "runtime/ui/quickjs_v8_facade.h"
#include "runtime/ui/quickjs_v8_scripts.h"
#include "runtime/ui/script_runtime.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/renderer/bindings/core/v8/local_window_proxy.h"
#include "third_party/blink/renderer/bindings/core/v8/script_controller.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_script_runner.h"
#include "third_party/blink/renderer/core/animation/animation_clock.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/platform/bindings/active_script_wrappable_manager.h"
#include "third_party/blink/renderer/platform/bindings/v8_per_isolate_data.h"
#include "third_party/blink/renderer/platform/context_lifecycle_observer.h"
#include "third_party/blink/renderer/platform/heap/thread_state.h"
#include "third_party/blink/renderer/platform/loader/fetch/memory_cache.h"
#include "third_party/blink/renderer/platform/mojo/mojo_binding_context.h"
#include "third_party/blink/renderer/platform/supplementable.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread_scheduler.h"
#include "third_party/blink/renderer/platform/weborigin/scheme_registry.h"
#include "third_party/blink/renderer/platform/wtf/allocator/partitions.h"

namespace mini_ui {
namespace {

class LocalResourceCapability final
    : public blink::GarbageCollected<LocalResourceCapability>
    , public blink::Supplement<blink::MojoBindingContext>
    , public blink::ContextLifecycleObserver {
public:
    static const char kSupplementName[];

    LocalResourceCapability(blink::MojoBindingContext& context,
        std::shared_ptr<LocalResources> resources)
        : blink::Supplement<blink::MojoBindingContext>(context)
        , resources_(std::move(resources))
    {
        SetContextLifecycleNotifier(&context);
    }

    bool GetInfo(const base::FilePath& path, base::File::Info* out) const
    {
        return resources_ && resources_->GetFileInfo(path, out);
    }

    std::shared_ptr<LocalResources> GetResources() const { return resources_; }

    void Trace(blink::Visitor* visitor) const override
    {
        blink::Supplement<blink::MojoBindingContext>::Trace(visitor);
        blink::ContextLifecycleObserver::Trace(visitor);
    }

private:
    void ContextDestroyed() override { resources_.reset(); }

    // No View, JS value, realm, or context ownership.
    std::shared_ptr<LocalResources> resources_;
};

const char LocalResourceCapability::kSupplementName[] = "MiniUILocalResources";

void AttachLocalResources(blink::MojoBindingContext& context,
    const std::shared_ptr<LocalResources>& resources)
{
    blink::Supplement<blink::MojoBindingContext>::ProvideTo(context,
        blink::MakeGarbageCollected<LocalResourceCapability>(context, resources));
}

class AnimationTaskObserver final : public blink::Thread::TaskObserver {
public:
    void WillProcessTask(const base::PendingTask&, bool) override
    {
        blink::AnimationClock::NotifyTaskStart();
    }
    void DidProcessTask(const base::PendingTask&) override { }
};

AnimationTaskObserver& TaskObserver()
{
    static AnimationTaskObserver observer;
    return observer;
}

bool Fail(Error* error, const char* message)
{
    if (error)
        *error = { ErrorCode::internal, message };
    return false;
}

} // namespace

void PrepareBlinkForQuickJS()
{
    InitializeNativeDiscardableMemory();
    blink::Platform::InitializeBlink();
}

void InitializeBlinkForQuickJS(blink::Platform* platform,
    blink::scheduler::WebThreadScheduler* scheduler)
{
    CHECK(platform);
    CHECK(scheduler);
    blink::Platform::InitializeMainThread(platform, scheduler);
    InitializeRetainedModules();
    // File fetches still pass through the realm's pinned LocalResources loader;
    // enabling the native Fetch parser does not enable a network transport.
    blink::SchemeRegistry::RegisterURLSchemeAsSupportingFetchAPI("file");
    WTF::Partitions::InitializeArrayBufferPartition();
    blink::Thread::Current()->AddTaskObserver(&TaskObserver());
}

void InitializeQuickJSBlinkIsolate(v8::Isolate* isolate)
{
    CHECK(isolate);
    CHECK(WTF::IsMainThread());
    auto* thread_state = blink::ThreadState::Current();
    CHECK(thread_state->IsMainThread());
    thread_state->AttachToIsolate(isolate, nullptr);
    bool initialized = false;
    try {
        blink::V8PerIsolateData::InitializeForQuickJS(isolate);
        initialized = true;
        blink::V8PerIsolateData::From(isolate)->SetActiveScriptWrappableManager(
            blink::MakeGarbageCollected<blink::ActiveScriptWrappableManager>());
    } catch (...) {
        if (initialized) {
            ShutdownQuickJSBlinkIsolate(isolate);
        } else {
            blink::Thread::Current()->RemoveTaskObserver(&TaskObserver());
            DetachQuickJSCppgcHeap(isolate);
            v8::detail::CollectWeakPersistents(isolate);
            thread_state->cpp_heap().Terminate();
            thread_state->DetachFromIsolate();
        }
        throw;
    }
    blink::ThreadScheduler::Current()->SetV8Isolate(isolate);
    isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
}

void ShutdownQuickJSBlinkIsolate(v8::Isolate* isolate)
{
    CHECK(isolate);
    CHECK(WTF::IsMainThread());
    blink::Thread::Current()->RemoveTaskObserver(&TaskObserver());
    blink::ThreadScheduler::Current()->SetV8Isolate(nullptr);
    blink::V8PerIsolateData::WillBeDestroyed(isolate);
    auto* thread_state = blink::ThreadState::Current();
    // The process is terminal: release cached SVG images while their isolated
    // pages and agent schedulers can still close in normal ownership order.
    blink::MemoryCache::Get()->EvictResources();
    CollectQuickJSGarbage(isolate, cppgc::EmbedderStackState::kNoHeapPointers);
    thread_state->cpp_heap().Collect(cppgc::EmbedderStackState::kNoHeapPointers);
    CollectQuickJSGarbage(isolate, cppgc::EmbedderStackState::kNoHeapPointers);
    // Drop bridge-owned native roots, raw descriptors and JS edges while every
    // native object is still alive. No script executes beyond this point.
    DetachQuickJSCppgcHeap(isolate);
    CollectQuickJSGarbage(isolate, cppgc::EmbedderStackState::kNoHeapPointers);
    thread_state->cpp_heap().Collect(cppgc::EmbedderStackState::kNoHeapPointers);
    CollectQuickJSGarbage(isolate, cppgc::EmbedderStackState::kNoHeapPointers);
    thread_state->cpp_heap().Terminate();
    CollectQuickJSGarbage(isolate, cppgc::EmbedderStackState::kNoHeapPointers);
    // The bridge detach is idempotent; ThreadState still owns its attachment.
    thread_state->DetachFromIsolate();
    blink::V8PerIsolateData::Destroy(isolate);
}

bool BindQuickJSRealm(blink::WebLocalFrame& web_frame, ScriptRealm& realm,
    Error* error)
{
    auto* isolate = v8::Isolate::TryGetCurrent();
    auto* context = realm.get();
    if (!isolate || !context
        || isolate->GetQuickJSRuntime() != JS_GetRuntime(context))
        return Fail(error, "Blink realm does not belong to the current QuickJS isolate");
    if (!WTF::IsMainThread())
        return Fail(error, "Blink realms must be bound on the UI thread");

    auto* frame = static_cast<blink::WebLocalFrameImpl&>(web_frame).GetFrame();
    if (!frame || !frame->DomWindow())
        return Fail(error, "Cannot bind a detached Blink frame");

    auto resources = realm.local_resources();
    auto& binding_context = *frame->DomWindow();
    if (!resources || binding_context.IsContextDestroyed())
        return Fail(error, "Cannot bind a realm without a live local resource capability");
    v8::HandleScope handles(isolate);
    auto& world = blink::DOMWrapperWorld::MainWorld(isolate);
    auto* proxy = frame->WindowProxyMaybeUninitialized(world);
    auto previous = proxy->ContextIfInitialized();
    if (!previous.IsEmpty()) {
        if (previous->GetQuickJSContext() == context) {
            AttachLocalResources(binding_context, resources);
            if (error)
                *error = {};
            return true;
        }
        // Keep the browsing context's actual outer WindowProxy. Context::New
        // reconnects it to the fresh inner Window and external document realm.
        proxy->ClearForNavigation();
    }

    // Revoke a previous grant before attempting a new bind. Failure must not
    // leave a different realm's filesystem capability on this context.
    static_cast<blink::MojoBindingContext&>(binding_context)
        .RemoveSupplement<LocalResourceCapability>();
    isolate->SetPendingContextForQuickJS(context);
    proxy = frame->DomWindow()->GetScriptController().WindowProxy(world);
    auto installed = proxy->ContextIfInitialized();
    if (installed.IsEmpty() || installed->GetQuickJSContext() != context) {
        isolate->SetPendingContextForQuickJS(nullptr);
        return Fail(error, "WindowProxy did not consume the document's QuickJS realm");
    }
    AttachLocalResources(binding_context, resources);
    if (error)
        *error = {};
    return true;
}

bool GetLocalFileInfo(const blink::MojoBindingContext& context,
    const base::FilePath& path, base::File::Info* out)
{
    if (!WTF::IsMainThread() || !out || context.IsContextDestroyed())
        return false;
    auto* capability = blink::Supplement<blink::MojoBindingContext>::From<
        LocalResourceCapability>(context);
    return capability && capability->GetInfo(path, out);
}

std::shared_ptr<LocalResources> GetLocalResources(
    const blink::MojoBindingContext& context)
{
    if (!WTF::IsMainThread() || context.IsContextDestroyed())
        return nullptr;
    auto* capability = blink::Supplement<blink::MojoBindingContext>::From<
        LocalResourceCapability>(context);
    return capability ? capability->GetResources() : nullptr;
}

void NotifyQuickJSPromiseRejection(JSContext* context, JSValueConst promise,
    JSValueConst reason, bool handled, void* isolate_data) noexcept
{
    auto* isolate = static_cast<v8::Isolate*>(isolate_data);
    CHECK(isolate);
    CHECK_EQ(isolate->GetQuickJSRuntime(), JS_GetRuntime(context));
    // Unattached realms still follow ScriptRuntime's error path; there is not
    // yet a native Window to receive a DOM rejection event.
    v8::detail::ForwardPromiseRejection(context, promise, reason, handled,
        &blink::V8ScriptRunner::NotifyQuickJSPromiseRejection);
}

} // namespace mini_ui
