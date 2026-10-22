// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "runtime/ui/blink_backend.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <imm.h>
#include <inputscope.h>
#endif

#include "base/at_exit.h"
#include "base/auto_reset.h"
#include "base/cancelable_callback.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/function_ref.h"
#include "base/message_loop/message_pump.h"
#include "base/memory/weak_ptr.h"
#include "base/run_loop.h"
#include "base/task/common/task_annotator.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "third_party/blink/renderer/platform/wtf/allocator/partitions.h"
#include "cc/paint/skia_paint_canvas.h"
#include "mojo/core/configuration.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mini_ui_blink_resources.h"
#include "mojo/core/embedder/embedder.h"
#include "runtime/ui/bindings_initializer.h"
#include "runtime/ui/blink_local_url_loader.h"
#include "runtime/ui/cppgc_platform.h"
#include "runtime/ui/local_resources.h"
#include "runtime/ui/native_clipboard.h"
#include "runtime/ui/native_heap.h"
#include "runtime/ui/script_runtime.h"
#include "runtime/ui/quickjs_bindings.h"
#include "runtime/ui/quickjs_v8_core.h"
#include "runtime/ui/quickjs_v8_internal.h"
#include "services/metrics/public/cpp/ukm_source_id.h"
#include "third_party/blink/public/common/input/web_coalesced_input_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "third_party/blink/public/common/metrics/document_update_reason.h"
#include "third_party/blink/public/common/page/browsing_context_group_info.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/mojom/page/prerender_page_param.mojom.h"
#include "third_party/blink/public/mojom/input/pointer_lock_context.mojom-blink.h"
#include "third_party/blink/public/mojom/input/pointer_lock_result.mojom-blink.h"
#include "third_party/blink/public/mojom/partitioned_popins/partitioned_popin_params.mojom.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "third_party/blink/public/platform/scheduler/web_thread_scheduler.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/public/platform/web_data.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_policy_container.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_local_frame_client.h"
#include "third_party/blink/public/web/web_navigation_control.h"
#include "third_party/blink/public/web/web_navigation_params.h"
#include "third_party/blink/public/web/web_non_composited_widget_client.h"
#include "third_party/blink/public/web/web_page_popup.h"
#include "third_party/blink/public/web/web_settings.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/public/web/web_view_client.h"
#if defined(_WIN32)
#include "third_party/blink/public/web/win/web_font_rendering.h"
#endif
#include "third_party/blink/renderer/core/frame/web_frame_widget_impl.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/html/html_frame_owner_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/platform/scheduler/public/main_thread_scheduler.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread_scheduler.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/heap/thread_state.h"
#include "third_party/blink/renderer/platform/wtf/casting.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "third_party/skia/include/core/SkSamplingOptions.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/resource/resource_scale_factor.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"
#include "ui/gfx/font_render_params.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/latency/latency_info.h"
#include "components/viz/common/surfaces/frame_sink_id.h"

namespace mini_ui {
namespace {

void ClearError(Error* error)
{
    if (error)
        *error = {};
}

bool Fail(Error* error, ErrorCode code, std::string message)
{
    if (error) {
        error->code = code;
        error->message = std::move(message);
    }
    return false;
}

blink::WebURL ToWebURL(std::string_view value)
{
    return blink::WebURL(blink::KURL(WTF::String::FromUTF8(value)));
}

class MiniUIBlinkPlatform final : public blink::Platform {
public:
    bool HasDataResource(int resource_id) const override
    {
        return detail::FindBuiltinBlinkResource(resource_id) != nullptr;
    }

    blink::WebData GetDataResource(int resource_id, ui::ResourceScaleFactor scale_factor) override
    {
        const int scale_percent = scale_factor == ui::kScaleFactorNone
            ? 0 : static_cast<int>(ui::GetScaleForResourceScaleFactor(scale_factor) * 100);
        const auto* resource = detail::FindBuiltinBlinkResource(resource_id, scale_percent);
        return resource
            ? blink::WebData(reinterpret_cast<const char*>(resource->data), resource->size)
            : blink::WebData();
    }

    std::string GetDataResourceString(int resource_id) override
    {
        const auto* resource = detail::FindBuiltinBlinkResource(resource_id);
        return resource
            ? std::string(reinterpret_cast<const char*>(resource->data), resource->size)
            : std::string();
    }

    blink::WebString DefaultLocale() override
    {
        return blink::WebString::FromUTF8("en-US");
    }

    blink::WebString UserAgent() override
    {
        return blink::WebString::FromUTF8("mini-ui/1 Blink/QuickJS");
    }

    viz::FrameSinkId GenerateFrameSinkId() override
    {
        return viz::FrameSinkId(1, next_frame_sink_id_.fetch_add(1));
    }

private:
    std::atomic<uint32_t> next_frame_sink_id_ { 1 };
};

class MojoEnvironment final {
public:
    MojoEnvironment() {
        mojo::core::Configuration configuration;
        configuration.disable_ipcz = true;
        mojo::core::Init(configuration);
    }
    ~MojoEnvironment() { mojo::core::ShutDown(); }
    MojoEnvironment(const MojoEnvironment&) = delete;
    MojoEnvironment& operator=(const MojoEnvironment&) = delete;
};

class BlinkProcess final {
public:
    static BlinkProcess& Get()
    {
        static BlinkProcess process;
        return process;
    }

    bool EnsureCurrentThread(Error* error) const
    {
        ClearError(error);
        if (base::PlatformThread::CurrentId() != thread_id_) {
            return Fail(error, ErrorCode::invalid_argument,
                "Blink UI views must be used on the Blink UI thread");
        }
        if (initialization_error_.code != ErrorCode::none) {
            if (error)
                *error = initialization_error_;
            return false;
        }
        return true;
    }

    ScriptRuntime& runtime() const { return *runtime_; }
    v8::Isolate* isolate() const { return isolate_; }

    void CollectGarbageIfNeeded()
    {
        if (collecting_garbage_ || !v8::detail::CanCollectAtHostIdle(isolate_))
            return;
        std::size_t js_bytes;
        if (!JS_GetHostGCAllocation(runtime_->get(), &js_bytes))
            return;
        auto& native_heap = blink::ThreadState::Current()->cpp_heap();
        const auto add_saturated = [](std::size_t a, std::size_t b) {
            const auto maximum = std::numeric_limits<std::size_t>::max();
            return a > maximum - b ? maximum : a + b;
        };
        const auto allocated = add_saturated(
            add_saturated(js_bytes, native_heap.EstimatedAllocatedBytes()),
            v8::detail::HandleArenaAllocatedBytes(isolate_));
        if (allocated < gc_allocation_limit_)
            return;
        // Budget allocation growth, not a hard pause deadline: both collectors
        // are atomic. An extra full budget bypasses the normal rate limit.
        const auto now = base::TimeTicks::Now();
        if (!last_gc_.is_null() && now - last_gc_ < base::Milliseconds(100)
            && allocated - gc_allocation_limit_ < kGCAllocationBudget)
            return;
        base::AutoReset<bool> collecting(&collecting_garbage_, true);
        CollectQuickJSGarbage(isolate_, cppgc::EmbedderStackState::kMayContainHeapPointers);
        native_heap.Collect(cppgc::EmbedderStackState::kMayContainHeapPointers);
        CollectQuickJSGarbage(isolate_, cppgc::EmbedderStackState::kMayContainHeapPointers);
        v8::detail::TrimHandleArenas(isolate_);
        CHECK(JS_GetHostGCAllocation(runtime_->get(), &js_bytes));
        const auto retained = add_saturated(
            add_saturated(js_bytes, native_heap.EstimatedAllocatedBytes()),
            v8::detail::HandleArenaAllocatedBytes(isolate_));
        gc_allocation_limit_ = add_saturated(
            retained, std::max(kGCAllocationBudget, retained));
        last_gc_ = base::TimeTicks::Now();
    }

    bool RunTask(base::FunctionRef<bool()> operation, Error* error)
    {
        if (base::TaskAnnotator::CurrentTaskForThread())
            return operation();
        base::RunLoop loop(base::RunLoop::Type::kNestableTasksAllowed);
        std::optional<bool> result;
        std::exception_ptr exception;
        // The borrowed operation and result storage stay on this synchronous
        // call's stack. Cancellation also makes an early native-loop exit safe.
        base::CancelableOnceClosure task(base::BindOnce(
            [](base::FunctionRef<bool()>* operation, std::optional<bool>* result,
                std::exception_ptr* exception, base::RunLoop* loop) {
                try {
                    *result = (*operation)();
                } catch (...) {
                    *exception = std::current_exception();
                }
                loop->Quit();
            },
            base::Unretained(&operation), base::Unretained(&result),
            base::Unretained(&exception), base::Unretained(&loop)));
        if (!scheduler_->DeprecatedDefaultTaskRunner()->PostTask(
                FROM_HERE, task.callback())) {
            return Fail(error, ErrorCode::internal, "Blink UI task runner is shut down");
        }
        loop.Run();
        task.Cancel();
        if (exception)
            std::rethrow_exception(exception);
        if (!result)
            return Fail(error, ErrorCode::internal, "native UI loop exited before the operation ran");
        return *result;
    }

    ~BlinkProcess()
    {
        CHECK_EQ(base::PlatformThread::CurrentId(), thread_id_);
        ShutdownNativeClipboard();
        if (isolate_) {
            {
                v8::Isolate::Scope isolate_scope(isolate_);
                base::RunLoop().RunUntilIdle();
                Error ignored;
                runtime_->DrainJobs(&ignored);
                JS_RunGC(runtime_->get());
                RunCppgcTasks();
                runtime_->SetPromiseRejectionObserver(nullptr);
                if (isolate_initialized_)
                    ShutdownQuickJSBlinkIsolate(isolate_);
                base::RunLoop().RunUntilIdle();
            }
            UnregisterQuickJSIsolate(isolate_);
            isolate_->Dispose();
            isolate_ = nullptr;
        }
        runtime_.reset();
        // Runtime/native finalizers can enqueue releases and completion replies.
        // Keep the renderer task runner and Mojo alive through the pool drain.
        if (scheduler_)
            base::RunLoop().RunUntilIdle();
        if (owned_thread_pool_) {
            owned_thread_pool_->Shutdown();
            if (scheduler_)
                base::RunLoop().RunUntilIdle();
        }
        if (scheduler_)
            scheduler_->Shutdown();
    }

private:
    BlinkProcess()
        : thread_id_(base::PlatformThread::CurrentId())
    {
        // Initialize process/native heap statics before this object's static
        // destructor is registered. They must outlive isolate/runtime teardown.
        // Retain this owner between Views: Blink's frozen names initialize once.
        try {
            if (!base::CommandLine::InitializedForCurrentProcess())
                base::CommandLine::Init(0, nullptr);
            // Worker stack registration also uses PartitionAlloc. Initialize its
            // shared address space before starting any owned pool workers.
            WTF::Partitions::Initialize();
            if (auto* thread_pool = base::ThreadPoolInstance::Get()) {
                // The host must publish a started pool before creating views.
                // Borrow it without touching its startup or shutdown lifetime.
                if (!thread_pool->WasStartedUnsafe()) {
                    Fail(&initialization_error_, ErrorCode::internal,
                        "The host ThreadPoolInstance must be started before creating a Blink UI view");
                    return;
                }
            } else {
                base::ThreadPoolInstance::CreateAndStartWithDefaultParams("MiniUI");
                owned_thread_pool_ = base::ThreadPoolInstance::Get();
            }
            ui::SetSupportedResourceScaleFactors({ui::k100Percent, ui::k200Percent});
            PrepareBlinkForQuickJS();
            mojo_environment_.emplace();
            scheduler_ = blink::scheduler::WebThreadScheduler::CreateMainThreadScheduler(
                base::MessagePump::Create(base::MessagePumpType::UI));
            if (!scheduler_) {
                Fail(&initialization_error_, ErrorCode::internal,
                    "could not create Blink main-thread scheduler");
                return;
            }
            InitializeBlinkForQuickJS(&platform_, scheduler_.get());
            runtime_ = ScriptRuntime::Create(&initialization_error_);
            if (!runtime_)
                return;
            isolate_ = v8::Isolate::CreateForQuickJS(runtime_->get());
            if (!isolate_) {
                Fail(&initialization_error_, ErrorCode::internal,
                    "could not create the QuickJS Blink isolate");
                return;
            }
            RegisterQuickJSIsolate(isolate_, runtime_->get());
            v8::Isolate::Scope isolate_scope(isolate_);
            InitializeQuickJSBlinkIsolate(isolate_);
            isolate_initialized_ = true;
#if defined(_WIN32)
            // The native host supplies the renderer's real Win32 system fonts.
            // Query at CSS's 96-DPI baseline; per-view scaling remains separate.
            NONCLIENTMETRICSW font_metrics {};
            font_metrics.cbSize = sizeof(font_metrics);
            if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,
                    sizeof(font_metrics), &font_metrics, 0, 96)) {
                Fail(&initialization_error_, ErrorCode::internal,
                    "could not query Win32 system font metrics");
                return;
            }
            const auto family = [](const LOGFONTW& font) {
                return blink::WebString::FromUTF16(std::u16string_view(
                    reinterpret_cast<const char16_t*>(font.lfFaceName)));
            };
            blink::WebFontRendering::SetMenuFontMetrics(
                family(font_metrics.lfMenuFont), std::abs(font_metrics.lfMenuFont.lfHeight));
            blink::WebFontRendering::SetSmallCaptionFontMetrics(
                family(font_metrics.lfSmCaptionFont), std::abs(font_metrics.lfSmCaptionFont.lfHeight));
            blink::WebFontRendering::SetStatusFontMetrics(
                family(font_metrics.lfStatusFont), std::abs(font_metrics.lfStatusFont.lfHeight));
            const auto rendering = gfx::GetFontRenderParams(gfx::FontRenderParamsQuery(), nullptr);
            blink::WebFontRendering::SetAntialiasedTextEnabled(rendering.antialiasing);
            blink::WebFontRendering::SetLCDTextEnabled(
                rendering.subpixel_rendering != gfx::FontRenderParams::SUBPIXEL_RENDERING_NONE);
#endif
            runtime_->SetPromiseRejectionObserver(
                &NotifyQuickJSPromiseRejection, isolate_);
        } catch (const std::bad_alloc&) {
            Fail(&initialization_error_, ErrorCode::internal,
                "Not enough memory to initialize the Blink UI process");
        }
    }

    base::AtExitManager at_exit_;
    // Sequence-local Mojo wait sets are destroyed with the scheduler/platform.
    // Keep Core alive until those owners have released their handles.
    std::optional<MojoEnvironment> mojo_environment_;
    MiniUIBlinkPlatform platform_;
    std::unique_ptr<blink::scheduler::WebThreadScheduler> scheduler_;
    base::ThreadPoolInstance* owned_thread_pool_ = nullptr;
    std::unique_ptr<ScriptRuntime> runtime_;
    v8::Isolate* isolate_ = nullptr;
    const base::PlatformThreadId thread_id_;
    Error initialization_error_;
    bool isolate_initialized_ = false;
    static constexpr std::size_t kGCAllocationBudget = 64 * 1024 * 1024;
    std::size_t gc_allocation_limit_ = kGCAllocationBudget;
    base::TimeTicks last_gc_;
    bool collecting_garbage_ = false;
};

blink::WebPointerProperties::Button MouseButton(int button)
{
    switch (button) {
    case 0:
        return blink::WebPointerProperties::Button::kLeft;
    case 1:
        return blink::WebPointerProperties::Button::kMiddle;
    case 2:
        return blink::WebPointerProperties::Button::kRight;
    case 3:
        return blink::WebPointerProperties::Button::kBack;
    case 4:
        return blink::WebPointerProperties::Button::kForward;
    default:
        return blink::WebPointerProperties::Button::kNoButton;
    }
}

int ButtonModifier(int button)
{
    switch (button) {
    case 0:
        return blink::WebInputEvent::kLeftButtonDown;
    case 1:
        return blink::WebInputEvent::kMiddleButtonDown;
    case 2:
        return blink::WebInputEvent::kRightButtonDown;
    case 3:
        return blink::WebInputEvent::kBackButtonDown;
    case 4:
        return blink::WebInputEvent::kForwardButtonDown;
    default:
        return 0;
    }
}

std::string TextEncodingFor(const Resource& resource)
{
    if (resource.mime_type.rfind("text/", 0) == 0
        || resource.mime_type == "application/javascript"
        || resource.mime_type == "application/json"
        || resource.mime_type == "image/svg+xml") {
        return "UTF-8";
    }
    return {};
}

} // namespace

class BlinkBackend::Impl final : public blink::WebViewClient,
                                 public blink::WebNonCompositedWidgetClient,
                                 public blink::mojom::blink::PointerLockContext {
public:
    class FrameClient final : public blink::WebLocalFrameClient {
    public:
        FrameClient(Impl& owner, bool self_owned)
            : owner_(owner)
            , self_owned_flag_(self_owned)
        {
        }

        ~FrameClient() override = default;

        bool InitializeRealm(Error* error)
        {
            realm_ = ScriptRealm::Create(owner_.runtime_, owner_.resources_, error);
            return !!realm_;
        }

        void Bind(blink::WebLocalFrame* frame)
        {
            frame_ = frame;
            if (self_owned_flag_)
                self_owned_.reset(this);
        }

        void ReleaseRealm() { realm_.reset(); }
        std::shared_ptr<ScriptRealm> realm() const
        {
            return realm_;
        }

        void BindToFrame(blink::WebNavigationControl* control) override
        {
            navigation_control_ = control;
            frame_ = control;
        }

        void FrameDetached(blink::DetachReason reason) override
        {
            blink::WebLocalFrame* frame = frame_;
            frame_ = nullptr;
            navigation_control_ = nullptr;
            if (!self_owned_flag_) {
                owner_.frame_ = nullptr;
                owner_.widget_ = nullptr;
            }
            if (frame)
                frame->Close(reason);
            realm_.reset();
            if (self_owned_flag_)
                self_owned_.reset();
        }

        void DidCommitNavigation(blink::WebHistoryCommitType, bool should_reset_browser_interface_broker,
            const blink::ParsedPermissionsPolicy&,
            const blink::DocumentPolicyFeatureState&) override
        {
            // Blink calls this after installing the new Document, before HTML
            // parsing. Same-document commits never call it and retain the realm.
            Error error;
            auto next = ScriptRealm::Create(owner_.runtime_, owner_.resources_, &error);
            if (next && should_reset_browser_interface_broker) {
                BindLocalBrowserInterfaceBroker(
                    frame_->GetBrowserInterfaceBroker().Reset(
                        frame_->GetTaskRunner(blink::TaskType::kInternalDefault)),
                    next->local_resources());
            }
            if (!next || !BindQuickJSRealm(*frame_, *next, &error)) {
                owner_.callback_error_ = std::move(error);
                frame_->Detach();
                return;
            }
            realm_ = std::move(next);
        }

        std::unique_ptr<blink::URLLoader> CreateURLLoaderForTesting() override
        {
            return std::make_unique<BlinkLocalURLLoader>(
                owner_.resources_, current_url_);
        }

        blink::WebLocalFrame* CreateChildFrame(
            blink::mojom::TreeScopeType scope,
            const blink::WebString&, const blink::WebString&,
            const blink::FramePolicy&, const blink::WebFrameOwnerProperties&,
            blink::FrameOwnerElementType,
            blink::WebPolicyContainerBindParams,
            ukm::SourceId document_ukm_source_id,
            FinishChildFrameCreationFn finish_creation) override
        {
            auto* client = new FrameClient(owner_, true);
            Error realm_error;
            if (!client->InitializeRealm(&realm_error)) {
                owner_.callback_error_ = std::move(realm_error);
                delete client;
                return nullptr;
            }
            blink::WebLocalFrame* child = frame_->CreateLocalChild(
                scope, client, nullptr, blink::LocalFrameToken());
            if (!child) {
                Fail(&owner_.callback_error_, ErrorCode::internal,
                    "could not create Blink child frame");
                delete client;
                return nullptr;
            }
            client->Bind(child);
            mojo::PendingRemote<blink::mojom::BrowserInterfaceBroker> browser_interface_broker;
            BindLocalBrowserInterfaceBroker(
                browser_interface_broker.InitWithNewPipeAndPassReceiver(),
                client->realm()->local_resources());
            // The completion constructs the CoreFrame and its live Document.
            finish_creation(child, blink::DocumentToken(), std::move(browser_interface_broker));
            if (!BindQuickJSRealm(*child, *client->realm(), &realm_error)) {
                owner_.callback_error_ = std::move(realm_error);
                child->Detach();
                return nullptr;
            }
            return child;
        }

        void BeginNavigation(
            std::unique_ptr<blink::WebNavigationInfo> info) override
        {
            if (!frame_ || !navigation_control_ || !info)
                return;

            const blink::KURL requested = info->url_request.Url();
            auto params = blink::WebNavigationParams::CreateFromInfo(*info);
            if (requested.IsAboutBlankURL() || requested.IsAboutSrcdocURL()) {
                params->fallback_base_url = info->requestor_base_url;
                const auto base = info->requestor_base_url.GetString().Utf8();
                current_url_.assign(base.data(), base.length());
            }
            if (requested.IsAboutBlankURL()) {
                params->url = requested;
                blink::WebNavigationParams::FillStaticResponse(
                    params.get(), "text/html", "UTF-8",
                    base::span<const char>());
            } else if (requested.IsAboutSrcdocURL()) {
                WTF::String source;
                auto* frame_impl = blink::DynamicTo<blink::WebLocalFrameImpl>(frame_);
                if (frame_impl && frame_impl->GetFrame()
                    && frame_impl->GetFrame()->Owner()) {
                    auto* owner = blink::DynamicTo<blink::HTMLFrameOwnerElement>(
                        frame_impl->GetFrame()->Owner());
                    if (owner && owner->hasAttribute(blink::html_names::kSrcdocAttr))
                        source = owner->getAttribute(blink::html_names::kSrcdocAttr);
                }
                blink::WebNavigationParams::FillStaticResponse(
                    params.get(), "text/html", "UTF-8", source.Utf8());
            } else {
                Resource resource;
                Error error;
                const auto requested_utf8 = requested.GetString().Utf8();
                if (!owner_.resources_->Read(
                        std::string_view(requested_utf8.data(),
                            requested_utf8.length()),
                        current_url_, &resource, &error)) {
                    owner_.callback_error_ = std::move(error);
                    return;
                }
                FillResource(params.get(), resource);
                current_url_ = resource.url;
            }
            navigation_control_->CommitNavigation(std::move(params), nullptr);
        }

        bool Commit(const Resource& resource, Error* error)
        {
            if (!navigation_control_) {
                return Fail(error, ErrorCode::internal,
                    "Blink frame has no navigation controller");
            }
            auto params = std::make_unique<blink::WebNavigationParams>();
            FillResource(params.get(), resource);
            current_url_ = resource.url;
            navigation_control_->CommitNavigation(std::move(params), nullptr);
            return true;
        }

    private:
        static void FillResource(
            blink::WebNavigationParams* params, const Resource& resource)
        {
            params->url = ToWebURL(resource.url);
            params->http_method = "GET";
            params->http_status_code = 200;
            const std::string encoding = TextEncodingFor(resource);
            blink::WebNavigationParams::FillStaticResponse(
                params, blink::WebString::FromUTF8(resource.mime_type),
                blink::WebString::FromUTF8(encoding),
                base::as_chars(base::span(resource.bytes)));
        }

        Impl& owner_;
        blink::WebLocalFrame* frame_ = nullptr;
        blink::WebNavigationControl* navigation_control_ = nullptr;
        std::shared_ptr<ScriptRealm> realm_;
        std::string current_url_;
        bool self_owned_flag_ = false;
        std::unique_ptr<FrameClient> self_owned_;
    };

    Impl(const ViewOptions& options, std::shared_ptr<LocalResources> resources,
        BlinkProcess& process)
        : resources_(std::move(resources))
        , process_(process)
        , runtime_(process.runtime())
        , isolate_(process.isolate())
        , width_(options.width)
        , height_(options.height)
        , device_scale_(options.device_scale)
        , native_window_(options.native_window)
        , main_client_(*this, false)
    {
    }

    ~Impl() override
    {
        CHECK(process_.EnsureCurrentThread(nullptr));
        weak_factory_invalidated_ = true;
        weak_factory_.InvalidateWeakPtrs();
        CHECK(process_.RunTask([&] {
            EndPointerLock();
#if defined(_WIN32)
            ReleaseNativeHostBindings();
#endif
            v8::Isolate::Scope isolate_scope(isolate_);
            if (view_) {
                view_->Close();
                view_ = nullptr;
                frame_ = nullptr;
                widget_ = nullptr;
            }
            CHECK(!popup_);
            main_client_.ReleaseRealm();
            Error ignored;
            runtime_.DrainJobs(&ignored);
            JS_RunGC(runtime_.get());
            RunCppgcTasks();
            return true;
        }, nullptr));
    }

    bool Initialize(Error* error)
    {
        v8::Isolate::Scope isolate_scope(isolate_);
        if (!main_client_.InitializeRealm(error))
            return false;

        auto agent_scheduler = blink::ThreadScheduler::Current()
                                   ->ToMainThreadScheduler()
                                   ->CreateAgentGroupScheduler();
        agent_group_scheduler_ =
            std::make_unique<blink::scheduler::WebAgentGroupScheduler>(
                std::move(agent_scheduler));

        view_ = blink::WebView::Create(this, false, nullptr, std::nullopt,
            false, false, nullptr,
            *agent_group_scheduler_, std::string(), SK_ColorWHITE,
            blink::BrowsingContextGroupInfo::CreateUnique(), nullptr, nullptr);
        if (!view_) {
            return Fail(error, ErrorCode::internal,
                "could not create Blink WebView");
        }

        blink::WebSettings* settings = view_->GetSettings();
        // Bare WebView settings start at zero, unlike canonical WebPreferences.
        // Initialize readable inherited/keyword fonts before creating a document.
        settings->SetDefaultFontSize(16);
        settings->SetDefaultFixedFontSize(13);
        settings->SetMinimumFontSize(0);
        settings->SetMinimumLogicalFontSize(6);
        settings->SetJavaScriptEnabled(true);
        settings->SetLoadsImagesAutomatically(true);
        settings->SetAllowFileAccessFromFileURLs(true);
        settings->SetAllowUniversalAccessFromFileURLs(false);
        settings->SetPluginsEnabled(false);

        mojo::PendingRemote<blink::mojom::BrowserInterfaceBroker> browser_interface_broker;
        BindLocalBrowserInterfaceBroker(
            browser_interface_broker.InitWithNewPipeAndPassReceiver(),
            main_client_.realm()->local_resources());
        frame_ = blink::WebLocalFrame::CreateMainFrame(view_, &main_client_,
            nullptr, std::move(browser_interface_broker), blink::LocalFrameToken(),
            blink::DocumentToken(), nullptr);
        if (!frame_) {
            return Fail(error, ErrorCode::internal,
                "could not create Blink main frame");
        }
        main_client_.Bind(frame_);

        widget_ = frame_->InitializeFrameWidget(
            mojo::NullAssociatedRemote(),
            blink::Platform::Current()->GenerateFrameSinkId());
        if (!widget_) {
            return Fail(error, ErrorCode::internal,
                "could not create Blink frame widget");
        }
        widget_->InitializeNonCompositing(this);
        view_->DidAttachLocalMainFrame();
        widget_->SetFocus(true);
        view_->SetIsActive(true);

        if (!BindQuickJSRealm(*frame_, *main_client_.realm(), error))
            return false;
        if (!Resize(width_, height_, device_scale_, error))
            return false;
#if defined(_WIN32)
        if (HWND window = BoundWindow()) {
            if (!EnsureNativeHostBinding(window))
                return Fail(error, ErrorCode::internal, "could not attach the native view host");
            HWND focus = GetFocus();
            NativeFocusChanged(focus == window || (focus && IsChild(window, focus)));
        }
#endif
        return true;
    }

    bool RunTask(base::FunctionRef<bool()> operation, Error* error)
    {
        if (!process_.EnsureCurrentThread(error))
            return false;
        auto weak = weak_factory_.GetWeakPtr();
        return process_.RunTask([&] {
            if (!weak)
                return Fail(error, ErrorCode::invalid_argument, "Blink view closed before its UI operation ran");
            return operation();
        }, error);
    }

    bool LoadFile(std::string_view relative_path, Error* error)
    {
        if (!process_.EnsureCurrentThread(error))
            return false;
        if (!base::TaskAnnotator::CurrentTaskForThread())
            return RunTask([&] { return LoadFile(relative_path, error); }, error);
        v8::Isolate::Scope isolate_scope(isolate_);
        Resource resource;
        if (!resources_->Read(relative_path, {}, &resource, error))
            return false;
        if (!main_client_.Commit(resource, error))
            return false;
        return PumpAndPaint(error);
    }

    bool Evaluate(std::string_view source, std::string_view source_url,
        std::string* json_result, Error* error)
    {
        if (!process_.EnsureCurrentThread(error))
            return false;
        if (!base::TaskAnnotator::CurrentTaskForThread())
            return RunTask([&] { return Evaluate(source, source_url, json_result, error); }, error);
        v8::Isolate::Scope isolate_scope(isolate_);
        // A script can synchronously navigate its frame and replace the
        // client's current realm. Keep the executing realm alive until return.
        auto realm = main_client_.realm();
        if (!realm) {
            if (!TakeCallbackError(error))
                return false;
            return Fail(error, ErrorCode::script, "Blink main frame has no document realm");
        }
        if (!realm->Evaluate(source, source_url, json_result, error))
            return false;
        return PumpAndPaint(error);
    }

    bool Resize(int width, int height, float scale, Error* error)
    {
        if (!process_.EnsureCurrentThread(error))
            return false;
        if (!base::TaskAnnotator::CurrentTaskForThread())
            return RunTask([&] { return Resize(width, height, scale, error); }, error);
        v8::Isolate::Scope isolate_scope(isolate_);
        if (!TakeCallbackError(error))
            return false;
        if (!widget_)
            return Fail(error, ErrorCode::render, "Blink frame is detached");
        if (width <= 0 || height <= 0 || !std::isfinite(scale) || scale <= 0) {
            return Fail(error, ErrorCode::invalid_argument,
                "view size and device scale must be positive and finite");
        }
        const double physical_width = std::ceil(width * static_cast<double>(scale));
        const double physical_height = std::ceil(height * static_cast<double>(scale));
        if (physical_width > std::numeric_limits<int>::max()
            || physical_height > std::numeric_limits<int>::max()) {
            return Fail(error, ErrorCode::invalid_argument,
                "physical view size is too large");
        }
        const int pixel_width = static_cast<int>(physical_width);
        const int pixel_height = static_cast<int>(physical_height);
        const size_t stride = static_cast<size_t>(pixel_width) * 4;
        if (pixel_width <= 0 || pixel_height <= 0
            || stride > std::numeric_limits<size_t>::max()
                    / static_cast<size_t>(pixel_height)) {
            return Fail(error, ErrorCode::invalid_argument,
                "physical view size overflows the pixel buffer");
        }

        if (popup_ && (width != width_ || height != height_ || scale != device_scale_)) {
            view_->CancelPagePopup();
            if (!widget_)
                return Fail(error, ErrorCode::render, "Blink frame detached while closing popup");
        }
        width_ = width;
        height_ = height;
        device_scale_ = scale;
        pixel_width_ = pixel_width;
        pixel_height_ = pixel_height;
        stride_ = stride;
        pixels_.assign(stride_ * static_cast<size_t>(pixel_height_), 0);
        widget_->SetDeviceScaleFactorForTesting(scale);
        widget_->Resize(RootWidget()->DIPsToCeiledBlinkSpace(gfx::Size(width, height)));
        UpdateScreenRects();
        return Paint(error);
    }

    bool Tick(double monotonic_seconds, Error* error)
    {
        if (!process_.EnsureCurrentThread(error))
            return false;
        if (!base::TaskAnnotator::CurrentTaskForThread())
            return RunTask([&] { return Tick(monotonic_seconds, error); }, error);
        v8::Isolate::Scope isolate_scope(isolate_);
        if (!TakeCallbackError(error))
            return false;
        if (!widget_)
            return Fail(error, ErrorCode::render, "Blink frame is detached");
        if (!std::isfinite(monotonic_seconds) || monotonic_seconds < 0
            || (has_tick_ && monotonic_seconds < last_tick_seconds_)) {
            return Fail(error, ErrorCode::invalid_argument,
                "Tick time must be finite, non-negative, and monotonic");
        }
        const bool should_begin_frame = !has_tick_ || animation_scheduled_;
        has_tick_ = true;
        last_tick_seconds_ = monotonic_seconds;
        frame_time_ = base::TimeTicks() + base::Seconds(monotonic_seconds);

        if (should_begin_frame) {
            animation_scheduled_ = false;
            static_cast<blink::WebFrameWidgetImpl*>(widget_)
                ->BeginMainFrame(frame_time_);
        }
        return PumpAndPaint(error);
    }

    FrameView frame() const
    {
        if (!process_.EnsureCurrentThread(nullptr))
            return {};
        return { pixels_.empty() ? nullptr : pixels_.data(), pixel_width_,
            pixel_height_, stride_, generation_ };
    }

    void Pointer(const PointerEvent& input)
    {
        if (!process_.EnsureCurrentThread(nullptr) || !widget_ || weak_factory_invalidated_)
            return;
        if (!base::TaskAnnotator::CurrentTaskForThread()) {
            RunTask([&] { Pointer(input); return true; }, nullptr);
            return;
        }
        v8::Isolate::Scope isolate_scope(isolate_);
        if (native_pointer_locked_ && (input.type == PointerEvent::move || input.type == PointerEvent::leave))
            return;
        const gfx::PointF position_in_dips(
            static_cast<float>(input.x), static_cast<float>(input.y));
        const gfx::PointF position = RootWidget()->DIPsToBlinkSpace(position_in_dips);
        const gfx::Point view_origin = ViewRect().origin();
        const gfx::PointF screen(position_in_dips.x() + view_origin.x(),
            position_in_dips.y() + view_origin.y());
        if (popup_ && input.type == PointerEvent::down
            && (position_in_dips.x() < popup_bounds_.x() || position_in_dips.x() >= popup_bounds_.right()
                || position_in_dips.y() < popup_bounds_.y() || position_in_dips.y() >= popup_bounds_.bottom())) {
            // A popup dismissal click is consumed, not replayed onto the opener
            // where it could immediately reopen the same select popup.
            view_->CancelPagePopup();
            Error ignored;
            PumpAndPaint(&ignored);
            return;
        }
        const gfx::PointF local(
            position.x() - (popup_ ? popup_offset_.x() : 0),
            position.y() - (popup_ ? popup_offset_.y() : 0));
        const base::TimeTicks timestamp = has_tick_
            ? frame_time_
            : base::TimeTicks::Now();
        int modifiers = static_cast<int>(input.modifiers) | button_modifiers_;
        auto button = blink::WebPointerProperties::Button::kNoButton;
        const int changed_button = ButtonModifier(input.button);

        if (input.type == PointerEvent::wheel) {
            blink::WebMouseWheelEvent event(
                blink::WebInputEvent::Type::kMouseWheel, modifiers, timestamp);
            event.pointer_type = blink::WebPointerProperties::PointerType::kMouse;
            event.SetPositionInWidget(local.x(), local.y());
            event.SetPositionInScreen(screen.x(), screen.y());
            event.delta_x = RootWidget()->DIPsToBlinkSpace(static_cast<float>(input.delta_x));
            event.delta_y = RootWidget()->DIPsToBlinkSpace(static_cast<float>(input.delta_y));
            event.delta_units = ui::ScrollGranularity::kScrollByPixel;
            event.event_action =
                blink::WebMouseWheelEvent::GetPlatformSpecificDefaultEventAction(
                    event);
            Dispatch(event);
            return;
        }

        blink::WebInputEvent::Type type = blink::WebInputEvent::Type::kMouseMove;
        int click_count = 0;
        switch (input.type) {
        case PointerEvent::move:
            type = blink::WebInputEvent::Type::kMouseMove;
            break;
        case PointerEvent::down:
            button = MouseButton(input.button);
            button_modifiers_ |= changed_button;
            modifiers |= changed_button;
            type = blink::WebInputEvent::Type::kMouseDown;
            click_count = 1;
            break;
        case PointerEvent::up:
            button = MouseButton(input.button);
            button_modifiers_ &= ~changed_button;
            modifiers &= ~changed_button;
            type = blink::WebInputEvent::Type::kMouseUp;
            click_count = 1;
            break;
        case PointerEvent::leave:
            type = blink::WebInputEvent::Type::kMouseLeave;
            break;
        case PointerEvent::wheel:
            return;
        }
        blink::WebMouseEvent event(type,
            local, screen,
            button, click_count, modifiers, timestamp);
        Dispatch(event);
    }

    void Key(const KeyEvent& input)
    {
        if (!process_.EnsureCurrentThread(nullptr) || !widget_ || weak_factory_invalidated_)
            return;
        if (!base::TaskAnnotator::CurrentTaskForThread()) {
            RunTask([&] { Key(input); return true; }, nullptr);
            return;
        }
        v8::Isolate::Scope isolate_scope(isolate_);
        if (native_pointer_locked_ && input.type == KeyEvent::Type::down && input.virtual_key == 0x1b) {
            EndPointerLock();
            Error ignored;
            PumpAndPaint(&ignored);
            return;
        }
        const base::TimeTicks timestamp = has_tick_
            ? frame_time_
            : base::TimeTicks::Now();
        int modifiers = static_cast<int>(input.modifiers);
        if (input.repeat && input.type != KeyEvent::Type::up)
            modifiers |= blink::WebInputEvent::kIsAutoRepeat;

        if (input.type == KeyEvent::Type::text) {
            auto* popup_target = popup_;
            auto* target = InputTarget();
            static_assert(blink::WebKeyboardEvent::kTextLengthCap > 2);
            size_t offset = 0;
            while (offset < input.text.size()) {
                blink::WebKeyboardEvent event(
                    blink::WebInputEvent::Type::kChar, modifiers, timestamp);
                uint32_t character = input.text[offset];
                size_t count = 1;
                if (character >= 0xd800 && character <= 0xdbff
                    && offset + 1 < input.text.size()
                    && input.text[offset + 1] >= 0xdc00
                    && input.text[offset + 1] <= 0xdfff) {
                    character = 0x10000
                        + ((character - 0xd800) << 10)
                        + (input.text[offset + 1] - 0xdc00);
                    count = 2;
                }
                std::copy_n(input.text.begin() + offset, count, event.text.begin());
                std::copy_n(input.text.begin() + offset, count,
                    event.unmodified_text.begin());
                event.dom_key = static_cast<uint32_t>(
                    ui::DomKey::FromCharacter(character));
                target->HandleInputEvent(
                    blink::WebCoalescedInputEvent(event, ui::LatencyInfo()));
                offset += count;
                if ((popup_target && popup_ != popup_target)
                    || (!popup_target && widget_ != target)) {
                    break;
                }
            }
            Error ignored;
            PumpAndPaint(&ignored);
            return;
        }

        const blink::WebInputEvent::Type type = input.type == KeyEvent::Type::down
            ? blink::WebInputEvent::Type::kRawKeyDown
            : blink::WebInputEvent::Type::kKeyUp;
        blink::WebKeyboardEvent event(type, modifiers, timestamp);
        event.windows_key_code = static_cast<int>(input.virtual_key);
        event.native_key_code = static_cast<int>(input.scan_code);
        const ui::DomCode code = ui::UsLayoutKeyboardCodeToDomCode(
            static_cast<ui::KeyboardCode>(input.virtual_key));
        event.dom_code = static_cast<int>(code);
        ui::DomKey key = ui::DomKey::UNIDENTIFIED;
        ui::KeyboardCode ignored_key_code;
        const bool mapped = ui::DomCodeToUsLayoutDomKey(
            code, modifiers, &key, &ignored_key_code);
        if (!mapped)
            key = ui::DomKey::UNIDENTIFIED;
        event.dom_key = static_cast<uint32_t>(key);
        Dispatch(event);
    }

    void RequestNonCompositedPointerLock(bool has_user_activation,
        bool unadjusted_movement, PointerLockRequestCallback callback) override
    {
        using Result = blink::mojom::blink::PointerLockResult;
        Result result = Result::kUnsupportedOptions;
#if defined(_WIN32)
        const auto window = static_cast<HWND>(native_window_);
        if (window && IsWindow(window)
            && GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId()) {
            if (pointer_lock_owner_)
                result = Result::kAlreadyLocked;
            else if (!has_user_activation)
                result = Result::kRequiresUserGesture;
            else if (popup_ || GetForegroundWindow() != GetAncestor(window, GA_ROOT)
                || (GetFocus() != window && !IsChild(window, GetFocus())))
                result = Result::kPermissionDenied;
            else
                result = BeginPointerLock(unadjusted_movement);
        }
#endif
        mojo::PendingRemote<blink::mojom::blink::PointerLockContext> context;
        if (result == Result::kSuccess) {
            context = pointer_lock_receiver_.BindNewPipeAndPassRemote();
            pointer_lock_receiver_.set_disconnect_handler(
                base::BindOnce(&Impl::EndPointerLock, base::Unretained(this)));
        }
        std::move(callback).Run(result, std::move(context));
    }

    void RequestMouseLockChange(bool unadjusted_movement,
        RequestMouseLockChangeCallback callback) override
    {
        using Result = blink::mojom::blink::PointerLockResult;
        Result result = Result::kWrongDocument;
#if defined(_WIN32)
        if (native_pointer_locked_) {
            result = SetUnadjustedMovement(unadjusted_movement);
            if (result == Result::kSuccess) {
                movement_remainder_x_ = movement_remainder_y_ = 0;
                if (!unadjusted_movement && !UpdatePointerClip()) {
                    EndPointerLock();
                    result = Result::kUnknownError;
                }
            }
        }
#endif
        std::move(callback).Run(result);
    }

    void ScheduleNonCompositedAnimation() override
    {
        animation_scheduled_ = true;
    }
    bool RequestSetCursor(const ui::Cursor& cursor) override
    {
#if defined(_WIN32)
        HWND window = BoundWindow();
        if (!window || weak_factory_invalidated_)
            return false;
        const bool custom = cursor.type() == ui::mojom::CursorType::kCustom;
        HCURSOR next = custom ? CreateNativeCustomCursor(window, cursor)
                              : NativeStockCursor(cursor.type());
        if (!next && cursor.type() != ui::mojom::CursorType::kNone)
            return false;
        if (!EnsureNativeHostBinding(window)) {
            if (custom)
                DestroyCursor(next);
            return false;
        }
        has_native_cursor_ = true;
        HCURSOR previous_owned = owned_cursor_;
        native_cursor_ = next;
        owned_cursor_ = custom ? next : nullptr;
        if (native_pointer_locked_) {
            // Unlock restores the latest view cursor, not a retired bitmap.
            saved_cursor_handle_ = next;
        } else {
            POINT point;
            HWND hovered = GetCursorPos(&point) ? WindowFromPoint(point) : nullptr;
            if (hovered == window || (hovered && IsChild(window, hovered))
                || (previous_owned && ::GetCursor() == previous_owned))
                ::SetCursor(next);
        }
        if (previous_owned)
            DestroyCursor(previous_owned);
        return true;
#else
        return false;
#endif
    }

    bool RequestTextInputState(ui::TextInputType type, ui::TextInputMode mode,
        const gfx::Rect& caret_bounds) override
    {
#if defined(_WIN32)
        HWND window = BoundWindow();
        if (!window || weak_factory_invalidated_)
            return false;
        const auto& api = ImeAPI();
        if (!api.set_scope || !EnsureNativeHostBinding(window))
            return false;
        const InputScope scope = NativeInputScope(type, mode);
        if (!native_input_scope_ || *native_input_scope_ != scope) {
            auto weak = weak_factory_.GetWeakPtr();
            const HRESULT result = api.set_scope(window, scope);
            if (!weak || FAILED(result))
                return false;
            native_input_scope_ = scope;
        }
        native_text_input_type_ = type;
        native_caret_bounds_ = caret_bounds;
        return UpdateNativeImePosition(window);
#else
        return false;
#endif
    }

    bool RequestTooltip(const blink::WebString& text, const gfx::Rect& bounds,
        bool keyboard, bool right_to_left) override
    {
#if defined(_WIN32)
        HWND window = BoundWindow();
        if (!window || weak_factory_invalidated_)
            return false;
        if (keyboard && text.IsEmpty())
            return RequestClearKeyboardTooltip();
        if (text.IsEmpty() && !native_tooltip_window_)
            return true;
        if (!EnsureNativeTooltipWindow(window))
            return false;
        auto weak = weak_factory_.GetWeakPtr();
        HWND tooltip = native_tooltip_window_;
        auto& stored_text = keyboard ? keyboard_tooltip_text_ : cursor_tooltip_text_;
        bool& registered = keyboard ? keyboard_tooltip_registered_ : cursor_tooltip_registered_;
        stored_text = text.Utf16();
        TOOLINFOW info {};
        info.cbSize = TTTOOLINFOW_V2_SIZE;
        info.hwnd = window;
        info.uId = keyboard ? 2 : 1;
        info.uFlags = TTF_TRANSPARENT | (keyboard ? TTF_TRACK | TTF_ABSOLUTE : TTF_SUBCLASS);
        if (right_to_left)
            info.uFlags |= TTF_RTLREADING;
        GetClientRect(window, &info.rect);
        info.lpszText = reinterpret_cast<LPWSTR>(stored_text.data());
        if (registered) {
            SendMessageW(tooltip, TTM_SETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&info));
        } else {
            const bool added = SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
            if (!weak || !added)
                return false;
            registered = true;
        }
        if (!weak)
            return false;
        if (keyboard) {
            const UINT dpi = GetDpiForWindow(window);
            if (!dpi)
                return false;
            const gfx::Rect physical = gfx::ScaleToRoundedRect(bounds, dpi / 96.f);
            POINT position { physical.x(), physical.bottom() };
            if (!ClientToScreen(window, &position))
                return false;
            SendMessageW(tooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(position.x, position.y));
            if (!weak)
                return false;
            keyboard_tooltip_active_ = true;
            SendMessageW(tooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&info));
            return weak && IsWindowVisible(tooltip);
        }
        if (stored_text.empty() && !keyboard_tooltip_active_)
            SendMessageW(tooltip, TTM_POP, 0, 0);
        return weak && IsWindow(tooltip);
#else
        return false;
#endif
    }

    bool RequestClearKeyboardTooltip() override
    {
#if defined(_WIN32)
        HWND window = BoundWindow();
        if (!window || weak_factory_invalidated_)
            return false;
        if (!native_tooltip_window_ || !keyboard_tooltip_active_)
            return true;
        TOOLINFOW info {};
        info.cbSize = TTTOOLINFOW_V2_SIZE;
        info.hwnd = window;
        info.uId = 2;
        keyboard_tooltip_active_ = false;
        auto weak = weak_factory_.GetWeakPtr();
        SendMessageW(native_tooltip_window_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&info));
        return !!weak;
#else
        return false;
#endif
    }


    bool RequestFocus() override
    {
#if defined(_WIN32)
        HWND window = BoundWindow();
        if (!window || weak_factory_invalidated_ || !IsWindowVisible(window)
            || !IsWindowEnabled(window))
            return false;
        HWND root = GetAncestor(window, GA_ROOT);
        if (!root || (GetForegroundWindow() != root && !SetForegroundWindow(root)))
            return false;
        SetFocus(window);
        if (!HasNativeFocus(window))
            return false;
        if (widget_)
            widget_->SetFocus(true);
        if (view_)
            view_->SetIsActive(true);
        return HasNativeFocus(window);
#else
        return false;
#endif
    }

    bool CanTakeFocus(bool reverse) const override
    {
#if defined(_WIN32)
        return NativeFocusTarget(reverse) != nullptr;
#else
        return false;
#endif
    }

    bool RequestTakeFocus(bool reverse) override
    {
#if defined(_WIN32)
        HWND target = NativeFocusTarget(reverse);
        if (!target)
            return false;
        SetFocus(target);
        if (!HasNativeFocus(target))
            return false;
        if (widget_)
            widget_->SetFocus(false);
        return HasNativeFocus(target);
#else
        return false;
#endif
    }

    bool RequestClose() override
    {
#if defined(_WIN32)
        HWND window = BoundWindow();
        return window && !weak_factory_invalidated_
            && PostMessageW(window, WM_CLOSE, 0, 0) != FALSE;
#else
        return false;
#endif
    }

    gfx::Rect WindowRect() const override
    {
#if defined(_WIN32)
        if (HWND window = BoundWindow()) {
            gfx::Rect bounds;
            ReadNativeScreenRect(window, false, &bounds);
            return bounds;
        }
#endif
        return OffscreenViewportRect();
    }

    gfx::Rect ViewRect() const override
    {
#if defined(_WIN32)
        if (HWND window = BoundWindow()) {
            gfx::Rect bounds;
            ReadNativeScreenRect(window, true, &bounds);
            return bounds;
        }
#endif
        return OffscreenViewportRect();
    }

    bool RequestSetWindowRect(const gfx::Rect& bounds) override
    {
        bool accepted = false;
#if defined(_WIN32)
        HWND window = BoundWindow();
        if (window && !weak_factory_invalidated_ && !bounds.IsEmpty()) {
            const UINT dpi = GetDpiForWindow(window);
            if (dpi) {
                const gfx::Rect pixels = gfx::ScaleToRoundedRect(bounds, dpi / 96.f);
                POINT position { pixels.x(), pixels.y() };
                bool position_valid = true;
                if (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) {
                    HWND parent = GetParent(window);
                    position_valid = parent && ScreenToClient(parent, &position);
                }
                if (position_valid) {
                    accepted = SetWindowPos(window, nullptr, position.x, position.y,
                        pixels.width(), pixels.height(), SWP_NOACTIVATE | SWP_NOZORDER) != FALSE;
                }
            }
        }
        // A host may constrain the request or forward WM_SIZE synchronously.
        // Reconcile from accepted native bounds, never from the pending estimate.
        if (window && widget_ && !weak_factory_invalidated_) {
            gfx::Rect actual_view;
            if (!ReadNativeScreenRect(window, true, &actual_view)) {
                accepted = false;
            } else if (!actual_view.IsEmpty()
                && (width_ != actual_view.width() || height_ != actual_view.height())) {
                Error error;
                if (!Resize(actual_view.width(), actual_view.height(), device_scale_, &error)) {
                    callback_error_ = std::move(error);
                    accepted = false;
                }
            }
        }
#endif
        UpdateScreenRects();
        return accepted;
    }

    void DidOpenNonCompositedPopup(
        blink::WebPagePopup* popup, const gfx::Rect& bounds) override
    {
        CHECK(popup);
        if (native_pointer_locked_)
            EndPointerLock();
        // A popup client's close callback can synchronously open a replacement
        // before the old popup sends DidClose. The newest open owns this slot.
        popup_ = popup;
        if (weak_factory_invalidated_ || !widget_) {
            view_->CancelPagePopup();
            return;
        }
        ApplyPopupBounds(bounds);
        if (popup_ == popup) {
            // Blurring the opener calls WebView::CancelPagePopup. Keep its page
            // focus and give the popup its own real keyboard focus instead.
            popup->SetFocus(true);
        }
    }

    void DidChangeNonCompositedPopupBounds(
        blink::WebPagePopup* popup, const gfx::Rect& bounds) override
    {
        if (popup_ != popup)
            return;
        ApplyPopupBounds(bounds);
    }

    void DidCloseNonCompositedPopup(blink::WebPagePopup* popup) override
    {
        if (popup_ != popup)
            return;
        // Native ownership can be released as soon as this callback returns.
        // Do not call any popup methods here, including geometry getters.
        popup_ = nullptr;
        popup_bounds_ = {};
        popup_offset_ = {};
        popup_size_ = {};
        animation_scheduled_ = true;
        if (widget_ && !weak_factory_invalidated_)
            widget_->SetFocus(true);
    }

private:
    gfx::Rect OffscreenViewportRect() const
    {
        return gfx::Rect(width_, height_);
    }

    void UpdateScreenRects()
    {
        if (!widget_ || weak_factory_invalidated_)
            return;
        const gfx::Rect view_rect = ViewRect();
        widget_->SetScreenRects(view_rect, WindowRect());
        if (popup_) {
            gfx::Rect popup_screen_rect = popup_bounds_;
            popup_screen_rect.Offset(view_rect.x(), view_rect.y());
            popup_->SetScreenRects(popup_screen_rect, popup_screen_rect);
        }
    }

#if defined(_WIN32)
    HWND BoundWindow() const
    {
        HWND window = static_cast<HWND>(native_window_);
        return window && IsWindow(window)
                && GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId()
            ? window : nullptr;
    }

    static bool HasNativeFocus(HWND target)
    {
        HWND focused = GetFocus();
        return focused == target || (focused && IsChild(target, focused));
    }

    HWND NativeFocusTarget(bool reverse) const
    {
        HWND window = BoundWindow();
        if (!window || weak_factory_invalidated_ || !IsWindowVisible(window)
            || !IsWindowEnabled(window))
            return nullptr;
        HWND dialog = GetAncestor(window, GA_ROOT);
        if (!dialog || dialog == window)
            return nullptr;
        HWND target = GetNextDlgTabItem(dialog, window, reverse);
        if (!target || target == window || IsChild(window, target)
            || !IsWindowVisible(target) || !IsWindowEnabled(target)
            || GetWindowThreadProcessId(target, nullptr) != GetCurrentThreadId())
            return nullptr;
        return target;
    }

    static bool ReadNativeScreenRect(HWND window, bool client, gfx::Rect* bounds)
    {
        const UINT dpi = GetDpiForWindow(window);
        RECT rect;
        if (!dpi)
            return false;
        if (client) {
            if (!GetClientRect(window, &rect))
                return false;
            POINT origin { rect.left, rect.top };
            if (!ClientToScreen(window, &origin))
                return false;
            OffsetRect(&rect, origin.x - rect.left, origin.y - rect.top);
        } else if (!GetWindowRect(window, &rect)) {
            return false;
        }
        *bounds = gfx::ScaleToRoundedRect(
            gfx::Rect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top),
            96.f / dpi);
        return true;
    }
#endif

    void EndPointerLock()
    {
        pointer_lock_receiver_.reset();
#if defined(_WIN32)
        if (!native_pointer_locked_)
            return;
        native_pointer_locked_ = false;
        auto window = static_cast<HWND>(native_window_);
        if (SetUnadjustedMovement(false) != blink::mojom::blink::PointerLockResult::kSuccess) {
            RAWINPUTDEVICE mouse { 1, 2, RIDEV_REMOVE, nullptr };
            RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
            unadjusted_movement_ = false;
            saved_raw_mouse_.reset();
        }
        if (window_subclassed_) {
            SubclassAPI().remove(window, PointerLockWindowProcedure,
                reinterpret_cast<UINT_PTR>(this));
            window_subclassed_ = false;
        }
        if (GetCapture() == window)
            ReleaseCapture();
        if (cursor_clipped_) {
            ClipCursor(&saved_clip_);
            cursor_clipped_ = false;
        }
        while (cursor_hide_calls_ > 0) {
            ShowCursor(TRUE);
            --cursor_hide_calls_;
        }
        SetCursor(saved_cursor_handle_);
        if (cursor_warped_) {
            SetCursorPos(saved_cursor_.x, saved_cursor_.y);
            cursor_warped_ = false;
        }
        movement_remainder_x_ = movement_remainder_y_ = 0;
        if (pointer_lock_owner_ == this)
            pointer_lock_owner_ = nullptr;
#endif
        button_modifiers_ = 0;
    }

#if defined(_WIN32)
    // Load the real common-controls subclass helpers without adding a new
    // embedding/link dependency. They safely coexist with the host's subclasses.
    struct NativeSubclassAPI {
        HMODULE module = LoadLibraryExW(L"comctl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        decltype(&InitCommonControlsEx) initialize = module
            ? reinterpret_cast<decltype(initialize)>(GetProcAddress(module, "InitCommonControlsEx")) : nullptr;
        decltype(&SetWindowSubclass) set = module
            ? reinterpret_cast<decltype(set)>(GetProcAddress(module, "SetWindowSubclass")) : nullptr;
        decltype(&RemoveWindowSubclass) remove = module
            ? reinterpret_cast<decltype(remove)>(GetProcAddress(module, "RemoveWindowSubclass")) : nullptr;
        decltype(&DefSubclassProc) next = module
            ? reinterpret_cast<decltype(next)>(GetProcAddress(module, "DefSubclassProc")) : nullptr;
    };

    static const NativeSubclassAPI& SubclassAPI()
    {
        static const NativeSubclassAPI api;
        return api;
    }
    void NativeFocusChanged(bool focused)
    {
        if (!widget_ || weak_factory_invalidated_)
            return;
        auto weak = weak_factory_.GetWeakPtr();
        RunTask([weak, focused] {
            if (!weak || !weak->widget_)
                return true;
            v8::Isolate::Scope isolate_scope(weak->isolate_);
            weak->widget_->SetFocus(focused);
            if (!weak || !weak->view_)
                return true;
            HWND window = weak->BoundWindow();
            weak->view_->SetIsActive(window && GetForegroundWindow() == GetAncestor(window, GA_ROOT));
            return true;
        }, nullptr);
    }

    bool EnsureNativeTooltipWindow(HWND window)
    {
        if (native_tooltip_window_ && IsWindow(native_tooltip_window_))
            return true;
        const auto& api = SubclassAPI();
        INITCOMMONCONTROLSEX controls { sizeof(controls), ICC_WIN95_CLASSES };
        if (!api.initialize || !api.initialize(&controls) || !EnsureNativeHostBinding(window))
            return false;
        auto weak = weak_factory_.GetWeakPtr();
        HWND tooltip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            window, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!weak) {
            if (IsWindow(tooltip))
                DestroyWindow(tooltip);
            return false;
        }
        if (!tooltip)
            return false;
        native_tooltip_window_ = tooltip;
        cursor_tooltip_registered_ = keyboard_tooltip_registered_ = false;
        keyboard_tooltip_active_ = false;
        const UINT dpi = GetDpiForWindow(window);
        SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, MulDiv(480, dpi ? dpi : 96, 96));
        return !!weak;
    }

    void RefreshNativeTooltipBounds(HWND window)
    {
        if (!native_tooltip_window_ || !cursor_tooltip_registered_)
            return;
        TOOLINFOW info {};
        info.cbSize = TTTOOLINFOW_V2_SIZE;
        info.hwnd = window;
        info.uId = 1;
        GetClientRect(window, &info.rect);
        SendMessageW(native_tooltip_window_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&info));
    }

    bool EnsureNativeHostBinding(HWND window)
    {
        if (native_host_subclassed_)
            return true;
        const auto& api = SubclassAPI();
        if (!api.set || !api.remove || !api.next
            || !api.set(window, NativeHostWindowProcedure,
                reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)))
            return false;
        native_host_subclassed_ = true;
        return true;
    }

    struct NativeImeAPI {
        HMODULE scope_module = LoadLibraryExW(L"msctf.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        HMODULE imm_module = LoadLibraryExW(L"imm32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        decltype(&::SetInputScope) set_scope = scope_module
            ? reinterpret_cast<decltype(set_scope)>(GetProcAddress(scope_module, "SetInputScope")) : nullptr;
        decltype(&::ImmGetContext) get_context = imm_module
            ? reinterpret_cast<decltype(get_context)>(GetProcAddress(imm_module, "ImmGetContext")) : nullptr;
        decltype(&::ImmReleaseContext) release_context = imm_module
            ? reinterpret_cast<decltype(release_context)>(GetProcAddress(imm_module, "ImmReleaseContext")) : nullptr;
        decltype(&::ImmSetCompositionWindow) set_composition_window = imm_module
            ? reinterpret_cast<decltype(set_composition_window)>(GetProcAddress(imm_module, "ImmSetCompositionWindow")) : nullptr;
        decltype(&::ImmSetCandidateWindow) set_candidate_window = imm_module
            ? reinterpret_cast<decltype(set_candidate_window)>(GetProcAddress(imm_module, "ImmSetCandidateWindow")) : nullptr;
    };

    static const NativeImeAPI& ImeAPI()
    {
        static const NativeImeAPI api;
        return api;
    }

    static InputScope NativeInputScope(ui::TextInputType type, ui::TextInputMode mode)
    {
        if (type == ui::TEXT_INPUT_TYPE_PASSWORD)
            return IS_PASSWORD;
        if (type == ui::TEXT_INPUT_TYPE_NONE)
            return IS_DEFAULT;
        switch (mode) {
        case ui::TEXT_INPUT_MODE_TEL: return IS_TELEPHONE_FULLTELEPHONENUMBER;
        case ui::TEXT_INPUT_MODE_URL: return IS_URL;
        case ui::TEXT_INPUT_MODE_EMAIL: return IS_EMAIL_SMTPEMAILADDRESS;
        case ui::TEXT_INPUT_MODE_NUMERIC: return IS_DIGITS;
        case ui::TEXT_INPUT_MODE_DECIMAL: return IS_NUMBER;
        case ui::TEXT_INPUT_MODE_SEARCH: return IS_SEARCH;
        default: break;
        }
        switch (type) {
        case ui::TEXT_INPUT_TYPE_TELEPHONE: return IS_TELEPHONE_FULLTELEPHONENUMBER;
        case ui::TEXT_INPUT_TYPE_URL: return IS_URL;
        case ui::TEXT_INPUT_TYPE_EMAIL: return IS_EMAIL_SMTPEMAILADDRESS;
        case ui::TEXT_INPUT_TYPE_NUMBER: return IS_NUMBER;
        case ui::TEXT_INPUT_TYPE_SEARCH: return IS_SEARCH;
        default: return IS_DEFAULT;
        }
    }

    bool UpdateNativeImePosition(HWND window)
    {
        HWND focus = GetFocus();
        // A collapsed caret has zero width but still has a real line height.
        if (native_text_input_type_ == ui::TEXT_INPUT_TYPE_NONE || native_caret_bounds_.height() <= 0
            || (focus != window && (!focus || !IsChild(window, focus))))
            return true;
        const auto& api = ImeAPI();
        const UINT dpi = GetDpiForWindow(window);
        if (!dpi || !api.get_context || !api.release_context
            || !api.set_composition_window || !api.set_candidate_window)
            return false;
        const gfx::Rect bounds = gfx::ScaleToRoundedRect(native_caret_bounds_, dpi / 96.f);
        COMPOSITIONFORM composition {};
        composition.dwStyle = CFS_POINT;
        composition.ptCurrentPos = { bounds.x(), bounds.y() };
        CANDIDATEFORM candidate {};
        candidate.dwStyle = CFS_CANDIDATEPOS;
        candidate.ptCurrentPos = { bounds.x(), bounds.bottom() };
        HIMC context = api.get_context(window);
        if (!context)
            return false;
        const bool composition_applied = api.set_composition_window(context, &composition);
        const bool candidate_applied = api.set_candidate_window(context, &candidate);
        const bool released = api.release_context(window, context);
        return composition_applied && candidate_applied && released;
    }

    static HCURSOR NativeStockCursor(ui::mojom::CursorType type)
    {
        using Type = ui::mojom::CursorType;
        LPCWSTR resource;
        switch (type) {
        case Type::kNull: case Type::kPointer: case Type::kContextMenu:
        case Type::kAlias: case Type::kCopy: case Type::kZoomIn:
        case Type::kZoomOut: case Type::kDndCopy: case Type::kDndLink:
            // Windows has no stock glyph for these CSS variants.
            resource = IDC_ARROW; break;
        case Type::kCross: case Type::kCell:
            resource = IDC_CROSS; break;
        case Type::kHand: case Type::kGrab: case Type::kGrabbing:
            resource = IDC_HAND; break;
        case Type::kIBeam: case Type::kVerticalText:
            resource = IDC_IBEAM; break;
        case Type::kWait: resource = IDC_WAIT; break;
        case Type::kProgress: resource = IDC_APPSTARTING; break;
        case Type::kHelp: resource = IDC_HELP; break;
        case Type::kEastResize: case Type::kWestResize: case Type::kEastWestResize:
        case Type::kColumnResize: case Type::kEastPanning: case Type::kWestPanning:
        case Type::kMiddlePanningHorizontal:
            resource = IDC_SIZEWE; break;
        case Type::kNorthResize: case Type::kSouthResize: case Type::kNorthSouthResize:
        case Type::kRowResize: case Type::kNorthPanning: case Type::kSouthPanning:
        case Type::kMiddlePanningVertical:
            resource = IDC_SIZENS; break;
        case Type::kNorthEastResize: case Type::kSouthWestResize:
        case Type::kNorthEastSouthWestResize: case Type::kNorthEastPanning:
        case Type::kSouthWestPanning:
            resource = IDC_SIZENESW; break;
        case Type::kNorthWestResize: case Type::kSouthEastResize:
        case Type::kNorthWestSouthEastResize: case Type::kNorthWestPanning:
        case Type::kSouthEastPanning:
            resource = IDC_SIZENWSE; break;
        case Type::kMove: case Type::kMiddlePanning: case Type::kDndMove:
            resource = IDC_SIZEALL; break;
        case Type::kNoDrop: case Type::kNotAllowed: case Type::kDndNone:
        case Type::kEastWestNoResize: case Type::kNorthSouthNoResize:
        case Type::kNorthEastSouthWestNoResize: case Type::kNorthWestSouthEastNoResize:
            resource = IDC_NO; break;
        case Type::kNone: case Type::kCustom:
            return nullptr;
        default:
            return nullptr;
        }
        return LoadCursorW(nullptr, resource);
    }

    static HCURSOR CreateNativeCustomCursor(HWND window, const ui::Cursor& cursor)
    {
        const UINT dpi = GetDpiForWindow(window);
        const float image_scale = cursor.image_scale_factor();
        SkPixmap source;
        if (!dpi || !std::isfinite(image_scale) || image_scale <= 0
            || !cursor.custom_bitmap().peekPixels(&source))
            return nullptr;
        const double scale = dpi / (96.0 * image_scale);
        const double scaled_width = std::ceil(source.width() * scale);
        const double scaled_height = std::ceil(source.height() * scale);
        if (scaled_width < 1 || scaled_height < 1
            || scaled_width > std::numeric_limits<int>::max()
            || scaled_height > std::numeric_limits<int>::max())
            return nullptr;
        const int width = static_cast<int>(scaled_width);
        const int height = static_cast<int>(scaled_height);
        if (!ui::Cursor::AreDimensionsValidForWeb(gfx::Size(width, height), dpi / 96.f))
            return nullptr;
        const size_t mask_stride = (static_cast<size_t>(width) + 15) / 16 * 2;
        std::vector<uint8_t> mask_bits(mask_stride * static_cast<size_t>(height), 0);
        BITMAPINFO bitmap {};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = width;
        bitmap.bmiHeader.biHeight = -height;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        HBITMAP color = CreateDIBSection(nullptr, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!color)
            return nullptr;
        const SkPixmap target(SkImageInfo::MakeN32Premul(width, height), pixels,
            static_cast<size_t>(width) * 4);
        if (!source.scalePixels(target, SkSamplingOptions(SkFilterMode::kLinear))) {
            DeleteObject(color);
            return nullptr;
        }
        HBITMAP mask = CreateBitmap(width, height, 1, 1, mask_bits.data());
        if (!mask) {
            DeleteObject(color);
            return nullptr;
        }
        ICONINFO info {};
        info.xHotspot = static_cast<DWORD>(std::clamp(
            std::lround(cursor.custom_hotspot().x() * scale), 0L, static_cast<long>(width - 1)));
        info.yHotspot = static_cast<DWORD>(std::clamp(
            std::lround(cursor.custom_hotspot().y() * scale), 0L, static_cast<long>(height - 1)));
        info.hbmColor = color;
        info.hbmMask = mask;
        HCURSOR result = reinterpret_cast<HCURSOR>(CreateIconIndirect(&info));
        DeleteObject(mask);
        DeleteObject(color);
        return result;
    }

    void ReleaseNativeHostBindings()
    {
        HWND window = static_cast<HWND>(native_window_);
        const bool remove_subclass = std::exchange(native_host_subclassed_, false);
        const bool clear_scope = native_input_scope_.has_value();
        native_input_scope_.reset();
        HCURSOR owned = std::exchange(owned_cursor_, nullptr);
        native_cursor_ = nullptr;
        has_native_cursor_ = false;
        HWND tooltip = std::exchange(native_tooltip_window_, nullptr);
        cursor_tooltip_registered_ = keyboard_tooltip_registered_ = false;
        keyboard_tooltip_active_ = false;
        if (remove_subclass && IsWindow(window))
            SubclassAPI().remove(window, NativeHostWindowProcedure, reinterpret_cast<UINT_PTR>(this));
        if (IsWindow(tooltip))
            DestroyWindow(tooltip);
        if (clear_scope && IsWindow(window) && ImeAPI().set_scope)
            ImeAPI().set_scope(window, IS_DEFAULT);
        if (owned) {
            if (::GetCursor() == owned)
                ::SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            DestroyCursor(owned);
        }
    }

    static LRESULT CALLBACK NativeHostWindowProcedure(HWND window, UINT message,
        WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR reference)
    {
        auto* self = reinterpret_cast<Impl*>(reference);
        const auto next = SubclassAPI().next;
        if (message == WM_NCDESTROY) {
            self->EndPointerLock();
            self->ReleaseNativeHostBindings();
        } else if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT
            && self->has_native_cursor_ && !self->native_pointer_locked_) {
            ::SetCursor(self->native_cursor_);
            return TRUE;
        } else if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
            HWND next_focus = reinterpret_cast<HWND>(wparam);
            auto weak = self->weak_factory_.GetWeakPtr();
            self->NativeFocusChanged(message == WM_SETFOCUS
                || (next_focus && IsChild(window, next_focus)));
            if (weak && message == WM_SETFOCUS)
                weak->UpdateNativeImePosition(window);
        } else if (message == WM_SIZE) {
            self->RefreshNativeTooltipBounds(window);
        } else if (message == WM_INPUTLANGCHANGE
            || message == WM_MOVE || message == WM_DPICHANGED
            || (message == WM_IME_SETCONTEXT && wparam)) {
            self->UpdateNativeImePosition(window);
        }
        return next(window, message, wparam, lparam);
    }


    blink::mojom::blink::PointerLockResult SetUnadjustedMovement(bool enable)
    {
        using Result = blink::mojom::blink::PointerLockResult;
        if (enable == unadjusted_movement_)
            return Result::kSuccess;
        if (enable) {
            UINT count = 0;
            if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) != 0)
                return Result::kUnsupportedOptions;
            std::vector<RAWINPUTDEVICE> devices(count);
            if (count && GetRegisteredRawInputDevices(devices.data(), &count,
                    sizeof(RAWINPUTDEVICE)) == UINT(-1))
                return Result::kUnsupportedOptions;
            saved_raw_mouse_.reset();
            for (const auto& device : devices) {
                if (device.usUsagePage == 1 && device.usUsage == 2) {
                    saved_raw_mouse_ = device;
                    break;
                }
            }
            RAWINPUTDEVICE mouse { 1, 2, 0, static_cast<HWND>(native_window_) };
            if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse)))
                return Result::kUnsupportedOptions;
            unadjusted_movement_ = true;
        } else {
            RAWINPUTDEVICE mouse = saved_raw_mouse_.value_or(
                RAWINPUTDEVICE { 1, 2, RIDEV_REMOVE, nullptr });
            if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse)))
                return Result::kUnknownError;
            unadjusted_movement_ = false;
            saved_raw_mouse_.reset();
        }
        return Result::kSuccess;
    }

    bool UpdatePointerClip()
    {
        auto window = static_cast<HWND>(native_window_);
        RECT clip;
        if (!GetClientRect(window, &clip) || clip.right <= 0 || clip.bottom <= 0)
            return false;
        POINT origin { 0, 0 };
        if (!ClientToScreen(window, &origin))
            return false;
        OffsetRect(&clip, origin.x, origin.y);
        pointer_center_ = { clip.left + (clip.right - clip.left) / 2,
            clip.top + (clip.bottom - clip.top) / 2 };
        if (!ClipCursor(&clip))
            return false;
        cursor_clipped_ = true;
        if (!SetCursorPos(pointer_center_.x, pointer_center_.y))
            return false;
        cursor_warped_ = true;
        return true;
    }

    blink::mojom::blink::PointerLockResult BeginPointerLock(bool unadjusted)
    {
        using Result = blink::mojom::blink::PointerLockResult;
        const auto& api = SubclassAPI();
        auto window = static_cast<HWND>(native_window_);
        if (!api.set || !api.remove || !api.next)
            return Result::kUnsupportedOptions;
        if (GetCapture() && GetCapture() != window)
            return Result::kPermissionDenied;
        if (!GetCursorPos(&saved_cursor_) || !GetClipCursor(&saved_clip_))
            return Result::kUnknownError;
        saved_cursor_handle_ = GetCursor();
        if (!api.set(window, PointerLockWindowProcedure,
                reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)))
            return Result::kUnknownError;
        window_subclassed_ = true;
        native_pointer_locked_ = true;
        pointer_lock_owner_ = this;
        SetCapture(window);
        if (GetCapture() != window || !UpdatePointerClip()) {
            EndPointerLock();
            return Result::kUnknownError;
        }
        const auto result = SetUnadjustedMovement(unadjusted);
        if (result != Result::kSuccess) {
            EndPointerLock();
            return result;
        }
        if (!native_pointer_locked_)
            return Result::kUserRejected;
        do {
            ++cursor_hide_calls_;
        } while (ShowCursor(FALSE) >= 0);
        SetCursor(nullptr);
        return Result::kSuccess;
    }

    static unsigned NativeModifiers()
    {
        unsigned modifiers = 0;
        if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= shift_key;
        if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= control_key;
        if (GetKeyState(VK_MENU) & 0x8000) modifiers |= alt_key;
        if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) modifiers |= meta_key;
        if (GetKeyState(VK_CAPITAL) & 1) modifiers |= caps_lock;
        if (GetKeyState(VK_NUMLOCK) & 1) modifiers |= num_lock;
        return modifiers;
    }

    void NativeRelativeMotion(int x, int y)
    {
        if ((!x && !y) || !native_pointer_locked_ || !widget_ || weak_factory_invalidated_)
            return;
        v8::Isolate::Scope isolate_scope(isolate_);
        blink::WebMouseEvent event(blink::WebInputEvent::Type::kMouseMove,
            gfx::PointF(), gfx::PointF(), blink::WebPointerProperties::Button::kNoButton,
            0, static_cast<int>(NativeModifiers()) | button_modifiers_, base::TimeTicks::Now());
        // Prevent Blink from recomputing deltas from the intentionally fixed
        // locked position. Raw requests carry the actual RAWMOUSE counts.
        event.is_raw_movement_event = true;
        event.movement_x = x;
        event.movement_y = y;
        Dispatch(event);
    }

    static LRESULT CALLBACK PointerLockWindowProcedure(HWND window, UINT message,
        WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR reference)
    {
        auto* self = reinterpret_cast<Impl*>(reference);
        const auto next = SubclassAPI().next;
        if (message == WM_KILLFOCUS || message == WM_CANCELMODE
            || message == WM_CLOSE || message == WM_DESTROY || message == WM_NCDESTROY
            || (message == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE)
            || (message == WM_CAPTURECHANGED && reinterpret_cast<HWND>(lparam) != window)) {
            self->EndPointerLock();
        } else if (self->native_pointer_locked_) {
            switch (message) {
            case WM_SETCURSOR:
                SetCursor(nullptr);
                return TRUE;
            case WM_INPUT: {
                if (!self->unadjusted_movement_)
                    return next(window, message, wparam, lparam);
                if (self->unadjusted_movement_ && GET_RAWINPUT_CODE_WPARAM(wparam) == RIM_INPUT) {
                    RAWINPUT input {};
                    UINT bytes = sizeof(input);
                    const UINT read = GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam),
                        RID_INPUT, &input, &bytes, sizeof(RAWINPUTHEADER));
                    if (read != UINT(-1) && read >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)
                        && input.header.dwType == RIM_TYPEMOUSE
                        && !(input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
                        self->NativeRelativeMotion(input.data.mouse.lLastX, input.data.mouse.lLastY);
                }
                // DefWindowProc performs the required foreground raw cleanup;
                // this registration belongs to us, not the host's raw handler.
                return DefWindowProcW(window, message, wparam, lparam);
            }
            case WM_MOUSEMOVE:
                if (!self->unadjusted_movement_) {
                    POINT point { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                    ClientToScreen(window, &point);
                    const double dx = (point.x - self->pointer_center_.x)
                        / self->device_scale_ + self->movement_remainder_x_;
                    const double dy = (point.y - self->pointer_center_.y)
                        / self->device_scale_ + self->movement_remainder_y_;
                    const int x = static_cast<int>(dx);
                    const int y = static_cast<int>(dy);
                    self->movement_remainder_x_ = dx - x;
                    self->movement_remainder_y_ = dy - y;
                    if (point.x != self->pointer_center_.x || point.y != self->pointer_center_.y) {
                        if (!SetCursorPos(self->pointer_center_.x, self->pointer_center_.y))
                            self->EndPointerLock();
                        else
                            self->NativeRelativeMotion(x, y);
                    }
                }
                return 0;
            case WM_MOUSELEAVE:
                return 0;
            case WM_KEYDOWN: case WM_SYSKEYDOWN:
                if (wparam == VK_ESCAPE) {
                    self->EndPointerLock();
                    v8::Isolate::Scope isolate_scope(self->isolate_);
                    Error ignored;
                    self->RunTask([&] { return self->PumpAndPaint(&ignored); }, &ignored);
                    return 0;
                }
                break;
            case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: {
                PointerEvent input;
                input.type = message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN
                        || message == WM_RBUTTONDOWN
                    ? PointerEvent::down : PointerEvent::up;
                input.button = message == WM_MBUTTONDOWN || message == WM_MBUTTONUP ? 1
                    : message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ? 2 : 0;
                input.modifiers = NativeModifiers();
                self->Pointer(input);
                return 0;
            }
            case WM_MOVE: case WM_SIZE: case WM_DPICHANGED:
                if (!self->UpdatePointerClip())
                    self->EndPointerLock();
                break;
            }
        }
        return next(window, message, wparam, lparam);
    }
#endif

    void ApplyPopupBounds(const gfx::Rect& requested)
    {
        CHECK(popup_);
        CHECK(widget_);
        const gfx::Size viewport(width_, height_);
        const gfx::Point view_origin = ViewRect().origin();
        gfx::Rect local_requested = requested;
        local_requested.Offset(-view_origin.x(), -view_origin.y());
        const int width = std::clamp(requested.width(), 0, viewport.width());
        const int height = std::clamp(requested.height(), 0, viewport.height());
        popup_bounds_ = gfx::Rect(
            std::clamp(local_requested.x(), 0, viewport.width() - width),
            std::clamp(local_requested.y(), 0, viewport.height() - height), width, height);
        popup_offset_ = RootWidget()->DIPsToBlinkSpace(gfx::PointF(popup_bounds_.origin()));
        popup_size_ = RootWidget()->DIPsToCeiledBlinkSpace(popup_bounds_.size());
        auto* popup = popup_;
        gfx::Rect popup_screen_rect = popup_bounds_;
        popup_screen_rect.Offset(view_origin.x(), view_origin.y());
        popup->SetScreenRects(popup_screen_rect, popup_screen_rect);
        if (popup_ == popup)
            popup->Resize(popup_size_);
        animation_scheduled_ = true;
    }

    blink::WebFrameWidgetImpl* RootWidget() const
    {
        return static_cast<blink::WebFrameWidgetImpl*>(widget_);
    }

    blink::WebWidget* InputTarget() const
    {
        return popup_ ? static_cast<blink::WebWidget*>(popup_) : widget_;
    }

    void Dispatch(const blink::WebInputEvent& event)
    {
        Error ignored;
        RunTask([&] {
            if (auto* target = InputTarget())
                target->HandleInputEvent(
                    blink::WebCoalescedInputEvent(event, ui::LatencyInfo()));
            return PumpAndPaint(&ignored);
        }, &ignored);
    }

    bool TakeCallbackError(Error* error)
    {
        if (callback_error_.code != ErrorCode::none) {
            if (error)
                *error = std::move(callback_error_);
            callback_error_ = {};
            return false;
        }
        return true;
    }

    bool PumpAndPaint(Error* error)
    {
        if (!TakeCallbackError(error) || !runtime_.DrainJobs(error))
            return false;
        RunCppgcTasks();
        process_.CollectGarbageIfNeeded();
        if (!TakeCallbackError(error))
            return false;
        return Paint(error);
    }

    bool Paint(Error* error)
    {
        if (!TakeCallbackError(error))
            return false;
        if (!view_ || !widget_ || pixels_.empty()) {
            return Fail(error, ErrorCode::render,
                "Blink view is not ready for painting");
        }
        widget_->UpdateAllLifecyclePhases(
            blink::DocumentUpdateReason::kBeginMainFrame);
        if (!TakeCallbackError(error))
            return false;
        if (!widget_)
            return Fail(error, ErrorCode::render, "Blink frame detached during layout");
#if defined(_WIN32)
        // No compositor pre-frame callback runs in this software-only host.
        if (native_host_subclassed_ && !popup_) {
            RootWidget()->UpdateTextInputState();
            RootWidget()->UpdateSelectionBounds();
            if (!TakeCallbackError(error))
                return false;
            if (!widget_)
                return Fail(error, ErrorCode::render, "Blink frame detached during input update");
        }
#endif
        auto* painted_popup = popup_;
        if (painted_popup) {
            painted_popup->UpdateLifecycle(blink::WebLifecycleUpdate::kAll,
                blink::DocumentUpdateReason::kPagePopup);
            if (!widget_)
                return Fail(error, ErrorCode::render, "Blink frame detached during popup layout");
        }
        if (!TakeCallbackError(error))
            return false;

        sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(
            SkImageInfo::MakeN32Premul(pixel_width_, pixel_height_),
            pixels_.data(), stride_);
        if (!surface) {
            return Fail(error, ErrorCode::render,
                "could not create CPU raster surface");
        }
        cc::SkiaPaintCanvas canvas(surface->getCanvas());
        canvas.clear(SkColors::kTransparent);
        canvas.save();
        view_->PaintContent(&canvas, gfx::Rect(pixel_width_, pixel_height_));
        if (painted_popup && popup_ == painted_popup && !popup_size_.IsEmpty()) {
            canvas.save();
            canvas.translate(popup_offset_.x(), popup_offset_.y());
            canvas.clipRect(SkRect::MakeWH(
                static_cast<float>(popup_size_.width()),
                static_cast<float>(popup_size_.height())));
            painted_popup->PaintContent(&canvas, gfx::Rect(popup_size_));
            canvas.restore();
        }
        canvas.restore();
        ++generation_;
        return true;
    }

    std::shared_ptr<LocalResources> resources_;
    BlinkProcess& process_;
    ScriptRuntime& runtime_;
    v8::Isolate* isolate_ = nullptr;
    int width_;
    int height_;
    float device_scale_;
    void* native_window_ = nullptr;
    bool native_pointer_locked_ = false;
    mojo::Receiver<blink::mojom::blink::PointerLockContext> pointer_lock_receiver_ { this };
#if defined(_WIN32)
    inline static Impl* pointer_lock_owner_ = nullptr;
    bool window_subclassed_ = false;
    bool native_host_subclassed_ = false;
    bool has_native_cursor_ = false;
    HCURSOR native_cursor_ = nullptr;
    HCURSOR owned_cursor_ = nullptr;
    ui::TextInputType native_text_input_type_ = ui::TEXT_INPUT_TYPE_NONE;
    std::optional<InputScope> native_input_scope_;
    gfx::Rect native_caret_bounds_;
    HWND native_tooltip_window_ = nullptr;
    bool cursor_tooltip_registered_ = false;
    bool keyboard_tooltip_registered_ = false;
    bool keyboard_tooltip_active_ = false;
    std::u16string cursor_tooltip_text_;
    std::u16string keyboard_tooltip_text_;
    bool unadjusted_movement_ = false;
    bool cursor_clipped_ = false;
    bool cursor_warped_ = false;
    int cursor_hide_calls_ = 0;
    RECT saved_clip_ {};
    POINT saved_cursor_ {};
    HCURSOR saved_cursor_handle_ = nullptr;
    POINT pointer_center_ {};
    double movement_remainder_x_ = 0;
    double movement_remainder_y_ = 0;
    std::optional<RAWINPUTDEVICE> saved_raw_mouse_;
#endif
    int pixel_width_ = 0;
    int pixel_height_ = 0;
    size_t stride_ = 0;
    std::vector<uint8_t> pixels_;
    uint64_t generation_ = 0;
    int button_modifiers_ = 0;
    bool has_tick_ = false;
    bool animation_scheduled_ = false;
    bool weak_factory_invalidated_ = false;
    double last_tick_seconds_ = 0;
    base::TimeTicks frame_time_;

    Error callback_error_;
    std::unique_ptr<blink::scheduler::WebAgentGroupScheduler>
        agent_group_scheduler_;
    blink::WebView* view_ = nullptr;
    blink::WebLocalFrame* frame_ = nullptr;
    blink::WebFrameWidget* widget_ = nullptr;
    // Borrowed from WebView; DidClose invalidates it before native release.
    blink::WebPagePopup* popup_ = nullptr;
    gfx::Rect popup_bounds_;
    gfx::PointF popup_offset_;
    gfx::Size popup_size_;
    FrameClient main_client_;
    base::WeakPtrFactory<Impl> weak_factory_ { this };
};

std::unique_ptr<BlinkBackend> BlinkBackend::Create(const ViewOptions& options,
    std::shared_ptr<LocalResources> resources, Error* error)
{
    ClearError(error);
    if (!resources) {
        Fail(error, ErrorCode::invalid_argument, "A Blink view requires local resources");
        return nullptr;
    }
    if (options.width <= 0 || options.height <= 0
        || !std::isfinite(options.device_scale)
        || options.device_scale <= 0) {
        Fail(error, ErrorCode::invalid_argument,
            "view size and device scale must be positive and finite");
        return nullptr;
    }
    BlinkProcess& process = BlinkProcess::Get();
    if (!process.EnsureCurrentThread(error))
        return nullptr;

    auto impl = std::make_unique<Impl>(options, std::move(resources), process);
    if (!process.RunTask([&] { return impl->Initialize(error); }, error))
        return nullptr;
    return std::unique_ptr<BlinkBackend>(new BlinkBackend(std::move(impl)));
}

BlinkBackend::BlinkBackend(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

BlinkBackend::~BlinkBackend() = default;

bool BlinkBackend::LoadFile(std::string_view path, Error* error)
{
    return impl_->LoadFile(path, error);
}

bool BlinkBackend::Evaluate(std::string_view source, std::string_view source_url,
    std::string* json_result, Error* error)
{
    return impl_->Evaluate(source, source_url, json_result, error);
}

bool BlinkBackend::Resize(
    int width, int height, float device_scale, Error* error)
{
    return impl_->Resize(width, height, device_scale, error);
}

bool BlinkBackend::Tick(double monotonic_seconds, Error* error)
{
    return impl_->Tick(monotonic_seconds, error);
}

FrameView BlinkBackend::frame() const
{
    return impl_->frame();
}

void BlinkBackend::Pointer(const PointerEvent& event)
{
    impl_->Pointer(event);
}

void BlinkBackend::Key(const KeyEvent& event)
{
    impl_->Key(event);
}

} // namespace mini_ui
