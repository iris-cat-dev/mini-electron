// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/bindings/source_location.h"

#include <memory>

#include "base/memory/ptr_util.h"
#include "base/tracing/protos/chrome_track_event.pbzero.h"
#include "third_party/blink/renderer/platform/bindings/script_forbidden_scope.h"
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS) || !defined(_WIN32)
#include "third_party/blink/renderer/platform/bindings/thread_debugger.h"
#endif
#include "third_party/blink/renderer/platform/bindings/v8_binding.h"
#include "third_party/blink/renderer/platform/bindings/v8_binding_macros.h"
#include "third_party/blink/renderer/platform/bindings/v8_per_isolate_data.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"
#include "third_party/blink/renderer/platform/instrumentation/tracing/traced_value.h"
#include "third_party/perfetto/include/perfetto/tracing/traced_proto.h"
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS) || !defined(_WIN32)
#include "v8/include/v8-inspector-protocol.h"
#endif
#include "v8/include/v8-debug.h"

namespace blink {

#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS) || !defined(_WIN32)
namespace {

String ToPlatformString(const v8_inspector::StringView& string)
{
    if (string.is8Bit()) {
        // SAFETY: v8_inspector::StringView guarantees characters8() and length()
        // are safe.
        return String(UNSAFE_BUFFERS(base::span(string.characters8(), string.length())));
    }
    // SAFETY: v8_inspector::StringView guarantees characters16() and length()
    // are safe.
    return String(UNSAFE_BUFFERS(base::span(reinterpret_cast<const UChar*>(string.characters16()), string.length())));
}

String ToPlatformString(std::unique_ptr<v8_inspector::StringBuffer> buffer)
{
    if (!buffer)
        return String();
    return ToPlatformString(buffer->string());
}

} // namespace
#endif

#if defined(MINI_ELECTRON_DISABLE_DEVTOOLS) && defined(_WIN32)
namespace {

std::unique_ptr<SourceLocation> CaptureNativeV8StackTrace(bool full)
{
    v8::Isolate* isolate = v8::Isolate::TryGetCurrent();
    if (!isolate || !isolate->InContext())
        return nullptr;

    ScriptForbiddenScope::AllowUserAgentScript allow_scripting;
    v8::Local<v8::StackTrace> stack_trace
        = v8::StackTrace::CurrentStackTrace(isolate, full ? 32 : 8, v8::StackTrace::kDetailed);
    if (stack_trace.IsEmpty() || stack_trace->GetFrameCount() == 0)
        return nullptr;

    StringBuilder formatted_stack;
    for (int i = 0; i < stack_trace->GetFrameCount(); ++i) {
        v8::Local<v8::StackFrame> frame = stack_trace->GetFrame(isolate, i);
        if (i)
            formatted_stack.Append('\n');
        formatted_stack.Append("    at ");
        String function = ToCoreStringWithNullCheck(isolate, frame->GetFunctionName());
        if (function.empty())
            formatted_stack.Append("<anonymous>");
        else
            formatted_stack.Append(function);
        formatted_stack.Append(" (");
        formatted_stack.Append(ToCoreStringWithNullCheck(isolate, frame->GetScriptNameOrSourceURL()));
        formatted_stack.Append(':');
        formatted_stack.Append(String::Number(frame->GetLineNumber()));
        formatted_stack.Append(':');
        formatted_stack.Append(String::Number(frame->GetColumn()));
        formatted_stack.Append(')');
    }

    v8::Local<v8::StackFrame> top = stack_trace->GetFrame(isolate, 0);
    return std::make_unique<SourceLocation>(
        ToCoreStringWithNullCheck(isolate, top->GetScriptNameOrSourceURL()), ToCoreStringWithNullCheck(isolate, top->GetFunctionName()),
        top->GetLineNumber(), top->GetColumn(), nullptr, top->GetScriptId(), formatted_stack.ToString());
}

} // namespace
#endif

// static
std::unique_ptr<SourceLocation> SourceLocation::CaptureWithFullStackTrace()
{
#if defined(MINI_ELECTRON_DISABLE_DEVTOOLS) && defined(_WIN32)
    if (auto location = CaptureNativeV8StackTrace(true))
        return location;
    return std::make_unique<SourceLocation>(String(), String(), 0, 0, nullptr, 0);
#else
    std::unique_ptr<v8_inspector::V8StackTrace> stack_trace = CaptureStackTraceInternal(true);
    if (stack_trace && !stack_trace->isEmpty()) {
        return CreateFromNonEmptyV8StackTraceInternal(std::move(stack_trace));
    }
    return std::make_unique<SourceLocation>(String(), String(), 0, 0, nullptr, 0);
#endif
}

#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS) || !defined(_WIN32)
// static
std::unique_ptr<v8_inspector::V8StackTrace> SourceLocation::CaptureStackTraceInternal(bool full)
{
    v8::Isolate* isolate = v8::Isolate::TryGetCurrent();
    ThreadDebugger* debugger = ThreadDebugger::From(isolate);
    if (!debugger || !isolate->InContext())
        return nullptr;
    v8_inspector::V8Inspector* inspector = debugger->GetV8Inspector();
    if (!inspector)
        return nullptr;
    ScriptForbiddenScope::AllowUserAgentScript allow_scripting;
    return inspector->captureStackTrace(full);
}

// static
std::unique_ptr<SourceLocation> SourceLocation::CreateFromNonEmptyV8StackTraceInternal(std::unique_ptr<v8_inspector::V8StackTrace> stack_trace)
{
    // Retrieve the data before passing the ownership to SourceLocation.
    String url = ToPlatformString(stack_trace->topSourceURL());
    String function = ToPlatformString(stack_trace->topFunctionName());
    unsigned line_number = stack_trace->topLineNumber();
    unsigned column_number = stack_trace->topColumnNumber();
    int script_id = stack_trace->topScriptId();
    return base::WrapUnique(new SourceLocation(url, function, line_number, column_number, std::move(stack_trace), script_id));
}
#endif

#if defined(MINI_ELECTRON_DISABLE_DEVTOOLS) && defined(_WIN32)
SourceLocation::SourceLocation(const String& url, const String& function, unsigned line_number, unsigned column_number, std::nullptr_t, int script_id,
    const String& stack_trace)
    : url_(url)
    , function_(function)
    , line_number_(line_number)
    , column_number_(column_number)
    , stack_trace_text_(stack_trace)
    , script_id_(script_id)
{
}
#else
SourceLocation::SourceLocation(const String& url, const String& function, unsigned line_number, unsigned column_number,
    std::unique_ptr<v8_inspector::V8StackTrace> stack_trace, int script_id)
    : url_(url)
    , function_(function)
    , line_number_(line_number)
    , column_number_(column_number)
    , stack_trace_(std::move(stack_trace))
    , script_id_(script_id)
{
}
#endif

SourceLocation::~SourceLocation() = default;

std::unique_ptr<SourceLocation> SourceLocation::Clone() const
{
#if defined(MINI_ELECTRON_DISABLE_DEVTOOLS) && defined(_WIN32)
    return base::WrapUnique(new SourceLocation(url_, function_, line_number_, column_number_, nullptr, script_id_, stack_trace_text_));
#else
    return base::WrapUnique(new SourceLocation(url_, function_, line_number_, column_number_, stack_trace_ ? stack_trace_->clone() : nullptr, script_id_));
#endif
}

void SourceLocation::WriteIntoTrace(perfetto::TracedProto<SourceLocation::Proto> proto) const
{
#if defined(MINI_ELECTRON_DISABLE_DEVTOOLS) && defined(_WIN32)
    return;
#else
    *(int*)1 = 1;
#endif
    //   if (!stack_trace_ || stack_trace_->isEmpty()) {
    //     return;
    //   }
    //
    //   proto->set_function_name(
    //       ToPlatformString(stack_trace_->topFunctionName()).Utf8());
    //   proto->set_script_id(stack_trace_->topScriptId());
    //   proto->set_url(ToPlatformString(stack_trace_->topSourceURL()).Utf8());
    //   proto->set_line_number(stack_trace_->topLineNumber());
    //   proto->set_column_number(stack_trace_->topColumnNumber());
    //   proto->set_stack_trace(ToString().Utf8());
    //
    //   // TODO(https://crbug.com/1396277): This should be a WriteIntoTrace function
    //   // once v8 has support for perfetto tracing (which is currently missing for v8
    //   // chromium).
    //   for (const auto& frame : stack_trace_->frames()) {
    //     auto& stack_trace_pb = *(proto->add_stack_frames());
    //     stack_trace_pb.set_function_name(
    //         ToPlatformString(frame.functionName).Utf8());
    //
    //     auto& script_location = *(stack_trace_pb.set_script_location());
    //     script_location.set_source_url(ToPlatformString(frame.sourceURL).Utf8());
    //     script_location.set_line_number(frame.lineNumber);
    //     script_location.set_column_number(frame.columnNumber);
    //   }
}

void SourceLocation::WriteIntoTrace(perfetto::TracedValue context) const
{
#if defined(MINI_ELECTRON_DISABLE_DEVTOOLS) && defined(_WIN32)
    return;
#else
    *(int*)1 = 1;
#endif
    //   if (!stack_trace_ || stack_trace_->isEmpty()) {
    //     return;
    //   }
    //
    //   // TODO(altimin): Consider replacing nested dict-inside-array with just an
    //   // array here.
    //   auto array = std::move(context).WriteArray();
    //   auto dict = array.AppendDictionary();
    //   // TODO(altimin): Add TracedValue support to v8::StringView and remove
    //   // ToPlatformString calls.
    //   dict.Add("functionName", ToPlatformString(stack_trace_->topFunctionName()));
    //   dict.Add("scriptId", String::Number(stack_trace_->topScriptId()));
    //   dict.Add("url", ToPlatformString(stack_trace_->topSourceURL()));
    //   dict.Add("lineNumber", stack_trace_->topLineNumber());
    //   dict.Add("columnNumber", stack_trace_->topColumnNumber());
}

void SourceLocation::ToTracedValue(TracedValue* value, const char* name) const
{
#if defined(MINI_ELECTRON_DISABLE_DEVTOOLS) && defined(_WIN32)
    return;
#else
    if (!stack_trace_ || stack_trace_->isEmpty())
        return;
    value->BeginArray(name);
    value->BeginDictionary();
    value->SetString("functionName", ToPlatformString(stack_trace_->topFunctionName()));
    value->SetInteger("scriptId", stack_trace_->topScriptId());
    value->SetString("url", ToPlatformString(stack_trace_->topSourceURL()));
    value->SetInteger("lineNumber", stack_trace_->topLineNumber());
    value->SetInteger("columnNumber", stack_trace_->topColumnNumber());

    value->BeginArray("stackFrames");
    for (const auto& frame : stack_trace_->frames()) {
        value->BeginDictionary();
        value->SetString("functionName", ToPlatformString(frame.functionName));

        value->BeginDictionary("scriptLocation");
        value->SetString("sourceURL", ToPlatformString(frame.sourceURL));
        value->SetInteger("lineNumber", frame.lineNumber);
        value->SetInteger("columnNumber", frame.columnNumber);
        value->EndDictionary(/*scriptLocation*/);

        value->EndDictionary();
    }
    value->EndArray(/*stackFrames*/);

    value->EndDictionary();
    value->EndArray();
#endif
}

String SourceLocation::ToString() const
{
#if defined(MINI_ELECTRON_DISABLE_DEVTOOLS) && defined(_WIN32)
    return stack_trace_text_;
#else
    if (!stack_trace_)
        return String();
    return ToPlatformString(stack_trace_->toString());
#endif
}

#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS) || !defined(_WIN32)
std::unique_ptr<v8_inspector::protocol::Runtime::API::StackTrace> SourceLocation::BuildInspectorObject() const
{
    return BuildInspectorObject(std::numeric_limits<int>::max());
}

std::unique_ptr<v8_inspector::protocol::Runtime::API::StackTrace> SourceLocation::BuildInspectorObject(int max_async_depth) const
{
    return stack_trace_ ? stack_trace_->buildInspectorObject(max_async_depth) : nullptr;
}
#endif

std::unique_ptr<SourceLocation> CaptureSourceLocation(const String& url, unsigned line_number, unsigned column_number)
{
#if defined(MINI_ELECTRON_DISABLE_DEVTOOLS) && defined(_WIN32)
    if (auto location = CaptureNativeV8StackTrace(false))
        return location;
    return std::make_unique<SourceLocation>(url, String(), line_number, column_number, nullptr);
#else
    std::unique_ptr<v8_inspector::V8StackTrace> stack_trace = SourceLocation::CaptureStackTraceInternal(false);
    if (stack_trace && !stack_trace->isEmpty()) {
        return SourceLocation::CreateFromNonEmptyV8StackTraceInternal(std::move(stack_trace));
    }
    return std::make_unique<SourceLocation>(url, String(), line_number, column_number, std::move(stack_trace));
#endif
}

std::unique_ptr<SourceLocation> CaptureSourceLocation()
{
#if defined(MINI_ELECTRON_DISABLE_DEVTOOLS) && defined(_WIN32)
    if (auto location = CaptureNativeV8StackTrace(false))
        return location;
    return std::make_unique<SourceLocation>(String(), String(), 0, 0, nullptr);
#else
    std::unique_ptr<v8_inspector::V8StackTrace> stack_trace = SourceLocation::CaptureStackTraceInternal(false);
    if (stack_trace && !stack_trace->isEmpty()) {
        return SourceLocation::CreateFromNonEmptyV8StackTraceInternal(std::move(stack_trace));
    }

    return std::make_unique<SourceLocation>(String(), String(), 0, 0, std::move(stack_trace));
#endif
}

std::unique_ptr<SourceLocation> CaptureSourceLocation(v8::Isolate* isolate, v8::Local<v8::Function> function)
{
    if (!function.IsEmpty())
        return std::make_unique<SourceLocation>(ToCoreStringWithUndefinedOrNullCheck(isolate, function->GetScriptOrigin().ResourceName()),
            ToCoreStringWithUndefinedOrNullCheck(isolate, function->GetName()), function->GetScriptLineNumber() + 1, function->GetScriptColumnNumber() + 1,
            nullptr, function->ScriptId());
    return std::make_unique<SourceLocation>(String(), String(), 0, 0, nullptr, 0);
}

} // namespace blink
