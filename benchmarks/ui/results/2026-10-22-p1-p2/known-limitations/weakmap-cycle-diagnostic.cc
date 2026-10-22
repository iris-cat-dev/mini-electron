// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "mini_ui/ui.h"
#include "runtime/ui/quickjs_bindings.h"
#include "runtime/ui/quickjs_v8_facade.h"
#include "runtime/ui/quickjs_v8_internal.h"
#include "third_party/blink/renderer/platform/bindings/script_wrappable.h"
#include "third_party/blink/renderer/platform/heap/thread_state.h"
#include "v8/include/cppgc/allocation.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/persistent.h"
#include "v8/include/cppgc/visitor.h"

namespace {

class Checks {
public:
    void Expect(bool condition, const char* message)
    {
        if (condition)
            return;
        ++failures_;
        std::fprintf(stderr, "FAIL runtime_checks: %s\n", message);
        std::fflush(stderr);
    }

    int failures() const { return failures_; }

private:
    int failures_ = 0;
};

Checks* g_checks = nullptr;
int g_deleted_getter_calls = 0;
int g_redefined_getter_calls = 0;
int g_lazy_reentrant_calls = 0;
int g_lazy_replacement_calls = 0;
int g_clone_lazy_calls = 0;
int g_named_calls = 0;
bool g_function_observed_busy = false;
bool g_microtask_observed_busy = false;
std::vector<int> g_handler_events;
int g_native_setter_data = -1;
int g_native_setter_value = -1;

v8::Local<v8::String> String(v8::Isolate* isolate, const char* text)
{
    return v8::String::NewFromUtf8(isolate, text).ToLocalChecked();
}

v8::Local<v8::Integer> Integer(v8::Isolate* isolate, int value)
{
    return v8::Integer::New(isolate, value);
}

bool IsInt(v8::MaybeLocal<v8::Value> maybe, v8::Local<v8::Context> context,
    int expected)
{
    v8::Local<v8::Value> value;
    if (!maybe.ToLocal(&value))
        return false;
    auto number = value->Int32Value(context);
    return number.IsJust() && number.FromJust() == expected;
}

bool IsUndefined(v8::MaybeLocal<v8::Value> maybe)
{
    v8::Local<v8::Value> value;
    return maybe.ToLocal(&value) && value->IsUndefined();
}

std::string NameText(v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info)
{
    if (!name->IsString()) {
        g_checks->Expect(false, "a string-only handler received a symbol");
        return {};
    }
    v8::String::Utf8Value text(info.GetIsolate(), name);
    if (!*text) {
        g_checks->Expect(false, "property name converted to UTF-8");
        return {};
    }
    return std::string(*text, static_cast<std::size_t>(text.length()));
}

void DataGetter(v8::Local<v8::Name>,
    const v8::PropertyCallbackInfo<v8::Value>& info)
{
    info.GetReturnValue().Set(info.Data());
}

void DataSetter(v8::Local<v8::Name>, v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<void>& info)
{
    auto context = info.GetIsolate()->GetCurrentContext();
    g_native_setter_data = info.Data()->Int32Value(context).FromMaybe(-1);
    g_native_setter_value = value->Int32Value(context).FromMaybe(-1);
}

void IdleGetter(v8::Local<v8::Name>,
    const v8::PropertyCallbackInfo<v8::Value>& info)
{
    g_checks->Expect(!v8::detail::CanCollectAtHostIdle(info.GetIsolate()),
        "direct C++ native accessor callback is not a host-idle safe point");
    std::size_t allocated;
    // The VM query alone need not see a JS frame for this direct C++ entry.
    static_cast<void>(JS_GetHostGCAllocation(info.GetIsolate()->GetQuickJSRuntime(), &allocated));
    info.GetReturnValue().Set(77);
}

void DeletedGetter(v8::Local<v8::Name>,
    const v8::PropertyCallbackInfo<v8::Value>& info)
{
    ++g_deleted_getter_calls;
    info.GetReturnValue().Set(31);
}

void RedefinedGetter(v8::Local<v8::Name>,
    const v8::PropertyCallbackInfo<v8::Value>& info)
{
    ++g_redefined_getter_calls;
    info.GetReturnValue().Set(41);
}

void ReplacementGetter(v8::Local<v8::Name>,
    const v8::PropertyCallbackInfo<v8::Value>& info)
{
    ++g_lazy_replacement_calls;
    info.GetReturnValue().Set(902);
}

void ReentrantLazyGetter(v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info)
{
    ++g_lazy_reentrant_calls;
    v8::Isolate* isolate = info.GetIsolate();
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Local<v8::Object> receiver = info.This();
    g_checks->Expect(!context.IsEmpty(), "lazy getter has an entered realm");
    g_checks->Expect(receiver->Delete(context, name).FromMaybe(false),
        "lazy getter can delete itself reentrantly");
    g_checks->Expect(receiver->SetNativeDataProperty(context, name,
        ReplacementGetter).FromMaybe(false),
        "lazy getter can install its replacement reentrantly");

    // These insertions force unordered_map growth after the callback's original
    // NativeProperty reference has been invalidated.
    for (int i = 0; i < 1536; ++i) {
        std::string key = "rehash_" + std::to_string(i);
        g_checks->Expect(receiver->SetNativeDataProperty(context,
            String(isolate, key.c_str()), DataGetter, nullptr, Integer(isolate, i))
                                 .FromMaybe(false),
            "lazy callback map-growth property installed");
    }
    info.GetReturnValue().Set(901);
}

void CloneLazyGetter(v8::Local<v8::Name>,
    const v8::PropertyCallbackInfo<v8::Value>& info)
{
    ++g_clone_lazy_calls;
    info.GetReturnValue().Set(73);
}

void ThrowingGetter(v8::Local<v8::Name>,
    const v8::PropertyCallbackInfo<v8::Value>& info)
{
    info.GetIsolate()->ThrowException(String(info.GetIsolate(),
        "runtime-check accessor exception"));
}

v8::Intercepted NamedGetter(v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info)
{
    ++g_named_calls;
    const std::string key = NameText(name, info);
    if (key == "hit") {
        g_handler_events.push_back(11);
        info.GetReturnValue().Set(111);
        return v8::Intercepted::kYes;
    }
    if (key == "pass")
        g_handler_events.push_back(12);
    else if (key == "missing")
        g_handler_events.push_back(13);
    else
        g_handler_events.push_back(19);
    return v8::Intercepted::kNo;
}

v8::Intercepted IndexedGetter(std::uint32_t index,
    const v8::PropertyCallbackInfo<v8::Value>& info)
{
    g_handler_events.push_back(100 + static_cast<int>(index));
    if (index == 7) {
        info.GetReturnValue().Set(707);
        return v8::Intercepted::kYes;
    }
    return v8::Intercepted::kNo;
}

bool DenyAccess(v8::Local<v8::Context>, v8::Local<v8::Object>,
    v8::Local<v8::Value>)
{
    g_handler_events.push_back(1);
    return false;
}

v8::Intercepted DeniedNamedGetter(v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info)
{
    const std::string key = NameText(name, info);
    g_handler_events.push_back(key == "permit" ? 21 : 22);
    if (key == "permit") {
        info.GetReturnValue().Set(311);
        return v8::Intercepted::kYes;
    }
    return v8::Intercepted::kNo;
}

v8::Intercepted DeniedIndexedGetter(std::uint32_t index,
    const v8::PropertyCallbackInfo<v8::Value>& info)
{
    g_handler_events.push_back(200 + static_cast<int>(index));
    if (index == 3) {
        info.GetReturnValue().Set(303);
        return v8::Intercepted::kYes;
    }
    return v8::Intercepted::kNo;
}

void IdleFunction(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    g_function_observed_busy =
        !v8::detail::CanCollectAtHostIdle(info.GetIsolate());
    info.GetReturnValue().Set(17);
}

void IdleMicrotask(void* data)
{
    auto* isolate = static_cast<v8::Isolate*>(data);
    g_microtask_observed_busy = !v8::detail::CanCollectAtHostIdle(isolate);
}

v8::MaybeLocal<v8::Value> RunScript(v8::Isolate* isolate,
    v8::Local<v8::Context> context, const char* source)
{
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(context, String(isolate, source)).ToLocal(&script))
        return {};
    return script->Run(context);
}

void CheckNativeProperties(Checks& checks, v8::Isolate* isolate,
    v8::Local<v8::Context> context)
{
    v8::HandleScope scope(isolate);
    v8::Local<v8::Object> object = v8::Object::New(isolate);
    auto string_key = String(isolate, "native");
    auto description = String(isolate, "same-description");
    auto symbol_a = v8::Symbol::New(isolate, description);
    auto symbol_b = v8::Symbol::New(isolate, description);

    checks.Expect(object->SetNativeDataProperty(context, string_key, DataGetter,
        nullptr, Integer(isolate, 10)).FromMaybe(false),
        "string native accessor installed");
    checks.Expect(object->SetNativeDataProperty(context, symbol_a, DataGetter,
        nullptr, Integer(isolate, 21)).FromMaybe(false),
        "first symbol native accessor installed");
    checks.Expect(object->SetNativeDataProperty(context, symbol_b, DataGetter,
        nullptr, Integer(isolate, 22)).FromMaybe(false),
        "second equal-description symbol native accessor installed");
    checks.Expect(IsInt(object->Get(context, string_key), context, 10),
        "string native accessor returns its data");
    checks.Expect(IsInt(object->Get(context, symbol_a), context, 21),
        "first symbol retains distinct native accessor identity");
    checks.Expect(IsInt(object->Get(context, symbol_b), context, 22),
        "second symbol retains distinct native accessor identity");
    auto setter_key = String(isolate, "setter-data");
    checks.Expect(object->SetNativeDataProperty(context, setter_key, DataGetter,
        DataSetter, Integer(isolate, 93)).FromMaybe(false),
        "native setter with explicit callback data installed");
    checks.Expect(object->Set(context, setter_key, Integer(isolate, 41)).FromMaybe(false)
            && g_native_setter_data == 93 && g_native_setter_value == 41,
        "native setter receives its own data, not the interceptor payload");

    auto deleted = String(isolate, "deleted");
    checks.Expect(object->SetNativeDataProperty(context, deleted, DeletedGetter)
                      .FromMaybe(false),
        "deletable native accessor installed");
    checks.Expect(object->Delete(context, deleted).FromMaybe(false),
        "deletable native accessor deleted");
    checks.Expect(object->DefineOwnProperty(context, deleted,
        Integer(isolate, 32)).FromMaybe(false),
        "deleted accessor key redefined as data");
    checks.Expect(IsInt(object->Get(context, deleted), context, 32),
        "redefinition does not retain deleted getter");
    checks.Expect(g_deleted_getter_calls == 0,
        "deleted native getter was never called after data redefinition");

    auto redefined = String(isolate, "redefined");
    checks.Expect(object->SetNativeDataProperty(context, redefined,
        RedefinedGetter, nullptr, {}, v8::DontEnum).FromMaybe(false),
        "configurable native accessor installed");
    v8::PropertyDescriptor descriptor;
    descriptor.set_configurable(false);
    checks.Expect(object->DefineProperty(context, redefined, descriptor)
                      .FromMaybe(false),
        "attribute-only define succeeds");
    checks.Expect(IsInt(object->Get(context, redefined), context, 41),
        "attribute-only define preserves native getter");
    const auto attributes = object->GetPropertyAttributes(context, redefined);
    checks.Expect(attributes.IsJust()
            && (attributes.FromJust() & v8::DontDelete)
            && (attributes.FromJust() & v8::DontEnum),
        "attribute-only define updates configuration without losing prior flags");
    checks.Expect(!object->Delete(context, redefined).FromMaybe(true),
        "non-configurable native property rejects deletion");

    auto replace_value = String(isolate, "replace-value");
    checks.Expect(object->SetNativeDataProperty(context, replace_value,
        RedefinedGetter).FromMaybe(false), "replaceable getter installed");
    const int before_replacement = g_redefined_getter_calls;
    checks.Expect(object->DefineOwnProperty(context, replace_value,
        Integer(isolate, 55)).FromMaybe(false),
        "native getter redefined as a data property");
    checks.Expect(IsInt(object->Get(context, replace_value), context, 55),
        "value redefinition removes native callback state");
    checks.Expect(g_redefined_getter_calls == before_replacement,
        "redefined data property does not invoke old getter");

    auto lazy = String(isolate, "lazy-reentrant");
    checks.Expect(object->SetLazyDataProperty(context, lazy,
        ReentrantLazyGetter).FromMaybe(false),
        "reentrant lazy accessor installed");
    checks.Expect(IsInt(object->Get(context, lazy), context, 901),
        "current lazy read returns the reentrant callback result");
    checks.Expect(IsInt(object->Get(context, lazy), context, 902),
        "replacement installed during lazy callback survives materialization");
    checks.Expect(g_lazy_reentrant_calls == 1 && g_lazy_replacement_calls == 1,
        "lazy generation guard preserves replacement across map rehash");

    auto throwing = String(isolate, "throwing");
    checks.Expect(object->SetNativeDataProperty(context, throwing,
        ThrowingGetter).FromMaybe(false), "throwing accessor installed");
    {
        v8::TryCatch caught(isolate);
        v8::Local<v8::Value> ignored;
        checks.Expect(!object->Get(context, throwing).ToLocal(&ignored),
            "accessor exception produces an empty MaybeLocal");
        checks.Expect(caught.HasCaught(), "accessor exception reaches TryCatch");
        checks.Expect(!v8::detail::CanCollectAtHostIdle(isolate),
            "pending accessor exception prevents host-idle collection");
    }
    checks.Expect(v8::detail::CanCollectAtHostIdle(isolate),
        "cleared accessor exception restores host-idle eligibility");
}

void CheckTemplatesAndClone(Checks& checks, v8::Isolate* isolate,
    v8::Local<v8::Context> context)
{
    v8::HandleScope scope(isolate);
    auto inherited = String(isolate, "inherited");
    auto parent = v8::FunctionTemplate::New(isolate);
    parent->InstanceTemplate()->SetNativeDataProperty(inherited, DataGetter,
        nullptr, Integer(isolate, 10), v8::DontDelete);
    auto child = v8::FunctionTemplate::New(isolate);
    child->Inherit(parent);
    child->InstanceTemplate()->SetNativeDataProperty(inherited, DataGetter,
        nullptr, Integer(isolate, 20), v8::None);

    auto parent_function = parent->GetFunction(context).ToLocalChecked();
    auto child_function = child->GetFunction(context).ToLocalChecked();
    auto parent_instance = parent_function->NewInstance(context).ToLocalChecked();
    auto child_instance = child_function->NewInstance(context).ToLocalChecked();
    checks.Expect(IsInt(parent_instance->Get(context, inherited), context, 10),
        "parent template installs its native accessor");
    checks.Expect(IsInt(child_instance->Get(context, inherited), context, 20),
        "derived template overrides inherited unforgeable accessor");
    checks.Expect(parent->HasInstance(child_instance)
            && child->HasInstance(child_instance),
        "derived instance retains both template brands");

    auto clone = child_instance->Clone();
    checks.Expect(!clone.IsEmpty()
            && IsInt(clone->Get(context, inherited), context, 20),
        "clone copies the effective derived native accessor");
    checks.Expect(child_instance->Delete(context, inherited).FromMaybe(false),
        "derived override remains configurable despite parent descriptor");
    checks.Expect(IsUndefined(child_instance->Get(context, inherited)),
        "deleting derived override does not reveal a duplicate parent entry");
    checks.Expect(IsInt(clone->Get(context, inherited), context, 20),
        "clone native-property map is independent of source deletion");
    checks.Expect(parent->HasInstance(clone) && child->HasInstance(clone),
        "clone preserves template inheritance identity");
    checks.Expect(!parent_instance->Delete(context, inherited).FromMaybe(true),
        "parent instance retains its own non-configurable descriptor");

    auto lazy_key = String(isolate, "clone-lazy");
    auto lazy_source = v8::Object::New(isolate);
    checks.Expect(lazy_source->SetLazyDataProperty(context, lazy_key,
        CloneLazyGetter).FromMaybe(false), "clone source lazy property installed");
    auto lazy_clone = lazy_source->Clone();
    checks.Expect(IsInt(lazy_clone->Get(context, lazy_key), context, 73),
        "clone materializes its lazy property");
    checks.Expect(IsInt(lazy_source->Get(context, lazy_key), context, 73),
        "source independently materializes its lazy property");
    checks.Expect(g_clone_lazy_calls == 2,
        "clone and source do not share lazy materialization state");
}

void CheckInterceptors(Checks& checks, v8::Isolate* isolate,
    v8::Local<v8::Context> context, v8::Local<v8::Context> other_context)
{
    v8::HandleScope scope(isolate);
    auto object_template = v8::ObjectTemplate::New(isolate);
    object_template->SetHandler(v8::NamedPropertyHandlerConfiguration(
        NamedGetter, nullptr, nullptr, nullptr, nullptr, {},
        v8::PropertyHandlerFlags::kOnlyInterceptStrings));
    object_template->SetHandler(v8::IndexedPropertyHandlerConfiguration(
        IndexedGetter));
    auto object = object_template->NewInstance(context).ToLocalChecked();
    checks.Expect(object->Set(context, String(isolate, "pass"),
        Integer(isolate, 222)).FromMaybe(false), "named fallback data installed");
    checks.Expect(object->Set(context, 8, Integer(isolate, 808)).FromMaybe(false),
        "indexed fallback data installed");
    auto skipped_symbol = v8::Symbol::New(isolate, String(isolate, "skip"));
    checks.Expect(object->Set(context, skipped_symbol,
        Integer(isolate, 333)).FromMaybe(false), "symbol fallback data installed");

    g_handler_events.clear();
    checks.Expect(IsInt(object->Get(context, String(isolate, "hit")), context, 111),
        "intercepted named getter supplies its result");
    checks.Expect(g_handler_events == std::vector<int>({11}),
        "intercepted named getter runs before fallback lookup");
    g_handler_events.clear();
    checks.Expect(IsInt(object->Get(context, String(isolate, "pass")), context, 222),
        "nonintercepted named getter falls through to real property");
    checks.Expect(g_handler_events == std::vector<int>({12}),
        "nonintercepted named getter is consulted exactly once");
    g_handler_events.clear();
    checks.Expect(IsUndefined(object->Get(context, String(isolate, "missing"))),
        "nonintercepted absent named lookup remains undefined");
    checks.Expect(g_handler_events == std::vector<int>({13}),
        "absent named lookup still observes the configured getter");
    const int named_before_symbol = g_named_calls;
    g_handler_events.clear();
    checks.Expect(IsInt(object->Get(context, skipped_symbol), context, 333),
        "string-only handler skips symbols and exposes real symbol property");
    checks.Expect(g_named_calls == named_before_symbol && g_handler_events.empty(),
        "skipped symbol never enters named callback");

    g_handler_events.clear();
    checks.Expect(IsInt(object->Get(context, 7), context, 707),
        "intercepted indexed getter supplies its result");
    checks.Expect(g_handler_events == std::vector<int>({107}),
        "intercepted indexed callback ordering is stable");
    g_handler_events.clear();
    checks.Expect(IsInt(object->Get(context, 8), context, 808),
        "nonintercepted indexed getter falls through to real element");
    checks.Expect(g_handler_events == std::vector<int>({108}),
        "nonintercepted indexed callback runs once");
    g_handler_events.clear();
    checks.Expect(IsUndefined(object->Get(context, 9)),
        "nonintercepted absent indexed lookup remains undefined");
    checks.Expect(g_handler_events == std::vector<int>({109}),
        "absent indexed lookup invokes the configured getter once");

    auto no_handler = v8::ObjectTemplate::New(isolate)->NewInstance(context)
                          .ToLocalChecked();
    checks.Expect(no_handler->Set(context, String(isolate, "plain"),
        Integer(isolate, 404)).FromMaybe(false), "plain data property installed");
    checks.Expect(IsInt(no_handler->Get(context, String(isolate, "plain")),
                      context, 404)
            && IsUndefined(no_handler->Get(context, 4)),
        "absent named and indexed handlers use ordinary lookup");

    auto guarded_template = v8::ObjectTemplate::New(isolate);
    v8::NamedPropertyHandlerConfiguration denied_named(DeniedNamedGetter,
        nullptr, nullptr, nullptr, nullptr, {},
        v8::PropertyHandlerFlags::kOnlyInterceptStrings);
    v8::IndexedPropertyHandlerConfiguration denied_indexed(DeniedIndexedGetter);
    guarded_template->SetAccessCheckCallbackAndHandler(DenyAccess,
        denied_named, denied_indexed);
    auto guarded = guarded_template->NewInstance(context).ToLocalChecked();
    checks.Expect(guarded->Set(context, String(isolate, "blocked"),
        Integer(isolate, 999)).FromMaybe(false), "guarded named fallback installed");
    checks.Expect(guarded->Set(context, 4, Integer(isolate, 444)).FromMaybe(false),
        "guarded indexed fallback installed");
    auto guarded_symbol = v8::Symbol::New(isolate, String(isolate, "guarded"));
    checks.Expect(guarded->Set(context, guarded_symbol,
        Integer(isolate, 555)).FromMaybe(false), "guarded symbol fallback installed");

    v8::Context::Scope enter_other(other_context);
    auto expect_denied = [&](v8::MaybeLocal<v8::Value> result,
                             const std::vector<int>& events,
                             const char* message) {
        v8::Local<v8::Value> value;
        checks.Expect(!result.ToLocal(&value), message);
        checks.Expect(g_handler_events == events,
            "access check precedes denied-realm fallback handler");
    };

    g_handler_events.clear();
    checks.Expect(IsInt(guarded->Get(other_context,
        String(isolate, "permit")), other_context, 311),
        "denied named handler may explicitly intercept access");
    checks.Expect(g_handler_events == std::vector<int>({1, 21}),
        "denied named intercept follows access check");

    g_handler_events.clear();
    {
        v8::TryCatch caught(isolate);
        expect_denied(guarded->Get(other_context, String(isolate, "blocked")),
            {1, 22}, "nonintercepted denied named access throws");
        checks.Expect(caught.HasCaught(), "denied named access is catchable");
    }
    g_handler_events.clear();
    {
        v8::TryCatch caught(isolate);
        expect_denied(guarded->Get(other_context, String(isolate, "absent")),
            {1, 22}, "absent denied named access still throws");
        checks.Expect(caught.HasCaught(), "absent denied named exception caught");
    }
    g_handler_events.clear();
    {
        v8::TryCatch caught(isolate);
        expect_denied(guarded->Get(other_context, guarded_symbol), {1},
            "string-only denied handler skips symbol before denial");
        checks.Expect(caught.HasCaught(), "denied symbol exception caught");
    }
    g_handler_events.clear();
    checks.Expect(IsInt(guarded->Get(other_context, 3), other_context, 303),
        "denied indexed handler may explicitly intercept access");
    checks.Expect(g_handler_events == std::vector<int>({1, 203}),
        "denied indexed intercept follows access check");
    g_handler_events.clear();
    {
        v8::TryCatch caught(isolate);
        expect_denied(guarded->Get(other_context, 4), {1, 204},
            "nonintercepted denied indexed access throws before fallback");
        checks.Expect(caught.HasCaught(), "denied indexed exception caught");
    }
}

void CheckIdleEligibility(Checks& checks, v8::Isolate* isolate,
    v8::Local<v8::Context> context)
{
    v8::HandleScope scope(isolate);
    checks.Expect(v8::detail::CanCollectAtHostIdle(isolate),
        "settled runtime reports host idle");
    auto function = v8::Function::New(context, IdleFunction).ToLocalChecked();
    checks.Expect(context->Global()->Set(context, String(isolate, "idleProbe"),
        function).FromMaybe(false), "idle probe exposed to genuine script");
    checks.Expect(IsInt(RunScript(isolate, context, "idleProbe()"), context, 17),
        "genuine facade script calls native callback");
    checks.Expect(g_function_observed_busy,
        "JS/native callback entry never reports host idle");
    checks.Expect(v8::detail::CanCollectAtHostIdle(isolate),
        "completed script callback returns to host idle");
    auto direct = v8::Object::New(isolate);
    auto direct_key = String(isolate, "direct-idle");
    checks.Expect(direct->SetNativeDataProperty(context, direct_key, IdleGetter)
                      .FromMaybe(false), "direct C++ callback probe installed");
    checks.Expect(IsInt(direct->Get(context, direct_key), context, 77),
        "direct C++ callback completes without a fake JS entry");

    isolate->EnqueueMicrotask(IdleMicrotask, isolate);
    checks.Expect(!v8::detail::CanCollectAtHostIdle(isolate),
        "pending genuine microtask prevents host-idle collection");
    isolate->PerformMicrotaskCheckpoint();
    checks.Expect(g_microtask_observed_busy,
        "running genuine microtask never reports host idle");
    checks.Expect(v8::detail::CanCollectAtHostIdle(isolate),
        "drained microtask checkpoint returns to host idle");
}

void CheckHandleArena(Checks& checks, v8::Isolate* isolate)
{
    v8::Global<v8::Array> persistent_array;
    v8::Global<v8::String> persistent_string;
    v8::detail::Cell* stable_array_cell = nullptr;
    v8::detail::Cell* stable_string_cell = nullptr;
    v8::Data* stable_array_view = nullptr;
    v8::Data* stable_string_view = nullptr;
    v8::Data* stable_secondary_view = nullptr;
    v8::detail::HandleArenaStats peak {};

    {
        v8::HandleScope burst(isolate);
        for (int i = 0; i < 8192; ++i) {
            auto array = v8::Array::New(isolate, 0);
            // This is a valid base-class view of a genuine Array and forces the
            // cell's secondary ApiView arena to be exercised.
            v8::Object* object_view = v8::Object::Cast(*array);
            checks.Expect(object_view->IsArray(), "array secondary ApiView is valid");
            if (i == 17) {
                persistent_array.Reset(isolate, array);
                stable_array_cell = v8::detail::CellOf(*array);
                stable_array_view = stable_array_cell->primary.view;
                stable_secondary_view = object_view;
            }
            if (i == 29) {
                auto text = String(isolate, "persistent-arena-value");
                persistent_string.Reset(isolate, text);
                stable_string_cell = v8::detail::CellOf(*text);
                stable_string_view = stable_string_cell->primary.view;
            }
        }
        peak = v8::detail::GetHandleArenaStats(isolate);
    }

    const auto released = v8::detail::GetHandleArenaStats(isolate);
    checks.Expect(peak.cell_capacity >= 8192
            && peak.view_capacity >= 8192,
        "arena burst allocated several thousand real Cells and ApiViews");
    checks.Expect(released.live_cells + 8000 < peak.live_cells,
        "scope release frees the burst's real handle cells");
    checks.Expect(released.cell_capacity == peak.cell_capacity
            && released.view_capacity == peak.view_capacity,
        "scope release alone leaves arena capacity available for trimming");
    checks.Expect(stable_array_cell && stable_string_cell
            && stable_array_cell->primary.view == stable_array_view
            && stable_array_cell->extra_views
            && stable_array_cell->extra_views->view == stable_secondary_view
            && stable_string_cell->primary.view == stable_string_view,
        "persistent Cells and primary/secondary ApiViews retain addresses before trim");

    mini_ui::CollectQuickJSGarbage(isolate,
        cppgc::EmbedderStackState::kNoHeapPointers);
    v8::detail::TrimHandleArenas(isolate);
    const auto trimmed = v8::detail::GetHandleArenaStats(isolate);
    std::printf("SNAPSHOT arena {\"peak_bytes\":%zu,\"peak_cells\":%zu,"
                "\"peak_views\":%zu,\"peak_live\":%zu,"
                "\"released_bytes\":%zu,\"released_cells\":%zu,"
                "\"released_views\":%zu,\"released_live\":%zu,"
                "\"trimmed_bytes\":%zu,\"trimmed_cells\":%zu,"
                "\"trimmed_views\":%zu,\"trimmed_live\":%zu}\n",
        peak.allocated_bytes, peak.cell_capacity, peak.view_capacity,
        peak.live_cells, released.allocated_bytes, released.cell_capacity,
        released.view_capacity, released.live_cells, trimmed.allocated_bytes,
        trimmed.cell_capacity, trimmed.view_capacity, trimmed.live_cells);
    std::fflush(stdout);
    checks.Expect(trimmed.allocated_bytes < released.allocated_bytes
            && trimmed.cell_capacity < released.cell_capacity
            && trimmed.view_capacity < released.view_capacity,
        "trim removes wholly free capacity from both arenas");
    checks.Expect(stable_array_cell->primary.view == stable_array_view
            && stable_array_cell->extra_views
            && stable_array_cell->extra_views->view == stable_secondary_view
            && stable_string_cell->primary.view == stable_string_view,
        "trim never moves live Cells or primary/secondary ApiViews");

    {
        v8::HandleScope verify(isolate);
        auto array = persistent_array.Get(isolate);
        auto text = persistent_string.Get(isolate);
        v8::String::Utf8Value utf8(isolate, text);
        checks.Expect(!array.IsEmpty() && array->IsArray(),
            "persistent array survives trim and joint GC");
        checks.Expect(*utf8 && std::string(*utf8) == "persistent-arena-value",
            "persistent string value survives trim and joint GC");
    }
    persistent_array.Reset();
    persistent_string.Reset();
    v8::detail::TrimHandleArenas(isolate);
}

void InstallNativeCycleTemplate(v8::Isolate* isolate,
    const blink::DOMWrapperWorld&, v8::Local<v8::Template> interface_template)
{
    interface_template.As<v8::FunctionTemplate>()->SetClassName(
        String(isolate, "RuntimeCheckNativeCycle"));
}

const blink::WrapperTypeInfo kNativeCycleWrapperTypeInfo = {
    gin::kEmbedderBlink,
    InstallNativeCycleTemplate,
    nullptr,
    "RuntimeCheckNativeCycle",
    nullptr,
    blink::kDOMWrappersTag,
    blink::kDOMWrappersTag,
    blink::WrapperTypeInfo::kWrapperTypeNoPrototype,
    blink::WrapperTypeInfo::kCustomWrappableId,
    blink::WrapperTypeInfo::kNotInheritFromActiveScriptWrappable,
    blink::WrapperTypeInfo::kCustomWrappableKind,
};

class NativeCycle final : public blink::ScriptWrappable {
public:
    NativeCycle(v8::Isolate* isolate, v8::Local<v8::Object> object)
        : js_(isolate, object)
        , self_(this)
    {
    }

    ~NativeCycle() override
    {
        js_.Reset();
        ++destructions_;
    }

    const blink::WrapperTypeInfo* GetWrapperTypeInfo() const override
    {
        return &kNativeCycleWrapperTypeInfo;
    }

    void Trace(cppgc::Visitor* visitor) const override
    {
        blink::ScriptWrappable::Trace(visitor);
        visitor->Trace(self_);
        visitor->Trace(js_);
    }

    static int destructions() { return destructions_; }

private:
    v8::TracedReference<v8::Object> js_;
    cppgc::Member<NativeCycle> self_;
    static inline int destructions_ = 0;
};

__declspec(noinline) void ConservativeCycleCollection(
    mini_ui::NativeHeap& heap, v8::Isolate* isolate, NativeCycle* object)
{
    NativeCycle* volatile stack_anchor = object;
    mini_ui::CollectQuickJSGarbage(isolate,
        cppgc::EmbedderStackState::kMayContainHeapPointers);
    heap.Collect(cppgc::EmbedderStackState::kMayContainHeapPointers);
    if (!stack_anchor)
        std::abort();
}

void CheckJointNativeCycle(Checks& checks, v8::Isolate* isolate,
    v8::Local<v8::Context> context)
{
    auto& heap = blink::ThreadState::Current()->cpp_heap();
    cppgc::Persistent<NativeCycle> root;
    v8::Global<v8::Object> weak_js;
    NativeCycle* node = nullptr;
    const int destruction_start = NativeCycle::destructions();
    const auto stats_start = mini_ui::GetQuickJSGCStats(isolate);
    {
        v8::HandleScope scope(isolate);
        auto object = v8::Object::New(isolate);
        checks.Expect(object->Set(context, String(isolate, "self"), object)
                          .FromMaybe(false),
            "JS half of traced cycle references itself");
        weak_js.Reset(isolate, object);
        weak_js.SetWeak();
        node = cppgc::MakeGarbageCollected<NativeCycle>(
            heap.GetAllocationHandle(), isolate, object);
        root = node;
        v8::Object::Wrap(isolate, object, node, blink::kDOMWrappersTag);
        checks.Expect(v8::Object::Unwrap<NativeCycle>(isolate, object,
            v8::CppHeapPointerTagRange(blink::kDOMWrappersTag)) == node,
            "real ScriptWrappable association round-trips through facade wrapper");
        checks.Expect(mini_ui::GetQuickJSWrapperTypeInfo(isolate,
            v8::detail::Borrow(*object)) == &kNativeCycleWrapperTypeInfo,
            "wrapper registry exposes the real custom WrapperTypeInfo");
    }

    mini_ui::CollectQuickJSGarbage(isolate,
        cppgc::EmbedderStackState::kNoHeapPointers);
    heap.Collect(cppgc::EmbedderStackState::kNoHeapPointers);
    checks.Expect(NativeCycle::destructions() == destruction_start
            && !weak_js.IsEmpty(),
        "cppgc persistent root preserves native self-cycle and traced JS value");
    const auto rooted_stats = mini_ui::GetQuickJSGCStats(isolate);
    checks.Expect(rooted_stats.wrapper_count == stats_start.wrapper_count + 1
            && rooted_stats.edge_count == stats_start.edge_count + 1,
        "rooted native/JS cycle uses real wrapper and traced-edge registries");

    root = nullptr;
    ConservativeCycleCollection(heap, isolate, node);
    checks.Expect(NativeCycle::destructions() == destruction_start
            && !weak_js.IsEmpty(),
        "conservative stack root preserves unpersisted native/traced-JS cycle");

    // The exact joint pass first removes the bridge-owned wrapper root and
    // ignores stale machine-stack words. Native collection can then reclaim the
    // self-cycle; its destructor resets the traced edge before the final JS pass.
    mini_ui::CollectQuickJSGarbage(isolate,
        cppgc::EmbedderStackState::kNoHeapPointers);
    heap.Collect(cppgc::EmbedderStackState::kNoHeapPointers);
    mini_ui::CollectQuickJSGarbage(isolate,
        cppgc::EmbedderStackState::kNoHeapPointers);
    v8::detail::CollectWeakPersistents(isolate);
    checks.Expect(NativeCycle::destructions() == destruction_start + 1,
        "precise native collection releases the unrooted self-cycle");
    checks.Expect(weak_js.IsEmpty(),
        "precise joint collection releases JS after native traced edge dies");
    const auto stats = mini_ui::GetQuickJSGCStats(isolate);
    checks.Expect(stats.wrapper_count == stats_start.wrapper_count
            && stats.edge_count == stats_start.edge_count,
        "precise collection removes the real wrapper and traced edge records");
    std::printf("SNAPSHOT joint_gc {\"epoch\":%llu,\"wrappers\":%zu,"
                "\"edges\":%zu,\"live_wrappers\":%zu,\"live_edges\":%zu,"
                "\"native_destructions\":%d}\n",
        static_cast<unsigned long long>(stats.epoch), stats.wrapper_count,
        stats.edge_count, stats.live_wrapper_count, stats.live_edge_count,
        NativeCycle::destructions() - destruction_start);
    std::fflush(stdout);
    weak_js.Reset();
}

void CheckJointEphemeron(Checks& checks, v8::Isolate* isolate,
    v8::Local<v8::Context> context)
{
    auto& heap = blink::ThreadState::Current()->cpp_heap();
    cppgc::Persistent<NativeCycle> key_root;
    v8::Global<v8::Object> map_root;
    v8::Global<v8::Object> weak_value;
    const int initial_destructions = NativeCycle::destructions();
    {
        v8::HandleScope scope(isolate);
        auto key = v8::Object::New(isolate);
        auto value = v8::Object::New(isolate);
        auto* native_key = cppgc::MakeGarbageCollected<NativeCycle>(
            heap.GetAllocationHandle(), isolate, key);
        auto* native_value = cppgc::MakeGarbageCollected<NativeCycle>(
            heap.GetAllocationHandle(), isolate, value);
        key_root = native_key;
        v8::Object::Wrap(isolate, key, native_key, blink::kDOMWrappersTag);
        v8::Object::Wrap(isolate, value, native_value, blink::kDOMWrappersTag);
        weak_value.Reset(isolate, value);
        weak_value.SetWeak();
        checks.Expect(value->Set(context, String(isolate, "key"), key).FromMaybe(false),
            "ephemeron value genuinely references its weak key");
        auto global = context->Global();
        checks.Expect(global->Set(context, String(isolate, "ephemeronKey"), key).FromMaybe(false)
                && global->Set(context, String(isolate, "ephemeronValue"), value).FromMaybe(false),
            "ephemeron setup exposes real key and value");
        auto map = RunScript(isolate, context,
            "new WeakMap([[ephemeronKey, ephemeronValue]])").ToLocalChecked().As<v8::Object>();
        map_root.Reset(isolate, map);
        checks.Expect(global->Delete(context, String(isolate, "ephemeronKey")).FromMaybe(false)
                && global->Delete(context, String(isolate, "ephemeronValue")).FromMaybe(false),
            "ephemeron setup removes temporary strong roots");
    }
    mini_ui::CollectQuickJSGarbage(isolate, cppgc::EmbedderStackState::kNoHeapPointers);
    heap.Collect(cppgc::EmbedderStackState::kNoHeapPointers);
    checks.Expect(!weak_value.IsEmpty()
            && NativeCycle::destructions() == initial_destructions,
        "native-rooted WeakMap key preserves the JS value and its native wrapper");
    key_root = nullptr;
    for (int round = 0; round < 3; ++round) {
        mini_ui::CollectQuickJSGarbage(isolate, cppgc::EmbedderStackState::kNoHeapPointers);
        heap.Collect(cppgc::EmbedderStackState::kNoHeapPointers);
    }
    mini_ui::CollectQuickJSGarbage(isolate, cppgc::EmbedderStackState::kNoHeapPointers);
    checks.Expect(weak_value.IsEmpty()
            && NativeCycle::destructions() == initial_destructions + 2,
        "live WeakMap does not retain a dead key/value/native back-reference cycle");
    map_root.Reset();
    weak_value.Reset();
}

class Fixture {
public:
    ~Fixture()
    {
        if (!owned_)
            return;
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    bool Initialize(int argc, wchar_t** argv, std::string* error)
    {
        if (argc > 2) {
            *error = "usage: runtime_checks.exe [resource-root-with-index.html]";
            return false;
        }
        std::error_code ec;
        if (argc == 2) {
            root_ = std::filesystem::absolute(argv[1], ec);
            if (ec) {
                *error = "could not resolve the supplied resource root";
                return false;
            }
        } else {
            root_ = std::filesystem::temp_directory_path(ec);
            if (ec) {
                *error = "could not resolve the temporary directory";
                return false;
            }
            root_ /= L"mini-ui-runtime-checks-"
                + std::to_wstring(GetCurrentProcessId()) + L"-"
                + std::to_wstring(GetTickCount64());
            if (!std::filesystem::create_directories(root_, ec) || ec) {
                *error = "could not create isolated resource fixture";
                return false;
            }
            owned_ = true;
            std::ofstream page(root_ / L"index.html", std::ios::binary);
            page << "<!doctype html><meta charset=utf-8><title>runtime checks</title>"
                    "<body>isolated runtime checks</body>";
            if (!page) {
                *error = "could not write isolated resource fixture";
                return false;
            }
        }
        if (!std::filesystem::is_regular_file(root_ / L"index.html", ec) || ec) {
            *error = "resource root must contain index.html";
            return false;
        }
        return true;
    }

    const std::filesystem::path& root() const { return root_; }
    bool owned() const { return owned_; }

private:
    std::filesystem::path root_;
    bool owned_ = false;
};

int RunChecks(const Fixture& fixture)
{
    Checks checks;
    g_checks = &checks;

    mini_ui::ViewOptions options;
    options.resource_root = fixture.root();
    options.width = 64;
    options.height = 64;
    options.device_scale = 1.0f;
    mini_ui::Error error;
    std::unique_ptr<mini_ui::View> view = mini_ui::View::Create(options, &error);
    if (!view) {
        std::fprintf(stderr, "FAIL runtime_checks: View::Create: %s\n",
            error.message.c_str());
        return 1;
    }
    if (!view->LoadFile("index.html", &error)) {
        std::fprintf(stderr, "FAIL runtime_checks: View::LoadFile: %s\n",
            error.message.c_str());
        return 1;
    }
    std::string document_result;
    if (!view->Evaluate("document.body !== null",
            "runtime-checks-bootstrap.js", &document_result, &error)
        || document_result != "true") {
        std::fprintf(stderr,
            "FAIL runtime_checks: genuine View realm bootstrap: %s result=%s\n",
            error.message.c_str(), document_result.c_str());
        return 1;
    }

    v8::Isolate* isolate = v8::Isolate::TryGetCurrent();
    if (!isolate || !isolate->GetQuickJSRuntime()) {
        std::fprintf(stderr,
            "FAIL runtime_checks: initialized Blink facade isolate unavailable\n");
        return 1;
    }

    {
        v8::Isolate::Scope isolate_scope(isolate);
        v8::HandleScope handles(isolate);
        auto context = v8::Context::New(isolate);
        auto other_context = v8::Context::New(isolate);
        if (context.IsEmpty() || other_context.IsEmpty()) {
            std::fprintf(stderr,
                "FAIL runtime_checks: could not create isolated facade realms\n");
            return 1;
        }
        v8::Context::Scope context_scope(context);

        CheckNativeProperties(checks, isolate, context);
        CheckTemplatesAndClone(checks, isolate, context);
        CheckInterceptors(checks, isolate, context, other_context);
        CheckIdleEligibility(checks, isolate, context);
        CheckHandleArena(checks, isolate);
        CheckJointNativeCycle(checks, isolate, context);
        CheckJointEphemeron(checks, isolate, context);
        checks.Expect(v8::detail::CanCollectAtHostIdle(isolate),
            "all checks leave runtime eligible for host-idle collection");
    }

    if (checks.failures()) {
        std::fprintf(stderr, "FAIL runtime_checks: %d invariant failure(s)\n",
            checks.failures());
        return 1;
    }
    std::printf("PASS runtime_checks: native properties, inheritance/clone, "
                "interceptor/access ordering, idle eligibility, handle-arena "
                "reclamation, and joint native/JS lifetime\n");
    std::fflush(stdout);
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Fixture fixture;
    std::string fixture_error;
    if (!fixture.Initialize(__argc, __wargv, &fixture_error)) {
        std::fprintf(stderr, "FAIL runtime_checks: %s\n", fixture_error.c_str());
        return 2;
    }
    std::printf("RUNTIME fixture=%s resource_root=%ls entry=index.html "
                "ownership=%s api=mini_ui::View+QuickJS-facade\n",
        fixture.owned() ? "temporary" : "argv", fixture.root().c_str(),
        fixture.owned() ? "owned-cleanup" : "borrowed");
    std::fflush(stdout);
    try {
        return RunChecks(fixture);
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "FAIL runtime_checks: uncaught exception: %s\n",
            exception.what());
    } catch (...) {
        std::fprintf(stderr, "FAIL runtime_checks: unknown uncaught exception\n");
    }
    return 1;
}
