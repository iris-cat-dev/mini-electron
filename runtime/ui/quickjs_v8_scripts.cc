// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "runtime/ui/quickjs_v8_scripts.h"
#include "runtime/ui/quickjs_v8_internal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace v8 {
namespace {
class OwnedValue {
public:
    OwnedValue(JSContext* context, JSValue value) : context_(context), value_(value) {}
    ~OwnedValue() { JS_FreeValue(context_, value_); }
    OwnedValue(const OwnedValue&) = delete;
    OwnedValue& operator=(const OwnedValue&) = delete;
    JSValueConst get() const { return value_; }
    JSValue release() { JSValue value = value_; value_ = JS_UNDEFINED; return value; }
private:
    JSContext* context_;
    JSValue value_;
};

bool Text(JSContext* context, JSValueConst value, std::string& result)
{
    size_t size = 0;
    const char* text = JS_ToCStringLen(context, &size, value);
    if (!text)
        return false;
    result.assign(text, size);
    JS_FreeCString(context, value, text);
    return true;
}

bool Fail(JSContext* context, const char* message)
{
    JS_ThrowTypeError(context, "%s", message);
    detail::CaptureException(detail::IsolateFor(context), context);
    return false;
}

bool Check(JSContext* context, int result)
{
    if (result >= 0)
        return true;
    detail::CaptureException(detail::IsolateFor(context), context);
    return false;
}

Local<String> StringValue(JSContext* context, std::string_view text)
{
    return detail::Adopt(context, JS_NewStringLen(context, text.data(), text.size())).As<String>();
}
Local<Value> OptionalStringValue(JSContext* context, std::string_view text)
{
    return text.empty() ? detail::Adopt(context, JS_UNDEFINED) : StringValue(context, text);
}

std::atomic<int> next_script_id{1};
// Realm-neutral native source/origin data; no engine pointer or JS root.
struct SourceInfo {
    std::string source, filename, source_url, source_map;
    int id = next_script_id.fetch_add(1, std::memory_order_relaxed);
    int line_offset = 0, column_offset = 0;
    ScriptOriginOptions options;
};
// This index never roots scripts or source text. Equal resource names do not
// identify equal scripts; ambiguous names deliberately yield no source metadata.
struct SourceRecord {
    std::weak_ptr<SourceInfo> info;
    bool ambiguous = false;
};
// UI-thread-only process statics: CRT initialization precedes BlinkProcess,
// so its shutdown and late context finalizers run before these registries die.
std::unordered_map<JSContext*, std::unordered_map<std::string, SourceRecord>> sources;
void RegisterSource(JSContext* context, const std::shared_ptr<SourceInfo>& info)
{
    auto [it, inserted] = sources[context].try_emplace(info->filename, SourceRecord{info, false});
    if (!inserted && it->second.info.lock() != info)
        it->second.ambiguous = true;
}
// Function origins use exact engine compilation identity, not the legacy
// filename index used by stack records that have no compilation ID.
std::unordered_map<JSContext*, std::unordered_map<uint64_t, std::weak_ptr<SourceInfo>>> compiled_sources;
void RegisterCompiledSource(JSContext* context, JSValueConst compiled,
    const std::shared_ptr<SourceInfo>& info)
{
    const uint64_t id = JS_GetCompilationId(context, compiled);
    if (id)
        compiled_sources[context][id] = info;
}

struct ScriptPayload final : detail::DataPayload {
    // Compiled Data artifacts retain their actual realm. Function API views
    // instead rely on the function's engine obj_ctx edge, never a metadata pin.
    explicit ScriptPayload(JSContext* ctx, bool pin_realm = true)
        : context(pin_realm ? JS_DupContext(ctx) : nullptr), runtime(JS_GetRuntime(ctx)) {}
    ~ScriptPayload() override
    {
        JS_FreeValueRT(runtime, resource_name);
        JS_FreeValueRT(runtime, source_map_url);
        detail::Release(host_options);
        if (context)
            JS_FreeContext(context);
    }
    JSContext* context;
    JSRuntime* runtime;
    std::shared_ptr<SourceInfo> info;
    JSValue resource_name = JS_UNDEFINED, source_map_url = JS_UNDEFINED;
    detail::Cell* host_options = nullptr;
    Module::SyntheticModuleEvaluationSteps synthetic = nullptr;
};

// Module-owned native metadata must never point back to a Context or Cell.
struct ModuleHostData {
    std::shared_ptr<SourceInfo> info;
    Module::SyntheticModuleEvaluationSteps synthetic = nullptr;
};
void FreeModuleHostData(JSRuntime*, void* opaque)
{
    delete static_cast<ModuleHostData*>(opaque);
}

// Callers are Scripts API views produced by the matching factory in this file:
// Script/Module -> ScriptPayload, ModuleRequest -> RequestPayload,
// Message -> MessagePayload, StackTrace -> TracePayload.
// Promise capabilities live in private VM properties, not per-handle metadata.
// Never use this helper to probe an arbitrary Value's or another slice's metadata.
template<class T> std::shared_ptr<T> Payload(const Data* value)
{
    return std::static_pointer_cast<T>(detail::Payload(value));
}

std::string MagicComment(const std::string& source, std::string_view key)
{
    size_t position = source.rfind(key);
    if (position == std::string::npos)
        return {};
    const size_t line = source.rfind('\n', position);
    const size_t start = line == std::string::npos ? 0 : line + 1;
    std::string_view prefix(source.data() + start, position - start);
    const size_t comment = prefix.find("//");
    if (comment == std::string_view::npos)
        return {};
    prefix.remove_prefix(comment + 2);
    if (prefix.find_first_not_of(" \t#@") != std::string_view::npos)
        return {};
    position += key.size();
    while (position < source.size() && (source[position] == ' ' || source[position] == '\t'))
        ++position;
    const size_t end = source.find_first_of("\r\n \t", position);
    return source.substr(position, end == std::string::npos ? end : end - position);
}

std::shared_ptr<ScriptPayload> MakeSource(JSContext* context, Local<String> source,
    const ScriptOrigin& origin, bool pin_realm = true)
{
    if (origin.Options().IsWasm()) {
        Fail(context, "QuickJS does not implement WebAssembly scripts");
        return {};
    }
    auto payload = std::make_shared<ScriptPayload>(context, pin_realm);
    auto info = std::make_shared<SourceInfo>();
    if (!Text(context, detail::Borrow(*source), info->source)) {
        detail::CaptureException(detail::IsolateFor(context), context);
        return {};
    }
    if (!origin.ResourceName().IsEmpty() && !JS_IsUndefined(detail::Borrow(*origin.ResourceName()))) {
        payload->resource_name = JS_DupValue(context, detail::Borrow(*origin.ResourceName()));
        if (!Text(context, payload->resource_name, info->filename)) {
            detail::CaptureException(detail::IsolateFor(context), context);
            return {};
        }
    }
    if (info->filename.empty())
        info->filename = "<anonymous>";
    if (info->filename.find('\0') != std::string::npos) {
        Fail(context, "Script resource name contains a null character");
        return {};
    }
    info->line_offset = origin.LineOffset();
    info->column_offset = origin.ColumnOffset();
    info->options = origin.Options();
    if (origin.ScriptId() >= 0)
        info->id = origin.ScriptId();
    info->source_url = MagicComment(info->source, "sourceURL=");
    info->source_map = MagicComment(info->source, "sourceMappingURL=");
    if (!origin.SourceMapUrl().IsEmpty() && !JS_IsUndefined(detail::Borrow(*origin.SourceMapUrl()))) {
        payload->source_map_url = JS_DupValue(context, detail::Borrow(*origin.SourceMapUrl()));
        if (!Text(context, payload->source_map_url, info->source_map)) {
            detail::CaptureException(detail::IsolateFor(context), context);
            return {};
        }
    }
    if (!origin.GetHostDefinedOptions().IsEmpty()) {
        payload->host_options = detail::CellOf(*origin.GetHostDefinedOptions());
        detail::Retain(payload->host_options);
    }
    payload->info = info;
    RegisterSource(context, info);
    return payload;
}

std::string CompileText(const SourceInfo& info)
{
    std::string text;
    if (info.line_offset > 0)
        text.append(static_cast<size_t>(info.line_offset), '\n');
    if (info.column_offset > 0)
        text.append(static_cast<size_t>(info.column_offset), ' ');
    text += info.source;
    return text;
}

JSValue CompileCode(JSContext* context, const SourceInfo& info, bool module)
{
    std::string padded;
    const std::string* text = &info.source;
    if (info.line_offset > 0 || info.column_offset > 0) {
        padded = CompileText(info);
        text = &padded;
    }
    // Debugging sourceURL comments must not change local-module resolution bases.
    const char* filename = info.filename.c_str();
    if (module)
        return JS_CompileModule(context, text->c_str(), text->size(), filename);
    return JS_Eval(context, text->c_str(), text->size(), filename,
        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
}

bool CompileOptionsSupported(JSContext* context, ScriptCompiler::CompileOptions options)
{
    if (options == ScriptCompiler::kNoCompileOptions || options == ScriptCompiler::kEagerCompile)
        return true;
    return Fail(context, "QuickJS does not support V8 code cache or compile hints");
}

Location OffsetLocation(const SourceInfo& info, int offset)
{
    if (offset < 0)
        return {-1, -1};
    // V8 positions count UTF-16 code units, not UTF-8 bytes.
    int position = 0, line = info.line_offset, column = info.column_offset;
    for (size_t i = 0; i < info.source.size() && position < offset;) {
        const unsigned char c = static_cast<unsigned char>(info.source[i]);
        size_t width = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        if (i + width > info.source.size())
            width = 1;
        const int units = width == 4 ? 2 : 1;
        if (c == '\r') {
            const bool crlf = i + 1 < info.source.size() && info.source[i + 1] == '\n';
            if (crlf && position + 1 == offset) {
                ++column;
            } else {
                ++line; column = 0;
                if (crlf) { ++i; ++position; }
            }
        } else if (c == '\n' || (width == 3 && c == 0xe2 &&
            static_cast<unsigned char>(info.source[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(info.source[i + 2]) == 0xa8 || static_cast<unsigned char>(info.source[i + 2]) == 0xa9))) {
            ++line; column = 0;
        } else {
            column += std::min(units, offset - position);
        }
        position += units; i += width;
    }
    if (position < offset)
        return {-1, -1};
    return {line, column};
}

JSModuleDef* ModuleDef(const Data* module)
{
    return static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(detail::Borrow(module)));
}

// Match sources' process lifetime, not the UI thread's TLS teardown lifetime.
std::unordered_map<JSModuleDef*, std::weak_ptr<ScriptPayload>> module_payloads;

struct ResolveState {
    Module::ResolveModuleCallback callback;
};
JSModuleDef* ResolveModule(JSContext* context, JSModuleDef* referrer, const char* specifier, void* opaque)
{
    auto* state = static_cast<ResolveState*>(opaque);
    auto isolate = detail::IsolateFor(context);
    auto name = StringValue(context, specifier);
    if (name.IsEmpty())
        return nullptr;
    Local<Module> result;
    if (!state->callback(detail::ContextForQuickJS(context), name,
            FixedArray::New(isolate, 0), detail::ModuleView(context, referrer)).ToLocal(&result)) {
        // Core owns the exception until the QuickJS callback boundary.
        if (detail::HasPendingException(isolate))
            JS_Throw(context, detail::TakePendingException(isolate));
        else
            JS_ThrowTypeError(context, "Module resolution failed without an exception");
        return nullptr;
    }
    if (detail::ContextOf(*result) != context) {
        JS_ThrowTypeError(context, "Module resolver returned a module from another context");
        return nullptr;
    }
    return ModuleDef(*result);
}

// Realm-neutral request copy: no JSModuleDef, JSAtom or JSContext is retained.
struct RequestPayload final : detail::DataPayload {
    std::string specifier;
    int offset = -1;
};

Local<Private> ResolverKey(JSContext* context, bool reject)
{
    auto name = StringValue(context, reject
            ? "mini_ui.Promise.Resolver.reject"
            : "mini_ui.Promise.Resolver.resolve");
    return name.IsEmpty() ? Local<Private>()
                          : Private::ForApi(detail::IsolateFor(context), name);
}
Maybe<bool> Settle(Promise::Resolver* resolver, Local<Context> context, Local<Value> value, bool reject)
{
    JSContext* ctx = detail::JSContextOf(context);
    if (detail::IsolateOf(resolver) != detail::IsolateFor(ctx)) {
        Fail(ctx, "Promise resolver belongs to another runtime");
        return Nothing<bool>();
    }
    auto key = ResolverKey(ctx, reject);
    Local<Value> function;
    if (key.IsEmpty() || !resolver->GetPrivate(context, key).ToLocal(&function))
        return Nothing<bool>();
    if (!function->IsFunction()) {
        Fail(ctx, "Promise resolver has no resolving capability");
        return Nothing<bool>();
    }
    JSValueConst argument = detail::Borrow(*value);
    OwnedValue result(ctx, JS_Call(ctx, detail::Borrow(*function),
        JS_UNDEFINED, 1, &argument));
    if (JS_IsException(result.get())) {
        detail::CaptureException(detail::IsolateFor(ctx), ctx);
        return Nothing<bool>();
    }
    return Just(true);
}
} // namespace

namespace detail {
Local<Module> ModuleView(JSContext* context, JSModuleDef* module)
{
    std::shared_ptr<ScriptPayload> payload;
    auto found = module_payloads.find(module);
    if (found != module_payloads.end())
        payload = found->second.lock();
    if (!payload) {
        payload = std::make_shared<ScriptPayload>(context);
        auto* host = static_cast<ModuleHostData*>(JS_GetModuleHostData(module));
        if (!host) {
            auto native = std::make_unique<ModuleHostData>();
            native->info = std::make_shared<SourceInfo>();
            JSAtom name = JS_GetModuleName(context, module);
            OwnedValue text(context, JS_AtomToString(context, name));
            JS_FreeAtom(context, name);
            if (JS_IsException(text.get()) || !Text(context, text.get(), native->info->filename)) {
                detail::CaptureException(detail::IsolateFor(context), context);
                return {};
            }
            host = native.release();
            JS_SetModuleHostData(module, host, FreeModuleHostData);
        }
        payload->info = host->info;
        payload->synthetic = host->synthetic;
        payload->resource_name = JS_NewStringLen(context, host->info->filename.data(), host->info->filename.size());
        if (JS_IsException(payload->resource_name)) {
            payload->resource_name = JS_UNDEFINED;
            detail::CaptureException(detail::IsolateFor(context), context);
            return {};
        }
        module_payloads[module] = payload;
        RegisterCompiledSource(context, JS_MKPTR(JS_TAG_MODULE, module), payload->info);
    }
    return detail::MakeLocal<Module>(detail::IsolateFor(context), context,
        JS_DupValue(context, JS_MKPTR(JS_TAG_MODULE, module)), payload);
}
void ForgetScriptContext(JSContext* context)
{
    sources.erase(context);
    compiled_sources.erase(context);
    for (auto it = module_payloads.begin(); it != module_payloads.end();) {
        auto payload = it->second.lock();
        if (!payload || payload->context == context)
            it = module_payloads.erase(it);
        else
            ++it;
    }
}
bool ForwardPromiseRejection(JSContext* context, JSValueConst promise, JSValueConst reason,
    bool handled, PromiseRejectCallback reporter)
{
    Isolate* isolate = IsolateFor(context);
    if (!isolate || !reporter)
        return false;
    Isolate::Scope isolate_scope(isolate);
    HandleScope handles(isolate);
    auto realm = ContextForQuickJS(context);
    if (realm.IsEmpty())
        return false;
    Context::Scope entered(realm);
    if (!JS_IsPromise(promise))
        return Fail(context, "Rejection observer received a non-Promise value");
    auto promise_handle = Adopt(context, JS_DupValue(context, promise));
    auto reason_handle = Adopt(context, JS_DupValue(context, reason));
    if (promise_handle.IsEmpty() || reason_handle.IsEmpty())
        return false;
    reporter(PromiseRejectMessage(promise_handle.As<Promise>(),
        handled ? kPromiseHandlerAddedAfterReject : kPromiseRejectWithNoHandler, reason_handle));
    return true;
}
} // namespace detail

MaybeLocal<Script> ScriptCompiler::Compile(Local<Context> context, Source* source, CompileOptions options, NoCacheReason)
{
    JSContext* ctx = detail::JSContextOf(context);
    if (!CompileOptionsSupported(ctx, options))
        return {};
    auto payload = MakeSource(ctx, source->source_, source->origin_);
    if (!payload)
        return {};
    JSValue compiled = CompileCode(ctx, *payload->info, false);
    if (JS_IsException(compiled)) {
        detail::CaptureException(detail::IsolateFor(ctx), ctx);
        return {};
    }
    RegisterCompiledSource(ctx, compiled, payload->info);
    return detail::MakeLocal<Script>(detail::IsolateFor(ctx), ctx, compiled, payload);
}
MaybeLocal<UnboundScript> ScriptCompiler::CompileUnboundScript(Isolate* isolate, Source* source, CompileOptions options, NoCacheReason)
{
    JSContext* ctx = detail::CurrentJSContext(isolate);
    if (!CompileOptionsSupported(ctx, options))
        return {};
    auto payload = MakeSource(ctx, source->source_, source->origin_);
    if (!payload)
        return {};
    JSValue compiled = CompileCode(ctx, *payload->info, false);
    if (JS_IsException(compiled)) {
        detail::CaptureException(isolate, ctx);
        return {};
    }
    RegisterCompiledSource(ctx, compiled, payload->info);
    return detail::MakeLocal<UnboundScript>(isolate, ctx, compiled, payload);
}
MaybeLocal<Module> ScriptCompiler::CompileModule(Isolate* isolate, Source* source, CompileOptions options, NoCacheReason)
{
    JSContext* ctx = detail::CurrentJSContext(isolate);
    if (!CompileOptionsSupported(ctx, options))
        return {};
    auto payload = MakeSource(ctx, source->source_, source->origin_);
    if (!payload)
        return {};
    JSValue compiled = CompileCode(ctx, *payload->info, true);
    if (JS_IsException(compiled)) {
        detail::CaptureException(isolate, ctx);
        return {};
    }
    RegisterCompiledSource(ctx, compiled, payload->info);
    auto module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
    module_payloads[module] = payload;
    JS_SetModuleHostData(module, new ModuleHostData{payload->info, nullptr}, FreeModuleHostData);
    OwnedValue meta(ctx, JS_GetImportMeta(ctx, module));
    if (JS_IsException(meta.get()) || !Check(ctx, JS_DefinePropertyValueStr(ctx, meta.get(), "url",
            JS_NewString(ctx, payload->info->filename.c_str()), JS_PROP_C_W_E))) {
        JS_FreeValue(ctx, compiled);
        if (JS_IsException(meta.get()))
            detail::CaptureException(isolate, ctx);
        return {};
    }
    return detail::MakeLocal<Module>(isolate, ctx, compiled, payload);
}
MaybeLocal<Function> ScriptCompiler::CompileFunction(Local<Context> context, Source* source,
    size_t argument_count, Local<String> arguments[], size_t extension_count,
    Local<Object> extensions[], CompileOptions options, NoCacheReason)
{
    JSContext* ctx = detail::JSContextOf(context);
    if (!CompileOptionsSupported(ctx, options))
        return {};
    auto payload = MakeSource(ctx, source->source_, source->origin_, false);
    if (!payload)
        return {};
    std::vector<JSValue> names(argument_count), scopes(extension_count);
    for (size_t i = 0; i < argument_count; ++i)
        names[i] = detail::Borrow(*arguments[i]);
    for (size_t i = 0; i < extension_count; ++i)
        scopes[i] = detail::Borrow(*extensions[i]);
    const auto& info = *payload->info;
    JSValue function = JS_CompileFunction(ctx, info.source.c_str(), info.source.size(),
        info.filename.c_str(), info.line_offset, info.column_offset,
        argument_count, names.data(), extension_count, scopes.data());
    if (JS_IsException(function)) {
        detail::CaptureException(detail::IsolateFor(ctx), ctx);
        return {};
    }
    RegisterCompiledSource(ctx, function, payload->info);
    return detail::MakeLocal<Function>(detail::IsolateFor(ctx), ctx, function, payload);
}
MaybeLocal<Script> Script::Compile(Local<Context> context, Local<String> source, ScriptOrigin* origin)
{
    ScriptCompiler::Source input(source, origin ? *origin : ScriptOrigin({}));
    return ScriptCompiler::Compile(context, &input);
}
MaybeLocal<Value> Script::Run(Local<Context> context)
{
    JSContext* ctx = detail::ContextOf(this);
    if (ctx != detail::JSContextOf(context)) {
        Fail(detail::JSContextOf(context), "Script::Run requires the context to which the script is bound");
        return {};
    }
    Context::Scope scope(context);
    detail::ExecutionScope execution(detail::IsolateOf(this));
    return detail::Adopt(ctx, JS_EvalFunction(ctx, JS_DupValue(ctx, detail::Borrow(this))));
}
MaybeLocal<Value> Script::Run(Local<Context> context, Local<Data> host_options)
{
    auto payload = Payload<ScriptPayload>(this);
    detail::Cell* previous = payload->host_options;
    payload->host_options = host_options.IsEmpty() ? nullptr : detail::CellOf(*host_options);
    detail::Retain(payload->host_options);
    struct Restore {
        ScriptPayload& payload;
        detail::Cell* previous;
        ~Restore() { detail::Release(payload.host_options); payload.host_options = previous; }
    } restore{*payload, previous};
    return Run(context);
}
Local<UnboundScript> Script::GetUnboundScript()
{
    JSContext* ctx = detail::ContextOf(this);
    return detail::MakeLocal<UnboundScript>(detail::IsolateOf(this), ctx,
        JS_DupValue(ctx, detail::Borrow(this)), detail::Payload(this));
}
Local<Value> Script::GetResourceName()
{
    auto payload = Payload<ScriptPayload>(this);
    return detail::Adopt(detail::ContextOf(this), JS_DupValue(detail::ContextOf(this), payload->resource_name));
}
Local<Script> UnboundScript::BindToCurrentContext()
{
    JSContext* ctx = detail::CurrentJSContext(detail::IsolateOf(this));
    auto payload = Payload<ScriptPayload>(this);
    RegisterSource(ctx, payload->info);
    JSValue compiled = ctx == detail::ContextOf(this) ? JS_DupValue(ctx, detail::Borrow(this))
        : CompileCode(ctx, *payload->info, false);
    if (JS_IsException(compiled)) {
        detail::CaptureException(detail::IsolateOf(this), ctx);
        return {};
    }
    RegisterCompiledSource(ctx, compiled, payload->info);
    return detail::MakeLocal<Script>(detail::IsolateOf(this), ctx, compiled, payload);
}
int UnboundScript::GetId() const { return Payload<ScriptPayload>(this)->info->id; }
Local<Value> UnboundScript::GetScriptName()
{
    auto p = Payload<ScriptPayload>(this);
    return detail::Adopt(detail::ContextOf(this), JS_DupValue(detail::ContextOf(this), p->resource_name));
}
Local<Value> UnboundScript::GetSourceURL() { return OptionalStringValue(detail::ContextOf(this), Payload<ScriptPayload>(this)->info->source_url); }
Local<Value> UnboundScript::GetSourceMappingURL() { return OptionalStringValue(detail::ContextOf(this), Payload<ScriptPayload>(this)->info->source_map); }
int UnboundScript::GetLineNumber(int position) { return OffsetLocation(*Payload<ScriptPayload>(this)->info, position).GetLineNumber(); }
int UnboundScript::GetColumnNumber(int position) { return OffsetLocation(*Payload<ScriptPayload>(this)->info, position).GetColumnNumber(); }
Local<Value> UnboundModuleScript::GetSourceURL() { return OptionalStringValue(detail::ContextOf(this), Payload<ScriptPayload>(this)->info->source_url); }
Local<Value> UnboundModuleScript::GetSourceMappingURL() { return OptionalStringValue(detail::ContextOf(this), Payload<ScriptPayload>(this)->info->source_map); }
Local<Value> ScriptOrModule::GetResourceName()
{
    auto p = Payload<ScriptPayload>(this);
    return detail::Adopt(detail::ContextOf(this), JS_DupValue(detail::ContextOf(this), p->resource_name));
}
Local<Data> ScriptOrModule::HostDefinedOptions()
{
    auto p = Payload<ScriptPayload>(this);
    return p->host_options ? detail::Localize(detail::IsolateOf(this), p->host_options) : Local<Data>();
}

Module::Status Module::GetStatus() const { return static_cast<Status>(JS_GetModuleStatus(ModuleDef(this))); }
Local<Value> Module::GetException() const { return detail::Adopt(detail::ContextOf(this), JS_GetModuleException(detail::ContextOf(this), ModuleDef(this))); }
Local<FixedArray> Module::GetModuleRequests() const
{
    JSContext* ctx = detail::ContextOf(this);
    auto isolate = detail::IsolateOf(this);
    const int count = JS_GetModuleRequestCount(ModuleDef(this));
    if (count < 0) {
        detail::CaptureException(isolate, ctx);
        return {};
    }
    auto result = FixedArray::New(isolate, count);
    for (int i = 0; i < count; ++i) {
        auto payload = std::make_shared<RequestPayload>();
        OwnedValue name(ctx, JS_GetModuleRequestName(ctx, ModuleDef(this), i));
        if (JS_IsException(name.get()) || !Text(ctx, name.get(), payload->specifier)) {
            detail::CaptureException(isolate, ctx);
            return {};
        }
        payload->offset = JS_GetModuleRequestOffset(ModuleDef(this), i);
        result->Set(i, detail::MakeLocal<ModuleRequest>(isolate, detail::NeutralJSContext(isolate), JS_UNDEFINED, payload));
    }
    return result;
}
Location Module::SourceOffsetToLocation(int offset) const
{
    auto p = Payload<ScriptPayload>(this);
    return p ? OffsetLocation(*p->info, offset) : Location(-1, -1);
}
int Module::GetIdentityHash() const
{
    uintptr_t identity = reinterpret_cast<uintptr_t>(ModuleDef(this));
    identity ^= identity >> 17;
    return static_cast<int>((identity & 0x3fffffff) + 1);
}
Maybe<bool> Module::InstantiateModule(Local<Context> context, ResolveModuleCallback callback, ResolveSourceCallback source_callback)
{
    JSContext* ctx = detail::JSContextOf(context);
    if (ctx != detail::ContextOf(this) || source_callback) {
        Fail(ctx, source_callback ? "QuickJS does not implement source-phase module imports" : "Module belongs to another context");
        return Nothing<bool>();
    }
    ResolveState state{callback};
    if (!Check(ctx, JS_InstantiateModule(ctx, ModuleDef(this), callback ? ResolveModule : nullptr, &state)))
        return Nothing<bool>();
    return Just(true);
}
MaybeLocal<Value> Module::Evaluate(Local<Context> context)
{
    JSContext* ctx = detail::JSContextOf(context);
    if (ctx != detail::ContextOf(this) || GetStatus() < kInstantiated) {
        Fail(ctx, "Module must be instantiated in its context before evaluation");
        return {};
    }
    Context::Scope scope(context);
    detail::ExecutionScope execution(detail::IsolateOf(this));
    return detail::Adopt(ctx, JS_EvaluateModule(ctx, ModuleDef(this)));
}
Local<Value> Module::GetModuleNamespace() { return detail::Adopt(detail::ContextOf(this), JS_GetModuleNamespace(detail::ContextOf(this), ModuleDef(this))); }
Local<UnboundModuleScript> Module::GetUnboundModuleScript()
{
    return detail::MakeLocal<UnboundModuleScript>(detail::IsolateOf(this), detail::ContextOf(this),
        JS_DupValue(detail::ContextOf(this), detail::Borrow(this)), detail::Payload(this));
}
int Module::ScriptId() const { auto p = Payload<ScriptPayload>(this); return p ? p->info->id : 0; }
bool Module::IsGraphAsync() const { return JS_IsModuleGraphAsync(ModuleDef(this)); }
bool Module::HasTopLevelAwait() const { return JS_GetModuleTopLevelAwait(ModuleDef(this)); }
bool Module::IsSourceTextModule() const { auto p = Payload<ScriptPayload>(this); return !p || !p->synthetic; }
bool Module::IsSyntheticModule() const { return !IsSourceTextModule(); }
Local<String> ModuleRequest::GetSpecifier() const { return StringValue(detail::ContextOf(this), Payload<RequestPayload>(this)->specifier); }
int ModuleRequest::GetSourceOffset() const { return Payload<RequestPayload>(this)->offset; }
Local<FixedArray> ModuleRequest::GetImportAttributes() const { return FixedArray::New(detail::IsolateOf(this), 0); }

MaybeLocal<Promise::Resolver> Promise::Resolver::New(Local<Context> context)
{
    JSContext* ctx = detail::JSContextOf(context);
    JSValue raw_functions[2] = {JS_UNDEFINED, JS_UNDEFINED};
    JSValue promise = JS_NewPromiseCapability(ctx, raw_functions);
    OwnedValue functions[] = {{ctx, raw_functions[0]}, {ctx, raw_functions[1]}};
    if (JS_IsException(promise)) {
        detail::CaptureException(detail::IsolateFor(ctx), ctx);
        return {};
    }
    auto resolver = detail::MakeLocal<Resolver>(detail::IsolateFor(ctx), ctx, promise);
    // The real resolving pair is owned by the promise's private VM graph.
    // TracedReference copies retain its one-shot/thenable-adoption state without
    // keeping a separate native payload or an invisible strong JS/context root.
    for (int index = 0; index < 2; ++index) {
        auto key = ResolverKey(ctx, index == 1);
        if (key.IsEmpty())
            return {};
        JSAtom atom = JS_ValueToAtom(ctx, detail::Borrow(*key));
        if (!atom) {
            detail::CaptureException(detail::IsolateFor(ctx), ctx);
            return {};
        }
        int result = JS_DefinePropertyValue(ctx, detail::Borrow(*resolver), atom,
            functions[index].release(), 0);
        JS_FreeAtom(ctx, atom);
        if (!Check(ctx, result))
            return {};
    }
    return resolver;
}
Local<Promise> Promise::Resolver::GetPromise()
{
    return detail::MakeLocal<Promise>(detail::IsolateOf(this), detail::ContextOf(this),
        JS_DupValue(detail::ContextOf(this), detail::Borrow(this)));
}
Maybe<bool> Promise::Resolver::Resolve(Local<Context> context, Local<Value> value) { return Settle(this, context, value, false); }
Maybe<bool> Promise::Resolver::Reject(Local<Context> context, Local<Value> value) { return Settle(this, context, value, true); }
MaybeLocal<Promise> Promise::Then(Local<Context> context, Local<Function> fulfilled) { return Then(context, fulfilled, {}); }
MaybeLocal<Promise> Promise::Catch(Local<Context> context, Local<Function> rejected) { return Then(context, {}, rejected); }
MaybeLocal<Promise> Promise::Then(Local<Context> context, Local<Function> fulfilled, Local<Function> rejected)
{
    JSContext* ctx = detail::JSContextOf(context);
    auto value = detail::Adopt(ctx, JS_PromiseThen(ctx, detail::Borrow(this),
        fulfilled.IsEmpty() ? JS_UNDEFINED : detail::Borrow(*fulfilled),
        rejected.IsEmpty() ? JS_UNDEFINED : detail::Borrow(*rejected)));
    return value.IsEmpty() ? MaybeLocal<Promise>() : MaybeLocal<Promise>(value.As<Promise>());
}
bool Promise::HasHandler() const
{
    int result = JS_PromiseHasHandler(detail::ContextOf(this), detail::Borrow(this));
    return Check(detail::ContextOf(this), result) && result != 0;
}
Local<Value> Promise::Result() { return detail::Adopt(detail::ContextOf(this), JS_PromiseResult(detail::ContextOf(this), detail::Borrow(this))); }
Promise::PromiseState Promise::State() { return static_cast<PromiseState>(JS_PromiseState(detail::ContextOf(this), detail::Borrow(this))); }
void Promise::MarkAsHandled() { Check(detail::ContextOf(this), JS_MarkPromiseHandled(detail::ContextOf(this), detail::Borrow(this))); }
MaybeLocal<Value> JSON::Parse(Local<Context> context, Local<String> input)
{
    JSContext* ctx = detail::JSContextOf(context);
    std::string text;
    if (!Text(ctx, detail::Borrow(*input), text)) {
        detail::CaptureException(detail::IsolateFor(ctx), ctx);
        return {};
    }
    return detail::Adopt(ctx, JS_ParseJSON(ctx, text.c_str(), text.size(), "<JSON>"));
}
MaybeLocal<String> JSON::Stringify(Local<Context> context, Local<Value> value, Local<String> gap)
{
    JSContext* ctx = detail::JSContextOf(context);
    auto result = detail::Adopt(ctx, JS_JSONStringify(ctx, detail::Borrow(*value), JS_UNDEFINED,
        gap.IsEmpty() ? JS_UNDEFINED : detail::Borrow(*gap)));
    if (result.IsEmpty() || !JS_IsString(detail::Borrow(*result)))
        return {};
    return result.As<String>();
}

namespace {
int IntProperty(JSContext* context, JSValueConst object, const char* name, int fallback = 0)
{
    OwnedValue value(context, JS_GetPropertyStr(context, object, name));
    int result = fallback;
    if (JS_IsException(value.get())) {
        detail::CaptureException(detail::IsolateFor(context), context);
        return fallback;
    }
    if (!JS_IsUndefined(value.get()) && JS_ToInt32(context, &result, value.get()) < 0) {
        detail::CaptureException(detail::IsolateFor(context), context);
        return fallback;
    }
    return result;
}
bool BoolProperty(JSContext* context, JSValueConst object, const char* name)
{
    OwnedValue value(context, JS_GetPropertyStr(context, object, name));
    if (JS_IsException(value.get())) {
        detail::CaptureException(detail::IsolateFor(context), context);
        return false;
    }
    return JS_ToBool(context, value.get()) > 0;
}
Local<String> StringProperty(const Data* object, const char* name)
{
    JSContext* ctx = detail::ContextOf(object);
    auto value = detail::Adopt(ctx, JS_GetPropertyStr(ctx, detail::Borrow(object), name));
    if (value.IsEmpty() || !JS_IsString(detail::Borrow(*value)))
        return {};
    return value.As<String>();
}
std::shared_ptr<SourceInfo> FindSource(JSContext* context, const std::string& filename)
{
    auto realm = sources.find(context);
    if (realm == sources.end())
        return {};
    auto source = realm->second.find(filename);
    return source == realm->second.end() || source->second.ambiguous ? nullptr : source->second.info.lock();
}
// Only metadata registered for this exact engine compilation can describe a
// function. Dynamic eval/Function units never inherit their caller's origin.
std::shared_ptr<SourceInfo> FunctionSource(const Function* function, JSValue* filename = nullptr)
{
    JSContext* context = detail::ContextOf(function);
    if (filename && JS_GetFunctionLocation(context, detail::Borrow(function), filename, nullptr) < 0) {
        detail::CaptureException(detail::IsolateOf(function), context);
        return {};
    }
    const uint64_t id = JS_GetCompilationId(context, detail::Borrow(function));
    if (!id)
        return {};
    auto realm = compiled_sources.find(context);
    if (realm == compiled_sources.end())
        return {};
    auto source = realm->second.find(id);
    return source == realm->second.end() ? nullptr : source->second.lock();
}
std::shared_ptr<SourceInfo> FrameSource(const Data* frame)
{
    auto name = StringProperty(frame, "fileName");
    if (name.IsEmpty())
        return {};
    std::string filename;
    if (!Text(detail::ContextOf(frame), detail::Borrow(*name), filename)) {
        detail::CaptureException(detail::IsolateOf(frame), detail::ContextOf(frame));
        return {};
    }
    return FindSource(detail::ContextOf(frame), filename);
}
// These payloads are realm-neutral native snapshots. Their backing JS error,
// frame-array and frame-record values retain any actual realm via obj_ctx.
struct MessagePayload final : detail::DataPayload {
    detail::ExceptionMetadata metadata;
    std::shared_ptr<SourceInfo> source;
    int position = -1;
};
struct TracePayload final : detail::DataPayload {
    int id = next_script_id.fetch_add(1, std::memory_order_relaxed);
};
Local<StackTrace> Trace(JSContext* context, JSValue frames)
{
    if (JS_IsException(frames)) {
        detail::CaptureException(detail::IsolateFor(context), context);
        return {};
    }
    return detail::MakeLocal<StackTrace>(detail::IsolateFor(context), context, frames, std::make_shared<TracePayload>());
}
std::shared_ptr<MessagePayload> CaptureMessage(Isolate* isolate, JSContext* context, JSValueConst error,
    const detail::ExceptionMetadata* captured = nullptr)
{
    auto payload = std::make_shared<MessagePayload>();
    if (captured)
        payload->metadata = *captured;
    JSValue filename = JS_UNDEFINED;
    int line = -1, column = -1;
    if (JS_IsObject(error) && JS_GetErrorLocation(context, error, &filename, &line, &column) < 0) {
        JS_FreeValue(context, filename);
        detail::CaptureException(isolate, context);
        return {};
    }
    OwnedValue name(context, filename);
    if (!JS_IsUndefined(name.get()) && !Text(context, name.get(), payload->metadata.resource_name)) {
        detail::CaptureException(isolate, context);
        return {};
    }
    if (line > 0)
        payload->metadata.line_number = line;
    // QuickJS records -1 for unavailable source columns; never invent one.
    if (column > 0) {
        payload->metadata.start_column = column - 1;
        payload->metadata.end_column = column;
    } else {
        payload->metadata.start_column = -1;
        payload->metadata.end_column = -1;
    }
    payload->source = FindSource(context, payload->metadata.resource_name);
    if (payload->source && payload->metadata.line_number > 0) {
        const auto& source = *payload->source;
        int wanted = payload->metadata.line_number - source.line_offset - 1;
        size_t begin = 0;
        while (wanted > 0 && begin < source.source.size()) {
            size_t end = source.source.find_first_of("\r\n", begin);
            if (end == std::string::npos) {
                begin = source.source.size();
                break;
            }
            begin = end + 1;
            if (source.source[end] == '\r' && begin < source.source.size() && source.source[begin] == '\n')
                ++begin;
            --wanted;
        }
        if (wanted == 0) {
            size_t end = source.source.find_first_of("\r\n", begin);
            payload->metadata.source_line = source.source.substr(begin,
                end == std::string::npos ? end : end - begin);
        }
        // A byte offset would not be a valid V8 UTF-16 source position.
        if (column > 0) {
            int units = 0;
            for (size_t i = 0; i < begin;) {
                const unsigned char c = static_cast<unsigned char>(source.source[i]);
                size_t width = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
                if (width > begin - i)
                    width = 1;
                units += width == 4 ? 2 : 1;
                i += width;
            }
            payload->position = units + column - 1;
        }
    }
    return payload;
}
Local<Message> MakeMessage(Isolate* isolate, JSContext* context, JSValueConst error,
    const detail::ExceptionMetadata* captured = nullptr)
{
    auto payload = CaptureMessage(isolate, context, error, captured);
    return payload ? detail::MakeLocal<Message>(isolate, context, JS_DupValue(context, error), payload) : Local<Message>();
}
JSValue ConstructNative(JSContext* context, const char* name, int count, JSValueConst* arguments)
{
    OwnedValue constructor(context, JS_GetIntrinsic(context, name));
    if (JS_IsException(constructor.get()))
        return JS_EXCEPTION;
    return JS_CallConstructor(context, constructor.get(), count, arguments);
}
Local<Value> NativeError(const char* name, Local<String> message, Local<Value> options)
{
    // Message strings may live in NeutralJSContext; errors belong to the entered realm.
    JSContext* context = detail::CurrentJSContext(detail::IsolateOf(*message));
    return detail::Adopt(context, JS_NewNativeError(context, name, detail::Borrow(*message),
        options.IsEmpty() ? JS_UNDEFINED : detail::Borrow(*options)));
}

int EvaluateSynthetic(JSContext* context, JSModuleDef* module)
{
    auto* host = static_cast<ModuleHostData*>(JS_GetModuleHostData(module));
    if (!host || !host->synthetic) {
        JS_ThrowTypeError(context, "Synthetic module has no evaluation callback");
        return -1;
    }
    Local<Value> result;
    auto view = detail::ModuleView(context, module);
    if (view.IsEmpty() || !host->synthetic(detail::ContextForQuickJS(context), view).ToLocal(&result)) {
        if (detail::HasPendingException(detail::IsolateFor(context)))
            JS_Throw(context, detail::TakePendingException(detail::IsolateFor(context)));
        else
            JS_ThrowTypeError(context, "Synthetic module evaluation failed without an exception");
        return -1;
    }
    if (!JS_IsPromise(detail::Borrow(*result)) ||
        JS_PromiseState(context, detail::Borrow(*result)) == Promise::kPending) {
        JS_ThrowTypeError(context, "This QuickJS engine requires a synthetic module callback to return a settled Promise");
        return -1;
    }
    if (JS_PromiseState(context, detail::Borrow(*result)) == Promise::kRejected) {
        JS_MarkPromiseHandled(context, detail::Borrow(*result));
        JS_Throw(context, JS_PromiseResult(context, detail::Borrow(*result)));
        return -1;
    }
    return 0;
}
} // namespace

Local<Module> Module::CreateSyntheticModule(Isolate* isolate, Local<String> name,
    const MemorySpan<const Local<String>>& exports, SyntheticModuleEvaluationSteps evaluate)
{
    JSContext* context = detail::CurrentJSContext(isolate);
    std::string filename;
    if (!Text(context, detail::Borrow(*name), filename)) {
        detail::CaptureException(isolate, context);
        return {};
    }
    if (filename.find('\0') != std::string::npos || !evaluate) {
        Fail(context, "Invalid synthetic module name or evaluation callback");
        return {};
    }
    auto payload = std::make_shared<ScriptPayload>(context);
    payload->info = std::make_shared<SourceInfo>();
    payload->info->filename = filename;
    payload->resource_name = JS_DupValue(context, detail::Borrow(*name));
    payload->synthetic = evaluate;
    JSModuleDef* module = JS_NewCModule(context, filename.c_str(), EvaluateSynthetic);
    if (!module) {
        detail::CaptureException(isolate, context);
        return {};
    }
    // JS_NewCModule supplies the context-list ownership, not a caller reference.
    auto result = detail::MakeLocal<Module>(isolate, context,
        JS_DupValue(context, JS_MKPTR(JS_TAG_MODULE, module)), payload);
    module_payloads[module] = payload;
    JS_SetModuleHostData(module, new ModuleHostData{payload->info, evaluate}, FreeModuleHostData);
    for (auto export_name : exports) {
        std::string text;
        if (!Text(context, detail::Borrow(*export_name), text) ||
            text.find('\0') != std::string::npos) {
            if (text.find('\0') != std::string::npos)
                JS_ThrowTypeError(context, "Synthetic export name contains a null character");
            detail::CaptureException(isolate, context);
            return {};
        }
        if (!Check(context, JS_AddModuleExport(context, module, text.c_str())))
            return {};
    }
    return result;
}
Maybe<bool> Module::SetSyntheticModuleExport(Isolate* isolate, Local<String> name, Local<Value> value)
{
    JSContext* context = detail::ContextOf(this);
    if (!IsSyntheticModule() || isolate != detail::IsolateOf(this)) {
        Fail(context, "SetSyntheticModuleExport requires a synthetic module in this isolate");
        return Nothing<bool>();
    }
    std::string text;
    if (!Text(context, detail::Borrow(*name), text)) {
        detail::CaptureException(isolate, context);
        return Nothing<bool>();
    }
    if (text.find('\0') != std::string::npos) {
        Fail(context, "Synthetic export name contains a null character");
        return Nothing<bool>();
    }
    return Check(context, JS_SetSyntheticModuleExport(context, ModuleDef(this), text.c_str(),
        detail::Borrow(*value))) ? Just(true) : Nothing<bool>();
}

Local<Value> Exception::RangeError(Local<String> message, Local<Value> options) { return NativeError("RangeError", message, options); }
Local<Value> Exception::ReferenceError(Local<String> message, Local<Value> options) { return NativeError("ReferenceError", message, options); }
Local<Value> Exception::SyntaxError(Local<String> message, Local<Value> options) { return NativeError("SyntaxError", message, options); }
Local<Value> Exception::TypeError(Local<String> message, Local<Value> options) { return NativeError("TypeError", message, options); }
Local<Value> Exception::Error(Local<String> message, Local<Value> options) { return NativeError("Error", message, options); }
Local<Message> Exception::CreateMessage(Isolate* isolate, Local<Value> exception)
{
    return MakeMessage(isolate, detail::ContextOf(*exception), detail::Borrow(*exception));
}
Local<StackTrace> Exception::GetStackTrace(Local<Value> exception)
{
    JSContext* context = detail::ContextOf(*exception);
    if (!JS_IsObject(detail::Borrow(*exception)))
        return {};
    OwnedValue frames(context, JS_GetErrorStackFrames(context, detail::Borrow(*exception)));
    if (JS_IsUndefined(frames.get()) || JS_IsNull(frames.get()))
        return {};
    if (!JS_IsException(frames.get()) && IntProperty(context, frames.get(), "length") == 0)
        return {};
    return Trace(context, frames.release());
}
Maybe<bool> Exception::CaptureStackTrace(Local<Context> context, Local<Object> object)
{
    return Check(detail::JSContextOf(context), JS_CaptureStackTrace(detail::JSContextOf(context),
        detail::Borrow(*object))) ? Just(true) : Nothing<bool>();
}
Location StackFrame::GetLocation() const
{
    const int line = IntProperty(detail::ContextOf(this), detail::Borrow(this), "lineNumber");
    const int column = IntProperty(detail::ContextOf(this), detail::Borrow(this), "columnNumber");
    return {line > 0 ? line - 1 : -1, column > 0 ? column - 1 : -1};
}
int StackFrame::GetScriptId() const { auto source = FrameSource(this); return source ? source->id : 0; }
Local<String> StackFrame::GetScriptName() const { return StringProperty(this, "fileName"); }
Local<String> StackFrame::GetScriptNameOrSourceURL() const { return GetScriptName(); }
Local<String> StackFrame::GetScriptSource() const
{
    auto source = FrameSource(this);
    return source ? StringValue(detail::ContextOf(this), source->source) : Local<String>();
}
Local<String> StackFrame::GetScriptSourceMappingURL() const
{
    auto source = FrameSource(this);
    return source && !source->source_map.empty() ? StringValue(detail::ContextOf(this), source->source_map) : Local<String>();
}
Local<String> StackFrame::GetFunctionName() const { return StringProperty(this, "functionName"); }
bool StackFrame::IsEval() const { return BoolProperty(detail::ContextOf(this), detail::Borrow(this), "isEval"); }
bool StackFrame::IsConstructor() const { return BoolProperty(detail::ContextOf(this), detail::Borrow(this), "isConstructor"); }
bool StackFrame::IsUserJavaScript() const { return IntProperty(detail::ContextOf(this), detail::Borrow(this), "lineNumber") > 0; }
int StackTrace::GetID() const { return Payload<TracePayload>(this)->id; }
Local<StackFrame> StackTrace::GetFrame(Isolate* isolate, uint32_t index) const
{
    if (isolate != detail::IsolateOf(this) || index >= static_cast<uint32_t>(GetFrameCount()))
        return {};
    JSContext* context = detail::ContextOf(this);
    JSValue frame = JS_GetPropertyUint32(context, detail::Borrow(this), index);
    if (JS_IsException(frame)) {
        detail::CaptureException(isolate, context);
        return {};
    }
    return detail::MakeLocal<StackFrame>(isolate, context, frame);
}
int StackTrace::GetFrameCount() const { return IntProperty(detail::ContextOf(this), detail::Borrow(this), "length"); }
Local<StackTrace> StackTrace::CurrentStackTrace(Isolate* isolate, int limit, StackTraceOptions)
{
    JSContext* context = detail::CurrentJSContext(isolate);
    return Trace(context, JS_CaptureStackFrames(context, std::max(0, limit)));
}
Local<String> StackTrace::CurrentScriptNameOrSourceURL(Isolate* isolate)
{
    auto trace = CurrentStackTrace(isolate, std::numeric_limits<int>::max());
    if (trace.IsEmpty())
        return {};
    for (int i = 0; i < trace->GetFrameCount(); ++i) {
        auto name = trace->GetFrame(isolate, i)->GetScriptNameOrSourceURL();
        if (!name.IsEmpty() && name->Length())
            return name;
    }
    return {};
}
Local<String> Message::Get() const
{
    JSContext* context = detail::ContextOf(this);
    auto value = detail::Adopt(context, JS_ToString(context, detail::Borrow(this)));
    return value.IsEmpty() ? Local<String>() : value.As<String>();
}
Isolate* Message::GetIsolate() const { return detail::IsolateOf(this); }
MaybeLocal<String> Message::GetSource(Local<Context>) const
{
    auto payload = Payload<MessagePayload>(this);
    return payload->source ? MaybeLocal<String>(StringValue(detail::ContextOf(this), payload->source->source)) : MaybeLocal<String>();
}
MaybeLocal<String> Message::GetSourceLine(Local<Context>) const
{
    auto payload = Payload<MessagePayload>(this);
    return payload->metadata.line_number > 0 && (payload->source || !payload->metadata.source_line.empty()) ?
        MaybeLocal<String>(StringValue(detail::ContextOf(this), payload->metadata.source_line)) : MaybeLocal<String>();
}
ScriptOrigin Function::GetScriptOrigin() const
{
    JSContext* context = detail::ContextOf(this);
    JSValue filename = JS_UNDEFINED;
    auto source = FunctionSource(this, &filename);
    auto name = detail::Adopt(context, filename);
    return ScriptOrigin(name, source ? source->line_offset : 0,
        source ? source->column_offset : 0, source && source->options.IsSharedCrossOrigin(),
        source ? source->id : -1,
        source ? OptionalStringValue(context, source->source_map) : Local<Value>(),
        source && source->options.IsOpaque(), false, source && source->options.IsModule());
}
int Function::ScriptId() const
{
    auto source = FunctionSource(this);
    return source ? source->id : -1;
}
ScriptOrigin Message::GetScriptOrigin() const
{
    auto payload = Payload<MessagePayload>(this);
    auto source = payload->source;
    return ScriptOrigin(GetScriptResourceName(), source ? source->line_offset : 0,
        source ? source->column_offset : 0, source && source->options.IsSharedCrossOrigin(),
        source ? source->id : -1, source ? OptionalStringValue(detail::ContextOf(this), source->source_map) : Local<Value>(),
        source && source->options.IsOpaque(), false, source && source->options.IsModule());
}
Local<Value> Message::GetScriptResourceName() const
{
    auto& name = Payload<MessagePayload>(this)->metadata.resource_name;
    return name.empty() ? detail::Adopt(detail::ContextOf(this), JS_UNDEFINED) : StringValue(detail::ContextOf(this), name);
}
Local<StackTrace> Message::GetStackTrace() const
{
    return Exception::GetStackTrace(detail::Adopt(detail::ContextOf(this), JS_DupValue(detail::ContextOf(this), detail::Borrow(this))));
}
Maybe<int> Message::GetLineNumber(Local<Context>) const
{
    const int line = Payload<MessagePayload>(this)->metadata.line_number;
    return line > 0 ? Just(line) : Nothing<int>();
}
int Message::GetStartPosition() const { return Payload<MessagePayload>(this)->position; }
int Message::GetEndPosition() const { const int start = GetStartPosition(); return start < 0 ? -1 : start + 1; }
int Message::ErrorLevel() const { return 1 << 3; } // Isolate::kMessageError.
int Message::GetStartColumn() const { return Payload<MessagePayload>(this)->metadata.start_column; }
Maybe<int> Message::GetStartColumn(Local<Context>) const { return GetStartColumn() < 0 ? Nothing<int>() : Just(GetStartColumn()); }
int Message::GetEndColumn() const { return Payload<MessagePayload>(this)->metadata.end_column; }
Maybe<int> Message::GetEndColumn(Local<Context>) const { return GetEndColumn() < 0 ? Nothing<int>() : Just(GetEndColumn()); }
bool Message::IsSharedCrossOrigin() const { auto p = Payload<MessagePayload>(this); return p->source && p->source->options.IsSharedCrossOrigin(); }
bool Message::IsOpaque() const { auto p = Payload<MessagePayload>(this); return p->source && p->source->options.IsOpaque(); }
void Message::PrintCurrentStackTrace(Isolate* isolate, std::ostream& out)
{
    auto trace = StackTrace::CurrentStackTrace(isolate, std::numeric_limits<int>::max());
    if (trace.IsEmpty())
        return;
    JSContext* context = detail::CurrentJSContext(isolate);
    for (int i = 0; i < trace->GetFrameCount(); ++i) {
        auto frame = trace->GetFrame(isolate, i);
        auto name = frame->GetFunctionName();
        auto file = frame->GetScriptNameOrSourceURL();
        std::string function_name, filename;
        if ((!name.IsEmpty() && !Text(context, detail::Borrow(*name), function_name)) ||
            (!file.IsEmpty() && !Text(context, detail::Borrow(*file), filename))) {
            detail::CaptureException(isolate, context);
            return;
        }
        out << "    at " << function_name << " (" << filename << ':' << frame->GetLineNumber();
        if (frame->GetColumn() > 0)
            out << ':' << frame->GetColumn();
        out << ")\n";
    }
}

namespace {
bool SameException(JSContext* context, JSValueConst left, JSValueConst right)
{
    if (JS_IsValueEq(context, left, right, true))
        return true;
    if (!JS_IsNumber(left) || !JS_IsNumber(right))
        return false;
    double a = 0, b = 0;
    JS_ToFloat64(context, &a, left);
    JS_ToFloat64(context, &b, right);
    return std::isnan(a) && std::isnan(b);
}
}

TryCatch::TryCatch(Isolate* isolate) : isolate_(isolate) { detail::PushTryCatch(isolate, this); }
TryCatch::~TryCatch()
{
    if (!rethrow_)
        detail::PopTryCatch(isolate_, this);
    if (caught_ && !rethrow_ && detail::HasPendingException(isolate_) &&
        SameException(context_, detail::PeekPendingException(isolate_), exception_))
        detail::ClearPendingException(isolate_);
    if (context_) {
        JS_FreeValue(context_, exception_);
        JS_FreeContext(context_);
    }
}
void TryCatch::Capture(JSContext* context, JSValueConst exception, bool terminated)
{
    if (context_) {
        JS_FreeValue(context_, exception_);
        JS_FreeContext(context_);
    }
    context_ = JS_DupContext(context);
    exception_ = JS_DupValue(context, exception);
    metadata_ = detail::PendingExceptionMetadata(isolate_);
    caught_ = true;
    terminated_ = terminated;
    message_payload_ = capture_message_ ? CaptureMessage(isolate_, context, exception, &metadata_) : nullptr;
}
bool TryCatch::HasCaught() const { return caught_; }
bool TryCatch::CanContinue() const { return !terminated_; }
bool TryCatch::HasTerminated() const { return terminated_; }
Local<Value> TryCatch::Exception() const
{
    return caught_ ? detail::Adopt(context_, JS_DupValue(context_, exception_)) : Local<Value>();
}
Local<Value> TryCatch::ReThrow()
{
    if (!caught_)
        return {};
    auto value = Exception();
    detail::PopTryCatch(isolate_, this);
    rethrow_ = true;
    detail::SetPendingExceptionMetadata(isolate_, metadata_);
    detail::Throw(isolate_, context_, JS_DupValue(context_, exception_));
    return value;
}
MaybeLocal<Value> TryCatch::StackTrace(Local<Context> context, Local<Value> exception)
{
    JSContext* ctx = detail::JSContextOf(context);
    if (!JS_IsObject(detail::Borrow(*exception)))
        return {};
    auto stack = detail::Adopt(ctx, JS_GetPropertyStr(ctx, detail::Borrow(*exception), "stack"));
    return stack.IsEmpty() || JS_IsUndefined(detail::Borrow(*stack)) ? MaybeLocal<Value>() : MaybeLocal<Value>(stack);
}
MaybeLocal<Value> TryCatch::StackTrace(Local<Context> context) const
{
    return caught_ ? StackTrace(context, Exception()) : MaybeLocal<Value>();
}
Local<v8::Message> TryCatch::Message() const
{
    if (!caught_ || !capture_message_)
        return {};
    return message_payload_ ? detail::MakeLocal<v8::Message>(isolate_, context_,
        JS_DupValue(context_, exception_), message_payload_) : MakeMessage(isolate_, context_, exception_, &metadata_);
}
void TryCatch::Reset()
{
    if (caught_ && detail::HasPendingException(isolate_) &&
        SameException(context_, detail::PeekPendingException(isolate_), exception_))
        detail::ClearPendingException(isolate_);
    if (context_) {
        JS_FreeValue(context_, exception_);
        JS_FreeContext(context_);
    }
    context_ = nullptr;
    exception_ = JS_UNDEFINED;
    caught_ = terminated_ = false;
    metadata_ = {};
    message_payload_.reset();
}

namespace {
// This is a facade-owned, versioned structured-clone wire format. It is not
// QuickJS bytecode, a V8 snapshot, or V8's private serialization format.
constexpr uint32_t kCloneVersion = 1;
constexpr uint8_t kCloneHeader[] = {0xff, 1, 'Q', 'J', 'S', 'C'};
enum class CloneTag : uint8_t {
    Leaf, Reference, Object, Array, Map, Set, RegExp, Error, Buffer,
    Transfer, SharedBuffer, View, Host
};
constexpr uint32_t kDataViewKind = 0xff;
constexpr unsigned kCloneDepthLimit = 512;

struct RetainedValue {
    JSRuntime* runtime;
    JSValue value;
    RetainedValue(JSRuntime* rt, JSValueConst val) : runtime(rt), value(JS_DupValueRT(rt, val)) {}
    RetainedValue(RetainedValue&& other) noexcept : runtime(other.runtime), value(other.value) { other.value = JS_UNDEFINED; }
    RetainedValue& operator=(RetainedValue&& other) noexcept
    {
        if (this != &other) {
            JS_FreeValueRT(runtime, value);
            runtime = other.runtime; value = other.value; other.value = JS_UNDEFINED;
        }
        return *this;
    }
    ~RetainedValue() { JS_FreeValueRT(runtime, value); }
    RetainedValue(const RetainedValue&) = delete;
    RetainedValue& operator=(const RetainedValue&) = delete;
};
uint32_t ArrayLength(JSContext* context, JSValueConst value, bool& ok)
{
    OwnedValue length(context, JS_GetPropertyStr(context, value, "length"));
    uint32_t count = 0;
    ok = !JS_IsException(length.get()) && JS_ToUint32(context, &count, length.get()) >= 0;
    return count;
}
uint32_t ViewKind(JSValueConst value)
{
    for (uint32_t kind = JS_UINT8C_TYPED_ARRAY; kind <= JS_FLOAT64_TYPED_ARRAY; ++kind) {
        if (JS_IsWhatTypedArray(value, static_cast<JS_TYPED_ARRAY>(kind)))
            return kind;
    }
    return kDataViewKind;
}
bool NativeLeaf(JSValueConst value)
{
    switch (JS_GetBuiltinType(value)) {
    case JS_BUILTIN_DATE:
    case JS_BUILTIN_BOOLEAN_OBJECT:
    case JS_BUILTIN_NUMBER_OBJECT:
    case JS_BUILTIN_STRING_OBJECT:
    case JS_BUILTIN_BIGINT_OBJECT:
        return true;
    default:
        return false;
    }
}
} // namespace

struct ValueSerializer::PrivateData {
    PrivateData(Isolate* iso, Delegate* del) : isolate(iso), delegate(del) {}
    ~PrivateData()
    {
        if (data) {
            if (delegate) delegate->FreeBufferMemory(data);
            else std::free(data);
        }
    }
    Isolate* isolate;
    Delegate* delegate;
    uint8_t* data = nullptr;
    size_t size = 0, capacity = 0;
    bool failed = false, released = false, header = false;
    bool custom_host_checked = false, custom_host = false, views_as_host = false;
    JSContext* active_context = nullptr;
    const char* transfer_error = nullptr;
    unsigned depth = 0;
    std::unordered_map<const void*, uint32_t> identities;
    std::vector<RetainedValue> objects;
    std::unordered_map<uint32_t, RetainedValue> transfers;

    JSContext* Context() const { return active_context ? active_context : detail::CurrentJSContext(isolate); }
    bool Error(const char* message)
    {
        failed = true;
        if (detail::HasPendingException(isolate))
            return false;
        if (delegate) {
            auto text = StringValue(Context(), message);
            if (!text.IsEmpty())
                delegate->ThrowDataCloneError(text);
        }
        if (!detail::HasPendingException(isolate))
            Fail(Context(), message);
        return false;
    }
    bool EngineError()
    {
        failed = true;
        detail::CaptureException(isolate, Context());
        return false;
    }
    bool Append(const void* source, size_t length)
    {
        if (failed)
            return false;
        if (released)
            return Error("Serializer buffer has already been released");
        if (length > std::numeric_limits<size_t>::max() - size)
            return Error("Structured clone buffer is too large");
        const size_t needed = size + length;
        if (needed > capacity) {
            size_t desired = capacity > std::numeric_limits<size_t>::max() / 2 ? needed : std::max(needed, capacity ? capacity * 2 : size_t(256));
            size_t allocated = desired;
            void* resized = delegate ? delegate->ReallocateBufferMemory(data, desired, &allocated) : std::realloc(data, desired);
            if (!resized) {
                JS_ThrowOutOfMemory(Context());
                return EngineError();
            }
            data = static_cast<uint8_t*>(resized);
            capacity = allocated;
            if (capacity < needed)
                return Error("Serializer delegate returned an undersized allocation");
        }
        if (length)
            std::memcpy(data + size, source, length);
        size = needed;
        return true;
    }
    bool Tag(CloneTag tag) { const auto byte = static_cast<uint8_t>(tag); return Append(&byte, 1); }
    bool Uint(uint64_t value)
    {
        uint8_t bytes[10];
        size_t count = 0;
        do {
            bytes[count] = static_cast<uint8_t>(value & 0x7f);
            value >>= 7;
            if (value)
                bytes[count] |= 0x80;
            ++count;
        } while (value);
        return Append(bytes, count);
    }
    bool Leaf(JSValueConst value)
    {
        size_t length = 0;
        uint8_t* bytes = JS_WriteObject(Context(), &length, value, 0);
        if (!bytes)
            return EngineError();
        const bool result = Tag(CloneTag::Leaf) && Uint(length) && Append(bytes, length);
        js_free(Context(), bytes);
        return result;
    }
    bool Properties(JSValueConst value)
    {
        JSContext* context = Context();
        JSPropertyEnum* properties = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(context, &properties, &count, value, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            return EngineError();
        bool result = Uint(count);
        for (uint32_t i = 0; result && i < count; ++i) {
            JSPropertyDescriptor descriptor{};
            const int exists = JS_GetOwnProperty(context, &descriptor, value, properties[i].atom);
            if (exists < 0) { result = EngineError(); break; }
            const bool enumerable = exists && (descriptor.flags & JS_PROP_ENUMERABLE);
            JS_FreeValue(context, descriptor.value);
            JS_FreeValue(context, descriptor.getter);
            JS_FreeValue(context, descriptor.setter);
            if (!Uint(enumerable ? 1 : 0)) { result = false; break; }
            if (!enumerable)
                continue;
            OwnedValue key(context, JS_AtomToString(context, properties[i].atom));
            if (JS_IsException(key.get())) { result = EngineError(); break; }
            // [[Get]] is intentional: structured clone invokes enumerable getters.
            OwnedValue property(context, JS_GetProperty(context, value, properties[i].atom));
            if (JS_IsException(property.get())) { result = EngineError(); break; }
            result = Leaf(key.get()) && Value(property.get());
        }
        JS_FreePropEnum(context, properties, count);
        return result;
    }
    bool Value(JSValueConst value)
    {
        if (failed)
            return false;
        if (depth >= kCloneDepthLimit)
            return Error("Structured clone graph is too deeply nested");
        struct Depth { unsigned& value; explicit Depth(unsigned& v) : value(v) { ++value; } ~Depth() { --value; } } guard(depth);
        JSContext* context = Context();
        if (JS_IsSymbol(value))
            return Error("Symbols cannot be cloned");
        if (!JS_IsObject(value))
            return Leaf(value);
        if (JS_IsFunction(context, value) || JS_IsProxy(value))
            return Error("Functions and proxies cannot be cloned");
        const void* identity = JS_VALUE_GET_PTR(value);
        auto previous = identities.find(identity);
        if (previous != identities.end())
            return Tag(CloneTag::Reference) && Uint(previous->second);
        if (objects.size() >= std::numeric_limits<uint32_t>::max())
            return Error("Structured clone contains too many objects");
        const auto id = static_cast<uint32_t>(objects.size());
        identities.emplace(identity, id);
        objects.emplace_back(JS_GetRuntime(context), value);
        const bool intrinsic_buffer = JS_IsArrayBuffer(value) || JS_IsSharedArrayBuffer(value) ||
            (JS_IsArrayBufferView(value) && !views_as_host);
        bool host = !intrinsic_buffer && (detail::IsHostObject(context, value) ||
            (views_as_host && JS_IsArrayBufferView(value)));
        Local<Object> local;
        if (delegate && (!intrinsic_buffer || JS_IsSharedArrayBuffer(value))) {
            auto adopted = detail::Adopt(context, JS_DupValue(context, value));
            if (adopted.IsEmpty())
                return false;
            local = adopted.As<Object>();
        }
        if (delegate && !intrinsic_buffer) {
            if (!custom_host_checked) {
                custom_host = delegate->HasCustomHostObject(isolate);
                custom_host_checked = true;
            }
            if (custom_host) {
                bool answer = false;
                if (!delegate->IsHostObject(isolate, local.As<Object>()).To(&answer))
                    return Error("Host object classification failed");
                host = host || answer;
            }
        }
        if (host) {
            if (!delegate)
                return Error("Host object requires a structured clone delegate");
            if (!Tag(CloneTag::Host) || !Uint(id))
                return false;
            bool written = false;
            if (!delegate->WriteHostObject(isolate, local.As<Object>()).To(&written) || !written)
                return Error("Host object delegate could not serialize the object");
            return !failed && !detail::HasPendingException(isolate);
        }
        if (JS_IsSharedArrayBuffer(value)) {
            if (!delegate)
                return Error("SharedArrayBuffer requires a structured clone delegate");
            uint32_t shared_id = 0;
            if (!delegate->GetSharedArrayBufferId(isolate, local.As<SharedArrayBuffer>()).To(&shared_id))
                return Error("SharedArrayBuffer delegate did not supply an ID");
            return Tag(CloneTag::SharedBuffer) && Uint(id) && Uint(shared_id);
        }
        if (JS_IsArrayBuffer(value)) {
            if (JS_IsDetachedArrayBuffer(value))
                return Error("Detached ArrayBuffer cannot be cloned");
            for (const auto& transfer : transfers) {
                if (JS_IsValueEq(context, transfer.second.value, value, true))
                    return Tag(CloneTag::Transfer) && Uint(id) && Uint(transfer.first);
            }
            size_t length = 0;
            uint8_t* bytes = JS_GetArrayBuffer(context, &length, value);
            if (!bytes && length)
                return EngineError();
            return Tag(CloneTag::Buffer) && Uint(id) && Uint(length) && Append(bytes, length);
        }
        if (JS_IsArrayBufferView(value)) {
            size_t offset = 0, length = 0, element_size = 0;
            OwnedValue buffer(context, JS_GetArrayBufferViewInfo(context, value, &offset, &length, &element_size));
            if (JS_IsException(buffer.get()))
                return EngineError();
            return Tag(CloneTag::View) && Uint(id) && Uint(ViewKind(value)) && Uint(offset) &&
                Uint(length) && Value(buffer.get());
        }
        if (JS_IsMap(value) || JS_IsSet(value)) {
            const bool map = JS_IsMap(value);
            OwnedValue entries(context, map ? JS_GetMapEntries(context, value) : JS_GetSetValues(context, value));
            if (JS_IsException(entries.get()))
                return EngineError();
            bool ok = false;
            uint32_t count = ArrayLength(context, entries.get(), ok);
            if (!ok)
                return EngineError();
            if (!Tag(map ? CloneTag::Map : CloneTag::Set) || !Uint(id) || !Uint(map ? count / 2 : count))
                return false;
            for (uint32_t i = 0; i < count; ++i) {
                OwnedValue entry(context, JS_GetPropertyUint32(context, entries.get(), i));
                if (JS_IsException(entry.get()))
                    return EngineError();
                if (!Value(entry.get()))
                    return false;
            }
            return true;
        }
        if (JS_IsRegExp(value)) {
            OwnedValue source(context, JS_CallIntrinsic(context, "RegExp.prototype", "source", value, 0, nullptr));
            if (JS_IsException(source.get()))
                return EngineError();
            // Each captured getter reads the native RegExp slots. The generic
            // flags getter would instead consult overridable receiver properties.
            static constexpr struct { const char* getter; char flag; } flag_getters[] = {
                {"global", 'g'}, {"ignoreCase", 'i'}, {"multiline", 'm'},
                {"dotAll", 's'}, {"unicode", 'u'}, {"sticky", 'y'}
            };
            char characters[sizeof(flag_getters) / sizeof(flag_getters[0])];
            size_t length = 0;
            for (const auto& entry : flag_getters) {
                OwnedValue enabled(context, JS_CallIntrinsic(context, "RegExp.prototype", entry.getter, value, 0, nullptr));
                if (JS_IsException(enabled.get()))
                    return EngineError();
                if (JS_ToBool(context, enabled.get()) > 0)
                    characters[length++] = entry.flag;
            }
            OwnedValue flags(context, JS_NewStringLen(context, characters, length));
            if (JS_IsException(flags.get()))
                return EngineError();
            return Tag(CloneTag::RegExp) && Uint(id) && Leaf(source.get()) && Leaf(flags.get());
        }
        if (JS_IsError(context, value)) {
            if (!Tag(CloneTag::Error) || !Uint(id))
                return false;
            for (const char* key : {"name", "message", "stack"}) {
                OwnedValue property(context, JS_GetPropertyStr(context, value, key));
                if (JS_IsException(property.get()))
                    return EngineError();
                // Error name/message are string-converted by the HTML algorithm.
                if (!JS_IsUndefined(property.get()) && !JS_IsString(property.get())) {
                    OwnedValue text(context, JS_ToString(context, property.get()));
                    if (JS_IsException(text.get()))
                        return EngineError();
                    if (!Leaf(text.get()))
                        return false;
                } else if (!Leaf(property.get())) {
                    return false;
                }
            }
            JSAtom cause_key = JS_NewAtom(context, "cause");
            JSPropertyDescriptor descriptor{};
            int has_cause = JS_GetOwnProperty(context, &descriptor, value, cause_key);
            if (has_cause >= 0) {
                JS_FreeValue(context, descriptor.value);
                JS_FreeValue(context, descriptor.getter);
                JS_FreeValue(context, descriptor.setter);
            }
            JS_FreeAtom(context, cause_key);
            if (has_cause < 0)
                return EngineError();
            if (!Uint(has_cause ? 1 : 0))
                return false;
            if (has_cause) {
                OwnedValue cause(context, JS_GetPropertyStr(context, value, "cause"));
                if (JS_IsException(cause.get()))
                    return EngineError();
                return Value(cause.get());
            }
            return true;
        }
        if (NativeLeaf(value)) {
            // These native records contain no recursively cloned object slots.
            // Reserve their identity explicitly around the primitive-only QJS record.
            return Tag(CloneTag::Object) && Uint(id) && Uint(1) && Leaf(value);
        }
        int is_array = JS_IsArray(context, value);
        if (is_array < 0)
            return EngineError();
        if (is_array) {
            bool ok = false;
            uint32_t length = ArrayLength(context, value, ok);
            if (!ok)
                return EngineError();
            return Tag(CloneTag::Array) && Uint(id) && Uint(length) && Properties(value);
        }
        if (JS_GetBuiltinType(value) == JS_BUILTIN_OBJECT)
            return Tag(CloneTag::Object) && Uint(id) && Uint(0) && Properties(value);
        return Error("This JavaScript object type cannot be cloned");
    }
};

bool ValueSerializer::Delegate::HasCustomHostObject(Isolate*) { return false; }
Maybe<bool> ValueSerializer::Delegate::IsHostObject(Isolate* isolate, Local<Object> value)
{
    return Just(detail::IsHostObject(detail::CurrentJSContext(isolate), detail::Borrow(*value)));
}
Maybe<bool> ValueSerializer::Delegate::WriteHostObject(Isolate* isolate, Local<Object>)
{
    auto message = StringValue(detail::CurrentJSContext(isolate), "Host object is not serializable");
    if (!message.IsEmpty())
        ThrowDataCloneError(message);
    return Nothing<bool>();
}
Maybe<uint32_t> ValueSerializer::Delegate::GetSharedArrayBufferId(Isolate* isolate, Local<SharedArrayBuffer>)
{
    auto message = StringValue(detail::CurrentJSContext(isolate), "SharedArrayBuffer is not serializable without a delegate ID");
    if (!message.IsEmpty())
        ThrowDataCloneError(message);
    return Nothing<uint32_t>();
}
void* ValueSerializer::Delegate::ReallocateBufferMemory(void* old, size_t size, size_t* actual)
{
    void* data = std::realloc(old, size);
    if (data)
        *actual = size;
    return data;
}
void ValueSerializer::Delegate::FreeBufferMemory(void* data) { std::free(data); }
ValueSerializer::ValueSerializer(Isolate* isolate) : ValueSerializer(isolate, nullptr) {}
ValueSerializer::ValueSerializer(Isolate* isolate, Delegate* delegate)
    : private_(std::make_unique<PrivateData>(isolate, delegate)) {}
ValueSerializer::~ValueSerializer() = default;
void ValueSerializer::WriteHeader()
{
    if (private_->header) { private_->Error("Structured clone header was already written"); return; }
    private_->header = private_->Append(kCloneHeader, sizeof(kCloneHeader));
}
Maybe<bool> ValueSerializer::WriteValue(Local<Context> context, Local<Value> value)
{
    auto& state = *private_;
    JSContext* previous = state.active_context;
    state.active_context = detail::JSContextOf(context);
    struct Restore { PrivateData& state; JSContext* previous; ~Restore() { state.active_context = previous; } } restore{state, previous};
    if (detail::IsolateFor(state.active_context) != state.isolate || value.IsEmpty()) {
        state.Error("Structured clone value and context must belong to this isolate");
        return Nothing<bool>();
    }
    Context::Scope entered(context);
    if (state.transfer_error) {
        state.Error(state.transfer_error);
        return Nothing<bool>();
    }
    if (!state.Value(detail::Borrow(*value))) {
        state.failed = true;
        return Nothing<bool>();
    }
    return Just(true);
}
std::pair<uint8_t*, size_t> ValueSerializer::Release()
{
    auto& state = *private_;
    if (state.released) { state.Error("Serializer buffer has already been released"); return {nullptr, 0}; }
    state.released = true;
    uint8_t* data = std::exchange(state.data, nullptr);
    size_t size = std::exchange(state.size, 0);
    state.capacity = 0;
    return {data, size};
}
void ValueSerializer::TransferArrayBuffer(uint32_t id, Local<ArrayBuffer> buffer)
{
    auto& state = *private_;
    JSContext* context = detail::ContextOf(*buffer);
    if (state.failed || state.transfer_error)
        return;
    // Registration can precede the caller's TryCatch; report at WriteValue.
    if (detail::IsolateOf(*buffer) != state.isolate || !JS_IsArrayBuffer(detail::Borrow(*buffer)) ||
        JS_IsDetachedArrayBuffer(detail::Borrow(*buffer))) {
        state.transfer_error = "Invalid or detached transferred ArrayBuffer";
        return;
    }
    if (state.transfers.find(id) != state.transfers.end()) { state.transfer_error = "Duplicate ArrayBuffer transfer ID"; return; }
    for (const auto& entry : state.transfers) {
        if (JS_IsValueEq(context, entry.second.value, detail::Borrow(*buffer), true)) {
            state.transfer_error = "ArrayBuffer appears more than once in the transfer list";
            return;
        }
    }
    state.transfers.emplace(id, RetainedValue(JS_GetRuntime(context), detail::Borrow(*buffer)));
}
void ValueSerializer::SetTreatArrayBufferViewsAsHostObjects(bool mode)
{
    if (mode && !private_->delegate) { private_->Error("ArrayBufferView host serialization requires a delegate"); return; }
    private_->views_as_host = mode;
}
void ValueSerializer::WriteUint32(uint32_t value) { private_->Uint(value); }
void ValueSerializer::WriteUint64(uint64_t value) { private_->Uint(value); }
void ValueSerializer::WriteDouble(double value)
{
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    uint8_t bytes[8];
    for (size_t i = 0; i < sizeof(bytes); ++i)
        bytes[i] = static_cast<uint8_t>(bits >> (i * 8));
    private_->Append(bytes, sizeof(bytes));
}
void ValueSerializer::WriteRawBytes(const void* source, size_t length) { private_->Append(source, length); }

struct ValueDeserializer::PrivateData {
    PrivateData(Isolate* iso, const uint8_t* bytes, size_t length, Delegate* del)
        : isolate(iso), data(bytes), size(length), delegate(del) {}
    Isolate* isolate;
    const uint8_t* data;
    size_t size, position = 0;
    Delegate* delegate;
    JSContext* active_context = nullptr;
    bool failed = false, header = false, legacy = false;
    uint32_t version = 0;
    unsigned depth = 0;
    std::vector<RetainedValue> objects;
    std::unordered_map<uint32_t, RetainedValue> transfers, shared;

    JSContext* Context() const { return active_context ? active_context : detail::CurrentJSContext(isolate); }
    JSValue Error(const char* message)
    {
        failed = true;
        if (!detail::HasPendingException(isolate))
            Fail(Context(), message);
        return JS_EXCEPTION;
    }
    JSValue EngineError()
    {
        failed = true;
        detail::CaptureException(isolate, Context());
        return JS_EXCEPTION;
    }
    bool Bytes(size_t count, const void** result)
    {
        if (failed || position > size || count > size - position || (count && !data))
            return false;
        *result = data ? data + position : nullptr;
        position += count;
        return true;
    }
    bool Uint(uint64_t* value)
    {
        uint64_t result = 0;
        for (unsigned shift = 0; shift < 70; shift += 7) {
            const void* pointer;
            if (!Bytes(1, &pointer))
                return false;
            const uint8_t byte = *static_cast<const uint8_t*>(pointer);
            if (shift == 63 && (byte & 0xfe))
                return false;
            result |= uint64_t(byte & 0x7f) << shift;
            if (!(byte & 0x80)) {
                *value = result;
                return true;
            }
        }
        return false;
    }
    bool Uint32(uint32_t* value)
    {
        uint64_t result;
        if (!Uint(&result) || result > std::numeric_limits<uint32_t>::max())
            return false;
        *value = static_cast<uint32_t>(result);
        return true;
    }
    bool Reserve(uint32_t id)
    {
        if (id != objects.size()) {
            Error("Invalid structured clone object ID");
            return false;
        }
        objects.emplace_back(JS_GetRuntime(Context()), JS_UNDEFINED);
        return true;
    }
    JSValue Remember(uint32_t id, JSValue value)
    {
        if (JS_IsException(value))
            return EngineError();
        if (!JS_IsObject(value)) {
            JS_FreeValue(Context(), value);
            return Error("Clone object record did not produce an object");
        }
        objects[id] = RetainedValue(JS_GetRuntime(Context()), value);
        return value;
    }
    bool Properties(JSValueConst object)
    {
        uint32_t count;
        if (!Uint32(&count) || count > size - position) {
            Error("Invalid structured clone property count");
            return false;
        }
        JSContext* context = Context();
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t present;
            if (!Uint32(&present) || present > 1) { Error("Invalid clone property presence marker"); return false; }
            if (!present)
                continue;
            OwnedValue key(context, Value());
            if (JS_IsException(key.get()))
                return false;
            if (!JS_IsString(key.get())) { Error("Clone property key is not a string"); return false; }
            OwnedValue value(context, Value());
            if (JS_IsException(value.get()))
                return false;
            JSAtom atom = JS_ValueToAtom(context, key.get());
            if (atom == JS_ATOM_NULL) { EngineError(); return false; }
            // Define an own data property, never invoke __proto__ setters.
            int result = JS_DefinePropertyValue(context, object, atom,
                JS_DupValue(context, value.get()), JS_PROP_C_W_E | JS_PROP_THROW);
            JS_FreeAtom(context, atom);
            if (result < 0) { EngineError(); return false; }
        }
        return true;
    }
    JSValue Value()
    {
        if (failed)
            return JS_EXCEPTION;
        if (depth >= kCloneDepthLimit)
            return Error("Structured clone graph is too deeply nested");
        struct Depth { unsigned& value; explicit Depth(unsigned& v) : value(v) { ++value; } ~Depth() { --value; } } guard(depth);
        JSContext* context = Context();
        const void* raw;
        if (!Bytes(1, &raw))
            return Error("Truncated structured clone record");
        const auto tag = static_cast<CloneTag>(*static_cast<const uint8_t*>(raw));
        if (tag == CloneTag::Leaf) {
            uint64_t length;
            if (!Uint(&length) || length > size - position || !Bytes(static_cast<size_t>(length), &raw))
                return Error("Truncated structured clone primitive");
            JSValue result = JS_ReadObject(context, static_cast<const uint8_t*>(raw), static_cast<size_t>(length), 0);
            if (JS_IsException(result))
                return EngineError();
            if (JS_IsObject(result)) {
                if (!NativeLeaf(result)) {
                    JS_FreeValue(context, result);
                    return Error("Invalid object in primitive clone record");
                }
            }
            return result;
        }
        uint32_t id;
        if (!Uint32(&id))
            return Error("Invalid structured clone object ID");
        if (tag == CloneTag::Reference) {
            if (id >= objects.size() || !JS_IsObject(objects[id].value))
                return Error("Invalid or unresolved structured clone reference");
            return JS_DupValue(context, objects[id].value);
        }
        if (!Reserve(id))
            return JS_EXCEPTION;
        switch (tag) {
        case CloneTag::Object: {
            uint32_t native;
            if (!Uint32(&native) || native > 1)
                return Error("Invalid structured clone object kind");
            if (native)
                return Remember(id, Value());
            OwnedValue object(context, Remember(id, JS_NewObject(context)));
            if (JS_IsException(object.get()) || !Properties(object.get()))
                return JS_EXCEPTION;
            return object.release();
        }
        case CloneTag::Array: {
            uint32_t length;
            if (!Uint32(&length))
                return Error("Invalid structured clone array length");
            OwnedValue array(context, Remember(id, JS_NewArray(context)));
            if (JS_IsException(array.get()))
                return JS_EXCEPTION;
            if (JS_SetPropertyStr(context, array.get(), "length", JS_NewUint32(context, length)) < 0)
                return EngineError();
            if (!Properties(array.get()))
                return JS_EXCEPTION;
            return array.release();
        }
        case CloneTag::Map:
        case CloneTag::Set: {
            uint32_t count;
            if (!Uint32(&count) || count > size - position)
                return Error("Invalid structured clone collection length");
            const bool map = tag == CloneTag::Map;
            OwnedValue collection(context, Remember(id, map ? JS_NewMap(context) : JS_NewSet(context)));
            if (JS_IsException(collection.get()))
                return JS_EXCEPTION;
            for (uint32_t i = 0; i < count; ++i) {
                OwnedValue key(context, Value());
                if (JS_IsException(key.get()))
                    return JS_EXCEPTION;
                if (map) {
                    OwnedValue value(context, Value());
                    if (JS_IsException(value.get()))
                        return JS_EXCEPTION;
                    if (JS_MapSet(context, collection.get(), key.get(), value.get()) < 0)
                        return EngineError();
                } else if (JS_SetAdd(context, collection.get(), key.get()) < 0) {
                    return EngineError();
                }
            }
            return collection.release();
        }
        case CloneTag::RegExp: {
            OwnedValue source(context, Value());
            if (JS_IsException(source.get()))
                return JS_EXCEPTION;
            OwnedValue flags(context, Value());
            if (JS_IsException(flags.get()))
                return JS_EXCEPTION;
            if (!JS_IsString(source.get()) || !JS_IsString(flags.get()))
                return Error("Invalid RegExp clone record");
            JSValueConst arguments[] = {source.get(), flags.get()};
            return Remember(id, ConstructNative(context, "RegExp", 2, arguments));
        }
        case CloneTag::Error: {
            OwnedValue name(context, Value());
            if (JS_IsException(name.get()))
                return JS_EXCEPTION;
            OwnedValue message(context, Value());
            if (JS_IsException(message.get()))
                return JS_EXCEPTION;
            OwnedValue stack(context, Value());
            if (JS_IsException(stack.get()))
                return JS_EXCEPTION;
            if ((!JS_IsUndefined(name.get()) && !JS_IsString(name.get())) ||
                (!JS_IsUndefined(message.get()) && !JS_IsString(message.get())) ||
                (!JS_IsUndefined(stack.get()) && !JS_IsString(stack.get())))
                return Error("Invalid Error clone fields");
            std::string type = "Error";
            if (!JS_IsUndefined(name.get()) && !Text(context, name.get(), type))
                return EngineError();
            if (type != "Error" && type != "TypeError" && type != "RangeError" &&
                type != "ReferenceError" && type != "SyntaxError" &&
                type != "EvalError" && type != "URIError")
                type = "Error";
            OwnedValue error(context, Remember(id,
                JS_NewNativeError(context, type.c_str(), message.get(), JS_UNDEFINED)));
            if (JS_IsException(error.get()))
                return JS_EXCEPTION;
            if (!JS_IsUndefined(stack.get()) && JS_DefinePropertyValueStr(context, error.get(), "stack",
                    JS_DupValue(context, stack.get()), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_THROW) < 0)
                return EngineError();
            uint32_t cause;
            if (!Uint32(&cause) || cause > 1)
                return Error("Invalid Error cause marker");
            if (cause) {
                OwnedValue value(context, Value());
                if (JS_IsException(value.get()))
                    return JS_EXCEPTION;
                if (JS_DefinePropertyValueStr(context, error.get(), "cause", value.release(),
                        JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_THROW) < 0)
                    return EngineError();
            }
            return error.release();
        }
        case CloneTag::Buffer: {
            uint64_t length;
            if (!Uint(&length) || length > size - position || !Bytes(static_cast<size_t>(length), &raw))
                return Error("Truncated ArrayBuffer clone data");
            return Remember(id, JS_NewArrayBufferCopy(context, static_cast<const uint8_t*>(raw), static_cast<size_t>(length)));
        }
        case CloneTag::Transfer:
        case CloneTag::SharedBuffer: {
            uint32_t transfer;
            if (!Uint32(&transfer))
                return Error("Invalid transferred buffer ID");
            const bool is_shared = tag == CloneTag::SharedBuffer;
            auto& table = is_shared ? shared : transfers;
            auto found = table.find(transfer);
            if (found == table.end() && is_shared && delegate) {
                Local<SharedArrayBuffer> buffer;
                if (!delegate->GetSharedArrayBufferFromId(isolate, transfer).ToLocal(&buffer))
                    return Error("SharedArrayBuffer delegate did not supply the requested buffer");
                if (detail::IsolateOf(*buffer) != isolate || !JS_IsSharedArrayBuffer(detail::Borrow(*buffer)))
                    return Error("SharedArrayBuffer delegate returned an invalid buffer");
                found = table.emplace(transfer, RetainedValue(JS_GetRuntime(context), detail::Borrow(*buffer))).first;
            }
            if (found == table.end())
                return Error("Missing transferred ArrayBuffer");
            if ((!is_shared && (!JS_IsArrayBuffer(found->second.value) || JS_IsDetachedArrayBuffer(found->second.value))) ||
                (is_shared && !JS_IsSharedArrayBuffer(found->second.value)))
                return Error("Invalid transferred ArrayBuffer");
            return Remember(id, JS_DupValue(context, found->second.value));
        }
        case CloneTag::View: {
            uint32_t kind;
            uint64_t offset, length;
            if (!Uint32(&kind) || !Uint(&offset) || !Uint(&length) ||
                offset > std::numeric_limits<size_t>::max() || length > std::numeric_limits<size_t>::max())
                return Error("Invalid ArrayBufferView clone bounds");
            OwnedValue buffer(context, Value());
            if (JS_IsException(buffer.get()))
                return JS_EXCEPTION;
            if (!JS_IsArrayBuffer(buffer.get()) && !JS_IsSharedArrayBuffer(buffer.get()))
                return Error("ArrayBufferView clone requires an ArrayBuffer");
            size_t buffer_length = 0;
            uint8_t* bytes = JS_GetArrayBuffer(context, &buffer_length, buffer.get());
            if (!bytes && buffer_length)
                return EngineError();
            if (offset > buffer_length || length > buffer_length - static_cast<size_t>(offset))
                return Error("ArrayBufferView clone exceeds buffer bounds");
            if (kind == kDataViewKind) {
                JSValueConst arguments[] = {buffer.get(), JS_NewFloat64(context, static_cast<double>(offset)),
                    JS_NewFloat64(context, static_cast<double>(length))};
                return Remember(id, ConstructNative(context, "DataView", 3, arguments));
            }
            if (kind > JS_FLOAT64_TYPED_ARRAY)
                return Error("Unknown typed array clone kind");
            size_t element_size = 1;
            switch (static_cast<JS_TYPED_ARRAY>(kind)) {
            case JS_INT16_TYPED_ARRAY: case JS_UINT16_TYPED_ARRAY: element_size = 2; break;
            case JS_INT32_TYPED_ARRAY: case JS_UINT32_TYPED_ARRAY: case JS_FLOAT32_TYPED_ARRAY: element_size = 4; break;
            case JS_FLOAT64_TYPED_ARRAY:
#ifdef CONFIG_BIGNUM
            case JS_BIG_INT64_TYPED_ARRAY: case JS_BIG_UINT64_TYPED_ARRAY:
#endif
                element_size = 8; break;
            default: break;
            }
            if (offset % element_size || length % element_size)
                return Error("Misaligned typed array clone bounds");
            return Remember(id, JS_NewTypedArray(context, static_cast<JS_TYPED_ARRAY>(kind),
                buffer.get(), static_cast<size_t>(offset), static_cast<size_t>(length) / element_size));
        }
        case CloneTag::Host: {
            if (!delegate)
                return Error("Host clone record requires a delegate");
            Local<Object> object;
            if (!delegate->ReadHostObject(isolate).ToLocal(&object))
                return Error("Host object delegate could not deserialize the object");
            if (detail::IsolateOf(*object) != isolate || !JS_IsObject(detail::Borrow(*object)))
                return Error("Host object delegate returned an invalid object");
            return Remember(id, JS_DupValue(context, detail::Borrow(*object)));
        }
        default:
            return Error("Unknown structured clone record tag");
        }
    }
};

MaybeLocal<Object> ValueDeserializer::Delegate::ReadHostObject(Isolate* isolate)
{
    Fail(detail::CurrentJSContext(isolate), "Host object is not deserializable");
    return {};
}
MaybeLocal<SharedArrayBuffer> ValueDeserializer::Delegate::GetSharedArrayBufferFromId(Isolate* isolate, uint32_t)
{
    Fail(detail::CurrentJSContext(isolate), "SharedArrayBuffer delegate has no buffer for this ID");
    return {};
}
ValueDeserializer::ValueDeserializer(Isolate* isolate, const uint8_t* data, size_t size)
    : ValueDeserializer(isolate, data, size, nullptr) {}
ValueDeserializer::ValueDeserializer(Isolate* isolate, const uint8_t* data, size_t size, Delegate* delegate)
    : private_(std::make_unique<PrivateData>(isolate, data, size, delegate)) {}
ValueDeserializer::~ValueDeserializer() = default;
Maybe<bool> ValueDeserializer::ReadHeader(Local<Context> context)
{
    auto& state = *private_;
    state.active_context = detail::JSContextOf(context);
    if (state.header || detail::IsolateFor(state.active_context) != state.isolate) {
        state.Error("Invalid structured clone header context or repeated header");
        state.active_context = nullptr;
        return Nothing<bool>();
    }
    const void* data = nullptr;
    bool ok = state.Bytes(sizeof(kCloneHeader), &data) &&
        std::memcmp(data, kCloneHeader, sizeof(kCloneHeader)) == 0;
    if (!ok) {
        state.Error(state.legacy ? "Legacy V8 clone data is not supported by the QuickJS facade" : "Invalid QuickJS structured clone header");
        state.active_context = nullptr;
        return Nothing<bool>();
    }
    state.version = kCloneVersion;
    state.header = true;
    state.active_context = nullptr;
    return Just(true);
}
MaybeLocal<Value> ValueDeserializer::ReadValue(Local<Context> context)
{
    auto& state = *private_;
    JSContext* previous = state.active_context;
    state.active_context = detail::JSContextOf(context);
    struct Restore { PrivateData& state; JSContext* previous; ~Restore() { state.active_context = previous; } } restore{state, previous};
    if (detail::IsolateFor(state.active_context) != state.isolate) {
        state.Error("Structured clone context belongs to another isolate");
        return {};
    }
    Context::Scope entered(context);
    JSValue result = state.Value();
    if (JS_IsException(result))
        return {};
    return detail::Adopt(state.active_context, result);
}
void ValueDeserializer::TransferArrayBuffer(uint32_t id, Local<ArrayBuffer> buffer)
{
    auto& state = *private_;
    JSValueConst value = detail::Borrow(*buffer);
    if (state.failed)
        return;
    if (detail::IsolateOf(*buffer) != state.isolate || !JS_IsArrayBuffer(value) || JS_IsDetachedArrayBuffer(value)) {
        state.Error("Invalid or detached transferred ArrayBuffer"); return;
    }
    if (state.transfers.find(id) != state.transfers.end()) { state.Error("Duplicate transferred ArrayBuffer ID"); return; }
    state.transfers.emplace(id, RetainedValue(JS_GetRuntime(detail::ContextOf(*buffer)), value));
}
void ValueDeserializer::TransferSharedArrayBuffer(uint32_t id, Local<SharedArrayBuffer> buffer)
{
    auto& state = *private_;
    JSValueConst value = detail::Borrow(*buffer);
    if (state.failed)
        return;
    if (detail::IsolateOf(*buffer) != state.isolate || !JS_IsSharedArrayBuffer(value)) {
        state.Error("Invalid transferred SharedArrayBuffer"); return;
    }
    if (state.shared.find(id) != state.shared.end()) { state.Error("Duplicate SharedArrayBuffer ID"); return; }
    state.shared.emplace(id, RetainedValue(JS_GetRuntime(detail::ContextOf(*buffer)), value));
}
void ValueDeserializer::SetSupportsLegacyWireFormat(bool enabled)
{
    if (private_->header) { private_->Error("Legacy wire format must be configured before ReadHeader"); return; }
    private_->legacy = enabled;
}
uint32_t ValueDeserializer::GetWireFormatVersion() const { return private_->version; }
bool ValueDeserializer::ReadUint32(uint32_t* value) { return value && private_->Uint32(value); }
bool ValueDeserializer::ReadUint64(uint64_t* value) { return value && private_->Uint(value); }
bool ValueDeserializer::ReadDouble(double* value)
{
    const void* data;
    if (!value || !private_->Bytes(8, &data))
        return false;
    uint64_t bits = 0;
    auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < 8; ++i)
        bits |= uint64_t(bytes[i]) << (i * 8);
    std::memcpy(value, &bits, sizeof(bits));
    return true;
}
bool ValueDeserializer::ReadRawBytes(size_t length, const void** data) { return data && private_->Bytes(length, data); }

} // namespace v8
