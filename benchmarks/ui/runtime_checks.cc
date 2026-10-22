// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include "mini_ui/ui.h"
#include "runtime/ui/quickjs_bindings.h"
#include "runtime/ui/script_runtime.h"
#include "runtime/ui/quickjs_v8_facade.h"
#include "runtime/ui/quickjs_v8_internal.h"
#include "third_party/blink/renderer/platform/bindings/script_wrappable.h"
#include "third_party/blink/renderer/platform/heap/thread_state.h"
#include "v8/include/cppgc/allocation.h"
#include "v8/include/cppgc/ephemeron-pair.h"
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

template <typename T>
std::string NameText(v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<T>& info)
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

v8::Intercepted ProtocolGetter(v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info)
{
    g_handler_events.push_back(303);
    if (NameText(name, info) != "virtual")
        return v8::Intercepted::kNo;
    info.GetReturnValue().Set(37);
    return v8::Intercepted::kYes;
}

v8::Intercepted ProtocolQuery(v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Integer>& info)
{
    g_handler_events.push_back(302);
    if (NameText(name, info) != "virtual")
        return v8::Intercepted::kNo;
    info.GetReturnValue().Set(static_cast<int>(v8::ReadOnly | v8::DontDelete));
    return v8::Intercepted::kYes;
}

v8::Intercepted ProtocolDescriptor(v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Value>& info)
{
    g_handler_events.push_back(301);
    if (NameText(name, info) != "explicit")
        return v8::Intercepted::kNo;
    auto* isolate = info.GetIsolate();
    g_checks->Expect(!v8::detail::CanCollectAtHostIdle(isolate),
        "descriptor callback retains its active native boundary");
    auto context = isolate->GetCurrentContext();
    auto descriptor = v8::Object::New(isolate);
    descriptor->Set(context, String(isolate, "value"), Integer(isolate, 99)).Check();
    descriptor->Set(context, String(isolate, "configurable"), v8::True(isolate)).Check();
    info.GetReturnValue().Set(descriptor);
    return v8::Intercepted::kYes;
}

v8::Intercepted ProtocolDelete(v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Boolean>& info)
{
    if (NameText(name, info) != "virtual")
        return v8::Intercepted::kNo;
    info.GetReturnValue().Set(false);
    return v8::Intercepted::kYes;
}

v8::Intercepted ProtocolDefine(v8::Local<v8::Name> name,
    const v8::PropertyDescriptor&, const v8::PropertyCallbackInfo<void>& info)
{
    if (NameText(name, info) != "virtual")
        return v8::Intercepted::kNo;
    info.GetReturnValue().Set(false);
    return v8::Intercepted::kYes;
}

void RejectingNativeSetter(v8::Local<v8::Name>, v8::Local<v8::Value>,
    const v8::PropertyCallbackInfo<void>& info)
{
    info.GetReturnValue().SetFalse();
}

v8::Intercepted ThrowingDefinitionQuery(v8::Local<v8::Name> name,
    const v8::PropertyCallbackInfo<v8::Integer>& info)
{
    if (NameText(name, info) != "throws")
        return v8::Intercepted::kNo;
    info.GetIsolate()->ThrowException(v8::Exception::Error(
        String(info.GetIsolate(), "definition query rejected")));
    return v8::Intercepted::kYes;
}

struct ProductionJobProbe {
    Checks& checks;
    mini_ui::ScriptRuntime& runtime;
    v8::Isolate* isolate;
    int calls = 0;
};

void CheckProductionJobBusy(ProductionJobProbe& probe)
{
    ++probe.calls;
    std::size_t allocated_bytes = 0;
    probe.checks.Expect(!JS_IsJobPending(probe.runtime.get()),
        "executing sole native job is no longer in the pending queue");
    probe.checks.Expect(!JS_GetHostGCAllocation(probe.runtime.get(), &allocated_bytes),
        "production native job is not a VM host-GC allocation safe point");
    probe.checks.Expect(!v8::detail::CanCollectAtHostIdle(probe.isolate),
        "production native job is not a facade host-idle safe point");
}

void InnerProductionMicrotask(void* opaque)
{
    CheckProductionJobBusy(*static_cast<ProductionJobProbe*>(opaque));
}

void OuterProductionMicrotask(void* opaque)
{
    auto& probe = *static_cast<ProductionJobProbe*>(opaque);
    CheckProductionJobBusy(probe);
    const auto before = v8::detail::GetHandleArenaStats(probe.isolate);
    v8::detail::TrimHandleArenas(probe.isolate);
    probe.checks.Expect(v8::detail::GetHandleArenaStats(probe.isolate).allocated_bytes
            == before.allocated_bytes,
        "active native job cannot trim handle arenas");
    probe.isolate->EnqueueMicrotask(InnerProductionMicrotask, opaque);
    mini_ui::Error error;
    probe.checks.Expect(probe.runtime.DrainJobs(&error),
        "native job can reenter the real production job drain");
    CheckProductionJobBusy(probe);
}

void FailingProductionMicrotask(void* opaque)
{
    auto& probe = *static_cast<ProductionJobProbe*>(opaque);
    CheckProductionJobBusy(probe);
    v8::HandleScope handles(probe.isolate);
    probe.isolate->ThrowException(v8::Exception::Error(
        String(probe.isolate, "production job rejected")));
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

void CheckPropertyProtocol(Checks& checks, v8::Isolate* isolate,
    v8::Local<v8::Context> context)
{
    v8::HandleScope scope(isolate);
    auto prototype = v8::ObjectTemplate::New(isolate);
    prototype->SetHandler(v8::NamedPropertyHandlerConfiguration(ProtocolGetter,
        nullptr, ProtocolQuery, ProtocolDelete, nullptr, ProtocolDefine,
        ProtocolDescriptor));
    auto object = prototype->NewInstance(context).ToLocalChecked();
    g_handler_events.clear();
    auto explicit_descriptor = object->GetOwnPropertyDescriptor(context,
        String(isolate, "explicit")).ToLocalChecked().As<v8::Object>();
    checks.Expect(IsInt(explicit_descriptor->Get(context, String(isolate, "value")),
                      context, 99)
            && g_handler_events == std::vector<int>({301}),
        "explicit property descriptor takes precedence over query and getter");
    g_handler_events.clear();
    auto virtual_descriptor = object->GetOwnPropertyDescriptor(context,
        String(isolate, "virtual")).ToLocalChecked().As<v8::Object>();
    checks.Expect(IsInt(virtual_descriptor->Get(context, String(isolate, "value")),
                      context, 37)
            && g_handler_events == std::vector<int>({301, 302, 303}),
        "unhandled descriptor falls through query then getter in order");
    context->Global()->Set(context, String(isolate, "protocol"), object).Check();
    auto result = RunScript(isolate, context,
        "(()=>{const d=Object.getOwnPropertyDescriptor(protocol,'virtual');"
        "if(d.writable||d.configurable||d.value!==37)return false;"
        "if(Reflect.deleteProperty(protocol,'virtual'))return false;"
        "if(Reflect.defineProperty(protocol,'virtual',{value:4}))return false;"
        "if(protocol.virtual!==37)return false;"
        "if(!Reflect.defineProperty(protocol,'ordinary',{value:8,configurable:false}))return false;"
        "return protocol.ordinary===8&&!Reflect.deleteProperty(protocol,'ordinary');})()");
    v8::Local<v8::Value> checked;
    checks.Expect(result.ToLocal(&checked) && checked->IsTrue(),
        "intercepted mutation failure and ordinary descriptor constraints remain observable");
    context->Global()->Delete(context, String(isolate, "protocol")).Check();
}

void CheckNativePropertyMutation(Checks& checks, v8::Isolate* isolate,
    v8::Local<v8::Context> context)
{
    v8::HandleScope handles(isolate);
    auto object = v8::Object::New(isolate);
    const auto fixed = static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete);
    object->SetNativeDataProperty(context, String(isolate, "locked"),
        DataGetter, nullptr, Integer(isolate, 41), fixed).Check();
    object->SetNativeDataProperty(context, String(isolate, "nan"),
        DataGetter, nullptr, v8::Number::New(isolate,
            std::numeric_limits<double>::quiet_NaN()), fixed).Check();
    object->SetNativeDataProperty(context, String(isolate, "negativeZero"),
        DataGetter, nullptr, v8::Number::New(isolate, -0.0), fixed).Check();
    object->SetNativeDataProperty(context, String(isolate, "reject"),
        DataGetter, RejectingNativeSetter, Integer(isolate, 17)).Check();
    context->Global()->Set(context, String(isolate, "nativeMutation"), object).Check();
    auto expect_script = [&](const char* source, const char* message) {
        v8::TryCatch caught(isolate);
        v8::Local<v8::Value> result;
        checks.Expect(RunScript(isolate, context, source).ToLocal(&result)
                && result->IsTrue(), message);
    };
    expect_script("!Reflect.defineProperty(nativeMutation,'locked',{value:undefined})"
                  "&&nativeMutation.locked===41",
        "native nonconfigurable readonly property rejects its hidden placeholder");
    expect_script("Reflect.defineProperty(nativeMutation,'locked',{value:41})"
                  "&&nativeMutation.locked===41",
        "native nonconfigurable readonly property accepts its callback-visible same value");
    expect_script("Reflect.defineProperty(nativeMutation,'nan',{value:NaN})"
                  "&&Number.isNaN(nativeMutation.nan)",
        "native readonly redefinition uses SameValue for NaN");
    expect_script("!Reflect.defineProperty(nativeMutation,'negativeZero',{value:0})"
                  "&&Reflect.defineProperty(nativeMutation,'negativeZero',{value:-0})"
                  "&&Object.is(nativeMutation.negativeZero,-0)",
        "native readonly redefinition distinguishes signed zero");
    expect_script("!Reflect.set(nativeMutation,'reject',2)&&nativeMutation.reject===17",
        "native setter rejection is observable through Reflect.set");
    expect_script("(()=>{'use strict';try{nativeMutation.reject=2;return false}"
                  "catch(e){return e instanceof TypeError&&nativeMutation.reject===17}})()",
        "native setter rejection throws for strict assignment");

    auto queried = v8::ObjectTemplate::New(isolate);
    queried->SetHandler(v8::NamedPropertyHandlerConfiguration(
        ProtocolGetter, nullptr, ProtocolQuery));
    context->Global()->Set(context, String(isolate, "definitionQuery"),
        queried->NewInstance(context).ToLocalChecked()).Check();
    expect_script("!Reflect.defineProperty(definitionQuery,'virtual',{value:4})"
                  "&&definitionQuery.virtual===37",
        "query-only readonly virtual property constrains definitions");
    auto throwing = v8::ObjectTemplate::New(isolate);
    throwing->SetHandler(v8::NamedPropertyHandlerConfiguration(
        nullptr, nullptr, ThrowingDefinitionQuery));
    context->Global()->Set(context, String(isolate, "definitionThrow"),
        throwing->NewInstance(context).ToLocalChecked()).Check();
    expect_script("(()=>{try{Reflect.defineProperty(definitionThrow,'throws',{value:1});"
                  "return false}catch(e){return e.message==='definition query rejected'}})()",
        "definition lookup propagates a query exception before ordinary storage mutation");
    context->Global()->Delete(context, String(isolate, "nativeMutation")).Check();
    context->Global()->Delete(context, String(isolate, "definitionQuery")).Check();
    context->Global()->Delete(context, String(isolate, "definitionThrow")).Check();
}

void CheckProductionJobs(Checks& checks)
{
    mini_ui::Error error;
    auto runtime = mini_ui::ScriptRuntime::Create(&error);
    checks.Expect(runtime != nullptr, "production job runtime created");
    if (!runtime)
        return;
    std::unique_ptr<v8::Isolate, void (*)(v8::Isolate*)> isolate(
        v8::Isolate::CreateForQuickJS(runtime->get()),
        [](v8::Isolate* value) { value->Dispose(); });
    checks.Expect(isolate != nullptr, "production job isolate created");
    if (!isolate)
        return;
    v8::Isolate::Scope isolate_scope(isolate.get());
    {
        v8::HandleScope burst(isolate.get());
        for (int index = 0; index < 768; ++index)
            v8::Array::New(isolate.get(), 0);
    }
    ProductionJobProbe probe {checks, *runtime, isolate.get()};
    isolate->EnqueueMicrotask(OuterProductionMicrotask, &probe);
    checks.Expect(runtime->DrainJobs(&error) && probe.calls == 3,
        "production job drain executes outer and nested native jobs");
    std::size_t allocated_bytes = 0;
    checks.Expect(v8::detail::CanCollectAtHostIdle(isolate.get())
            && JS_GetHostGCAllocation(runtime->get(), &allocated_bytes),
        "completed nested job drain restores host-idle eligibility");
    isolate->EnqueueMicrotask(FailingProductionMicrotask, &probe);
    checks.Expect(!runtime->DrainJobs(&error) && probe.calls == 4
            && error.message.find("production job rejected") != std::string::npos,
        "production job exception is propagated and consumed");
    checks.Expect(v8::detail::CanCollectAtHostIdle(isolate.get())
            && JS_GetHostGCAllocation(runtime->get(), &allocated_bytes)
            && runtime->DrainJobs(&error),
        "exceptional and empty production drains restore idle state");
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

void CheckRegExpOwnership(Checks& checks, v8::Isolate* isolate)
{
    auto* runtime = isolate->GetQuickJSRuntime();
    auto* context = JS_NewContext(runtime);
    checks.Expect(context != nullptr, "RegExp ownership realm created");
    if (!context)
        return;
    JSValue global = JS_GetGlobalObject(context);
    JSValue constructor = JS_GetPropertyStr(context, global, "RegExp");
    JS_FreeValue(context, global);
    const auto* header = static_cast<const JSRefCountHeader*>(JS_VALUE_GET_PTR(constructor));
    const int initial_references = header->ref_count;
    const char* source =
        "for(let i=0;i<256;++i){"
        "const m=/(ab)(c)/.exec('abc');"
        "if(m[0]!=='abc'||m[1]!=='ab'||m[2]!=='c')throw Error('capture');"
        "} RegExp['$&']==='abc'&&RegExp.$1==='ab'&&RegExp.$2==='c'";
    JSValue result = JS_Eval(context, source, std::char_traits<char>::length(source),
        "regexp-ownership.js", JS_EVAL_TYPE_GLOBAL);
    checks.Expect(!JS_IsException(result) && JS_ToBool(context, result) == 1,
        "repeated RegExp execution preserves matches and legacy captures");
    JS_FreeValue(context, result);
    checks.Expect(header->ref_count == initial_references,
        "RegExp execution releases every acquired constructor reference");
    const char* throwing =
        "(()=>{const r=/x/;Object.defineProperty(r,'constructor',"
        "{get(){throw Error('constructor-getter');}});"
        "try{r.exec('x');return false;}catch(e){return e.message==='constructor-getter';}})()";
    result = JS_Eval(context, throwing, std::char_traits<char>::length(throwing),
        "regexp-constructor-error.js", JS_EVAL_TYPE_GLOBAL);
    checks.Expect(!JS_IsException(result) && JS_ToBool(context, result) == 1,
        "RegExp constructor getter exception propagates without fabricated match");
    JS_FreeValue(context, result);
    JSValue exception = JS_GetException(context);
    JS_FreeValue(context, exception);
    JS_FreeValue(context, constructor);
    JS_FreeContext(context);
}

class NativeEphemeron final : public cppgc::GarbageCollected<NativeEphemeron> {
public:
    NativeEphemeron(NativeCycle* key, NativeCycle* value) : pair_(key, value) {}
    void Trace(cppgc::Visitor* visitor) const { visitor->Trace(pair_); }
private:
    cppgc::EphemeronPair<NativeCycle, NativeCycle> pair_;
};

void CheckJointEphemeron(Checks& checks, v8::Isolate* isolate,
    v8::Local<v8::Context> context)
{
    auto& heap = blink::ThreadState::Current()->cpp_heap();
    cppgc::Persistent<NativeCycle> key_root;
    cppgc::Persistent<NativeEphemeron> pair_root;
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
        pair_root = cppgc::MakeGarbageCollected<NativeEphemeron>(
            heap.GetAllocationHandle(), native_key, native_value);
        v8::Object::Wrap(isolate, key, native_key, blink::kDOMWrappersTag);
        v8::Object::Wrap(isolate, value, native_value, blink::kDOMWrappersTag);
        weak_value.Reset(isolate, value);
        weak_value.SetWeak();
        checks.Expect(value->Set(context, String(isolate, "key"), key).FromMaybe(false),
            "native ephemeron value genuinely references its weak key");
    }
    mini_ui::CollectQuickJSGarbage(isolate, cppgc::EmbedderStackState::kNoHeapPointers);
    heap.Collect(cppgc::EmbedderStackState::kNoHeapPointers);
    checks.Expect(!weak_value.IsEmpty()
            && NativeCycle::destructions() == initial_destructions,
        "native-rooted ephemeron key preserves the JS value and its native wrapper");
    key_root = nullptr;
    for (int round = 0; round < 3; ++round) {
        mini_ui::CollectQuickJSGarbage(isolate, cppgc::EmbedderStackState::kNoHeapPointers);
        heap.Collect(cppgc::EmbedderStackState::kNoHeapPointers);
    }
    mini_ui::CollectQuickJSGarbage(isolate, cppgc::EmbedderStackState::kNoHeapPointers);
    checks.Expect(weak_value.IsEmpty()
            && NativeCycle::destructions() == initial_destructions + 2,
        "live native ephemeron does not retain a dead key/value/JS back-reference cycle");
    pair_root = nullptr;
    weak_value.Reset();
}

void CheckActiveBlobIO(Checks& checks, mini_ui::View& view, v8::Isolate* isolate)
{
    mini_ui::Error error;
    std::string result;
    const char* start = R"JS((()=>{
      globalThis.gcIO={finished:0,errors:[],buffers:[]};
      for(let i=1;i<=4;++i){
        const bytes=new Uint8Array(262144);bytes.fill(i);
        const url=URL.createObjectURL(new Blob([bytes],{type:'application/octet-stream'}));
        fetch(url).then(r=>r.arrayBuffer()).then(buffer=>{
          const data=new Uint8Array(buffer);
          if(data.length!==262144||data[0]!==i||data[data.length-1]!==i)
            throw Error('blob bytes differ');
          ++gcIO.finished;
        }).catch(e=>gcIO.errors.push(String(e))).finally(()=>URL.revokeObjectURL(url));
      }
      for(let i=0;i<5;++i)gcIO.buffers.push(new Uint8Array(16*1024*1024));
      return true;
    })())JS";
    if (!view.Evaluate(start, "gc-active-blob.js", &result, &error)) {
        std::fprintf(stderr, "active Blob setup: %s\n", error.message.c_str());
        checks.Expect(false, "real pending Blob reads and allocation burst start");
        return;
    }
    // Exercise actual native service requests before pumping their completions.
    mini_ui::CollectQuickJSGarbage(isolate, cppgc::EmbedderStackState::kMayContainHeapPointers);
    blink::ThreadState::Current()->cpp_heap().Collect(cppgc::EmbedderStackState::kMayContainHeapPointers);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool finished = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!view.Tick(now, &error)
            || !view.Evaluate("gcIO.finished===4&&gcIO.errors.length===0",
                "gc-active-blob-result.js", &result, &error))
            break;
        if (result == "true") {
            finished = true;
            break;
        }
    }
    if (!finished) {
        view.Evaluate("JSON.stringify(gcIO.errors)", "gc-active-blob-errors.js", &result, &error);
        std::fprintf(stderr, "active Blob completion: %s %s\n", error.message.c_str(), result.c_str());
    }
    checks.Expect(finished, "active native Blob reads preserve actual bytes and promise delivery across GC");
    checks.Expect(view.Evaluate("delete globalThis.gcIO", "gc-active-blob-cleanup.js", &result, &error),
        "active Blob fixture releases its allocation roots");
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

void CheckIdentityCollections(Checks& checks, v8::Isolate* isolate,
    v8::Local<v8::Context> context)
{
    v8::Local<v8::Value> result;
    checks.Expect(RunScript(isolate, context, R"JS((()=>{
        const objects = Array.from({length:4096}, (_, i)=>({i}));
        const symbols = objects.map(()=>Symbol('same description'));
        const map = new Map(), set = new Set();
        const weakMap = new WeakMap(), weakSet = new WeakSet();
        for (let i=0; i<objects.length; ++i) {
            map.set(objects[i], i).set(symbols[i], ~i);
            set.add(objects[i]).add(symbols[i]);
            weakMap.set(objects[i], i);
            weakSet.add(objects[i]);
        }
        for (let i=0; i<objects.length; ++i) {
            if (map.get(objects[i])!==i || map.get(symbols[i])!==~i
                || !set.has(objects[i]) || !set.has(symbols[i])
                || weakMap.get(objects[i])!==i || !weakSet.has(objects[i]))
                return false;
        }
        if (map.has({i:0}) || map.has(Symbol('same description')))
            return false;
        const iterator=map.keys();
        if (iterator.next().value!==objects[0]) return false;
        map.delete(symbols[0]);
        map.set(objects[0], 9000);
        map.set(symbols[0], 8000);
        if (iterator.next().value!==objects[1]) return false;
        let last;
        for (const key of iterator) last=key;
        if (last!==symbols[0] || map.get(objects[0])!==9000)
            return false;
        for (let i=0; i<objects.length; i+=2) {
            if (!weakMap.delete(objects[i]) || !weakSet.delete(objects[i])
                || weakMap.has(objects[i]) || weakSet.has(objects[i]))
                return false;
        }
        const target={}, proxy=new Proxy(target, {
            get(){throw new Error('identity lookup must not access properties')},
            ownKeys(){throw new Error('identity lookup must not enumerate')}
        });
        map.set(target, 17).set(proxy, 19);
        map.set(NaN, 23).set(-0, 29).set(+0, 31);
        if (map.get(proxy)!==19 || map.get(target)!==17
            || map.get(NaN)!==23 || map.get(-0)!==31)
            return false;
        map.clear(); set.clear();
        return map.size===0 && set.size===0 && iterator.next().done;
    })())JS").ToLocal(&result) && result->IsTrue(),
        "identity-key collections preserve resize, equality, live iteration, deletion and clearing");
}

void CheckFieldReads(Checks& checks, v8::Isolate* isolate,
    v8::Local<v8::Context> context)
{
    v8::Local<v8::Value> result;
    checks.Expect(RunScript(isolate, context, R"JS((()=>{
        function read(o) { return o.value; }
        function invoke(o) { return o.method(); }
        const proto={value:17}, object=Object.create(proto);
        object.value=23;
        if (read(object)!==23) return false;
        let gets=0;
        Object.defineProperty(object,'value',{configurable:true,get(){
            ++gets;
            if (this!==object) throw new Error('getter receiver');
            delete this.value;
            return 29;
        }});
        if (read(object)!==29 || read(object)!==17 || gets!==1) return false;
        Object.setPrototypeOf(object,{value:31});
        if (read(object)!==31) return false;
        Object.defineProperty(object,'value',{value:37,writable:true});
        object.value=41;
        if (read(object)!==41) return false;
        for (let i=0;i<256;++i) {
            const o={};
            for (let j=0;j<(i%32);++j) o['slot'+j]=j;
            o.value={i};
            if (read(o).i!==i) return false;
            delete o.value;
            if (read(o)!==undefined) return false;
        }
        const shrinking={};
        for(let i=0;i<128;++i) shrinking['prefix'+i]=i;
        shrinking.value=61;
        if(read(shrinking)!==61) return false;
        for(let i=0;i<128;++i) delete shrinking['prefix'+i];
        shrinking.extra=67;
        if(read(shrinking)!==61) return false;
        const readers=[], targets=[];
        for(let i=0;i<513;++i) {
            const name='field'+i, target={};
            for(let j=0;j<i%8;++j) target['prefix'+j]=j;
            target[name]=i;
            readers.push(new Function('o','return o.'+name));
            targets.push(target);
        }
        for(let pass=0;pass<3;++pass)
            for(let i=0;i<readers.length;++i)
                if(readers[i](targets[i])!==i) return false;
        const holder={value:43,method(){return this.value;}};
        if (invoke(holder)!==43) return false;
        const inherited=Object.create(holder);
        inherited.value=47;
        if (invoke(inherited)!==47) return false;
        const accessor={value:53,get method(){return holder.method;}};
        if (invoke(accessor)!==53) return false;
        let traps=0;
        const proxy=new Proxy(holder,{get(target,key,receiver){
            ++traps; return Reflect.get(target,key,receiver);
        }});
        if (read(proxy)!==43 || invoke(proxy)!==43 || traps!==3) return false;
        const sentinel={};
        try { read(new Proxy({}, {get(){throw sentinel;}})); return false; }
        catch(e) { if(e!==sentinel) return false; }
        for (const absent of [null,undefined]) {
            try { read(absent); return false; }
            catch(e) { if(!(e instanceof TypeError)) return false; }
        }
        if ('hello'.length!==5 || [1,2,3].length!==3) return false;
        const revocable=Proxy.revocable(holder,{});
        revocable.revoke();
        try { read(revocable.proxy); return false; }
        catch(e) { if(!(e instanceof TypeError)) return false; }
        return read({value:{alive:59}}).alive===59;
    })())JS").ToLocal(&result) && result->IsTrue(),
        "field reads preserve mutation, getters, receivers, prototypes, proxies and value lifetime");
}

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
        CheckIdentityCollections(checks, isolate, context);
        CheckFieldReads(checks, isolate, context);
        CheckIdleEligibility(checks, isolate, context);
        CheckPropertyProtocol(checks, isolate, context);
        CheckNativePropertyMutation(checks, isolate, context);
        CheckHandleArena(checks, isolate);
        CheckJointNativeCycle(checks, isolate, context);
        CheckJointEphemeron(checks, isolate, context);
        CheckRegExpOwnership(checks, isolate);
        checks.Expect(v8::detail::CanCollectAtHostIdle(isolate),
            "all checks leave runtime eligible for host-idle collection");
    }
    CheckProductionJobs(checks);
    CheckActiveBlobIO(checks, *view, isolate);

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
