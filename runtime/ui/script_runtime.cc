// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "runtime/ui/script_runtime.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef CONFIG_BIGNUM
#define CONFIG_BIGNUM 1
#endif
#include "third_party/quickjs/quickjs.h"

#include "runtime/ui/local_resources.h"

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

class ScopedJSValue final {
public:
    ScopedJSValue(JSContext* context, JSValue value)
        : context_(context)
        , value_(value)
    {
    }

    ~ScopedJSValue() { JS_FreeValue(context_, value_); }

    ScopedJSValue(const ScopedJSValue&) = delete;
    ScopedJSValue& operator=(const ScopedJSValue&) = delete;

    JSValueConst get() const { return value_; }
    JSValue release()
    {
        const JSValue value = value_;
        value_ = JS_UNDEFINED;
        return value;
    }

private:
    JSContext* context_;
    JSValue value_;
};

std::string ValueToString(JSContext* context, JSValueConst value, bool* succeeded)
{
    std::size_t length = 0;
    const char* characters = JS_ToCStringLen(context, &length, value);
    if (!characters) {
        *succeeded = false;
        return {};
    }
    try {
        std::string result(characters, length);
        JS_FreeCString(context, value, characters);
        *succeeded = true;
        return result;
    } catch (...) {
        JS_FreeCString(context, value, characters);
        throw;
    }
}

std::string DescribeValue(JSContext* context, JSValueConst value, bool include_stack)
{
    bool converted = false;
    std::string description = ValueToString(context, value, &converted);
    if (!converted) {
        ScopedJSValue conversion_exception(context, JS_GetException(context));
        bool exception_converted = false;
        description = ValueToString(
            context, conversion_exception.get(), &exception_converted);
        if (!exception_converted) {
            ScopedJSValue nested_exception(context, JS_GetException(context));
            static_cast<void>(nested_exception);
            description = "JavaScript value could not be converted to text";
        } else {
            description = "Exception while converting JavaScript error to text: "
                + description;
        }
    }

    if (!include_stack || !JS_IsError(context, value))
        return description;

    ScopedJSValue stack(context, JS_GetPropertyStr(context, value, "stack"));
    if (JS_IsException(stack.get())) {
        ScopedJSValue stack_exception(context, JS_GetException(context));
        const std::string stack_error =
            DescribeValue(context, stack_exception.get(), false);
        if (!stack_error.empty())
            description += "\nException while reading JavaScript stack: "
                + stack_error;
        return description;
    }

    if (!JS_IsUndefined(stack.get())) {
        bool stack_converted = false;
        std::string stack_text =
            ValueToString(context, stack.get(), &stack_converted);
        if (stack_converted && !stack_text.empty() && stack_text != description) {
            description.push_back('\n');
            description += stack_text;
        } else if (!stack_converted) {
            ScopedJSValue stack_conversion_exception(
                context, JS_GetException(context));
            const std::string stack_error = DescribeValue(
                context, stack_conversion_exception.get(), false);
            if (!stack_error.empty())
                description += "\nException while converting JavaScript stack: "
                    + stack_error;
        }
    }
    return description;
}

bool TakeException(JSContext* context, Error* error)
{
    ScopedJSValue exception(context, JS_GetException(context));
    std::string message = DescribeValue(context, exception.get(), true);
    if (message.empty())
        message = "JavaScript exception";
    return Fail(error, ErrorCode::script, std::move(message));
}

bool HasModuleExtension(std::string_view source_url)
{
    const std::size_t suffix = source_url.find_first_of("?#");
    if (suffix != std::string_view::npos)
        source_url = source_url.substr(0, suffix);
    if (source_url.size() < 4)
        return false;
    source_url.remove_prefix(source_url.size() - 4);
    return source_url[0] == '.'
        && (source_url[1] == 'm' || source_url[1] == 'M')
        && (source_url[2] == 'j' || source_url[2] == 'J')
        && (source_url[3] == 's' || source_url[3] == 'S');
}

bool SetImportMetaUrl(JSContext* context, JSModuleDef* module,
    const std::string& url)
{
    JSValue meta = JS_GetImportMeta(context, module);
    if (JS_IsException(meta))
        return false;
    JSValue url_value = JS_NewStringLen(context, url.data(), url.size());
    if (JS_IsException(url_value)) {
        JS_FreeValue(context, meta);
        return false;
    }
    const int result = JS_DefinePropertyValueStr(context, meta, "url",
        url_value, JS_PROP_C_W_E);
    JS_FreeValue(context, meta);
    return result >= 0;
}

struct TransparentStringHash {
    using is_transparent = void;

    std::size_t operator()(std::string_view value) const noexcept
    {
        return std::hash<std::string_view>()(value);
    }
};

// Owned by the JSContext, not the ScriptRealm handle: queued jobs and facade
// handles may keep an old document alive after navigation or View destruction.
// The runtime owns every context, so this back-pointer must not own the runtime.
struct RealmRecord {
    ScriptRuntimeState* state;
    std::shared_ptr<LocalResources> resources;
    std::unordered_map<std::string, std::string> resolved_names;
    std::unordered_map<std::string, Resource,
        TransparentStringHash, std::equal_to<>> pending_modules;
};

void FinalizeRealmRecord(JSRuntime*, void* opaque)
{
    delete static_cast<RealmRecord*>(opaque);
}

} // namespace

class ScriptRuntimeState final {
public:
    ScriptRuntimeState() = default;

    ~ScriptRuntimeState()
    {
        SetPromiseRejectionObserver(nullptr, nullptr);
        if (!runtime_)
            return;
        JS_SetHostPromiseRejectionTracker(runtime_, nullptr, nullptr);
        JS_SetModuleLoaderFunc(runtime_, nullptr, nullptr, nullptr);
        JS_SetExternalGCRootTracer(runtime_, nullptr, nullptr);
        for (const UnhandledRejection& rejection : unhandled_rejections_) {
            JS_FreeValueRT(runtime_, rejection.promise);
            JS_FreeValueRT(runtime_, rejection.reason);
            JS_FreeContext(rejection.context);
        }
        unhandled_rejections_.clear();
        JS_FreeRuntime(runtime_);
    }

    void SetPromiseRejectionObserver(
        ScriptRuntime::PromiseRejectionObserver observer, void* user_data) noexcept
    {
        rejection_observer_ = observer;
        rejection_observer_data_ = observer ? user_data : nullptr;
    }

    bool Initialize(Error* error)
    {
        runtime_ = JS_NewRuntime();
        if (!runtime_)
            return Fail(error, ErrorCode::internal, "Cannot create the QuickJS runtime");
        JS_SetRuntimeInfo(runtime_, "mini_ui");
        JS_SetCanBlock(runtime_, false);
        JS_SetModuleLoaderFunc(runtime_, &NormalizeModule, &LoadModule, this);
        JS_SetHostPromiseRejectionTracker(runtime_, &TrackPromiseRejection, this);
        return true;
    }

    JSRuntime* runtime() const { return runtime_; }

    bool DrainJobs(Error* error)
    {
        for (;;) {
            JSContext* job_context = nullptr;
            const int result = JS_ExecutePendingJob(
                runtime_, &job_context, nullptr, nullptr);
            if (result > 0)
                continue;
            if (result < 0) {
                if (job_context)
                    return TakeException(job_context, error);
                return Fail(error, ErrorCode::script,
                    "A QuickJS pending job failed after its realm was destroyed");
            }
            break;
        }

        if (lost_unhandled_rejection_) {
            lost_unhandled_rejection_ = false;
            return Fail(error, ErrorCode::script,
                "An unhandled Promise rejection could not be retained");
        }
        if (unhandled_rejections_.empty())
            return true;

        std::string message;
        try {
            for (const UnhandledRejection& rejection : unhandled_rejections_) {
                if (!message.empty())
                    message.push_back('\n');
                message += "Unhandled Promise rejection: ";
                message += DescribeValue(rejection.context, rejection.reason, true);
            }
        } catch (const std::bad_alloc&) {
            message = "One or more unhandled Promise rejections occurred";
        }
        ClearUnhandledRejections();
        return Fail(error, ErrorCode::script, std::move(message));
    }

    void ClearRejectionsForContext(JSContext* context)
    {
        auto iterator = unhandled_rejections_.begin();
        while (iterator != unhandled_rejections_.end()) {
            if (iterator->context != context) {
                ++iterator;
                continue;
            }
            JS_FreeValue(iterator->context, iterator->promise);
            JS_FreeValue(iterator->context, iterator->reason);
            JS_FreeContext(iterator->context);
            iterator = unhandled_rejections_.erase(iterator);
        }
    }

private:
    struct UnhandledRejection {
        JSContext* context;
        JSValue promise;
        JSValue reason;
    };

    static char* NormalizeModule(JSContext* context, const char* base_name,
        const char* module_name, void* opaque)
    {
        auto* state = static_cast<ScriptRuntimeState*>(opaque);
        try {
            return state->NormalizeModuleImpl(context,
                base_name ? std::string_view(base_name) : std::string_view(),
                module_name ? std::string_view(module_name) : std::string_view());
        } catch (const std::bad_alloc&) {
            JS_ThrowOutOfMemory(context);
        } catch (...) {
            JS_ThrowInternalError(context,
                "Unexpected failure while resolving a local ES module");
        }
        return nullptr;
    }

    char* NormalizeModuleImpl(JSContext* context, std::string_view base_name,
        std::string_view module_name)
    {
        RealmRecord* realm = GetRealm(context);
        if (!realm)
            return nullptr;
        std::string resolution_key;
        resolution_key.reserve(base_name.size() + module_name.size() + 1);
        resolution_key.append(base_name);
        resolution_key.push_back('\n');
        resolution_key.append(module_name);

        auto cached = realm->resolved_names.find(resolution_key);
        if (cached != realm->resolved_names.end())
            return CopyModuleName(context, cached->second);

        Resource resource;
        Error resource_error;
        if (!realm->resources->Read(module_name, base_name, &resource, &resource_error)) {
            JS_ThrowReferenceError(context, "Cannot resolve local ES module '%.*s': %s",
                static_cast<int>(std::min<std::size_t>(
                    module_name.size(), static_cast<std::size_t>(0x7fffffff))),
                module_name.data(), resource_error.message.c_str());
            return nullptr;
        }

        std::string normalized_name = resource.url;
        realm->resolved_names.emplace(std::move(resolution_key), normalized_name);
        realm->pending_modules.try_emplace(normalized_name, std::move(resource));
        return CopyModuleName(context, normalized_name);
    }

    static char* CopyModuleName(JSContext* context, const std::string& name)
    {
        char* result = static_cast<char*>(js_malloc(context, name.size() + 1));
        if (!result)
            return nullptr;
        std::memcpy(result, name.data(), name.size());
        result[name.size()] = '\0';
        return result;
    }

    static JSModuleDef* LoadModule(JSContext* context, const char* module_name,
        void* opaque)
    {
        auto* state = static_cast<ScriptRuntimeState*>(opaque);
        try {
            return state->LoadModuleImpl(context,
                module_name ? std::string_view(module_name) : std::string_view());
        } catch (const std::bad_alloc&) {
            JS_ThrowOutOfMemory(context);
        } catch (...) {
            JS_ThrowInternalError(context,
                "Unexpected failure while loading a local ES module");
        }
        return nullptr;
    }

    JSModuleDef* LoadModuleImpl(JSContext* context, std::string_view module_name)
    {
        RealmRecord* realm = GetRealm(context);
        if (!realm)
            return nullptr;
        Resource resource;
        auto pending = realm->pending_modules.find(module_name);
        if (pending != realm->pending_modules.end()) {
            resource = std::move(pending->second);
            realm->pending_modules.erase(pending);
        } else {
            Error resource_error;
            if (!realm->resources->Read(module_name, {}, &resource, &resource_error)) {
                JS_ThrowReferenceError(context,
                    "Cannot load local ES module '%.*s': %s",
                    static_cast<int>(std::min<std::size_t>(
                        module_name.size(), static_cast<std::size_t>(0x7fffffff))),
                    module_name.data(), resource_error.message.c_str());
                return nullptr;
            }
        }

        const std::size_t source_size = resource.bytes.size();
        resource.bytes.push_back(0);
        const char* source =
            reinterpret_cast<const char*>(resource.bytes.data());
        JSValue compiled = JS_Eval(context, source, source_size,
            resource.url.c_str(), JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled))
            return nullptr;

        JSModuleDef* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
        if (!SetImportMetaUrl(context, module, resource.url)) {
            JS_FreeValue(context, compiled);
            return nullptr;
        }
        JS_FreeValue(context, compiled);
        return module;
    }

    static void TrackPromiseRejection(JSContext* context,
        JSValueConst promise, JSValueConst reason, JS_BOOL is_handled,
        void* opaque)
    {
        auto* state = static_cast<ScriptRuntimeState*>(opaque);
        if (is_handled) {
            auto iterator = std::find_if(state->unhandled_rejections_.begin(),
                state->unhandled_rejections_.end(),
                [promise](const UnhandledRejection& entry) {
                    return JS_VALUE_IS_EQ(entry.promise, promise);
                });
            if (iterator != state->unhandled_rejections_.end()) {
                JS_FreeValue(iterator->context, iterator->promise);
                JS_FreeValue(iterator->context, iterator->reason);
                JS_FreeContext(iterator->context);
                state->unhandled_rejections_.erase(iterator);
            }
            state->NotifyPromiseRejection(context, promise, reason, true);
            return;
        }

        UnhandledRejection rejection {
            JS_DupContext(context),
            JS_DupValue(context, promise),
            JS_DupValue(context, reason),
        };
        try {
            state->unhandled_rejections_.push_back(rejection);
        } catch (...) {
            JS_FreeValue(context, rejection.promise);
            JS_FreeValue(context, rejection.reason);
            JS_FreeContext(rejection.context);
            state->lost_unhandled_rejection_ = true;
        }
        state->NotifyPromiseRejection(context, promise, reason, false);
    }

    void NotifyPromiseRejection(JSContext* context, JSValueConst promise,
        JSValueConst reason, bool handled) noexcept
    {
        if (rejection_observer_)
            rejection_observer_(context, promise, reason, handled, rejection_observer_data_);
    }

    void ClearUnhandledRejections()
    {
        for (const UnhandledRejection& rejection : unhandled_rejections_) {
            JS_FreeValue(rejection.context, rejection.promise);
            JS_FreeValue(rejection.context, rejection.reason);
            JS_FreeContext(rejection.context);
        }
        unhandled_rejections_.clear();
    }

    RealmRecord* GetRealm(JSContext* context)
    {
        auto* realm = static_cast<RealmRecord*>(JS_GetContextOpaque(context));
        if (!realm || realm->state != this || !realm->resources) {
            JS_ThrowInternalError(context, "Local ES module has no document resource capability");
            return nullptr;
        }
        return realm;
    }

    JSRuntime* runtime_ = nullptr;
    ScriptRuntime::PromiseRejectionObserver rejection_observer_ = nullptr;
    void* rejection_observer_data_ = nullptr;
    std::vector<UnhandledRejection> unhandled_rejections_;
    bool lost_unhandled_rejection_ = false;
};

ScriptRuntime::ScriptRuntime(std::shared_ptr<ScriptRuntimeState> state)
    : state_(std::move(state))
{
}

ScriptRuntime::~ScriptRuntime()
{
    state_->SetPromiseRejectionObserver(nullptr, nullptr);
}

std::unique_ptr<ScriptRuntime> ScriptRuntime::Create(Error* error)
{
    ClearError(error);
    try {
        auto state = std::make_shared<ScriptRuntimeState>();
        if (!state->Initialize(error))
            return nullptr;
        return std::unique_ptr<ScriptRuntime>(new ScriptRuntime(std::move(state)));
    } catch (const std::bad_alloc&) {
        Fail(error, ErrorCode::internal,
            "Not enough memory to create the QuickJS runtime");
        return nullptr;
    }
}

JSRuntime* ScriptRuntime::get() const
{
    return state_->runtime();
}

void ScriptRuntime::SetPromiseRejectionObserver(
    PromiseRejectionObserver observer, void* user_data) noexcept
{
    state_->SetPromiseRejectionObserver(observer, user_data);
}

bool ScriptRuntime::DrainJobs(Error* error)
{
    ClearError(error);
    try {
        return state_->DrainJobs(error);
    } catch (const std::bad_alloc&) {
        return Fail(error, ErrorCode::internal,
            "Not enough memory to drain QuickJS pending jobs");
    }
}

ScriptRealm::ScriptRealm(std::shared_ptr<ScriptRuntimeState> state,
    JSContext* context)
    : state_(std::move(state))
    , context_(context)
{
}

ScriptRealm::~ScriptRealm()
{
    if (!context_)
        return;
    state_->ClearRejectionsForContext(context_);
    JS_FreeContext(context_);
}

std::unique_ptr<ScriptRealm> ScriptRealm::Create(
    ScriptRuntime& runtime, std::shared_ptr<LocalResources> resources, Error* error)
{
    ClearError(error);
    if (!resources) {
        Fail(error, ErrorCode::invalid_argument, "A script realm requires local resources");
        return nullptr;
    }
    JSContext* context = JS_NewContext(runtime.get());
    if (!context) {
        Fail(error, ErrorCode::internal, "Cannot create a QuickJS realm");
        return nullptr;
    }
    try {
        auto record = std::make_unique<RealmRecord>();
        record->state = runtime.state_.get();
        record->resources = std::move(resources);
        JS_SetContextOpaque(context, record.get());
        JS_SetContextOpaqueFinalizer(context, &FinalizeRealmRecord);
        record.release();
        return std::unique_ptr<ScriptRealm>(
            new ScriptRealm(runtime.state_, context));
    } catch (const std::bad_alloc&) {
        JS_FreeContext(context);
        Fail(error, ErrorCode::internal,
            "Not enough memory to create a QuickJS realm");
        return nullptr;
    }
}

JSContext* ScriptRealm::get() const
{
    return context_;
}

std::shared_ptr<LocalResources> ScriptRealm::local_resources() const
{
    auto* record = static_cast<RealmRecord*>(JS_GetContextOpaque(context_));
    if (!record || record->state != state_.get())
        return {};
    return record->resources;
}

bool ScriptRealm::Evaluate(std::string_view source,
    std::string_view source_url, std::string* json_result, Error* error)
{
    return EvaluateInternal(source, source_url, false, json_result, error);
}

bool ScriptRealm::EvaluateModule(std::string_view source,
    std::string_view source_url, std::string* json_result, Error* error)
{
    return EvaluateInternal(source, source_url, true, json_result, error);
}

bool ScriptRealm::EvaluateInternal(std::string_view source,
    std::string_view source_url, bool force_module,
    std::string* json_result, Error* error)
{
    ClearError(error);
    if (json_result)
        json_result->clear();
    if (source_url.find('\0') != std::string_view::npos)
        return Fail(error, ErrorCode::invalid_argument,
            "JavaScript source URL contains a null character");

    try {
        std::string source_copy(source);
        std::string filename = source_url.empty()
            ? std::string("<eval>") : std::string(source_url);
        const bool is_module = force_module || HasModuleExtension(source_url)
            || JS_DetectModule(source_copy.c_str(), source_copy.size());

        JSValue result_value;
        if (is_module) {
            ScopedJSValue compiled(context_,
                JS_Eval(context_, source_copy.data(), source_copy.size(),
                    filename.c_str(),
                    JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY));
            if (JS_IsException(compiled.get()))
                return TakeException(context_, error);

            JSModuleDef* module = static_cast<JSModuleDef*>(
                JS_VALUE_GET_PTR(compiled.get()));
            if (!SetImportMetaUrl(context_, module, filename))
                return TakeException(context_, error);
            result_value = JS_EvalFunction(context_, compiled.release());
        } else {
            result_value = JS_Eval(context_, source_copy.data(),
                source_copy.size(), filename.c_str(), JS_EVAL_TYPE_GLOBAL);
        }

        ScopedJSValue result(context_, result_value);
        if (JS_IsException(result.get()))
            return TakeException(context_, error);
        if (!state_->DrainJobs(error))
            return false;

        if (json_result) {
            ScopedJSValue json(context_, JS_JSONStringify(
                context_, result.get(), JS_UNDEFINED, JS_UNDEFINED));
            if (JS_IsException(json.get()))
                return TakeException(context_, error);
            if (!JS_IsUndefined(json.get())) {
                bool converted = false;
                std::string text =
                    ValueToString(context_, json.get(), &converted);
                if (!converted)
                    return TakeException(context_, error);
                *json_result = std::move(text);
            }
        }
        if (json_result && !state_->DrainJobs(error)) {
            json_result->clear();
            return false;
        }
        return true;
    } catch (const std::bad_alloc&) {
        return Fail(error, ErrorCode::internal,
            "Not enough memory to evaluate JavaScript");
    }
}

} // namespace mini_ui
