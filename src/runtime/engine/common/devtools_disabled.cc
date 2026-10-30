// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "third_party/blink/renderer/core/inspector/main_thread_debugger.h"
#include "third_party/blink/renderer/core/inspector/worker_inspector_controller.h"
#include "third_party/blink/renderer/core/inspector/worker_thread_debugger.h"

#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_core.h"
#include "third_party/blink/renderer/core/core_probe_sink.h"
#include "third_party/blink/renderer/core/events/error_event.h"
#include "third_party/blink/renderer/core/frame/frame_console.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/inspector/inspector_audits_issue.h"
#include "third_party/blink/renderer/core/inspector/inspector_issue_storage.h"
#include "third_party/blink/renderer/core/inspector/worker_devtools_params.h"
#include "third_party/blink/renderer/core/workers/worker_or_worklet_global_scope.h"
#include "third_party/blink/renderer/core/workers/worker_reporting_proxy.h"
#include "third_party/blink/renderer/core/workers/worker_thread.h"
#include "third_party/blink/renderer/core/workers/worklet_global_scope.h"
#include "third_party/blink/renderer/platform/bindings/source_location.h"
#include "base/no_destructor.h"
#include "third_party/blink/renderer/bindings/core/v8/capture_source_location.h"
#include "third_party/blink/renderer/platform/bindings/v8_binding.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"
#include "v8/src/debug/debug-interface.h"

#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
#error "This file is only for DevTools-free targets."
#endif

namespace blink {

namespace {

LocalFrame* ToLocalFrame(ExecutionContext* context)
{
    if (!context)
        return nullptr;
    if (auto* window = DynamicTo<LocalDOMWindow>(context))
        return window->GetFrame();
    if (context->IsMainThreadWorkletGlobalScope())
        return To<WorkletGlobalScope>(context)->GetFrame();
    return nullptr;
}

} // namespace

namespace {

// V8ConsoleMessage's plain-text formatting, without remote objects or sessions.
class ConsoleArgumentFormatter {
public:
    static String Format(v8::Local<v8::Context> context, v8::Local<v8::Value> value)
    {
        ConsoleArgumentFormatter formatter(context);
        if (!formatter.Append(value) || formatter.try_catch_.HasCaught())
            return String();
        return formatter.text_.ToString();
    }

private:
    explicit ConsoleArgumentFormatter(v8::Local<v8::Context> context)
        : context_(context)
        , isolate_(context->GetIsolate())
        , arrays_(isolate_)
        , try_catch_(isolate_)
    {
    }

    bool Append(v8::Local<v8::Value> value, bool array_item = false)
    {
        if (value.IsEmpty() || (array_item && value->IsNullOrUndefined()))
            return true;
        if (value->IsBigIntObject())
            value = value.As<v8::BigIntObject>()->ValueOf();
        else if (value->IsBooleanObject())
            value = v8::Boolean::New(isolate_, value.As<v8::BooleanObject>()->ValueOf());
        else if (value->IsNumberObject())
            value = v8::Number::New(isolate_, value.As<v8::NumberObject>()->ValueOf());
        else if (value->IsStringObject())
            value = value.As<v8::StringObject>()->ValueOf();
        else if (value->IsSymbolObject())
            value = value.As<v8::SymbolObject>()->ValueOf();

        if (value->IsSymbol()) {
            text_.Append("Symbol(");
            auto description = value.As<v8::Symbol>()->Description(isolate_);
            bool result = description->IsUndefined() || Append(description);
            text_.Append(')');
            return result;
        }
        if (value->IsArray()) {
            auto array = value.As<v8::Array>();
            for (auto visited : arrays_) {
                if (visited == array)
                    return true;
            }
            uint32_t length = array->Length();
            if (length > remaining_array_items_ || arrays_.size() > 32)
                return false;
            remaining_array_items_ -= length;
            arrays_.push_back(array);
            bool result = true;
            for (uint32_t i = 0; i < length; ++i) {
                if (i)
                    text_.Append(',');
                v8::Local<v8::Value> item;
                if (!array->Get(context_, i).ToLocal(&item))
                    continue;
                if (!Append(item, true)) {
                    result = false;
                    break;
                }
            }
            arrays_.pop_back();
            return result;
        }
        if (value->IsProxy()) {
            text_.Append("[object Proxy]");
            return true;
        }
        v8::Local<v8::String> text;
        if (value->IsObject() && !value->IsDate() && !value->IsFunction()
            && !value->IsNativeError() && !value->IsRegExp()) {
            if (value.As<v8::Object>()->ObjectProtoToString(context_).ToLocal(&text))
                return AppendText(text);
        }
        if (!value->ToString(context_).ToLocal(&text) || !AppendText(text))
            return false;
        if (value->IsBigInt())
            text_.Append('n');
        return true;
    }

    bool AppendText(v8::Local<v8::String> text)
    {
        if (try_catch_.HasCaught())
            return false;
        text_.Append(ToCoreString(isolate_, text));
        return true;
    }

    v8::Local<v8::Context> context_;
    v8::Isolate* isolate_;
    StringBuilder text_;
    v8::LocalVector<v8::Array> arrays_;
    v8::TryCatch try_catch_;
    uint32_t remaining_array_items_ = 10000;
};

} // namespace

// Console output is an embedder API, independent of the DevTools inspector.
// This delegate is stateless: it retains no isolate, context, frame or JS value.
class DevToolsFreeConsoleDelegate final : public v8::debug::ConsoleDelegate {
public:
    static DevToolsFreeConsoleDelegate* Get()
    {
        static base::NoDestructor<DevToolsFreeConsoleDelegate> delegate;
        return delegate.get();
    }

    void Log(const v8::debug::ConsoleCallArguments& args, const v8::debug::ConsoleContext&) override
    {
        Report(args, mojom::ConsoleMessageLevel::kInfo);
    }
    void Info(const v8::debug::ConsoleCallArguments& args, const v8::debug::ConsoleContext&) override
    {
        Report(args, mojom::ConsoleMessageLevel::kInfo);
    }
    void Warn(const v8::debug::ConsoleCallArguments& args, const v8::debug::ConsoleContext&) override
    {
        Report(args, mojom::ConsoleMessageLevel::kWarning);
    }
    void Error(const v8::debug::ConsoleCallArguments& args, const v8::debug::ConsoleContext&) override
    {
        Report(args, mojom::ConsoleMessageLevel::kError);
    }
    void Debug(const v8::debug::ConsoleCallArguments& args, const v8::debug::ConsoleContext&) override
    {
        Report(args, mojom::ConsoleMessageLevel::kVerbose);
    }

private:
    static void Report(const v8::debug::ConsoleCallArguments& args, mojom::ConsoleMessageLevel level)
    {
        v8::Isolate* isolate = args.GetIsolate();
        v8::HandleScope handles(isolate);
        v8::Local<v8::Context> context = isolate->GetCurrentContext();
        if (context.IsEmpty())
            return;
        auto* debugger = static_cast<ThreadDebuggerCommonImpl*>(ThreadDebugger::From(isolate));
        ExecutionContext* execution_context = ToExecutionContext(context);
        if (!debugger || !execution_context)
            return;
        auto location = CaptureSourceLocation(isolate, execution_context);
        StringBuilder message;
        for (int i = 0; i < args.Length(); ++i) {
            if (i)
                message.Append(' ');
            message.Append(ConsoleArgumentFormatter::Format(context, args[i]));
        }
        debugger->ReportConsoleMessage(execution_context, mojom::ConsoleMessageSource::kConsoleApi,
            level, message.ToString(), location.get());
    }
};

ThreadDebuggerCommonImpl::ThreadDebuggerCommonImpl(v8::Isolate* isolate)
    : ThreadDebugger(isolate)
    , isolate_(isolate)
{
    v8::debug::SetConsoleDelegate(isolate_, DevToolsFreeConsoleDelegate::Get());
}

ThreadDebuggerCommonImpl::~ThreadDebuggerCommonImpl()
{
    v8::debug::SetConsoleDelegate(isolate_, nullptr);
}

void ThreadDebuggerCommonImpl::AsyncTaskScheduled(const StringView&, void*, bool) { }
void ThreadDebuggerCommonImpl::AsyncTaskCanceled(void*) { }
void ThreadDebuggerCommonImpl::AllAsyncTasksCanceled() { }
void ThreadDebuggerCommonImpl::AsyncTaskStarted(void*) { }
void ThreadDebuggerCommonImpl::AsyncTaskFinished(void*) { }

v8_inspector::V8StackTraceId ThreadDebuggerCommonImpl::StoreCurrentStackTrace(const StringView&)
{
    return v8_inspector::V8StackTraceId();
}

void ThreadDebuggerCommonImpl::ExternalAsyncTaskStarted(const v8_inspector::V8StackTraceId&) { }
void ThreadDebuggerCommonImpl::ExternalAsyncTaskFinished(const v8_inspector::V8StackTraceId&) { }

unsigned ThreadDebuggerCommonImpl::PromiseRejected(
    v8::Local<v8::Context> context,
    const String& error_message,
    v8::Local<v8::Value>,
    std::unique_ptr<SourceLocation> location)
{
    String message = error_message;
    if (message.empty()) {
        message = "Uncaught (in promise)";
    } else if (message.StartsWith("Uncaught ")) {
        message = "Uncaught (in promise)" + StringView(message, 8);
    }
    ReportConsoleMessage(ToExecutionContext(context), mojom::ConsoleMessageSource::kJavaScript,
        mojom::ConsoleMessageLevel::kError, message, location.get());
    return 0;
}

void ThreadDebuggerCommonImpl::PromiseRejectionRevoked(v8::Local<v8::Context>, unsigned) { }

MainThreadDebugger::MainThreadDebugger(v8::Isolate* isolate)
    : ThreadDebuggerCommonImpl(isolate)
    , paused_(false)
{
}

MainThreadDebugger::~MainThreadDebugger() = default;

MainThreadDebugger* MainThreadDebugger::Instance(v8::Isolate* isolate)
{
    ThreadDebugger* debugger = ThreadDebugger::From(isolate);
    DCHECK(debugger && !debugger->IsWorker());
    return static_cast<MainThreadDebugger*>(debugger);
}

void MainThreadDebugger::SetClientMessageLoop(std::unique_ptr<ClientMessageLoop> client_message_loop)
{
    client_message_loop_ = std::move(client_message_loop);
}

int MainThreadDebugger::ContextGroupId(LocalFrame*) { return 0; }
int MainThreadDebugger::ContextGroupId(ExecutionContext*) { return 0; }
void MainThreadDebugger::DidClearContextsForFrame(LocalFrame*) { }
void MainThreadDebugger::ContextCreated(ScriptState*, LocalFrame*, const SecurityOrigin*) { }
void MainThreadDebugger::ContextWillBeDestroyed(ScriptState*) { }

void MainThreadDebugger::ReportConsoleMessage(
    ExecutionContext* context,
    mojom::ConsoleMessageSource source,
    mojom::ConsoleMessageLevel level,
    const String& message,
    SourceLocation* location)
{
    if (LocalFrame* frame = ToLocalFrame(context))
        frame->Console().ReportMessageToClient(source, level, message, location);
}

void MainThreadDebugger::ExceptionThrown(ExecutionContext* context, ErrorEvent* event)
{
    if (LocalFrame* frame = ToLocalFrame(context)) {
        frame->Console().ReportMessageToClient(mojom::ConsoleMessageSource::kJavaScript,
            mojom::ConsoleMessageLevel::kError, event->MessageForConsole(), event->Location());
    }
}

WorkerThreadDebugger* WorkerThreadDebugger::From(v8::Isolate* isolate)
{
    ThreadDebugger* debugger = ThreadDebugger::From(isolate);
    if (!debugger)
        return nullptr;
    DCHECK(debugger->IsWorker());
    return static_cast<WorkerThreadDebugger*>(debugger);
}

WorkerThreadDebugger::WorkerThreadDebugger(v8::Isolate* isolate)
    : ThreadDebuggerCommonImpl(isolate)
{
}

WorkerThreadDebugger::~WorkerThreadDebugger() = default;

int WorkerThreadDebugger::ContextGroupId(WorkerThread* worker_thread)
{
    return worker_thread->GetWorkerThreadId();
}

int WorkerThreadDebugger::ContextGroupId(ExecutionContext* context)
{
    return ContextGroupId(To<WorkerOrWorkletGlobalScope>(context)->GetThread());
}

void WorkerThreadDebugger::WorkerThreadCreated(WorkerThread*) { }
void WorkerThreadDebugger::WorkerThreadDestroyed(WorkerThread*) { }

void WorkerThreadDebugger::ContextCreated(WorkerThread*, const KURL&, v8::Local<v8::Context>) { }
void WorkerThreadDebugger::ContextWillBeDestroyed(WorkerThread*, v8::Local<v8::Context>) { }
void WorkerThreadDebugger::PauseWorkerOnStart(WorkerThread*) { }

void WorkerThreadDebugger::ReportConsoleMessage(
    ExecutionContext* context,
    mojom::ConsoleMessageSource source,
    mojom::ConsoleMessageLevel level,
    const String& message,
    SourceLocation* location)
{
    if (!context)
        return;
    auto* scope = DynamicTo<WorkerOrWorkletGlobalScope>(context);
    if (scope) {
        scope->GetThread()->GetWorkerReportingProxy().ReportConsoleMessage(source, level, message, location);
    }
}

void WorkerThreadDebugger::ExceptionThrown(WorkerThread* worker_thread, ErrorEvent* event)
{
    worker_thread->GetWorkerReportingProxy().ReportConsoleMessage(
        mojom::ConsoleMessageSource::kJavaScript, mojom::ConsoleMessageLevel::kError,
        event->MessageForConsole(), event->Location());
}

WorkerInspectorController* WorkerInspectorController::Create(
    WorkerThread* thread,
    const KURL& url,
    scoped_refptr<InspectorTaskRunner> inspector_task_runner,
    std::unique_ptr<WorkerDevToolsParams> devtools_params)
{
    return MakeGarbageCollected<WorkerInspectorController>(thread, url,
        WorkerThreadDebugger::From(thread->GetIsolate()), std::move(inspector_task_runner),
        std::move(devtools_params));
}

WorkerInspectorController::WorkerInspectorController(
    WorkerThread*,
    const KURL&,
    WorkerThreadDebugger*,
    scoped_refptr<InspectorTaskRunner>,
    std::unique_ptr<WorkerDevToolsParams>)
    : probe_sink_(MakeGarbageCollected<CoreProbeSink>())
{
}

WorkerInspectorController::~WorkerInspectorController() = default;
void WorkerInspectorController::Dispose() { }
void WorkerInspectorController::FlushProtocolNotifications() { }
void WorkerInspectorController::WaitForDebuggerIfNeeded() { }

void WorkerInspectorController::Trace(Visitor* visitor) const
{
    visitor->Trace(probe_sink_);
}

#if defined(_WIN32)
void AuditsIssue::ReportQuirksModeIssue(ExecutionContext*, bool, DOMNodeId, String, String, String) { }

void AuditsIssue::ReportCorsIssue(ExecutionContext*, int64_t, RendererCorsIssueCode, String, String, String,
    std::optional<base::UnguessableToken>)
{
}

void AuditsIssue::ReportAttributionIssue(
    ExecutionContext*, mojom::blink::AttributionReportingIssueType, Element*, const String&, const String&)
{
}

void AuditsIssue::ReportSharedArrayBufferIssue(ExecutionContext*, bool, SharedArrayBufferIssueType) { }
void AuditsIssue::ReportDeprecationIssue(ExecutionContext*, String) { }
void AuditsIssue::ReportClientHintIssue(LocalDOMWindow*, ClientHintIssueReason) { }

AuditsIssue AuditsIssue::CreateBlockedByResponseIssue(
    network::mojom::BlockedByResponseReason, uint64_t, DocumentLoader*, const ResourceError&, const base::UnguessableToken&)
{
    return AuditsIssue();
}

void AuditsIssue::ReportMixedContentIssue(
    const KURL&, const KURL&, mojom::blink::RequestContextType, LocalFrame*, MixedContentResolutionStatus, const String&)
{
}

AuditsIssue AuditsIssue::CreateContentSecurityPolicyIssue(
    const SecurityPolicyViolationEventInit&, bool, ContentSecurityPolicyViolationType, LocalFrame*, Element*, SourceLocation*,
    std::optional<base::UnguessableToken>)
{
    return AuditsIssue();
}

protocol::Audits::GenericIssueErrorType AuditsIssue::GenericIssueErrorTypeToProtocol(mojom::blink::GenericIssueErrorType)
{
    return protocol::Audits::GenericIssueErrorType();
}

void AuditsIssue::ReportGenericIssue(LocalFrame*, mojom::blink::GenericIssueErrorType, int) { }
void AuditsIssue::ReportGenericIssue(LocalFrame*, mojom::blink::GenericIssueErrorType, int, const String&) { }
void AuditsIssue::ReportStylesheetLoadingLateImportIssue(Document*, const KURL&, WTF::OrdinalNumber, WTF::OrdinalNumber) { }

void AuditsIssue::ReportPropertyRuleIssue(
    Document*, const KURL&, WTF::OrdinalNumber, WTF::OrdinalNumber, protocol::Audits::PropertyRuleIssueReason, const String&)
{
}

void AuditsIssue::ReportStylesheetLoadingRequestFailedIssue(
    Document*, const KURL&, const String&, const KURL&, WTF::OrdinalNumber, WTF::OrdinalNumber, const String&)
{
}

InspectorIssueStorage::InspectorIssueStorage() = default;
InspectorIssueStorage::~InspectorIssueStorage() = default;
void InspectorIssueStorage::AddInspectorIssue(ExecutionContext*, AuditsIssue) { }
void InspectorIssueStorage::AddInspectorIssue(CoreProbeSink*, AuditsIssue) { }
void InspectorIssueStorage::Clear() { }
wtf_size_t InspectorIssueStorage::size() const { return 0; }
protocol::Audits::InspectorIssue* InspectorIssueStorage::at(wtf_size_t) const { return nullptr; }
#endif

} // namespace blink
