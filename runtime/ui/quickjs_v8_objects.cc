// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "runtime/ui/quickjs_v8_objects.h"
#include "runtime/ui/quickjs_bindings.h"
#include "runtime/ui/quickjs_v8_internal.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <string>
#include <array>
#include <atomic>
#include <mutex>
#include <utility>
#include <unordered_map>
#include <vector>

namespace v8 {
namespace {
struct Root {
    JSRuntime* runtime = nullptr;
    JSValue value = JS_UNDEFINED;
    Root() = default;
    Root(JSContext* context, JSValueConst value) : runtime(JS_GetRuntime(context)), value(JS_DupValue(context,value)) {}
    Root(const Root& other) : runtime(other.runtime), value(runtime ? JS_DupValueRT(runtime,other.value) : JS_UNDEFINED) {}
    Root(Root&& other) noexcept : runtime(other.runtime), value(std::exchange(other.value,JS_UNDEFINED)) {}
    Root& operator=(Root other) noexcept { std::swap(runtime,other.runtime); std::swap(value,other.value); return *this; }
    ~Root() { if (runtime) JS_FreeValueRT(runtime,value); }
    void Mark(JSRuntime* runtime, JS_MarkFunc* mark, void* userdata) const { JS_MarkValue(runtime,value,mark,userdata); }
};
Root Hold(const Data* data) { return data ? Root(detail::ContextOf(data),detail::Borrow(data)) : Root(); }
template<class T> Root Hold(Local<T> value) { return Hold(*value); }
JSValueConst Raw(const Data* data) { return data ? detail::Borrow(data) : JS_UNDEFINED; }
template<class T> JSValueConst Raw(Local<T> value) { return Raw(*value); }
int Attributes(PropertyAttribute attributes) {
    return ((attributes & ReadOnly) ? 0 : JS_PROP_WRITABLE) | ((attributes & DontEnum) ? 0 : JS_PROP_ENUMERABLE) | ((attributes & DontDelete) ? 0 : JS_PROP_CONFIGURABLE);
}
PropertyAttribute FromFlags(int flags) {
    return static_cast<PropertyAttribute>(((flags & JS_PROP_WRITABLE) ? 0 : ReadOnly) | ((flags & JS_PROP_ENUMERABLE) ? 0 : DontEnum) | ((flags & JS_PROP_CONFIGURABLE) ? 0 : DontDelete));
}
Maybe<bool> Result(JSContext* context,int result) {
    if (result < 0) { detail::CaptureException(detail::IsolateFor(context),context); return Nothing<bool>(); }
    return Just(result != 0);
}
void FreeDescriptor(JSContext* context,JSPropertyDescriptor& descriptor) {
    JS_FreeValue(context,descriptor.value); JS_FreeValue(context,descriptor.getter); JS_FreeValue(context,descriptor.setter);
}
struct NativeProperty {
    Root key, data;
    AccessorNameGetterCallback getter = nullptr;
    AccessorNameSetterCallback setter = nullptr;
    PropertyAttribute attributes = None;
    bool lazy = false;
    uint64_t generation = 0;
};
uint64_t NextNativePropertyGeneration() {
    static std::atomic<uint64_t> generation{1};
    return generation.fetch_add(1,std::memory_order_relaxed);
}
struct TemplateEntry {
    enum Kind { value, accessor, native, intrinsic } kind = value;
    Root key, data, getter, setter;
    NativeProperty native_property;
    PropertyAttribute attributes = None;
    Intrinsic intrinsic_value = kArrayPrototype;
    bool private_key = false;
};
struct TemplatePayload : detail::DataPayload {
    bool function = false;
    std::vector<TemplateEntry> entries;
    Root instance, prototype, parent, provider, signature, callback_data, class_name, interface_name, access_data, cache_property;
    Root named_data,indexed_data,denied_named_data,denied_indexed_data;
    NamedPropertyHandlerConfiguration named, denied_named;
    IndexedPropertyHandlerConfiguration indexed, denied_indexed;
    FunctionCallback callback = nullptr;
    AccessCheckCallback access_check = nullptr;
    int internal_fields = 0;
    int length = 0;
    bool immutable = false, undetectable = false, code_like = false;
    uint32_t exception_context = 0;
    bool read_only_prototype = false, remove_prototype = false, accept_any_receiver = true;
    bool prototype_requested = false;
    ConstructorBehavior constructor_behavior = ConstructorBehavior::kAllow;
    bool instantiated = false;
    void Mark(JSRuntime* rt,JS_MarkFunc* mark,void* userdata) const {
        for (const Root* root : {&instance,&prototype,&parent,&provider,&signature,&callback_data,&class_name,&interface_name,&access_data,&cache_property,&named_data,&indexed_data,&denied_named_data,&denied_indexed_data}) root->Mark(rt,mark,userdata);
        for (const auto& entry : entries) {
            entry.key.Mark(rt,mark,userdata); entry.data.Mark(rt,mark,userdata); entry.getter.Mark(rt,mark,userdata); entry.setter.Mark(rt,mark,userdata);
            entry.native_property.key.Mark(rt,mark,userdata); entry.native_property.data.Mark(rt,mark,userdata);
        }
    }
};
struct Field {
    Root value;
    void* pointer = nullptr;
    bool is_pointer = false;
};
struct NativeState {
    Isolate* isolate = nullptr;
    JSContext* origin = nullptr; // Borrowed; the engine traces JSObject.obj_ctx.
    // Context metadata is resolved through Core's weak map, never owned or marked here.
    Root properties, brand, function_template, object_template, callback_data, global_target;
    std::shared_ptr<TemplatePayload> template_payload;
    std::vector<Field> fields;
    // Keys borrow atom references from matching descriptors in properties. Insertions
    // follow descriptor definition, and the map is destroyed/cleared before properties.
    std::unordered_map<JSAtom,NativeProperty> native_properties;
    NamedPropertyHandlerConfiguration named,denied_named;
    IndexedPropertyHandlerConfiguration indexed,denied_indexed;
    Root named_data,indexed_data,denied_named_data,denied_indexed_data,access_data;
    AccessCheckCallback access_check = nullptr;
    FunctionCallback callback = nullptr;
    void* wrapper = nullptr;
    CppHeapPointerTag wrapper_tag = CppHeapPointerTag::kNullTag;
    bool undetectable = false,code_like = false,accept_any_receiver = true,immutable = false;
    bool detached = false;
    int identity_hash = 0;
};
struct ContextPayload : detail::DataPayload {
    JSContext* context = nullptr; // Borrowed; strong API Cells, not metadata, pin it.
    Root security_token,default_token,extras,global_proxy,global_inner,code_generation_error;
    std::vector<Field> fields;
    std::vector<std::pair<Root,Root>> functions;
    std::vector<std::pair<std::string,Root>> builtins;
    MicrotaskQueue* microtask_queue = nullptr;
    bool code_generation_allowed = true;
    void Mark(JSRuntime* runtime,JS_MarkFunc* mark,void* userdata) const {
        for (const Root* root : {&security_token,&default_token,&extras,&global_proxy,&global_inner,&code_generation_error}) root->Mark(runtime,mark,userdata);
        for (const auto& field : fields) field.value.Mark(runtime,mark,userdata);
        for (const auto& entry : functions) { entry.first.Mark(runtime,mark,userdata); entry.second.Mark(runtime,mark,userdata); }
        for (const auto& entry : builtins) entry.second.Mark(runtime,mark,userdata);
    }
};
JSClassID host_class = 0,callable_class = 0,template_class = 0;
NativeState* State(JSValueConst value) {
    if (!JS_IsObject(value)) return nullptr;
    for (JSClassID id : {host_class,callable_class,template_class}) if (id) if (auto* state = static_cast<NativeState*>(JS_GetOpaque(value,id))) return state;
    return nullptr;
}
NativeState* PropertyState(JSValueConst value) {
    auto* state = State(value);
    return state && !JS_IsUndefined(state->global_target.value) ? State(state->global_target.value) : state;
}
std::shared_ptr<TemplatePayload> TemplateData(const Data* data) { return std::static_pointer_cast<TemplatePayload>(detail::Payload(data)); }
TemplatePayload* TemplateOf(JSValueConst value) { auto* state = State(value); return state ? state->template_payload.get() : nullptr; }
std::shared_ptr<ContextPayload> Realm(const Data* data) { return std::static_pointer_cast<ContextPayload>(detail::Payload(data)); }
Local<Value> View(JSContext* context,JSValueConst value) { return detail::Adopt(context,JS_DupValue(context,value)); }
Local<Object> ObjectView(JSContext* context,JSValueConst value) {
    if (!JS_IsObject(value)) return {};
    return View(context,value).As<Object>();
}
bool Same(JSContext* context,JSValueConst a,JSValueConst b) { return JS_IsValueEq(context,a,b,true); }
int Own(JSContext* context,JSPropertyDescriptor* desc,JSValueConst object,JSAtom atom) {
    auto* state = PropertyState(object);
    return JS_GetOwnProperty(context,desc,state && !state->template_payload ? state->properties.value : object,atom);
}
bool Index(JSContext* context,JSAtom atom,uint32_t* result) {
    JSValue key = JS_AtomToValue(context,atom);
    if (!JS_IsString(key)) { JS_FreeValue(context,key); return false; }
    size_t length = 0;
    const char* text = JS_ToCStringLen(context,&length,key);
    bool valid = text && length && length <= 10 && (length == 1 || text[0] != '0');
    uint64_t value = 0;
    if (valid) for (size_t i = 0;i < length;++i) { if (text[i] < '0' || text[i] > '9') { valid = false; break; } value = value * 10 + unsigned(text[i] - '0'); }
    valid = valid && value < std::numeric_limits<uint32_t>::max();
    if (text) JS_FreeCString(context,key,text);
    JS_FreeValue(context,key);
    if (valid) *result = static_cast<uint32_t>(value);
    return valid;
}
NativeProperty* FindNative(NativeState* state,JSAtom atom) {
    auto found = state->native_properties.find(atom);
    return found == state->native_properties.end() ? nullptr : &found->second;
}
void StoreNative(NativeState* state,JSAtom atom,NativeProperty property) {
    state->native_properties.insert_or_assign(atom,std::move(property));
}
bool Matches(JSContext* context,JSValueConst value,JSValueConst wanted) {
    auto* expected = TemplateOf(wanted);
    if (!expected || !expected->function) return false;
    auto* state = State(value);
    if (!state) return false;
    JSValue current = JS_DupValue(context,state->brand.value);
    while (auto* metadata = TemplateOf(current)) {
        if (Same(context,current,wanted)) { JS_FreeValue(context,current); return true; }
        JSValue next = JS_DupValue(context,metadata->parent.value);
        JS_FreeValue(context,current); current = next;
    }
    JS_FreeValue(context,current); return false;
}
int Allowed(JSContext* context,JSValueConst object,NativeState* state) {
    if (!state->access_check || context == state->origin) return 1;
    auto* accessing = detail::LookupContext(context);
    auto* owning = detail::LookupContext(state->origin);
    if (accessing && owning) {
        auto a = Realm(accessing), b = Realm(owning);
        if (a && b && Same(context,a->security_token.value,b->security_token.value)) return 1;
    }
    if (!accessing) { JS_ThrowTypeError(context,"Accessing realm is not registered"); return -1; }
    detail::CallbackState callback_frame(state->isolate,context);
    bool allowed = state->access_check(Local<Context>(accessing),ObjectView(context,object),View(context,state->access_data.value));
    if (detail::HasPendingException(state->isolate)) { JS_Throw(context,detail::TakePendingException(state->isolate)); return -1; }
    return allowed ? 1 : 0;
}
struct Handler {
    NativeState* state;
    bool denied = false,indexed = false,skip = false;
    uint32_t index = 0;
    const NamedPropertyHandlerConfiguration* named = nullptr;
    const IndexedPropertyHandlerConfiguration* indices = nullptr;
    JSValueConst data = JS_UNDEFINED;
};
int HasReal(JSContext* context,JSValueConst object,JSAtom atom,bool prototypes) {
    JSValue current = JS_DupValue(context,object);
    int result = 0;
    while (JS_IsObject(current)) {
        result = Own(context,nullptr,current,atom);
        if (result || !prototypes) break;
        JSValue next = JS_GetPrototypeForEmbedding(context,current); JS_FreeValue(context,current); current = next;
        if (JS_IsException(current)) { result = -1; break; }
    }
    JS_FreeValue(context,current); return result;
}
Handler Select(JSContext* context,JSValueConst object,JSAtom atom,NativeState* state,int allowed) {
    Handler result{state}; result.denied = allowed == 0;
    result.indexed = Index(context,atom,&result.index);
    result.named = result.denied ? &state->denied_named : &state->named;
    result.indices = result.denied ? &state->denied_indexed : &state->indexed;
    result.data = result.indexed ? (result.denied ? state->denied_indexed_data.value : state->indexed_data.value) : (result.denied ? state->denied_named_data.value : state->named_data.value);
    if (!result.indexed) {
        int flags = static_cast<int>(result.named->flags);
        JSValue key = JS_AtomToValue(context,atom);
        result.skip = (flags & int(PropertyHandlerFlags::kOnlyInterceptStrings)) && JS_IsSymbol(key);
        JS_FreeValue(context,key);
        if (!result.skip && (flags & int(PropertyHandlerFlags::kNonMasking)) && allowed) result.skip = HasReal(context,object,atom,true) != 0;
    }
    return result;
}
void Setup(detail::CallbackState& callback,JSValueConst receiver,JSValueConst holder,JSValueConst data,bool should_throw = false,bool enter_holder = true) {
    if (auto* holder_state = enter_holder ? PropertyState(holder) : nullptr) {
        if (auto* owner = detail::LookupContext(holder_state->origin)) {
            if (!callback.entered_context.IsEmpty()) detail::LeaveContext(callback.isolate,*callback.entered_context);
            callback.entered_context = Local<Context>(owner);
            detail::EnterContext(callback.isolate,owner);
        }
    }
    callback.receiver = ObjectView(callback.context,receiver);
    callback.holder = ObjectView(callback.context,holder);
    callback.data = View(callback.context,data);
    callback.should_throw = should_throw;
}
bool Pending(detail::CallbackState& callback) {
    if (!detail::HasPendingException(callback.isolate)) return false;
    JS_Throw(callback.context,detail::TakePendingException(callback.isolate)); return true;
}
JSValue AccessDenied(JSContext* context) { return JS_ThrowTypeError(context,"Blocked cross-origin access"); }
int Denied(JSContext* context) { AccessDenied(context); return -1; }
int SetFailure(JSContext*,int);
int HostOwn(JSContext*,JSPropertyDescriptor*,JSValueConst,JSAtom);
int HostNames(JSContext*,JSPropertyEnum**,uint32_t*,JSValueConst);
int HostDelete(JSContext*,JSValueConst,JSAtom,int);
int HostDefine(JSContext*,JSValueConst,JSAtom,JSValueConst,JSValueConst,JSValueConst,int);
int HostHas(JSContext*,JSValueConst,JSAtom);
JSValue HostGet(JSContext*,JSValueConst,JSAtom,JSValueConst);
int HostSet(JSContext*,JSValueConst,JSAtom,JSValueConst,JSValueConst,int);
int SetHostProperty(JSContext*,JSValueConst,JSAtom,JSValueConst,JSValueConst,int,bool);
JSValue HostGetPrototype(JSContext*,JSValueConst);
int HostSetPrototype(JSContext*,JSValueConst,JSValueConst,int);
int HostIsExtensible(JSContext*,JSValueConst);
int HostPreventExtensions(JSContext*,JSValueConst);
JSValue NativeCall(JSContext*,JSValueConst,JSValueConst,int,JSValueConst*,int);
void Finalize(JSRuntime*,JSValue object) {
    auto* state = State(object);
    if (!state) return;
    if (state->wrapper) mini_ui::ClearQuickJSWrapper(state->isolate,object);
    delete state;
}
void Mark(JSRuntime* runtime,JSValueConst object,JS_MarkFunc* mark,void* userdata) {
    auto* state = State(object); if (!state) return;
    for (const Root* root : {&state->properties,&state->brand,&state->function_template,&state->object_template,&state->callback_data,&state->global_target,&state->named_data,&state->indexed_data,&state->denied_named_data,&state->denied_indexed_data,&state->access_data}) root->Mark(runtime,mark,userdata);
    for (const auto& field : state->fields) field.value.Mark(runtime,mark,userdata);
    for (const auto& property : state->native_properties) { property.second.key.Mark(runtime,mark,userdata); property.second.data.Mark(runtime,mark,userdata); }
    if (state->template_payload) state->template_payload->Mark(runtime,mark,userdata);
    if (state->wrapper) mini_ui::MarkQuickJSWrapper(state->isolate,object,mark);
}
bool EnsureClasses(JSContext* context) {
    static std::once_flag class_ids;
    std::call_once(class_ids,[] { JS_NewClassID(&host_class); JS_NewClassID(&callable_class); JS_NewClassID(&template_class); });
    JSRuntime* runtime = JS_GetRuntime(context);
    static JSClassExoticMethods exotic{HostOwn,HostNames,nullptr,HostDefine,HostHas,HostGet,HostSet,HostGetPrototype,HostSetPrototype,HostIsExtensible,HostPreventExtensions,HostDelete};
    for (auto id : {host_class,callable_class,template_class}) {
        if (JS_IsRegisteredClass(runtime,id)) continue;
        JSClassDef definition{};
        definition.class_name = id == template_class ? "DOMTemplate" : id == callable_class ? "Function" : "Object";
        definition.finalizer = Finalize; definition.gc_mark = Mark;
        if (id == callable_class) definition.call = NativeCall;
        if (id != template_class) definition.exotic = &exotic;
        if (JS_NewClass(runtime,id,&definition) < 0) return false;
    }
    return true;
}
JSValue SavedBuiltin(JSContext*,std::string_view);
JSValue BuiltinPrototype(JSContext* context,const char* name) {
    const char* cached_name = !std::strcmp(name,"Object") ? "Object.prototype"
        : !std::strcmp(name,"Function") ? "Function.prototype"
        : !std::strcmp(name,"Array") ? "Array.prototype"
        : !std::strcmp(name,"Error") ? "Error.prototype" : nullptr;
    JSValue saved = cached_name ? SavedBuiltin(context,cached_name) : JS_UNDEFINED;
    if (!JS_IsUndefined(saved)) return saved;
    JSValue global = JS_GetGlobalObject(context);
    JSValue constructor = JS_GetPropertyStr(context,global,name);
    JS_FreeValue(context,global);
    if (JS_IsException(constructor)) return constructor;
    JSValue prototype = JS_GetPropertyStr(context,constructor,"prototype"); JS_FreeValue(context,constructor); return prototype;
}
JSValue NewHost(JSContext* context,bool callable = false,JSValueConst prototype = JS_UNDEFINED) {
    if (!EnsureClasses(context)) return JS_EXCEPTION;
    JSValue proto = JS_IsUndefined(prototype) ? BuiltinPrototype(context,callable ? "Function" : "Object") : JS_DupValue(context,prototype);
    if (JS_IsException(proto)) return proto;
    JSValue object = JS_NewObjectProtoClass(context,proto,callable ? callable_class : host_class);
    JS_FreeValue(context,proto);
    if (JS_IsException(object)) return object;
    auto state = std::make_unique<NativeState>(); state->isolate = detail::IsolateFor(context); state->origin = context;
    JSValue storage = JS_NewObjectProto(context,JS_NULL);
    if (JS_IsException(storage)) { JS_FreeValue(context,object); return storage; }
    state->properties = Root(context,storage); JS_FreeValue(context,storage);
    if (callable) JS_SetConstructorBit(context,object,true);
    JS_SetOpaque(object,state.release()); return object;
}
bool IsWindowProxy(const NativeState* state) {
    return state->detached || !JS_IsUndefined(state->global_target.value);
}
JSValue HostGetPrototype(JSContext* context,JSValueConst object) {
    auto* state = State(object);
    if (state->detached) return JS_NULL;
    HandleScope scope(state->isolate);
    int allowed = Allowed(context,object,PropertyState(object));
    if (allowed < 0) return JS_EXCEPTION;
    if (!allowed) return IsWindowProxy(state) ? JS_NULL : AccessDenied(context);
    return JS_GetPrototypeForEmbedding(context,object);
}
int HostSetPrototype(JSContext* context,JSValueConst object,JSValueConst prototype,int flags) {
    auto* state = State(object);
    JSValue current = HostGetPrototype(context,object);
    if (JS_IsException(current)) return -1;
    bool unchanged = Same(context,current,prototype);
    JS_FreeValue(context,current);
    if (unchanged) return 1;
    if (IsWindowProxy(state) || state->immutable) return SetFailure(context,flags);
    int extensible = JS_IsExtensible(context,state->properties.value);
    if (extensible < 0) return -1;
    if (!extensible) return SetFailure(context,flags);
    if (!JS_ShouldThrowOnError(context,flags)) {
        // The trusted setter throws on cycles; Reflect must instead return false.
        current = JS_DupValue(context,prototype);
        while (JS_IsObject(current)) {
            if (Same(context,current,object)) {
                JS_FreeValue(context,current);
                return 0;
            }
            if (JS_IsProxy(current)) break;
            JSValue next = JS_GetPrototypeForEmbedding(context,current);
            JS_FreeValue(context,current); current = next;
            if (JS_IsException(current)) return -1;
        }
        JS_FreeValue(context,current);
    }
    return JS_SetPrototypeForEmbedding(context,object,prototype);
}
int HostIsExtensible(JSContext* context,JSValueConst object) {
    auto* state = State(object);
    if (IsWindowProxy(state)) return 1;
    HandleScope scope(state->isolate);
    int allowed = Allowed(context,object,state);
    if (allowed < 0) return -1;
    if (!allowed) return Denied(context);
    return JS_IsExtensible(context,state->properties.value);
}
int HostPreventExtensions(JSContext* context,JSValueConst object) {
    auto* state = State(object);
    if (IsWindowProxy(state)) return 0;
    HandleScope scope(state->isolate);
    int allowed = Allowed(context,object,state);
    if (allowed < 0) return -1;
    if (!allowed) return Denied(context);
    return JS_PreventExtensions(context,state->properties.value);
}
Local<Function> InstantiateFunction(JSContext*,JSValueConst);
JSValue InstantiateObject(JSContext*,JSValueConst,JSValueConst,JSValueConst);
int InstallEntries(JSContext*,JSValueConst,TemplatePayload*,TemplatePayload* most_derived = nullptr);
int CopyProperties(JSContext*,JSValueConst,JSValueConst,JSAtom replaced = JS_ATOM_NULL,JSValueConst replacement = JS_UNDEFINED);
void CopyConfiguration(NativeState* state,TemplatePayload* metadata) {
    state->fields.resize(metadata->internal_fields);
    state->callback = metadata->callback; state->callback_data = metadata->callback_data;
    state->named = metadata->named; state->named.data.Clear(); state->named_data = metadata->named_data;
    state->indexed = metadata->indexed; state->indexed.data.Clear(); state->indexed_data = metadata->indexed_data;
    state->denied_named = metadata->denied_named; state->denied_named.data.Clear(); state->denied_named_data = metadata->denied_named_data;
    state->denied_indexed = metadata->denied_indexed; state->denied_indexed.data.Clear(); state->denied_indexed_data = metadata->denied_indexed_data;
    state->access_check = metadata->access_check; state->access_data = metadata->access_data;
    state->undetectable = metadata->undetectable; state->code_like = metadata->code_like;
    state->immutable = metadata->immutable;
}
void CheckMutable(const std::shared_ptr<TemplatePayload>& metadata) { if (!metadata || metadata->instantiated) std::abort(); }
} // namespace

namespace detail {
CallbackState::CallbackState(Isolate* isolate,JSContext* context) : isolate(isolate), context(context) {
    isolate->Enter();
    EnterNativeCallback(isolate);
    if (auto* view = LookupContext(context)) { entered_context = Local<Context>(view); EnterContext(isolate,view); }
}
CallbackState::~CallbackState() {
    JS_FreeValue(context,result);
    if (!entered_context.IsEmpty()) LeaveContext(isolate,*entered_context);
    LeaveNativeCallback(isolate);
    isolate->Exit();
}
void CallbackState::SetResult(JSValue owned) {
    JS_FreeValue(context,result); result_set = true;
    if (JS_IsException(owned)) { result = JS_UNDEFINED; CaptureException(isolate,context); }
    else result = owned;
}
bool IsHostObject(JSContext*,JSValueConst value) { auto* state = State(value); return state && !state->template_payload && (state->wrapper || !state->fields.empty() || !JS_IsUndefined(state->brand.value)); }
Local<Context> ContextForQuickJS(JSContext* context) { auto* view = LookupContext(context); return view ? Local<Context>::New(IsolateOf(view),Local<Context>(view)) : Local<Context>(); }
JSContext* JSContextOf(Local<Context> context) { return context.IsEmpty() ? nullptr : ContextOf(*context); }
Local<Object> LegacyPropertyHolder(Local<Object> holder) {
    if (holder.IsEmpty()) return {};
    auto* state = State(Borrow(*holder));
    return state && !JS_IsUndefined(state->global_target.value) ? ObjectView(ContextOf(*holder),state->global_target.value) : holder;
}
void* UnwrapObject(Isolate* isolate,const Object* object,CppHeapPointerTagRange range) {
    if (!object || IsolateOf(object) != isolate) return nullptr;
    auto* state = State(Borrow(object));
    if (!state)
        return GetBuiltinWrapper(isolate, Borrow(object), range);
    if (state->isolate != isolate || !state->wrapper) return nullptr;
    auto tag = static_cast<uint16_t>(state->wrapper_tag), lower = static_cast<uint16_t>(range.lower_bound), upper = static_cast<uint16_t>(range.upper_bound);
    return lower <= upper && upper <= static_cast<uint16_t>(CppHeapPointerTag::kLastTag) && tag >= lower && tag <= upper ? state->wrapper : nullptr;
}
void WrapObject(Isolate* isolate,Object* object,void* pointer,CppHeapPointerTag tag) {
    if (!object || IsolateOf(object) != isolate) std::abort();
    if (static_cast<uint16_t>(tag) > static_cast<uint16_t>(CppHeapPointerTag::kLastTag)) std::abort();
    auto* state = State(Borrow(object));
    if (!state) {
        SetBuiltinWrapper(isolate, Borrow(object), pointer, tag);
        return;
    }
    if (state->template_payload) std::abort();
    if (state->wrapper) mini_ui::ClearQuickJSWrapper(isolate,Borrow(object));
    state->wrapper = pointer; state->wrapper_tag = pointer ? tag : CppHeapPointerTag::kNullTag;
    if (pointer) mini_ui::RegisterQuickJSWrapper(isolate,Borrow(object),static_cast<blink::ScriptWrappable*>(pointer));
}
} // namespace detail

PropertyDescriptor::PropertyDescriptor() = default;
PropertyDescriptor::PropertyDescriptor(Local<Value> value) { private_.value = value; if (!value.IsEmpty()) private_.flags |= JS_PROP_HAS_VALUE; }
PropertyDescriptor::PropertyDescriptor(Local<Value> value,bool writable) : PropertyDescriptor(value) { private_.flags |= JS_PROP_HAS_WRITABLE | (writable ? JS_PROP_WRITABLE : 0); }
PropertyDescriptor::PropertyDescriptor(Local<Value> getter,Local<Value> setter) { private_.getter = getter; private_.setter = setter; if (!getter.IsEmpty()) private_.flags |= JS_PROP_HAS_GET; if (!setter.IsEmpty()) private_.flags |= JS_PROP_HAS_SET; }
PropertyDescriptor::~PropertyDescriptor() = default;
Local<Value> PropertyDescriptor::value() const { return private_.value; }
Local<Value> PropertyDescriptor::get() const { return private_.getter; }
Local<Value> PropertyDescriptor::set() const { return private_.setter; }
bool PropertyDescriptor::has_value() const { return private_.flags & JS_PROP_HAS_VALUE; }
bool PropertyDescriptor::has_get() const { return private_.flags & JS_PROP_HAS_GET; }
bool PropertyDescriptor::has_set() const { return private_.flags & JS_PROP_HAS_SET; }
void PropertyDescriptor::set_enumerable(bool value) { private_.flags = (private_.flags & ~JS_PROP_ENUMERABLE) | JS_PROP_HAS_ENUMERABLE | (value ? JS_PROP_ENUMERABLE : 0); }
bool PropertyDescriptor::enumerable() const { return private_.flags & JS_PROP_ENUMERABLE; }
bool PropertyDescriptor::has_enumerable() const { return private_.flags & JS_PROP_HAS_ENUMERABLE; }
void PropertyDescriptor::set_configurable(bool value) { private_.flags = (private_.flags & ~JS_PROP_CONFIGURABLE) | JS_PROP_HAS_CONFIGURABLE | (value ? JS_PROP_CONFIGURABLE : 0); }
bool PropertyDescriptor::configurable() const { return private_.flags & JS_PROP_CONFIGURABLE; }
bool PropertyDescriptor::has_configurable() const { return private_.flags & JS_PROP_HAS_CONFIGURABLE; }
bool PropertyDescriptor::writable() const { return private_.flags & JS_PROP_WRITABLE; }
bool PropertyDescriptor::has_writable() const { return private_.flags & JS_PROP_HAS_WRITABLE; }

namespace {
JSValue DescriptorObject(JSContext* context,const JSPropertyDescriptor& descriptor) {
    JSValue result = JS_NewObject(context);
    if (JS_IsException(result)) return result;
    int status = 1;
    if (descriptor.flags & JS_PROP_GETSET) {
        status = JS_DefinePropertyValueStr(context,result,"get",JS_DupValue(context,descriptor.getter),JS_PROP_C_W_E);
        if (status >= 0) status = JS_DefinePropertyValueStr(context,result,"set",JS_DupValue(context,descriptor.setter),JS_PROP_C_W_E);
    } else {
        status = JS_DefinePropertyValueStr(context,result,"value",JS_DupValue(context,descriptor.value),JS_PROP_C_W_E);
        if (status >= 0) status = JS_DefinePropertyValueStr(context,result,"writable",JS_NewBool(context,descriptor.flags & JS_PROP_WRITABLE),JS_PROP_C_W_E);
    }
    if (status >= 0) status = JS_DefinePropertyValueStr(context,result,"enumerable",JS_NewBool(context,descriptor.flags & JS_PROP_ENUMERABLE),JS_PROP_C_W_E);
    if (status >= 0) status = JS_DefinePropertyValueStr(context,result,"configurable",JS_NewBool(context,descriptor.flags & JS_PROP_CONFIGURABLE),JS_PROP_C_W_E);
    if (status < 0) { JS_FreeValue(context,result); return JS_EXCEPTION; }
    return result;
}
int ReadDescriptor(JSContext* context,JSValueConst object,JSPropertyDescriptor* descriptor) {
    if (!JS_IsObject(object)) { JS_ThrowTypeError(context,"Property descriptor is not an object"); return -1; }
    *descriptor = {0,JS_UNDEFINED,JS_UNDEFINED,JS_UNDEFINED};
    for (const char* key : {"enumerable","configurable","value","writable","get","set"}) {
        JSAtom atom = JS_NewAtom(context,key); int present = JS_HasProperty(context,object,atom);
        if (present < 0) { JS_FreeAtom(context,atom); FreeDescriptor(context,*descriptor); return -1; }
        if (!present) { JS_FreeAtom(context,atom); continue; }
        JSValue value = JS_GetProperty(context,object,atom); JS_FreeAtom(context,atom);
        if (JS_IsException(value)) { FreeDescriptor(context,*descriptor); return -1; }
        if (!std::strcmp(key,"value")) { descriptor->value = value; descriptor->flags |= JS_PROP_HAS_VALUE; continue; }
        if (!std::strcmp(key,"get") || !std::strcmp(key,"set")) {
            if (!JS_IsUndefined(value) && !JS_IsFunction(context,value)) { JS_FreeValue(context,value); FreeDescriptor(context,*descriptor); JS_ThrowTypeError(context,"Accessor must be callable"); return -1; }
            if (key[0] == 'g') { descriptor->getter = value; descriptor->flags |= JS_PROP_HAS_GET; }
            else { descriptor->setter = value; descriptor->flags |= JS_PROP_HAS_SET; }
            descriptor->flags |= JS_PROP_GETSET; continue;
        }
        int bit = !std::strcmp(key,"enumerable") ? JS_PROP_ENUMERABLE : !std::strcmp(key,"configurable") ? JS_PROP_CONFIGURABLE : JS_PROP_WRITABLE;
        descriptor->flags |= bit << JS_PROP_HAS_SHIFT;
        if (JS_ToBool(context,value)) descriptor->flags |= bit;
        JS_FreeValue(context,value);
    }
    if ((descriptor->flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) && (descriptor->flags & (JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE))) { FreeDescriptor(context,*descriptor); JS_ThrowTypeError(context,"Mixed property descriptor"); return -1; }
    return 1;
}
int InterceptOwn(JSContext* context,JSPropertyDescriptor* descriptor,JSValueConst object,JSAtom atom,Handler& handler) {
    if (handler.skip) return 0;
    const bool has_handler = handler.indexed
        ? ((descriptor && handler.indices->descriptor) || handler.indices->query || handler.indices->getter)
        : ((descriptor && handler.named->descriptor) || handler.named->query || handler.named->getter);
    if (!has_handler) return 0;
    detail::CallbackState callback(handler.state->isolate,context); Setup(callback,object,object,handler.data);
    Local<Name> key;
    if (!handler.indexed) key = detail::Adopt(context,JS_AtomToValue(context,atom)).As<Name>();
    Intercepted intercepted = Intercepted::kNo;
    if (descriptor && handler.indexed && handler.indices->descriptor) intercepted = handler.indices->descriptor(handler.index,PropertyCallbackInfo<Value>(callback));
    else if (descriptor && !handler.indexed && handler.named->descriptor) intercepted = handler.named->descriptor(key,PropertyCallbackInfo<Value>(callback));
    if (Pending(callback)) return -1;
    if (intercepted == Intercepted::kYes) {
        JSPropertyDescriptor temporary;
        if (ReadDescriptor(context,callback.result,&temporary) < 0) return -1;
        if (descriptor) *descriptor = temporary; else FreeDescriptor(context,temporary);
        return 1;
    }
    if (handler.indexed && handler.indices->query) intercepted = handler.indices->query(handler.index,PropertyCallbackInfo<Integer>(callback));
    else if (!handler.indexed && handler.named->query) intercepted = handler.named->query(key,PropertyCallbackInfo<Integer>(callback));
    if (Pending(callback)) return -1;
    if (intercepted == Intercepted::kYes) {
        int32_t attributes = 0; if (JS_ToInt32(context,&attributes,callback.result) < 0) return -1;
        if (descriptor) {
            callback.SetResult(JS_UNDEFINED);
            if (handler.indexed && handler.indices->getter) handler.indices->getter(handler.index,PropertyCallbackInfo<Value>(callback));
            else if (!handler.indexed && handler.named->getter) handler.named->getter(key,PropertyCallbackInfo<Value>(callback));
            if (Pending(callback)) return -1;
            *descriptor = {Attributes(static_cast<PropertyAttribute>(attributes)),JS_DupValue(context,callback.result),JS_UNDEFINED,JS_UNDEFINED};
        }
        return 1;
    }
    if (handler.indexed && handler.indices->getter) intercepted = handler.indices->getter(handler.index,PropertyCallbackInfo<Value>(callback));
    else if (!handler.indexed && handler.named->getter) intercepted = handler.named->getter(key,PropertyCallbackInfo<Value>(callback));
    if (Pending(callback)) return -1;
    if (intercepted == Intercepted::kYes) {
        if (descriptor) *descriptor = {JS_PROP_C_W_E,JS_DupValue(context,callback.result),JS_UNDEFINED,JS_UNDEFINED};
        return 1;
    }
    return 0;
}
JSValue NativeGet(JSContext*,JSValueConst,JSValueConst,JSAtom,const NativeProperty&);
int HostOwn(JSContext* context,JSPropertyDescriptor* descriptor,JSValueConst object,JSAtom atom) {
    HandleScope scope(detail::IsolateFor(context));
    if (State(object)->detached) return Denied(context);
    auto* state = PropertyState(object); int allowed = Allowed(context,object,state); if (allowed < 0) return -1;
    Handler handler = Select(context,object,atom,state,allowed);
    int intercepted = InterceptOwn(context,descriptor,object,atom,handler);
    if (intercepted) return intercepted;
    if (!allowed) return Denied(context);
    int found = JS_GetOwnProperty(context,descriptor,state->properties.value,atom);
    if (found > 0 && descriptor) if (auto* property = FindNative(state,atom)) if (property->getter) {
        JSValue value = NativeGet(context,object,object,atom,*property);
        if (JS_IsException(value)) { FreeDescriptor(context,*descriptor); return -1; }
        JS_FreeValue(context,descriptor->value); descriptor->value = value;
    }
    return found;
}
JSValue NativeGet(JSContext* context,JSValueConst object,JSValueConst receiver,JSAtom atom,const NativeProperty& property) {
    auto* state = PropertyState(object);
    // The callback may erase, replace, or rehash its map entry. Snapshot every scalar
    // needed afterward; Setup and key pin roots, and stable_atom pins a lazy key.
    auto getter = property.getter;
    bool lazy = property.lazy;
    uint64_t generation = property.generation;
    JSAtom stable_atom = lazy ? JS_DupAtom(context,atom) : atom;
    detail::CallbackState callback(state->isolate,context); Setup(callback,receiver,object,property.data.value);
    auto key = View(context,property.key.value).As<Name>();
    assert(getter);
    getter(key,PropertyCallbackInfo<Value>(callback));
    if (Pending(callback)) {
        if (lazy) JS_FreeAtom(context,stable_atom);
        return JS_EXCEPTION;
    }
    if (lazy) {
        auto* current = FindNative(state,stable_atom);
        if (current && current->generation == generation) {
            int result;
            PropertyAttribute attributes = current->attributes;
            if ((attributes & (ReadOnly | DontDelete)) == (ReadOnly | DontDelete)) {
                JSValue replacement = JS_NewObjectProto(context,JS_NULL);
                if (JS_IsException(replacement)) { JS_FreeAtom(context,stable_atom); return replacement; }
                result = CopyProperties(context,state->properties.value,replacement,stable_atom,callback.result);
                if (result >= 0) {
                    int extensible = JS_IsExtensible(context,state->properties.value);
                    if (extensible < 0) result = -1;
                    else if (!extensible) result = JS_PreventExtensions(context,replacement);
                }
                if (result >= 0) state->properties = Root(context,replacement);
                JS_FreeValue(context,replacement);
            } else result = JS_DefineProperty(context,state->properties.value,stable_atom,callback.result,JS_UNDEFINED,JS_UNDEFINED,Attributes(attributes) | JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE | JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_CONFIGURABLE);
            if (result < 0) { JS_FreeAtom(context,stable_atom); return JS_EXCEPTION; }
            current = FindNative(state,stable_atom);
            if (current && current->generation == generation) { current->getter = nullptr; current->setter = nullptr; current->lazy = false; }
        }
        JS_FreeAtom(context,stable_atom);
    }
    return JS_DupValue(context,callback.result);
}
JSValue HostGet(JSContext* context,JSValueConst object,JSAtom atom,JSValueConst receiver) {
    HandleScope scope(detail::IsolateFor(context));
    if (State(object)->detached) return AccessDenied(context);
    auto* state = PropertyState(object); int allowed = Allowed(context,object,state); if (allowed < 0) return JS_EXCEPTION;
    Handler handler = Select(context,object,atom,state,allowed);
    bool has_getter = handler.indexed ? handler.indices->getter != nullptr : handler.named->getter != nullptr;
    if (!handler.skip && has_getter) {
        detail::CallbackState callback(state->isolate,context); Setup(callback,receiver,object,handler.data);
        Intercepted intercepted;
        if (handler.indexed) intercepted = handler.indices->getter(handler.index,PropertyCallbackInfo<Value>(callback));
        else {
            auto key = detail::Adopt(context,JS_AtomToValue(context,atom)).As<Name>();
            intercepted = handler.named->getter(key,PropertyCallbackInfo<Value>(callback));
        }
        if (Pending(callback)) return JS_EXCEPTION;
        if (intercepted == Intercepted::kYes) return JS_DupValue(context,callback.result);
    }
    if (!allowed) return AccessDenied(context);
    if (auto* property = FindNative(state,atom)) if (property->getter) return NativeGet(context,object,receiver,atom,*property);
    JSPropertyDescriptor descriptor;
    int found = JS_GetOwnProperty(context,&descriptor,state->properties.value,atom);
    if (found < 0) return JS_EXCEPTION;
    if (found) {
        JSValue result;
        if (descriptor.flags & JS_PROP_GETSET) result = JS_IsUndefined(descriptor.getter) ? JS_UNDEFINED : JS_Call(context,descriptor.getter,receiver,0,nullptr);
        else result = JS_DupValue(context,descriptor.value);
        FreeDescriptor(context,descriptor); return result;
    }
    JSValue prototype = JS_GetPrototypeForEmbedding(context,object);
    if (JS_IsException(prototype)) return prototype;
    JSValue result = JS_IsNull(prototype) ? JS_UNDEFINED : JS_GetPropertyInternal(context,prototype,atom,receiver,0);
    JS_FreeValue(context,prototype); return result;
}
int HostHas(JSContext* context,JSValueConst object,JSAtom atom) {
    HandleScope scope(detail::IsolateFor(context));
    if (State(object)->detached) return Denied(context);
    auto* state = PropertyState(object); int allowed = Allowed(context,object,state); if (allowed < 0) return -1;
    Handler handler = Select(context,object,atom,state,allowed);
    int result = InterceptOwn(context,nullptr,object,atom,handler); if (result) return result;
    if (!allowed) return Denied(context);
    result = JS_GetOwnProperty(context,nullptr,state->properties.value,atom); if (result) return result;
    JSValue prototype = JS_GetPrototypeForEmbedding(context,object); if (JS_IsException(prototype)) return -1;
    result = JS_IsNull(prototype) ? 0 : JS_HasProperty(context,prototype,atom); JS_FreeValue(context,prototype); return result;
}
int HostDelete(JSContext* context,JSValueConst object,JSAtom atom,int flags) {
    HandleScope scope(detail::IsolateFor(context));
    if (State(object)->detached) return Denied(context);
    auto* state = PropertyState(object); int allowed = Allowed(context,object,state); if (allowed < 0) return -1;
    Handler handler = Select(context,object,atom,state,allowed);
    if (!handler.skip && (handler.indexed ? handler.indices->deleter != nullptr : handler.named->deleter != nullptr)) {
        detail::CallbackState callback(state->isolate,context); Setup(callback,object,object,handler.data,JS_ShouldThrowOnError(context,flags)); callback.result = JS_TRUE;
        Intercepted intercepted;
        if (handler.indexed) intercepted = handler.indices->deleter(handler.index,PropertyCallbackInfo<Boolean>(callback));
        else intercepted = handler.named->deleter(detail::Adopt(context,JS_AtomToValue(context,atom)).As<Name>(),PropertyCallbackInfo<Boolean>(callback));
        if (Pending(callback)) return -1;
        if (intercepted == Intercepted::kYes) return JS_ToBool(context,callback.result);
    }
    if (!allowed) return Denied(context);
    int result = JS_DeleteProperty(context,state->properties.value,atom,flags);
    if (result > 0) state->native_properties.erase(atom);
    return result;
}
int DefineHostProperty(JSContext* context,JSValueConst object,JSAtom atom,JSValueConst value,JSValueConst getter,JSValueConst setter,int flags,bool skip_setter) {
    HandleScope scope(detail::IsolateFor(context));
    if (State(object)->detached) return Denied(context);
    auto* state = PropertyState(object); int allowed = Allowed(context,object,state); if (allowed < 0) return -1;
    Handler handler = Select(context,object,atom,state,allowed);
    JSPropertyDescriptor current{0,JS_UNDEFINED,JS_UNDEFINED,JS_UNDEFINED};
    bool has_current = false;
    // V8 resolves the interceptor-visible current descriptor before invoking
    // the definer; query-backed descriptors therefore constrain fallback.
    const bool has_lookup = !handler.skip && (handler.indexed
        ? (handler.indices->descriptor || handler.indices->query || handler.indices->getter)
        : (handler.named->descriptor || handler.named->query || handler.named->getter));
    const bool has_setter = !skip_setter && !handler.skip && (handler.indexed
        ? handler.indices->setter != nullptr : handler.named->setter != nullptr);
    if (has_lookup) {
        int intercepted = InterceptOwn(context,&current,object,atom,handler);
        if (intercepted < 0) return -1;
        has_current = intercepted > 0;
    }
    if (!has_current && allowed && (flags & JS_PROP_HAS_VALUE)) {
        if (auto* property = FindNative(state,atom)) if (property->getter && (property->attributes & (ReadOnly | DontDelete)) == (ReadOnly | DontDelete)) {
            int found = JS_GetOwnProperty(context,&current,state->properties.value,atom);
            if (found < 0) { FreeDescriptor(context,current); return -1; }
            if (found) {
                JSValue visible = NativeGet(context,object,object,atom,*property);
                if (JS_IsException(visible)) { FreeDescriptor(context,current); return -1; }
                JS_FreeValue(context,current.value); current.value = visible;
                has_current = true;
            }
        }
    }
    if (!has_current && allowed && has_setter) {
        int found = JS_GetOwnProperty(context,&current,state->properties.value,atom);
        if (found < 0) { FreeDescriptor(context,current); return -1; }
        has_current = found > 0;
    }
    if (!handler.skip && (handler.indexed ? handler.indices->definer != nullptr : handler.named->definer != nullptr)) {
        detail::CallbackState callback(state->isolate,context); Setup(callback,object,object,handler.data,JS_ShouldThrowOnError(context,flags));
        PropertyDescriptor descriptor;
        auto* data = descriptor.get_private(); data->flags = flags;
        if (flags & JS_PROP_HAS_VALUE) data->value = View(context,value);
        if (flags & JS_PROP_HAS_GET) data->getter = View(context,getter);
        if (flags & JS_PROP_HAS_SET) data->setter = View(context,setter);
        Intercepted intercepted;
        if (handler.indexed) intercepted = handler.indices->definer(handler.index,descriptor,PropertyCallbackInfo<void>(callback));
        else intercepted = handler.named->definer(detail::Adopt(context,JS_AtomToValue(context,atom)).As<Name>(),descriptor,PropertyCallbackInfo<void>(callback));
        if (Pending(callback)) { if (has_current) FreeDescriptor(context,current); return -1; }
        if (intercepted == Intercepted::kYes) {
            if (has_current) FreeDescriptor(context,current);
            return JS_ToBool(context,callback.result) ? 1 : SetFailure(context,flags);
        }
    }
    if (!allowed) { if (has_current) FreeDescriptor(context,current); return Denied(context); }
    if (!has_current && !has_setter) {
        int result = JS_DefineProperty(context,state->properties.value,atom,value,getter,setter,flags & ~JS_PROP_NO_EXOTIC);
        if (result > 0) {
            if (flags & (JS_PROP_HAS_VALUE | JS_PROP_HAS_GET | JS_PROP_HAS_SET)) state->native_properties.erase(atom);
            else if (auto* property = FindNative(state,atom)) {
                JSPropertyDescriptor updated;
                if (JS_GetOwnProperty(context,&updated,state->properties.value,atom) > 0) { property->attributes = FromFlags(updated.flags); FreeDescriptor(context,updated); }
            }
        }
        return result;
    }
    // Validate against the callback-visible descriptor rather than the native
    // property's backing bookkeeping slot.
    JSValue validation = JS_NewObjectProto(context,JS_NULL);
    if (JS_IsException(validation)) { FreeDescriptor(context,current); return -1; }
    const bool current_accessor = has_current && (current.flags & JS_PROP_GETSET);
    int result = 1;
    if (has_current) {
        int current_flags = (current.flags & JS_PROP_C_W_E) |
            JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_CONFIGURABLE |
            (current_accessor ? JS_PROP_HAS_GET | JS_PROP_HAS_SET : JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE);
        result = JS_DefineProperty(context,validation,atom,current.value,current.getter,current.setter,current_flags);
    } else {
        int extensible = JS_IsExtensible(context,state->properties.value);
        if (extensible < 0) result = -1;
        else if (!extensible) result = JS_PreventExtensions(context,validation);
    }
    if (result > 0) result = JS_DefineProperty(context,validation,atom,value,getter,setter,flags & ~JS_PROP_NO_EXOTIC);
    JSPropertyDescriptor updated{0,JS_UNDEFINED,JS_UNDEFINED,JS_UNDEFINED};
    if (result > 0) result = JS_GetOwnProperty(context,&updated,validation,atom);
    JS_FreeValue(context,validation);
    if (result <= 0) { FreeDescriptor(context,updated); FreeDescriptor(context,current); return result; }
    // An explicit value on a mutable data property still reaches an
    // interceptor setter even when SameValue; only immutable data shortcuts it.
    if (has_current) {
        bool unchanged = !(flags & JS_PROP_HAS_VALUE);
        if (flags & JS_PROP_HAS_CONFIGURABLE)
            unchanged = unchanged && bool(flags & JS_PROP_CONFIGURABLE) == bool(current.flags & JS_PROP_CONFIGURABLE);
        if (flags & JS_PROP_HAS_ENUMERABLE)
            unchanged = unchanged && bool(flags & JS_PROP_ENUMERABLE) == bool(current.flags & JS_PROP_ENUMERABLE);
        if (flags & JS_PROP_HAS_WRITABLE)
            unchanged = unchanged && !current_accessor &&
                bool(flags & JS_PROP_WRITABLE) == bool(current.flags & JS_PROP_WRITABLE);
        if (flags & JS_PROP_HAS_GET)
            unchanged = unchanged && current_accessor &&
                View(context,getter)->SameValue(View(context,current.getter));
        if (flags & JS_PROP_HAS_SET)
            unchanged = unchanged && current_accessor &&
                View(context,setter)->SameValue(View(context,current.setter));
        if (!unchanged && !current_accessor &&
            (current.flags & (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)) == 0 &&
            (flags & JS_PROP_HAS_VALUE))
            unchanged = View(context,value)->SameValue(View(context,current.value));
        if (unchanged) {
            FreeDescriptor(context,updated); FreeDescriptor(context,current);
            return 1;
        }
    }
    const bool updated_accessor = updated.flags & JS_PROP_GETSET;
    // Applying a data descriptor gives an interceptor setter the first
    // opportunity to handle the write, without re-entering the definer.
    if (has_setter && !updated_accessor) {
        detail::CallbackState callback(state->isolate,context);
        Setup(callback,object,object,handler.data,JS_ShouldThrowOnError(context,flags));
        Intercepted intercepted;
        if (handler.indexed) intercepted = handler.indices->setter(handler.index,View(context,updated.value),PropertyCallbackInfo<void>(callback));
        else intercepted = handler.named->setter(detail::Adopt(context,JS_AtomToValue(context,atom)).As<Name>(),View(context,updated.value),PropertyCallbackInfo<void>(callback));
        if (Pending(callback)) {
            FreeDescriptor(context,updated); FreeDescriptor(context,current);
            return -1;
        }
        if (intercepted == Intercepted::kYes) {
            FreeDescriptor(context,updated); FreeDescriptor(context,current);
            return JS_ToBool(context,callback.result) ? 1 : SetFailure(context,flags);
        }
    }
    int updated_flags = (updated.flags & JS_PROP_C_W_E) |
        JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_CONFIGURABLE |
        (updated_accessor ? JS_PROP_HAS_GET | JS_PROP_HAS_SET : JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE) |
        (flags & (JS_PROP_THROW | JS_PROP_THROW_STRICT));
    result = JS_DefineProperty(context,state->properties.value,atom,updated.value,updated.getter,updated.setter,updated_flags);
    if (result > 0) {
        if (flags & (JS_PROP_HAS_VALUE | JS_PROP_HAS_GET | JS_PROP_HAS_SET))
            state->native_properties.erase(atom);
        else if (auto* property = FindNative(state,atom)) {
            JSPropertyDescriptor stored;
            if (JS_GetOwnProperty(context,&stored,state->properties.value,atom) > 0) { property->attributes = FromFlags(stored.flags); FreeDescriptor(context,stored); }
        }
    }
    FreeDescriptor(context,updated); FreeDescriptor(context,current);
    return result;
}
int HostDefine(JSContext* context,JSValueConst object,JSAtom atom,JSValueConst value,JSValueConst getter,JSValueConst setter,int flags) {
    return DefineHostProperty(context,object,atom,value,getter,setter,flags,false);
}
int SetFailure(JSContext* context,int flags) { if (JS_ShouldThrowOnError(context,flags)) { JS_ThrowTypeError(context,"Cannot assign to property"); return -1; } return 0; }
int OrdinarySet(JSContext* context,JSValueConst object,JSAtom atom,JSValueConst value,JSValueConst receiver,int flags,bool receiver_setter_checked) {
    JSPropertyDescriptor descriptor;
    int found = JS_GetOwnProperty(context,&descriptor,object,atom); if (found < 0) return -1;
    if (!found) {
        JSValue prototype = JS_GetPrototypeForEmbedding(context,object); if (JS_IsException(prototype)) return -1;
        if (!JS_IsNull(prototype)) {
            int result;
            if (State(prototype)) result = SetHostProperty(context,prototype,atom,value,receiver,flags,receiver_setter_checked);
            else result = OrdinarySet(context,prototype,atom,value,receiver,flags,receiver_setter_checked);
            JS_FreeValue(context,prototype); return result;
        }
        JS_FreeValue(context,prototype);
        descriptor = {JS_PROP_C_W_E,JS_UNDEFINED,JS_UNDEFINED,JS_UNDEFINED};
    }
    if (descriptor.flags & JS_PROP_GETSET) {
        if (JS_IsUndefined(descriptor.setter)) { FreeDescriptor(context,descriptor); return SetFailure(context,flags); }
        JSValue result = JS_Call(context,descriptor.setter,receiver,1,&value);
        FreeDescriptor(context,descriptor); if (JS_IsException(result)) return -1; JS_FreeValue(context,result); return 1;
    }
    bool writable = descriptor.flags & JS_PROP_WRITABLE; FreeDescriptor(context,descriptor);
    if (!writable || !JS_IsObject(receiver)) return SetFailure(context,flags);
    JSPropertyDescriptor own;
    found = JS_GetOwnProperty(context,&own,receiver,atom); if (found < 0) return -1;
    int define_flags;
    if (found) {
        bool rejected = (own.flags & JS_PROP_GETSET) || !(own.flags & JS_PROP_WRITABLE); FreeDescriptor(context,own);
        if (rejected) return SetFailure(context,flags);
        define_flags = JS_PROP_HAS_VALUE | (flags & (JS_PROP_THROW | JS_PROP_THROW_STRICT));
    } else define_flags = JS_PROP_C_W_E | JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE |
        JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_CONFIGURABLE | (flags & (JS_PROP_THROW | JS_PROP_THROW_STRICT));
    if (receiver_setter_checked && State(receiver))
        return DefineHostProperty(context,receiver,atom,value,JS_UNDEFINED,JS_UNDEFINED,define_flags,true);
    return JS_DefineProperty(context,receiver,atom,value,JS_UNDEFINED,JS_UNDEFINED,define_flags);
}
int SetHostProperty(JSContext* context,JSValueConst object,JSAtom atom,JSValueConst value,JSValueConst receiver,int flags,bool receiver_setter_checked) {
    HandleScope scope(detail::IsolateFor(context));
    if (State(object)->detached) return Denied(context);
    auto* state = PropertyState(object); int allowed = Allowed(context,object,state); if (allowed < 0) return -1;
    Handler handler = Select(context,object,atom,state,allowed);
    auto* receiver_state = State(receiver);
    bool receiver_is_holder = Same(context,object,receiver) || (receiver_state && Same(context,receiver_state->global_target.value,object));
    // Prototype interceptors contribute descriptors; only the receiver (or its
    // hidden inner Window) receives setter callbacks.
    if (!handler.skip && receiver_is_holder &&
        (handler.indexed ? handler.indices->setter != nullptr : handler.named->setter != nullptr)) {
        detail::CallbackState callback(state->isolate,context);
        Setup(callback,receiver,object,handler.data,JS_ShouldThrowOnError(context,flags));
        Intercepted intercepted;
        if (handler.indexed) intercepted = handler.indices->setter(handler.index,View(context,value),PropertyCallbackInfo<void>(callback));
        else intercepted = handler.named->setter(detail::Adopt(context,JS_AtomToValue(context,atom)).As<Name>(),View(context,value),PropertyCallbackInfo<void>(callback));
        if (Pending(callback)) return -1;
        if (intercepted == Intercepted::kYes) return JS_ToBool(context,callback.result) ? 1 : SetFailure(context,flags);
        receiver_setter_checked = true;
    }
    if (!allowed) return Denied(context);
    if (auto* property = FindNative(state,atom)) if (property->getter || property->setter) {
        if (property->lazy) return OrdinarySet(context,object,atom,value,receiver,flags,receiver_setter_checked);
        if ((property->attributes & ReadOnly) || !property->setter) return SetFailure(context,flags);
        auto setter = property->setter;
        auto key = View(context,property->key.value).As<Name>();
        detail::CallbackState callback(state->isolate,context);
        Setup(callback,receiver,object,property->data.value,JS_ShouldThrowOnError(context,flags));
        setter(key,View(context,value),PropertyCallbackInfo<void>(callback));
        if (Pending(callback)) return -1;
        return JS_ToBool(context,callback.result) ? 1 : SetFailure(context,flags);
    }
    return OrdinarySet(context,object,atom,value,receiver,flags,receiver_setter_checked);
}
int HostSet(JSContext* context,JSValueConst object,JSAtom atom,JSValueConst value,JSValueConst receiver,int flags) {
    return SetHostProperty(context,object,atom,value,receiver,flags,false);
}
int HostNames(JSContext* context,JSPropertyEnum** output,uint32_t* length,JSValueConst object) {
    HandleScope scope(detail::IsolateFor(context));
    *output = nullptr; *length = 0;
    if (State(object)->detached) return Denied(context);
    auto* state = PropertyState(object); int allowed = Allowed(context,object,state); if (allowed < 0) return -1;
    struct Key { JSAtom atom; uint32_t index; size_t ordinal; int kind; };
    std::vector<Key> keys;
    auto fail = [&] { for (const auto& key : keys) JS_FreeAtom(context,key.atom); return -1; };
    auto append = [&](JSAtom atom) {
        if (std::find_if(keys.begin(),keys.end(),[atom](const Key& key) { return key.atom == atom; }) != keys.end()) return 0;
        JSValue value = JS_AtomToValue(context,atom);
        if (JS_IsException(value)) return -1;
        int kind = JS_IsSymbol(value) ? 2 : 1; JS_FreeValue(context,value);
        uint32_t index = 0; if (kind == 1 && Index(context,atom,&index)) kind = 0;
        keys.push_back({JS_DupAtom(context,atom),index,keys.size(),kind}); return 0;
    };
    if (allowed) {
        JSPropertyEnum* own = nullptr; uint32_t count = 0;
        if (JS_GetOwnPropertyNames(context,&own,&count,state->properties.value,JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0) return -1;
        bool failed = false;
        for (uint32_t i = 0;i < count;++i) if (append(own[i].atom) < 0) { failed = true; break; }
        JS_FreePropEnum(context,own,count); if (failed) return fail();
    }
    const auto& named = allowed ? state->named : state->denied_named;
    const auto& indexed = allowed ? state->indexed : state->denied_indexed;
    for (bool indices : {true,false}) {
        auto enumerator = indices ? indexed.enumerator : named.enumerator;
        if (!enumerator) continue;
        detail::CallbackState callback(state->isolate,context);
        Setup(callback,object,object,indices ? (allowed ? state->indexed_data.value : state->denied_indexed_data.value) : (allowed ? state->named_data.value : state->denied_named_data.value));
        enumerator(PropertyCallbackInfo<Array>(callback));
        if (Pending(callback)) return fail();
        if (JS_IsUndefined(callback.result)) continue;
        int array = JS_IsArray(context,callback.result);
        if (array <= 0) { if (!array) JS_ThrowTypeError(context,"Interceptor enumerator must return an Array"); return fail(); }
        JSValue size = JS_GetPropertyStr(context,callback.result,"length"); uint32_t count = 0;
        int status = JS_IsException(size) ? -1 : JS_ToUint32(context,&count,size); JS_FreeValue(context,size);
        for (uint32_t i = 0;status >= 0 && i < count;++i) {
            JSValue key = JS_GetPropertyUint32(context,callback.result,i);
            if (JS_IsException(key)) { status = -1; break; }
            JSAtom atom = JS_ValueToAtom(context,key); JS_FreeValue(context,key);
            if (!atom) { status = -1; break; }
            status = append(atom); JS_FreeAtom(context,atom);
        }
        if (status < 0) return fail();
    }
    if (!allowed && !named.enumerator && !indexed.enumerator) { Denied(context); return fail(); }
    std::sort(keys.begin(),keys.end(),[](const Key& a,const Key& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.kind == 0 ? a.index < b.index : a.ordinal < b.ordinal;
    });
    auto* table = static_cast<JSPropertyEnum*>(js_malloc(context,std::max<size_t>(1,keys.size()) * sizeof(JSPropertyEnum)));
    if (!table) return fail();
    for (size_t i = 0;i < keys.size();++i) table[i] = {1,keys[i].atom};
    *output = table; *length = static_cast<uint32_t>(keys.size()); return 0;
}
} // namespace

namespace {
JSContext* CheckedContext(Local<Context> context,const Data* object = nullptr) {
    if (context.IsEmpty()) return nullptr;
    JSContext* raw = detail::ContextOf(*context);
    if (object && detail::IsolateOf(object) != detail::IsolateOf(*context)) {
        JS_ThrowTypeError(raw,"Object belongs to a different runtime");
        detail::CaptureException(detail::IsolateOf(*context),raw); return nullptr;
    }
    return raw;
}
struct Atom {
    JSContext* context;
    JSAtom value;
    Atom(JSContext* context,JSValueConst key) : context(context),value(JS_ValueToAtom(context,key)) {}
    ~Atom() { JS_FreeAtom(context,value); }
};
JSValue Reflect(JSContext* context,const char* method,int argc,JSValueConst* argv) {
    JSValue reflect = SavedBuiltin(context,"Reflect");
    JSValue function = SavedBuiltin(context,std::string("Reflect.") + method);
    JSValue result = JS_Call(context,function,reflect,argc,argv);
    JS_FreeValue(context,function); JS_FreeValue(context,reflect); return result;
}
JSValue ObjectBuiltin(JSContext* context,const char* method,int argc,JSValueConst* argv) {
    JSValue constructor = SavedBuiltin(context,"Object");
    JSValue function = SavedBuiltin(context,std::string("Object.") + method);
    JSValue result = JS_Call(context,function,constructor,argc,argv);
    JS_FreeValue(context,function); JS_FreeValue(context,constructor); return result;
}
MaybeLocal<Value> CallObject(JSContext* context,JSValueConst function,JSValueConst receiver,int argc,Local<Value> argv[],bool construct) {
    auto* isolate = detail::IsolateFor(context);
    Isolate::Scope isolate_scope(isolate);
    auto realm = detail::ContextForQuickJS(context); if (realm.IsEmpty()) return {};
    Context::Scope entered(realm); detail::ExecutionScope execution(isolate);
    std::array<JSValue,16> local_arguments;
    std::vector<JSValue> overflow;
    JSValue* arguments = local_arguments.data();
    if (argc > int(local_arguments.size())) { overflow.resize(argc); arguments = overflow.data(); }
    for (int i = 0;i < argc;++i) arguments[i] = Raw(argv[i]);
    return detail::Adopt(context,construct ? JS_CallConstructor(context,function,argc,arguments) : JS_Call(context,function,receiver,argc,arguments));
}
MaybeLocal<Value> RealGet(JSContext* context,JSValueConst object,JSAtom atom,bool start_at_prototype) {
    JSValue current = start_at_prototype ? JS_GetPrototypeForEmbedding(context,object) : JS_DupValue(context,object);
    while (JS_IsObject(current)) {
        JSPropertyDescriptor descriptor;
        int found = Own(context,&descriptor,current,atom);
        if (found < 0) { JS_FreeValue(context,current); detail::CaptureException(detail::IsolateFor(context),context); return {}; }
        if (found) {
            JSValue value;
            if (auto* state = State(current)) {
                auto* native = FindNative(state,atom);
                if (native && native->getter) value = NativeGet(context,current,object,atom,*native);
                else value = (descriptor.flags & JS_PROP_GETSET) ? (JS_IsUndefined(descriptor.getter) ? JS_UNDEFINED : JS_Call(context,descriptor.getter,object,0,nullptr)) : JS_DupValue(context,descriptor.value);
            } else value = (descriptor.flags & JS_PROP_GETSET) ? (JS_IsUndefined(descriptor.getter) ? JS_UNDEFINED : JS_Call(context,descriptor.getter,object,0,nullptr)) : JS_DupValue(context,descriptor.value);
            FreeDescriptor(context,descriptor); JS_FreeValue(context,current); return detail::Adopt(context,value);
        }
        JSValue next = JS_GetPrototypeForEmbedding(context,current); JS_FreeValue(context,current); current = next;
    }
    if (JS_IsException(current)) detail::CaptureException(detail::IsolateFor(context),context);
    JS_FreeValue(context,current); return {};
}
Maybe<PropertyAttribute> RealAttributes(JSContext* context,JSValueConst object,JSAtom atom,bool start_at_prototype) {
    JSValue current = start_at_prototype ? JS_GetPrototypeForEmbedding(context,object) : JS_DupValue(context,object);
    while (JS_IsObject(current)) {
        JSPropertyDescriptor descriptor;
        int found = Own(context,&descriptor,current,atom);
        if (found < 0) { JS_FreeValue(context,current); detail::CaptureException(detail::IsolateFor(context),context); return Nothing<PropertyAttribute>(); }
        if (found) { auto result = FromFlags(descriptor.flags); FreeDescriptor(context,descriptor); JS_FreeValue(context,current); return Just(result); }
        JSValue next = JS_GetPrototypeForEmbedding(context,current); JS_FreeValue(context,current); current = next;
    }
    if (JS_IsException(current)) detail::CaptureException(detail::IsolateFor(context),context);
    JS_FreeValue(context,current); return Nothing<PropertyAttribute>();
}
}

Local<Object> Object::New(Isolate* isolate) {
    JSContext* context = detail::CurrentJSContext(isolate);
    return detail::Adopt(context,NewHost(context)).As<Object>();
}
Local<Object> Object::New(Isolate* isolate,Local<Value> prototype,Local<Name>* keys,Local<Value>* values,size_t count) {
    JSContext* context = detail::CurrentJSContext(isolate);
    JSValue proto = JS_IsObject(Raw(prototype)) || JS_IsNull(Raw(prototype)) ? JS_DupValue(context,Raw(prototype)) : BuiltinPrototype(context,"Object");
    JSValue object = NewHost(context,false,proto); JS_FreeValue(context,proto);
    if (JS_IsException(object)) { detail::CaptureException(isolate,context); return {}; }
    for (size_t i = 0;i < count;++i) {
        Atom atom(context,Raw(keys[i]));
        if (!atom.value || JS_DefinePropertyValue(context,object,atom.value,JS_DupValue(context,Raw(values[i])),JS_PROP_C_W_E) < 0) {
            JS_FreeValue(context,object); detail::CaptureException(isolate,context); return {};
        }
    }
    return detail::Adopt(context,object).As<Object>();
}
Maybe<bool> Object::Set(Local<Context> context,Local<Value> key,Local<Value> value) {
    auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>();
    Atom atom(raw,Raw(key)); if (!atom.value) return Result(raw,-1);
    return Result(raw,JS_SetProperty(raw,Raw(this),atom.value,JS_DupValue(raw,Raw(value))));
}
Maybe<bool> Object::Set(Local<Context> context,Local<Value> key,Local<Value> value,MaybeLocal<Object> receiver) {
    auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>();
    Local<Object> local; if (!receiver.ToLocal(&local)) return Set(context,key,value);
    JSValue arguments[]{Raw(this),Raw(key),Raw(value),Raw(local)};
    JSValue result = Reflect(raw,"set",4,arguments);
    if (JS_IsException(result)) return Result(raw,-1);
    int success = JS_ToBool(raw,result); JS_FreeValue(raw,result); return Result(raw,success);
}
Maybe<bool> Object::Set(Local<Context> context,uint32_t index,Local<Value> value) {
    auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>();
    return Result(raw,JS_SetPropertyUint32(raw,Raw(this),index,JS_DupValue(raw,Raw(value))));
}
MaybeLocal<Value> Object::Get(Local<Context> context,Local<Value> key) {
    auto* raw = CheckedContext(context,this); if (!raw) return {};
    Atom atom(raw,Raw(key)); if (!atom.value) { detail::CaptureException(detail::IsolateFor(raw),raw); return {}; }
    return detail::Adopt(raw,JS_GetProperty(raw,Raw(this),atom.value));
}
MaybeLocal<Value> Object::Get(Local<Context> context,Local<Value> key,MaybeLocal<Object> receiver) {
    auto* raw = CheckedContext(context,this); if (!raw) return {};
    Local<Object> local; if (!receiver.ToLocal(&local)) return Get(context,key);
    Atom atom(raw,Raw(key)); if (!atom.value) { detail::CaptureException(detail::IsolateFor(raw),raw); return {}; }
    return detail::Adopt(raw,JS_GetPropertyInternal(raw,Raw(this),atom.value,Raw(local),0));
}
MaybeLocal<Value> Object::Get(Local<Context> context,uint32_t index) { auto* raw = CheckedContext(context,this); return raw ? MaybeLocal<Value>(detail::Adopt(raw,JS_GetPropertyUint32(raw,Raw(this),index))) : MaybeLocal<Value>(); }
Maybe<bool> Object::Has(Local<Context> context,Local<Value> key) { auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>(); Atom atom(raw,Raw(key)); return Result(raw,atom.value ? JS_HasProperty(raw,Raw(this),atom.value) : -1); }
Maybe<bool> Object::Has(Local<Context> context,uint32_t index) { auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>(); JSAtom atom = JS_NewAtomUInt32(raw,index); int result = atom ? JS_HasProperty(raw,Raw(this),atom) : -1; JS_FreeAtom(raw,atom); return Result(raw,result); }
Maybe<bool> Object::Delete(Local<Context> context,Local<Value> key) { auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>(); Atom atom(raw,Raw(key)); return Result(raw,atom.value ? JS_DeleteProperty(raw,Raw(this),atom.value,0) : -1); }
Maybe<bool> Object::Delete(Local<Context> context,uint32_t index) { auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>(); JSAtom atom = JS_NewAtomUInt32(raw,index); int result = atom ? JS_DeleteProperty(raw,Raw(this),atom,0) : -1; JS_FreeAtom(raw,atom); return Result(raw,result); }
Maybe<bool> Object::CreateDataProperty(Local<Context> context,Local<Name> key,Local<Value> value) { return DefineOwnProperty(context,key,value,None); }
Maybe<bool> Object::CreateDataProperty(Local<Context> context,uint32_t index,Local<Value> value) { auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>(); return Result(raw,JS_DefinePropertyValueUint32(raw,Raw(this),index,JS_DupValue(raw,Raw(value)),JS_PROP_C_W_E)); }
Maybe<bool> Object::DefineOwnProperty(Local<Context> context,Local<Name> key,Local<Value> value,PropertyAttribute attributes) {
    auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>(); Atom atom(raw,Raw(key));
    return Result(raw,atom.value ? JS_DefinePropertyValue(raw,Raw(this),atom.value,JS_DupValue(raw,Raw(value)),Attributes(attributes)) : -1);
}
Maybe<bool> Object::DefineProperty(Local<Context> context,Local<Name> key,PropertyDescriptor& descriptor) {
    auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>(); Atom atom(raw,Raw(key)); auto* data = descriptor.get_private();
    return Result(raw,atom.value ? JS_DefineProperty(raw,Raw(this),atom.value,Raw(data->value),Raw(data->getter),Raw(data->setter),data->flags) : -1);
}
Maybe<PropertyAttribute> Object::GetPropertyAttributes(Local<Context> context,Local<Value> key) {
    auto* raw = CheckedContext(context,this); if (!raw) return Nothing<PropertyAttribute>(); Atom atom(raw,Raw(key));
    if (!atom.value) { detail::CaptureException(detail::IsolateFor(raw),raw); return Nothing<PropertyAttribute>(); }
    JSValue current = JS_DupValue(raw,Raw(this));
    while (JS_IsObject(current)) {
        JSPropertyDescriptor descriptor; int found = JS_GetOwnProperty(raw,&descriptor,current,atom.value);
        if (found < 0) { JS_FreeValue(raw,current); detail::CaptureException(detail::IsolateFor(raw),raw); return Nothing<PropertyAttribute>(); }
        if (found) { auto attributes = FromFlags(descriptor.flags); FreeDescriptor(raw,descriptor); JS_FreeValue(raw,current); return Just(attributes); }
        JSValue next = JS_GetPrototype(raw,current); JS_FreeValue(raw,current); current = next;
    }
    if (JS_IsException(current)) { JS_FreeValue(raw,current); detail::CaptureException(detail::IsolateFor(raw),raw); return Nothing<PropertyAttribute>(); }
    JS_FreeValue(raw,current); return Just(None);
}
MaybeLocal<Value> Object::GetOwnPropertyDescriptor(Local<Context> context,Local<Name> key) {
    auto* raw = CheckedContext(context,this); if (!raw) return {}; Atom atom(raw,Raw(key)); JSPropertyDescriptor descriptor;
    int found = atom.value ? JS_GetOwnProperty(raw,&descriptor,Raw(this),atom.value) : -1;
    if (found < 0) { detail::CaptureException(detail::IsolateFor(raw),raw); return {}; }
    if (!found) return detail::Adopt(raw,JS_UNDEFINED);
    JSValue result = DescriptorObject(raw,descriptor); FreeDescriptor(raw,descriptor); return detail::Adopt(raw,result);
}
void Object::SetAccessorProperty(Local<Name> name,Local<Function> getter,Local<Function> setter,PropertyAttribute attributes) {
    JSContext* context = detail::ContextOf(this); Atom atom(context,Raw(name));
    if (!atom.value || JS_DefinePropertyGetSet(context,Raw(this),atom.value,JS_DupValue(context,Raw(getter)),JS_DupValue(context,Raw(setter)),Attributes(attributes) & ~JS_PROP_WRITABLE) < 0) detail::CaptureException(detail::IsolateOf(this),context);
}
namespace {
Maybe<bool> AddNative(Object* object,Local<Context> context,Local<Name> name,AccessorNameGetterCallback getter,AccessorNameSetterCallback setter,Local<Value> data,PropertyAttribute attributes,bool lazy) {
    JSContext* raw = CheckedContext(context,object); if (!raw) return Nothing<bool>();
    auto* state = PropertyState(Raw(object));
    if (!state) {
        JS_ThrowTypeError(raw,"Native data properties require a facade API object"); return Result(raw,-1);
    }
    Atom atom(raw,Raw(name)); if (!atom.value) return Result(raw,-1);
    int result = JS_DefineProperty(raw,state->properties.value,atom.value,JS_UNDEFINED,JS_UNDEFINED,JS_UNDEFINED,Attributes(attributes) | JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE | JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_CONFIGURABLE | JS_PROP_THROW);
    if (result > 0) {
        NativeProperty property; property.key = Hold(name); property.data = Hold(data); property.getter = getter; property.setter = setter; property.attributes = attributes; property.lazy = lazy;
        property.generation = NextNativePropertyGeneration();
        StoreNative(state,atom.value,std::move(property));
    }
    return Result(raw,result);
}
}
Maybe<bool> Object::SetNativeDataProperty(Local<Context> context,Local<Name> name,AccessorNameGetterCallback getter,AccessorNameSetterCallback setter,Local<Value> data,PropertyAttribute attributes,SideEffectType,SideEffectType) { return AddNative(this,context,name,getter,setter,data,attributes,false); }
Maybe<bool> Object::SetLazyDataProperty(Local<Context> context,Local<Name> name,AccessorNameGetterCallback getter,Local<Value> data,PropertyAttribute attributes,SideEffectType,SideEffectType) { return AddNative(this,context,name,getter,nullptr,data,attributes,true); }
namespace {
JSValueConst PrivateStorage(Object* object) { auto* state = State(Raw(object)); return state ? state->properties.value : Raw(object); }
}
Maybe<bool> Object::HasPrivate(Local<Context> context,Local<Private> key) { auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>(); Atom atom(raw,Raw(key)); return Result(raw,atom.value ? JS_GetOwnProperty(raw,nullptr,PrivateStorage(this),atom.value) : -1); }
Maybe<bool> Object::SetPrivate(Local<Context> context,Local<Private> key,Local<Value> value) { auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>(); Atom atom(raw,Raw(key)); return Result(raw,atom.value ? JS_DefinePropertyValue(raw,PrivateStorage(this),atom.value,JS_DupValue(raw,Raw(value)),JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) : -1); }
Maybe<bool> Object::DeletePrivate(Local<Context> context,Local<Private> key) { auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>(); Atom atom(raw,Raw(key)); return Result(raw,atom.value ? JS_DeleteProperty(raw,PrivateStorage(this),atom.value,0) : -1); }
MaybeLocal<Value> Object::GetPrivate(Local<Context> context,Local<Private> key) { auto* raw = CheckedContext(context,this); if (!raw) return {}; Atom atom(raw,Raw(key)); if (!atom.value) { detail::CaptureException(detail::IsolateOf(this),raw); return {}; } JSPropertyDescriptor descriptor; int found = JS_GetOwnProperty(raw,&descriptor,PrivateStorage(this),atom.value); if (found < 0) { detail::CaptureException(detail::IsolateOf(this),raw); return {}; } if (!found) return detail::Adopt(raw,JS_UNDEFINED); JSValue value = JS_DupValue(raw,descriptor.value); FreeDescriptor(raw,descriptor); return detail::Adopt(raw,value); }
MaybeLocal<Array> Object::GetPropertyNames(Local<Context> context) { return GetPropertyNames(context,KeyCollectionMode::kIncludePrototypes,static_cast<PropertyFilter>(ONLY_ENUMERABLE | SKIP_SYMBOLS),IndexFilter::kIncludeIndices,KeyConversionMode::kKeepNumbers); }
MaybeLocal<Array> Object::GetOwnPropertyNames(Local<Context> context) { return GetOwnPropertyNames(context,static_cast<PropertyFilter>(ONLY_ENUMERABLE | SKIP_SYMBOLS),KeyConversionMode::kKeepNumbers); }
MaybeLocal<Array> Object::GetOwnPropertyNames(Local<Context> context,PropertyFilter filter,KeyConversionMode conversion) { return GetPropertyNames(context,KeyCollectionMode::kOwnOnly,filter,IndexFilter::kIncludeIndices,conversion); }
MaybeLocal<Array> Object::GetPropertyNames(Local<Context> context,KeyCollectionMode mode,PropertyFilter filter,IndexFilter indices,KeyConversionMode conversion) {
    auto* raw = CheckedContext(context,this); if (!raw) return {};
    JSValue array = JS_NewArray(raw); if (JS_IsException(array)) { detail::CaptureException(detail::IsolateOf(this),raw); return {}; }
    JSValue current = JS_DupValue(raw,Raw(this)); std::vector<JSAtom> seen; uint32_t offset = 0; bool failed = false;
    while (JS_IsObject(current) && !failed) {
        JSPropertyEnum* properties = nullptr; uint32_t count = 0;
        int flags = ((filter & SKIP_STRINGS) ? 0 : JS_GPN_STRING_MASK) | ((filter & SKIP_SYMBOLS) ? 0 : JS_GPN_SYMBOL_MASK);
        if (JS_GetOwnPropertyNames(raw,&properties,&count,current,flags) < 0) { failed = true; break; }
        for (uint32_t i = 0;i < count && !failed;++i) {
            JSAtom atom = properties[i].atom;
            if (std::find(seen.begin(),seen.end(),atom) != seen.end()) continue;
            seen.push_back(JS_DupAtom(raw,atom));
            uint32_t index = 0; bool indexed = Index(raw,atom,&index);
            if (indexed && (indices == IndexFilter::kSkipIndices || conversion == KeyConversionMode::kNoNumbers)) continue;
            JSPropertyDescriptor descriptor; int found = JS_GetOwnProperty(raw,&descriptor,current,atom);
            if (found < 0) { failed = true; break; } if (!found) continue;
            bool include = (!(filter & ONLY_ENUMERABLE) || (descriptor.flags & JS_PROP_ENUMERABLE)) && (!(filter & ONLY_CONFIGURABLE) || (descriptor.flags & JS_PROP_CONFIGURABLE)) && (!(filter & ONLY_WRITABLE) || (descriptor.flags & JS_PROP_WRITABLE));
            FreeDescriptor(raw,descriptor); if (!include) continue;
            JSValue key = indexed && conversion == KeyConversionMode::kKeepNumbers ? JS_NewUint32(raw,index) : JS_AtomToValue(raw,atom);
            if (JS_IsException(key) || JS_SetPropertyUint32(raw,array,offset++,key) < 0) failed = true;
        }
        JS_FreePropEnum(raw,properties,count); if (mode == KeyCollectionMode::kOwnOnly) break;
        JSValue next = JS_GetPrototype(raw,current); JS_FreeValue(raw,current); current = next;
        if (JS_IsException(current)) failed = true;
    }
    JS_FreeValue(raw,current); for (JSAtom atom : seen) JS_FreeAtom(raw,atom);
    if (failed) { JS_FreeValue(raw,array); detail::CaptureException(detail::IsolateOf(this),raw); return {}; }
    return detail::Adopt(raw,array).As<Array>();
}
Local<Value> Object::GetPrototype() {
    auto* state = State(Raw(this)); auto* context = detail::ContextOf(this);
    return detail::Adopt(context,state && !JS_IsUndefined(state->global_target.value) ? JS_DupValue(context,state->global_target.value) : JS_GetPrototypeForEmbedding(context,Raw(this)));
}
Local<Value> Object::GetPrototypeV2() { return detail::Adopt(detail::ContextOf(this),JS_GetPrototypeForEmbedding(detail::ContextOf(this),Raw(this))); }
Maybe<bool> Object::SetPrototype(Local<Context> context,Local<Value> prototype) { auto* raw = CheckedContext(context,this); return raw ? Result(raw,JS_SetPrototype(raw,Raw(this),Raw(prototype))) : Nothing<bool>(); }
Maybe<bool> Object::SetPrototypeV2(Local<Context> context,Local<Value> prototype) { return SetPrototype(context,prototype); }
Local<Object> Object::FindInstanceInPrototypeChain(Local<FunctionTemplate> wanted) {
    auto* context = detail::ContextOf(this); JSValue current = JS_DupValue(context,Raw(this));
    while (JS_IsObject(current)) {
        if (Matches(context,current,Raw(wanted))) return detail::Adopt(context,current).As<Object>();
        JSValue next = JS_GetPrototype(context,current); JS_FreeValue(context,current); current = next;
    }
    if (JS_IsException(current)) detail::CaptureException(detail::IsolateOf(this),context);
    JS_FreeValue(context,current); return {};
}
MaybeLocal<String> Object::ObjectProtoToString(Local<Context> context) {
    auto* raw = CheckedContext(context,this); if (!raw) return {};
    JSValue function = SavedBuiltin(raw,"Object.prototype.toString");
    JSValue result = JS_IsException(function) ? JS_EXCEPTION : JS_Call(raw,function,Raw(this),0,nullptr); JS_FreeValue(raw,function);
    auto value = detail::Adopt(raw,result); return value.IsEmpty() ? MaybeLocal<String>() : MaybeLocal<String>(value.As<String>());
}
Local<String> Object::GetConstructorName() {
    auto* context = detail::ContextOf(this);
    if (auto* state = State(Raw(this))) if (auto* metadata = TemplateOf(state->brand.value)) {
        if (JS_IsString(metadata->class_name.value)) return View(context,metadata->class_name.value).As<String>();
    }
    JSValue constructor = JS_GetPropertyStr(context,Raw(this),"constructor");
    if (JS_IsException(constructor)) { detail::CaptureException(detail::IsolateOf(this),context); return {}; }
    JSValue name = JS_IsObject(constructor) ? JS_GetPropertyStr(context,constructor,"name") : JS_UNDEFINED; JS_FreeValue(context,constructor);
    if (JS_IsUndefined(name)) name = JS_NewString(context,"Object");
    JSValue string = JS_IsException(name) ? JS_EXCEPTION : JS_ToString(context,name); JS_FreeValue(context,name);
    auto value = detail::Adopt(context,string); return value.IsEmpty() ? Local<String>() : value.As<String>();
}
Maybe<bool> Object::SetIntegrityLevel(Local<Context> context,IntegrityLevel level) {
    auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>(); JSValue argument = Raw(this);
    JSValue value = ObjectBuiltin(raw,level == IntegrityLevel::kFrozen ? "freeze" : "seal",1,&argument);
    if (JS_IsException(value)) return Result(raw,-1); JS_FreeValue(raw,value); return Just(true);
}
int Object::InternalFieldCount() const { auto* state = State(Raw(this)); return state ? static_cast<int>(state->fields.size()) : 0; }
Local<Data> Object::GetInternalField(int index) {
    auto* state = State(Raw(this)); if (!state || index < 0 || size_t(index) >= state->fields.size() || state->fields[index].is_pointer) std::abort();
    return View(detail::ContextOf(this),state->fields[index].value.value);
}
void Object::SetInternalField(int index,Local<Data> data) {
    auto* state = State(Raw(this)); if (!state || index < 0 || size_t(index) >= state->fields.size() || (!data.IsEmpty() && !data->IsValue())) std::abort();
    state->fields[index].value = Hold(data); state->fields[index].pointer = nullptr; state->fields[index].is_pointer = false;
    if (index < 2) detail::UpdateWeakInternalField(detail::IsolateOf(this),Raw(this),index,nullptr);
}
void* Object::GetAlignedPointerFromInternalField(int index) {
    auto* state = State(Raw(this));
    if (!state || index < 0 || size_t(index) >= state->fields.size()) std::abort();
    auto& field = state->fields[index];
    if (!field.is_pointer && !JS_IsUndefined(field.value.value)) std::abort();
    return field.is_pointer ? field.pointer : nullptr;
}
void Object::SetAlignedPointerInInternalField(int index,void* pointer) {
    auto* state = State(Raw(this)); if (!state || index < 0 || size_t(index) >= state->fields.size() || (reinterpret_cast<uintptr_t>(pointer) & 1)) std::abort();
    auto& field = state->fields[index]; field.value = Root(); field.pointer = pointer; field.is_pointer = true;
    if (index < 2) detail::UpdateWeakInternalField(detail::IsolateOf(this),Raw(this),index,pointer);
}
void Object::SetAlignedPointerInInternalFields(int argc,int indices[],void* pointers[]) { for (int i = 0;i < argc;++i) SetAlignedPointerInInternalField(indices[i],pointers[i]); }
Maybe<bool> Object::HasOwnProperty(Local<Context> context,Local<Name> key) { auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>(); Atom atom(raw,Raw(key)); return Result(raw,atom.value ? JS_GetOwnProperty(raw,nullptr,Raw(this),atom.value) : -1); }
Maybe<bool> Object::HasOwnProperty(Local<Context> context,uint32_t index) { auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>(); JSAtom atom = JS_NewAtomUInt32(raw,index); int result = atom ? JS_GetOwnProperty(raw,nullptr,Raw(this),atom) : -1; JS_FreeAtom(raw,atom); return Result(raw,result); }
Maybe<bool> Object::HasRealNamedProperty(Local<Context> context,Local<Name> key) { auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>(); Atom atom(raw,Raw(key)); return Result(raw,atom.value ? Own(raw,nullptr,Raw(this),atom.value) : -1); }
Maybe<bool> Object::HasRealIndexedProperty(Local<Context> context,uint32_t index) { auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>(); JSAtom atom = JS_NewAtomUInt32(raw,index); int result = atom ? Own(raw,nullptr,Raw(this),atom) : -1; JS_FreeAtom(raw,atom); return Result(raw,result); }
Maybe<bool> Object::HasRealNamedCallbackProperty(Local<Context> context,Local<Name> key) { auto* raw = CheckedContext(context,this); if (!raw) return Nothing<bool>(); Atom atom(raw,Raw(key)); auto* state = State(Raw(this)); return Just(state && FindNative(state,atom.value)); }
MaybeLocal<Value> Object::GetRealNamedProperty(Local<Context> context,Local<Name> key) { auto* raw = CheckedContext(context,this); if (!raw) return {}; Atom atom(raw,Raw(key)); if (!atom.value) { detail::CaptureException(detail::IsolateOf(this),raw); return {}; } return RealGet(raw,Raw(this),atom.value,false); }
MaybeLocal<Value> Object::GetRealNamedPropertyInPrototypeChain(Local<Context> context,Local<Name> key) { auto* raw = CheckedContext(context,this); if (!raw) return {}; Atom atom(raw,Raw(key)); if (!atom.value) { detail::CaptureException(detail::IsolateOf(this),raw); return {}; } return RealGet(raw,Raw(this),atom.value,true); }
Maybe<PropertyAttribute> Object::GetRealNamedPropertyAttributes(Local<Context> context,Local<Name> key) { auto* raw = CheckedContext(context,this); if (!raw) return Nothing<PropertyAttribute>(); Atom atom(raw,Raw(key)); return RealAttributes(raw,Raw(this),atom.value,false); }
Maybe<PropertyAttribute> Object::GetRealNamedPropertyAttributesInPrototypeChain(Local<Context> context,Local<Name> key) { auto* raw = CheckedContext(context,this); if (!raw) return Nothing<PropertyAttribute>(); Atom atom(raw,Raw(key)); return RealAttributes(raw,Raw(this),atom.value,true); }
bool Object::HasNamedLookupInterceptor() const { auto* state = State(Raw(this)); return state && (state->named.getter || state->named.query || state->named.descriptor); }
bool Object::HasIndexedLookupInterceptor() const { auto* state = State(Raw(this)); return state && (state->indexed.getter || state->indexed.query || state->indexed.descriptor); }
int Object::GetIdentityHash() {
    auto* state = State(Raw(this));
    if (state) {
        static std::atomic<uint32_t> next{1};
        if (!state->identity_hash) state->identity_hash = int((next.fetch_add(1,std::memory_order_relaxed) & 0x3fffffff) + 1);
        return state->identity_hash;
    }
    uint64_t pointer = reinterpret_cast<uintptr_t>(JS_VALUE_GET_PTR(Raw(this)));
    return int(((pointer >> 4) ^ (pointer >> 32)) & 0x3fffffff) + 1;
}
Local<Object> Object::Clone() {
    auto* context = detail::ContextOf(this);
    int array = JS_IsArray(context,Raw(this));
    if (array < 0) { detail::CaptureException(detail::IsolateOf(this),context); return {}; }
    auto* source = State(Raw(this));
    if (!source && JS_IsFunction(context,Raw(this))) {
        JS_ThrowTypeError(context,"Object::Clone cannot copy JavaScript function bytecode"); detail::CaptureException(detail::IsolateOf(this),context); return {};
    }
    JSValue prototype = JS_GetPrototype(context,Raw(this));
    if (JS_IsException(prototype)) { detail::CaptureException(detail::IsolateOf(this),context); return {}; }
    JSValue clone = array ? JS_NewArray(context) : NewHost(context,IsCallable(),prototype);
    if (array && !JS_IsException(clone) && JS_SetPrototype(context,clone,prototype) < 0) { JS_FreeValue(context,clone); clone = JS_EXCEPTION; }
    JS_FreeValue(context,prototype);
    if (JS_IsException(clone)) { detail::CaptureException(detail::IsolateOf(this),context); return {}; }
    auto* target = State(clone);
    if (source) {
        target->fields = source->fields; target->brand = source->brand;
        target->callback = source->callback; target->callback_data = source->callback_data;
        target->function_template = source->function_template; target->object_template = source->object_template; target->accept_any_receiver = source->accept_any_receiver;
        target->undetectable = source->undetectable; target->code_like = source->code_like; target->immutable = source->immutable;
        target->named = source->named; target->indexed = source->indexed; target->named_data = source->named_data; target->indexed_data = source->indexed_data;
        target->access_check = source->access_check; target->access_data = source->access_data; target->denied_named = source->denied_named; target->denied_indexed = source->denied_indexed; target->denied_named_data = source->denied_named_data; target->denied_indexed_data = source->denied_indexed_data;
        JS_SetConstructorBit(context,clone,JS_IsConstructor(context,Raw(this)));
        if (source->undetectable) JS_SetIsHTMLDDA(context,clone);
    }
    bool failed = CopyProperties(context,source ? source->properties.value : Raw(this),target ? target->properties.value : clone) < 0;
    if (!failed && source) target->native_properties = source->native_properties;
    if (!failed && source && source->immutable) failed = JS_SetImmutablePrototype(context,clone) < 0;
    if (!failed && !JS_IsExtensible(context,Raw(this))) failed = JS_PreventExtensions(context,clone) < 0;
    if (failed) { JS_FreeValue(context,clone); detail::CaptureException(detail::IsolateOf(this),context); return {}; }
    if (source && source->wrapper) {
        target->wrapper = source->wrapper; target->wrapper_tag = source->wrapper_tag;
        mini_ui::RegisterQuickJSWrapper(target->isolate,clone,static_cast<blink::ScriptWrappable*>(target->wrapper));
    }
    return detail::Adopt(context,clone).As<Object>();
}
Local<Object> Object::Clone(Isolate* isolate) { if (isolate != detail::IsolateOf(this)) std::abort(); return Clone(); }
MaybeLocal<Context> Object::GetCreationContext() { auto* context = JS_GetObjectContext(Raw(this)); return context ? MaybeLocal<Context>(detail::ContextForQuickJS(context)) : MaybeLocal<Context>(); }
MaybeLocal<Context> Object::GetCreationContext(Isolate* isolate) { return isolate == detail::IsolateOf(this) ? GetCreationContext() : MaybeLocal<Context>(); }
Local<Context> Object::GetCreationContextChecked() { return GetCreationContext().ToLocalChecked(); }
void* Object::GetAlignedPointerFromEmbedderDataInCreationContext(Isolate* isolate,int index) { Local<Context> context; return GetCreationContext(isolate).ToLocal(&context) ? context->GetAlignedPointerFromEmbedderData(index) : nullptr; }
void* Object::GetAlignedPointerFromEmbedderDataInCreationContext(int index) { return GetAlignedPointerFromEmbedderDataInCreationContext(detail::IsolateOf(this),index); }
bool Object::IsCallable() const { return JS_IsFunction(detail::ContextOf(this),Raw(this)); }
bool Object::IsConstructor() const { return JS_IsConstructor(detail::ContextOf(this),Raw(this)); }
bool Object::IsApiWrapper() const { auto* state = State(Raw(this)); return state && (state->wrapper || !state->fields.empty() || !JS_IsUndefined(state->brand.value)); }
bool Object::IsUndetectable() const { auto* state = State(Raw(this)); return state && state->undetectable; }
bool Object::IsCodeLike(Isolate*) const { auto* state = State(Raw(this)); return state && state->code_like; }
MaybeLocal<Value> Object::CallAsFunction(Local<Context> context,Local<Value> receiver,int argc,Local<Value> argv[]) { auto* raw = CheckedContext(context,this); return raw ? CallObject(raw,Raw(this),Raw(receiver),argc,argv,false) : MaybeLocal<Value>(); }
MaybeLocal<Value> Object::CallAsConstructor(Local<Context> context,int argc,Local<Value> argv[]) { auto* raw = CheckedContext(context,this); return raw ? CallObject(raw,Raw(this),JS_UNDEFINED,argc,argv,true) : MaybeLocal<Value>(); }
Isolate* Object::GetIsolate() { return detail::IsolateOf(this); }

namespace {
Local<ObjectTemplate> ObjectTemplateView(JSContext* context,JSValueConst value) {
    auto* state = State(value);
    if (!state || !state->template_payload || state->template_payload->function) return {};
    return detail::MakeLocal<ObjectTemplate>(state->isolate,context,JS_DupValue(context,value),state->template_payload);
}
JSValue NewTemplate(JSContext* context,const std::shared_ptr<TemplatePayload>& payload) {
    context = detail::NeutralJSContext(detail::IsolateFor(context));
    if (!EnsureClasses(context)) return JS_EXCEPTION;
    JSValue object = JS_NewObjectProtoClass(context,JS_NULL,template_class);
    if (JS_IsException(object)) return object;
    auto state = std::make_unique<NativeState>(); state->isolate = detail::IsolateFor(context); state->origin = context; state->template_payload = payload;
    JS_SetOpaque(object,state.release()); return object;
}
Root NewObjectTemplateRoot(JSContext* context) {
    auto metadata = std::make_shared<TemplatePayload>(); JSValue object = NewTemplate(context,metadata);
    Root root(context,object); JS_FreeValue(context,object); return root;
}
JSValue IntrinsicValue(JSContext* context,Intrinsic intrinsic) {
    JSValue cached = SavedBuiltin(context,std::string("@intrinsic") + std::to_string(int(intrinsic)));
    if (!JS_IsUndefined(cached)) return cached;
    if (intrinsic == kArrayPrototype || intrinsic == kErrorPrototype) return BuiltinPrototype(context,intrinsic == kArrayPrototype ? "Array" : "Error");
    if (intrinsic >= kArrayProto_entries && intrinsic <= kArrayProto_values) {
        JSValue prototype = BuiltinPrototype(context,"Array");
        if (JS_IsException(prototype)) return prototype;
        const char* names[]{"entries","forEach","keys","values"};
        JSValue value = JS_GetPropertyStr(context,prototype,names[int(intrinsic)]); JS_FreeValue(context,prototype); return value;
    }
    if (intrinsic == kObjProto_valueOf) {
        JSValue prototype = BuiltinPrototype(context,"Object");
        if (JS_IsException(prototype)) return prototype;
        JSValue value = JS_GetPropertyStr(context,prototype,"valueOf"); JS_FreeValue(context,prototype); return value;
    }
    if (intrinsic == kMapIteratorPrototype || intrinsic == kSetIteratorPrototype) {
        JSValue constructor = SavedBuiltin(context,intrinsic == kMapIteratorPrototype ? "Map" : "Set");
        JSValue object = JS_IsException(constructor) ? JS_EXCEPTION : JS_CallConstructor(context,constructor,0,nullptr); JS_FreeValue(context,constructor);
        if (JS_IsException(object)) return object;
        JSValue iterator = JS_Invoke(context,object,JS_SymbolGetIteratorAtom(context),0,nullptr); JS_FreeValue(context,object);
        if (JS_IsException(iterator)) return iterator;
        JSValue prototype = JS_GetPrototype(context,iterator); JS_FreeValue(context,iterator); return prototype;
    }
    const char* source = intrinsic == kAsyncIteratorPrototype ? "Object.getPrototypeOf(Object.getPrototypeOf(Object.getPrototypeOf((async function*(){})())))" : "Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()))";
    return JS_Eval(context,source,std::strlen(source),"<DOM intrinsic>",JS_EVAL_TYPE_GLOBAL);
}
JSValue Materialize(JSContext* context,JSValueConst value) {
    if (auto* metadata = TemplateOf(value)) {
        if (metadata->function) {
            auto function = InstantiateFunction(context,value);
            return function.IsEmpty() ? JS_EXCEPTION : JS_DupValue(context,Raw(function));
        }
        return InstantiateObject(context,value,JS_UNDEFINED,JS_UNDEFINED);
    }
    return JS_DupValue(context,value);
}
bool InstanceEntryIsOverridden(JSContext* context,TemplatePayload* most_derived,TemplatePayload* owner,const TemplateEntry& entry) {
    for (auto* current = most_derived; current && current != owner; current = TemplateOf(current->parent.value)) {
        auto* instance = TemplateOf(current->instance.value);
        if (!instance) continue;
        for (const auto& candidate : instance->entries) {
            if (candidate.private_key == entry.private_key && Same(context,candidate.key.value,entry.key.value)) return true;
        }
    }
    return false;
}
int InstallEntries(JSContext* context,JSValueConst object,TemplatePayload* metadata,TemplatePayload* most_derived) {
    auto* state = State(object);
    auto* owner = most_derived ? TemplateOf(metadata->parent.value) : nullptr;
    for (const auto& entry : metadata->entries) {
        // Resolve template inheritance before defining unforgeable properties.
        // Only the most-derived descriptor may seal a given instance key.
        if (owner && most_derived != owner && InstanceEntryIsOverridden(context,most_derived,owner,entry)) continue;
        Atom atom(context,entry.key.value); if (!atom.value) return -1;
        JSValueConst storage = state->properties.value;
        int status = 1;
        if (entry.kind == TemplateEntry::native) {
            NativeProperty property = entry.native_property;
            property.generation = NextNativePropertyGeneration();
            status = JS_DefineProperty(context,storage,atom.value,JS_UNDEFINED,JS_UNDEFINED,JS_UNDEFINED,Attributes(entry.attributes) | JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE | JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_CONFIGURABLE);
            if (status > 0) StoreNative(state,atom.value,std::move(property));
        } else if (entry.kind == TemplateEntry::accessor) {
            JSValue getter = JS_IsUndefined(entry.getter.value) ? JS_UNDEFINED : Materialize(context,entry.getter.value);
            JSValue setter = JS_IsUndefined(entry.setter.value) ? JS_UNDEFINED : Materialize(context,entry.setter.value);
            if (JS_IsException(getter) || JS_IsException(setter)) { JS_FreeValue(context,getter); JS_FreeValue(context,setter); return -1; }
            status = JS_DefinePropertyGetSet(context,storage,atom.value,getter,setter,Attributes(entry.attributes) & ~JS_PROP_WRITABLE);
        } else {
            JSValue value = entry.kind == TemplateEntry::intrinsic ? IntrinsicValue(context,entry.intrinsic_value) : Materialize(context,entry.data.value);
            if (JS_IsException(value)) return -1;
            status = JS_DefinePropertyValue(context,storage,atom.value,value,Attributes(entry.attributes));
        }
        if (status <= 0) { if (!status) JS_ThrowTypeError(context,"Cannot instantiate template property"); return -1; }
    }
    return 0;
}
int InstallInstance(JSContext* context,JSValueConst object,JSValueConst function_template,TemplatePayload* most_derived = nullptr) {
    auto* metadata = TemplateOf(function_template); if (!metadata) return 0;
    if (!most_derived) most_derived = metadata;
    if (!JS_IsUndefined(metadata->parent.value)) if (InstallInstance(context,object,metadata->parent.value,most_derived) < 0) return -1;
    auto* instance = TemplateOf(metadata->instance.value);
    if (!instance) return 0;
    auto* state = State(object);
    if (instance->internal_fields > int(state->fields.size())) state->fields.resize(instance->internal_fields);
    if (instance->callback) { state->callback = instance->callback; state->callback_data = instance->callback_data; }
    if (instance->named.getter || instance->named.query || instance->named.descriptor || instance->named.setter || instance->named.enumerator) { state->named = instance->named; state->named.data.Clear(); state->named_data = instance->named_data; }
    if (instance->indexed.getter || instance->indexed.query || instance->indexed.descriptor || instance->indexed.setter || instance->indexed.enumerator) { state->indexed = instance->indexed; state->indexed.data.Clear(); state->indexed_data = instance->indexed_data; }
    if (instance->access_check) {
        state->access_check = instance->access_check; state->access_data = instance->access_data;
        state->denied_named = instance->denied_named; state->denied_named.data.Clear(); state->denied_named_data = instance->denied_named_data;
        state->denied_indexed = instance->denied_indexed; state->denied_indexed.data.Clear(); state->denied_indexed_data = instance->denied_indexed_data;
    }
    state->undetectable |= instance->undetectable; state->code_like |= instance->code_like;
    if (instance->undetectable) JS_SetIsHTMLDDA(context,object);
    instance->instantiated = true;
    state->immutable |= instance->immutable;
    if (InstallEntries(context,object,instance,most_derived) < 0) return -1;
    return instance->immutable ? JS_SetImmutablePrototype(context,object) : 0;
}
JSValue InstantiateObject(JSContext* context,JSValueConst object_template,JSValueConst prototype,JSValueConst brand) {
    auto* metadata = TemplateOf(object_template);
    if (!metadata) return JS_ThrowTypeError(context,"Invalid ObjectTemplate");
    bool callable = metadata->callback != nullptr;
    for (JSValue current = brand; !callable;) {
        auto* constructor = TemplateOf(current); if (!constructor) break;
        auto* instance = TemplateOf(constructor->instance.value);
        callable = instance && instance->callback;
        current = constructor->parent.value;
    }
    JSValue object = NewHost(context,callable,prototype);
    if (JS_IsException(object)) return object;
    auto* state = State(object); CopyConfiguration(state,metadata);
    state->brand = Root(context,brand);
    if (!JS_IsUndefined(brand) && InstallInstance(context,object,brand) < 0) { JS_FreeValue(context,object); return JS_EXCEPTION; }
    auto* constructor = TemplateOf(brand);
    if ((!constructor || !Same(context,constructor->instance.value,object_template)) && InstallEntries(context,object,metadata) < 0) { JS_FreeValue(context,object); return JS_EXCEPTION; }
    if (state->callback || JS_IsUndefined(brand)) state->object_template = Root(context,object_template);
    metadata->instantiated = true;
    if (metadata->undetectable) JS_SetIsHTMLDDA(context,object);
    if (metadata->immutable && JS_SetImmutablePrototype(context,object) < 0) { JS_FreeValue(context,object); return JS_EXCEPTION; }
    return object;
}
Local<Function> InstantiateFunction(JSContext* context,JSValueConst value) {
    auto* metadata = TemplateOf(value);
    auto* context_view = detail::LookupContext(context);
    if (!metadata || !metadata->function || !context_view) { JS_ThrowTypeError(context,"Function template requires a registered realm"); detail::CaptureException(detail::IsolateFor(context),context); return {}; }
    auto realm = Realm(context_view);
    for (auto& entry : realm->functions) if (Same(context,entry.first.value,value)) return detail::Adopt(context,JS_DupValue(context,entry.second.value)).As<Function>();
    JSValue function = NewHost(context,true);
    if (JS_IsException(function)) { detail::CaptureException(detail::IsolateFor(context),context); return {}; }
    auto* state = State(function); state->callback = metadata->callback; state->callback_data = metadata->callback_data; state->function_template = Root(context,value); state->accept_any_receiver = metadata->accept_any_receiver;
    JS_SetConstructorBit(context,function,metadata->constructor_behavior == ConstructorBehavior::kAllow && !metadata->remove_prototype);
    realm->functions.emplace_back(Root(context,value),Root(context,function));
    metadata->instantiated = true;
    if (auto* instance = TemplateOf(metadata->instance.value)) instance->instantiated = true;
    int status = JS_DefinePropertyValueStr(context,state->properties.value,"length",JS_NewInt32(context,metadata->length),JS_PROP_CONFIGURABLE);
    if (status >= 0) status = JS_DefinePropertyValueStr(context,state->properties.value,"name",JS_IsUndefined(metadata->class_name.value) ? JS_NewStringLen(context,"",0) : JS_DupValue(context,metadata->class_name.value),JS_PROP_CONFIGURABLE);
    JSValue parent_prototype = JS_UNDEFINED;
    if (status >= 0 && !JS_IsUndefined(metadata->parent.value)) {
        auto parent = InstantiateFunction(context,metadata->parent.value);
        if (parent.IsEmpty()) status = -1;
        else {
            status = JS_SetPrototype(context,function,Raw(parent));
            parent_prototype = JS_GetPropertyStr(context,Raw(parent),"prototype");
            if (JS_IsException(parent_prototype)) status = -1;
        }
    }
    if (status >= 0 && !metadata->remove_prototype) {
        JSValue prototype;
        bool provided = !JS_IsUndefined(metadata->provider.value);
        if (provided) {
            auto provider = InstantiateFunction(context,metadata->provider.value);
            prototype = provider.IsEmpty() ? JS_EXCEPTION : JS_GetPropertyStr(context,Raw(provider),"prototype");
        } else prototype = InstantiateObject(context,metadata->prototype.value,parent_prototype,JS_UNDEFINED);
        if (JS_IsException(prototype)) status = -1;
        else {
            auto* prototype_state = State(prototype);
            if (!provided && prototype_state && JS_DefinePropertyValueStr(context,prototype_state->properties.value,"constructor",JS_DupValue(context,function),JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) < 0) status = -1;
            if (status >= 0) status = JS_DefinePropertyValueStr(context,state->properties.value,"prototype",JS_DupValue(context,prototype),metadata->read_only_prototype ? 0 : JS_PROP_WRITABLE);
        }
        JS_FreeValue(context,prototype);
    }
    JS_FreeValue(context,parent_prototype);
    if (status >= 0) status = InstallEntries(context,function,metadata);
    if (status < 0) {
        realm->functions.erase(std::remove_if(realm->functions.begin(),realm->functions.end(),[&](const auto& entry) { return Same(context,entry.first.value,value); }),realm->functions.end());
        JS_FreeValue(context,function); if (!detail::HasPendingException(detail::IsolateFor(context))) detail::CaptureException(detail::IsolateFor(context),context); return {};
    }
    return detail::Adopt(context,function).As<Function>();
}
JSValue NativeCall(JSContext* caller,JSValueConst function,JSValueConst this_value,int argc,JSValueConst* argv,int flags) {
    auto* state = State(function); if (!state) return JS_ThrowTypeError(caller,"Invalid native function");
    Isolate* isolate = state->isolate; Isolate::Scope isolate_scope(isolate); HandleScope handles(isolate);
    JSContext* context = state->origin;
    auto* context_view = detail::LookupContext(context);
    if (!context_view) return JS_ThrowTypeError(caller,"Native function realm has been detached");
    Context::Scope entered{Local<Context>(context_view)};
    auto* metadata = TemplateOf(state->function_template.value);
    bool construct = flags & JS_CALL_FLAG_CONSTRUCTOR;
    if (construct && metadata && (metadata->constructor_behavior == ConstructorBehavior::kThrow || metadata->remove_prototype)) return JS_ThrowTypeError(caller,"Function is not a constructor");
    JSValue receiver = JS_UNDEFINED;
    if (construct) {
        JSValue prototype = JS_GetPropertyStr(context,this_value,"prototype");
        if (JS_IsException(prototype)) goto exception;
        if (!JS_IsObject(prototype)) {
            JS_FreeValue(context,prototype);
            JSContext* target_context = JS_GetObjectContext(this_value);
            prototype = BuiltinPrototype(target_context ? target_context : context,"Object");
        }
        if (metadata) receiver = InstantiateObject(context,metadata->instance.value,prototype,state->function_template.value);
        else if (!JS_IsUndefined(state->object_template.value)) receiver = InstantiateObject(context,state->object_template.value,prototype,state->brand.value);
        else receiver = NewHost(context,false,prototype);
        JS_FreeValue(context,prototype);
        if (JS_IsException(receiver)) goto exception;
    } else {
        if (JS_IsNull(this_value) || JS_IsUndefined(this_value)) receiver = JS_GetGlobalThis(context);
        else if (JS_IsObject(this_value)) receiver = JS_DupValue(context,this_value);
        else {
            JSValue object = SavedBuiltin(context,"Object");
            receiver = JS_IsException(object) ? JS_EXCEPTION : JS_Call(context,object,JS_UNDEFINED,1,&this_value); JS_FreeValue(context,object);
            if (JS_IsException(receiver)) goto exception;
        }
    }
    if (metadata && !JS_IsUndefined(metadata->signature.value)) {
        auto* signature = TemplateOf(metadata->signature.value);
        if (signature && !JS_IsUndefined(signature->parent.value) && !Matches(context,receiver,signature->parent.value)) {
            JS_FreeValue(context,receiver); return JS_ThrowTypeError(caller,"Illegal invocation");
        }
    }
    if (!state->accept_any_receiver) if (auto* receiver_state = State(receiver)) {
        int allowed = Allowed(caller,receiver,receiver_state);
        if (allowed <= 0) { JS_FreeValue(context,receiver); return allowed < 0 ? JS_EXCEPTION : AccessDenied(caller); }
    }
    if (!construct && metadata && !JS_IsUndefined(metadata->cache_property.value)) {
        auto* receiver_state = State(receiver);
        JSValueConst storage = receiver_state ? receiver_state->properties.value : receiver;
        JSAtom atom = JS_ValueToAtom(context,metadata->cache_property.value);
        JSPropertyDescriptor cached;
        int found = atom ? JS_GetOwnProperty(context,&cached,storage,atom) : -1; JS_FreeAtom(context,atom);
        if (found < 0) { JS_FreeValue(context,receiver); goto exception; }
        if (found) {
            JSValue result = JS_DupValue(context,cached.value); FreeDescriptor(context,cached);
            JS_FreeValue(context,receiver); return result;
        }
    }
    {
        detail::CallbackState callback(isolate,context);
        Setup(callback,receiver,receiver,state->callback_data.value,false,false);
        callback.construct = construct; callback.new_target = View(context,construct ? this_value : JS_UNDEFINED);
        std::array<Local<Value>,16> local_arguments;
        std::vector<Local<Value>> overflow;
        Local<Value>* arguments = local_arguments.data();
        if (argc > int(local_arguments.size())) { overflow.resize(argc); arguments = overflow.data(); }
        for (int i = 0;i < argc;++i) arguments[i] = View(context,argv[i]);
        callback.arguments = arguments; callback.argument_count = argc;
        if (state->callback) state->callback(FunctionCallbackInfo<Value>(callback));
        if (Pending(callback)) { JS_FreeValue(context,receiver); goto exception; }
        JSValue result = construct && !JS_IsObject(callback.result) ? JS_DupValue(context,receiver) : JS_DupValue(context,callback.result);
        JS_FreeValue(context,receiver); return result;
    }
exception:
    if (detail::HasPendingException(isolate)) return JS_Throw(caller,detail::TakePendingException(isolate));
    if (caller != context) return JS_Throw(caller,JS_GetException(context));
    return JS_EXCEPTION;
}
void AddEntry(Template* target,TemplateEntry entry) {
    auto metadata = TemplateData(target); CheckMutable(metadata);
    auto* context = detail::ContextOf(target);
    for (auto& old : metadata->entries) if (Same(context,old.key.value,entry.key.value) && old.private_key == entry.private_key) { old = std::move(entry); return; }
    metadata->entries.push_back(std::move(entry));
}
}

void Template::Set(Local<Name> name,Local<Data> value,PropertyAttribute attributes) {
    TemplateEntry entry; entry.key = Hold(name); entry.data = Hold(value); entry.attributes = attributes; AddEntry(this,std::move(entry));
}
void Template::Set(Isolate* isolate,const char* name,Local<Data> value,PropertyAttribute attributes) { Set(String::NewFromUtf8(isolate,name,NewStringType::kInternalized).ToLocalChecked(),value,attributes); }
void Template::SetPrivate(Local<Private> name,Local<Data> value,PropertyAttribute attributes) { TemplateEntry entry; entry.key = Hold(name); entry.data = Hold(value); entry.attributes = attributes; entry.private_key = true; AddEntry(this,std::move(entry)); }
void Template::SetAccessorProperty(Local<Name> name,Local<FunctionTemplate> getter,Local<FunctionTemplate> setter,PropertyAttribute attributes) { TemplateEntry entry; entry.kind = TemplateEntry::accessor; entry.key = Hold(name); entry.getter = Hold(getter); entry.setter = Hold(setter); entry.attributes = attributes; AddEntry(this,std::move(entry)); }
void Template::SetNativeDataProperty(Local<Name> name,AccessorNameGetterCallback getter,AccessorNameSetterCallback setter,Local<Value> data,PropertyAttribute attributes,SideEffectType,SideEffectType) {
    TemplateEntry entry; entry.kind = TemplateEntry::native; entry.key = Hold(name); entry.attributes = attributes;
    entry.native_property.key = entry.key; entry.native_property.data = Hold(data); entry.native_property.getter = getter; entry.native_property.setter = setter; entry.native_property.attributes = attributes; AddEntry(this,std::move(entry));
}
void Template::SetLazyDataProperty(Local<Name> name,AccessorNameGetterCallback getter,Local<Value> data,PropertyAttribute attributes,SideEffectType,SideEffectType) {
    TemplateEntry entry; entry.kind = TemplateEntry::native; entry.key = Hold(name); entry.attributes = attributes;
    entry.native_property.key = entry.key; entry.native_property.data = Hold(data); entry.native_property.getter = getter; entry.native_property.attributes = attributes; entry.native_property.lazy = true; AddEntry(this,std::move(entry));
}
void Template::SetIntrinsicDataProperty(Local<Name> name,Intrinsic intrinsic,PropertyAttribute attributes) { TemplateEntry entry; entry.kind = TemplateEntry::intrinsic; entry.key = Hold(name); entry.intrinsic_value = intrinsic; entry.attributes = attributes; AddEntry(this,std::move(entry)); }
Local<FunctionTemplate> FunctionTemplate::New(Isolate* isolate,FunctionCallback callback,Local<Value> data,Local<Signature> signature,int length,ConstructorBehavior behavior,SideEffectType,const CFunction*,uint16_t,uint16_t,uint16_t) {
    auto* context = detail::NeutralJSContext(isolate); auto metadata = std::make_shared<TemplatePayload>();
    metadata->function = true; metadata->callback = callback; metadata->callback_data = Hold(data); metadata->signature = Hold(signature); metadata->length = length; metadata->constructor_behavior = behavior;
    metadata->remove_prototype = behavior == ConstructorBehavior::kThrow;
    metadata->instance = NewObjectTemplateRoot(context); metadata->prototype = NewObjectTemplateRoot(context);
    if (JS_IsException(metadata->instance.value) || JS_IsException(metadata->prototype.value)) { detail::CaptureException(isolate,context); return {}; }
    JSValue object = NewTemplate(context,metadata);
    if (!JS_IsException(object)) TemplateOf(metadata->instance.value)->parent = Root(context,object);
    return detail::MakeLocal<FunctionTemplate>(isolate,context,object,metadata);
}
MaybeLocal<Function> FunctionTemplate::GetFunction(Local<Context> context) { auto* raw = CheckedContext(context,this); return raw ? MaybeLocal<Function>(InstantiateFunction(raw,Raw(this))) : MaybeLocal<Function>(); }
void FunctionTemplate::SetCallHandler(FunctionCallback callback,Local<Value> data,SideEffectType,const MemorySpan<const CFunction>&) { auto metadata = TemplateData(this); CheckMutable(metadata); metadata->callback = callback; metadata->callback_data = Hold(data); }
void FunctionTemplate::SetLength(int value) { auto metadata = TemplateData(this); CheckMutable(metadata); metadata->length = value; }
Local<ObjectTemplate> FunctionTemplate::InstanceTemplate() { auto metadata = TemplateData(this); return ObjectTemplateView(detail::ContextOf(this),metadata->instance.value); }
Local<ObjectTemplate> FunctionTemplate::PrototypeTemplate() {
    auto metadata = TemplateData(this); if (!JS_IsUndefined(metadata->provider.value)) std::abort();
    metadata->prototype_requested = true;
    return ObjectTemplateView(detail::ContextOf(this),metadata->prototype.value);
}
void FunctionTemplate::Inherit(Local<FunctionTemplate> parent) {
    auto metadata = TemplateData(this); CheckMutable(metadata); if (!JS_IsUndefined(metadata->provider.value)) std::abort();
    for (JSValue current = Raw(parent); !JS_IsUndefined(current);) {
        if (Same(detail::ContextOf(this),current,Raw(this))) std::abort();
        auto* ancestor = TemplateOf(current); if (!ancestor) break; current = ancestor->parent.value;
    }
    metadata->parent = Hold(parent);
}
void FunctionTemplate::SetPrototypeProviderTemplate(Local<FunctionTemplate> provider) {
    auto metadata = TemplateData(this); CheckMutable(metadata);
    if (metadata->prototype_requested || !JS_IsUndefined(metadata->parent.value)) std::abort();
    for (JSValue current = Raw(provider); !JS_IsUndefined(current);) {
        if (Same(detail::ContextOf(this),current,Raw(this))) std::abort();
        auto* ancestor = TemplateOf(current); if (!ancestor) break; current = ancestor->provider.value;
    }
    metadata->provider = Hold(provider);
}
void FunctionTemplate::SetClassName(Local<String> name) { auto metadata = TemplateData(this); CheckMutable(metadata); metadata->class_name = Hold(name); }
void FunctionTemplate::SetInterfaceName(Local<String> name) { auto metadata = TemplateData(this); CheckMutable(metadata); metadata->interface_name = Hold(name); }
void FunctionTemplate::SetAcceptAnyReceiver(bool value) { auto metadata = TemplateData(this); CheckMutable(metadata); metadata->accept_any_receiver = value; }
void FunctionTemplate::ReadOnlyPrototype() { auto metadata = TemplateData(this); CheckMutable(metadata); metadata->read_only_prototype = true; }
void FunctionTemplate::RemovePrototype() { auto metadata = TemplateData(this); CheckMutable(metadata); metadata->remove_prototype = true; }
bool FunctionTemplate::HasInstance(Local<Value> object) { return Matches(detail::ContextOf(this),Raw(object),Raw(this)); }
bool FunctionTemplate::IsLeafTemplateForApiObject(Local<Value> object) const { auto* state = State(Raw(object)); return state && Same(detail::ContextOf(this),state->brand.value,Raw(this)); }
Local<ObjectTemplate> ObjectTemplate::New(Isolate* isolate,Local<FunctionTemplate> constructor) { auto* context = detail::NeutralJSContext(isolate); auto metadata = std::make_shared<TemplatePayload>(); metadata->parent = Hold(constructor); return detail::MakeLocal<ObjectTemplate>(isolate,context,NewTemplate(context,metadata),metadata); }
MaybeLocal<Object> ObjectTemplate::NewInstance(Local<Context> context) {
    auto* raw = CheckedContext(context,this); if (!raw) return {};
    auto metadata = TemplateData(this); JSValue prototype = JS_UNDEFINED; JSValueConst brand = metadata->parent.value;
    if (!JS_IsUndefined(brand)) { auto function = InstantiateFunction(raw,brand); if (function.IsEmpty()) return {}; prototype = JS_GetPropertyStr(raw,Raw(function),"prototype"); if (JS_IsException(prototype)) { detail::CaptureException(detail::IsolateOf(this),raw); return {}; } }
    JSValue result = InstantiateObject(raw,Raw(this),prototype,brand); JS_FreeValue(raw,prototype);
    auto value = detail::Adopt(raw,result); return value.IsEmpty() ? MaybeLocal<Object>() : MaybeLocal<Object>(value.As<Object>());
}
void ObjectTemplate::SetHandler(const NamedPropertyHandlerConfiguration& handler) { auto metadata = TemplateData(this); CheckMutable(metadata); metadata->named = handler; metadata->named.data.Clear(); metadata->named_data = Hold(handler.data); }
void ObjectTemplate::SetHandler(const IndexedPropertyHandlerConfiguration& handler) { auto metadata = TemplateData(this); CheckMutable(metadata); metadata->indexed = handler; metadata->indexed.data.Clear(); metadata->indexed_data = Hold(handler.data); }
void ObjectTemplate::SetCallAsFunctionHandler(FunctionCallback callback,Local<Value> data) { auto metadata = TemplateData(this); CheckMutable(metadata); metadata->callback = callback; metadata->callback_data = Hold(data); }
void ObjectTemplate::SetAccessCheckCallback(AccessCheckCallback callback,Local<Value> data) { auto metadata = TemplateData(this); CheckMutable(metadata); metadata->access_check = callback; metadata->access_data = Hold(data); }
void ObjectTemplate::SetAccessCheckCallbackAndHandler(AccessCheckCallback callback,const NamedPropertyHandlerConfiguration& named,const IndexedPropertyHandlerConfiguration& indexed,Local<Value> data) {
    SetAccessCheckCallback(callback,data); auto metadata = TemplateData(this); metadata->denied_named = named; metadata->denied_named.data.Clear(); metadata->denied_named_data = Hold(named.data);
    metadata->denied_indexed = indexed; metadata->denied_indexed.data.Clear(); metadata->denied_indexed_data = Hold(indexed.data);
}
void ObjectTemplate::MarkAsUndetectable() { auto metadata = TemplateData(this); CheckMutable(metadata); metadata->undetectable = true; }
int ObjectTemplate::InternalFieldCount() const { return TemplateData(this)->internal_fields; }
void ObjectTemplate::SetInternalFieldCount(int value) { if (value < 0) std::abort(); auto metadata = TemplateData(this); CheckMutable(metadata); metadata->internal_fields = value; }
bool ObjectTemplate::IsImmutableProto() const { return TemplateData(this)->immutable; }
void ObjectTemplate::SetImmutableProto() { auto metadata = TemplateData(this); CheckMutable(metadata); metadata->immutable = true; }
void ObjectTemplate::SetCodeLike() { auto metadata = TemplateData(this); CheckMutable(metadata); metadata->code_like = true; }
bool ObjectTemplate::IsCodeLike() const { return TemplateData(this)->code_like; }
Local<Signature> Signature::New(Isolate* isolate,Local<FunctionTemplate> receiver) { auto* context = detail::NeutralJSContext(isolate); auto metadata = std::make_shared<TemplatePayload>(); metadata->parent = Hold(receiver); return detail::MakeLocal<Signature>(isolate,context,NewTemplate(context,metadata),metadata); }
MaybeLocal<Function> Function::New(Local<Context> context,FunctionCallback callback,Local<Value> data,int length,ConstructorBehavior behavior,SideEffectType) {
    auto* raw = CheckedContext(context); if (!raw) return {};
    Context::Scope scope(context);
    JSValue function = NewHost(raw,true);
    if (JS_IsException(function)) { detail::CaptureException(context->GetIsolate(),raw); return {}; }
    auto* state = State(function); state->callback = callback; state->callback_data = Hold(data);
    JS_SetConstructorBit(raw,function,behavior == ConstructorBehavior::kAllow);
    int status = JS_DefinePropertyValueStr(raw,state->properties.value,"length",JS_NewInt32(raw,length),JS_PROP_CONFIGURABLE);
    if (status >= 0) status = JS_DefinePropertyValueStr(raw,state->properties.value,"name",JS_NewStringLen(raw,"",0),JS_PROP_CONFIGURABLE);
    if (status >= 0 && behavior == ConstructorBehavior::kAllow) {
        JSValue prototype = NewHost(raw);
        if (JS_IsException(prototype)) status = -1;
        else {
            status = JS_DefinePropertyValueStr(raw,State(prototype)->properties.value,"constructor",JS_DupValue(raw,function),JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
            if (status >= 0) status = JS_DefinePropertyValueStr(raw,state->properties.value,"prototype",JS_DupValue(raw,prototype),JS_PROP_WRITABLE);
        }
        JS_FreeValue(raw,prototype);
    }
    if (status < 0) { JS_FreeValue(raw,function); detail::CaptureException(context->GetIsolate(),raw); return {}; }
    return detail::Adopt(raw,function).As<Function>();
}
MaybeLocal<Object> Function::NewInstance(Local<Context> context,int argc,Local<Value> argv[]) const { auto* raw = CheckedContext(context,this); if (!raw) return {}; Local<Value> result; if (!CallObject(raw,Raw(this),JS_UNDEFINED,argc,argv,true).ToLocal(&result)) return {}; return result.As<Object>(); }
MaybeLocal<Value> Function::Call(Local<Context> context,Local<Value> receiver,int argc,Local<Value> argv[]) { return CallAsFunction(context,receiver,argc,argv); }
void Function::SetName(Local<String> name) { auto* context = detail::ContextOf(this); auto* state = State(Raw(this)); if (JS_DefinePropertyValueStr(context,state ? state->properties.value : Raw(this),"name",JS_DupValue(context,Raw(name)),JS_PROP_CONFIGURABLE) < 0) detail::CaptureException(detail::IsolateOf(this),context); }
Local<Value> Function::GetName() const { return detail::Adopt(detail::ContextOf(this),JS_GetPropertyStr(detail::ContextOf(this),Raw(this),"name")); }
Local<Value> Function::GetInferredName() const { return GetName(); }
Local<Value> Function::GetDebugName() const { auto* context = detail::ContextOf(this); JSValue display = JS_GetPropertyStr(context,Raw(this),"displayName"); if (JS_IsString(display)) return detail::Adopt(context,display); if (JS_IsException(display)) { detail::CaptureException(detail::IsolateOf(this),context); return {}; } JS_FreeValue(context,display); return GetName(); }
int Function::GetScriptLineNumber() const {
    auto* context = detail::ContextOf(this);
    int line = -1;
    if (JS_GetFunctionLocation(context,Raw(this),nullptr,&line) < 0)
        detail::CaptureException(detail::IsolateOf(this),context);
    return line > 0 ? line - 1 : -1;
}
int Function::GetScriptColumnNumber() const {
    // QuickJS bytecode records no function-start column.
    return -1;
}
Local<Value> Function::GetBoundFunction() const {
    auto* context = detail::ContextOf(this);
    return detail::Adopt(context,JS_GetBoundFunction(context,Raw(this)));
}
MaybeLocal<String> Function::FunctionProtoToString(Local<Context> context) {
    auto* raw = CheckedContext(context,this); if (!raw) return {};
    JSValue to_string = SavedBuiltin(raw,"Function.prototype.toString");
    JSValue string = JS_IsException(to_string) ? JS_EXCEPTION : JS_Call(raw,to_string,Raw(this),0,nullptr); JS_FreeValue(raw,to_string);
    auto value = detail::Adopt(raw,string); return value.IsEmpty() ? MaybeLocal<String>() : MaybeLocal<String>(value.As<String>());
}

namespace {
int CopyProperties(JSContext* context,JSValueConst source,JSValueConst target,JSAtom replaced,JSValueConst replacement) {
    JSPropertyEnum* properties = nullptr; uint32_t count = 0;
    if (JS_GetOwnPropertyNames(context,&properties,&count,source,JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK | JS_GPN_PRIVATE_MASK) < 0) return -1;
    int result = 0;
    for (uint32_t i = 0;i < count;++i) {
        JSPropertyDescriptor descriptor; int found = JS_GetOwnProperty(context,&descriptor,source,properties[i].atom);
        if (found < 0) { result = -1; break; } if (!found) continue;
        if (properties[i].atom == replaced) { JS_FreeValue(context,descriptor.value); descriptor.value = JS_DupValue(context,replacement); }
        int flags = descriptor.flags | JS_PROP_HAS_ENUMERABLE | JS_PROP_HAS_CONFIGURABLE;
        flags |= descriptor.flags & JS_PROP_GETSET ? JS_PROP_HAS_GET | JS_PROP_HAS_SET : JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE;
        if (JS_DefineProperty(context,target,properties[i].atom,descriptor.value,descriptor.getter,descriptor.setter,flags) < 0) result = -1;
        FreeDescriptor(context,descriptor); if (result < 0) break;
    }
    JS_FreePropEnum(context,properties,count); return result;
}
void FinalizeContext(JSRuntime*,void* data) {
    auto* payload = static_cast<std::shared_ptr<ContextPayload>*>(data);
    detail::UnregisterContext((*payload)->context);
    delete payload;
}
void MarkContext(JSRuntime* runtime,void* data,JS_MarkFunc* mark,void* userdata) {
    (*static_cast<std::shared_ptr<ContextPayload>*>(data))->Mark(runtime,mark,userdata);
}
Local<Context> RegisterRealm(Isolate* isolate,JSContext* owned) {
    if (!owned || JS_GetRuntime(owned) != isolate->GetQuickJSRuntime()) { if (owned) JS_FreeContext(owned); return {}; }
    if (auto* existing = detail::LookupContext(owned)) { JS_FreeContext(owned); return Local<Context>::New(isolate,Local<Context>(existing)); }
    if (JS_GetContextNativeData(owned)) {
        JS_ThrowTypeError(owned,"Context native metadata is already owned"); detail::CaptureException(isolate,owned); JS_FreeContext(owned); return {};
    }
    auto payload = std::make_shared<ContextPayload>(); payload->context = owned;
    JSValue global = JS_GetGlobalObject(owned);
    auto context = detail::MakeLocal<Context>(isolate,owned,JS_UNDEFINED,payload);
    if (context.IsEmpty()) { JS_FreeValue(owned,global); JS_FreeContext(owned); return {}; }
    JS_FreeContext(owned);
    payload->default_token = Root(owned,global); payload->security_token = payload->default_token;
    JSValue global_this = JS_GetGlobalThis(owned);
    payload->global_proxy = Root(owned,global_this); JS_FreeValue(owned,global_this);
    JSValue extras = JS_NewObjectProto(owned,JS_NULL); payload->extras = Root(owned,extras); JS_FreeValue(owned,extras);
    detail::RegisterContext(isolate,owned,*context);
    for (const char* name : {"Object","Function","Array","Error","Map","Set","Proxy","Reflect"}) {
        JSValue builtin = JS_GetPropertyStr(owned,global,name);
        if (JS_IsException(builtin)) { JS_FreeValue(owned,global); detail::CaptureException(isolate,owned); detail::UnregisterContext(owned); return {}; }
        payload->builtins.emplace_back(name,Root(owned,builtin));
        if (JS_IsObject(builtin)) {
            JSValue prototype = JS_GetPropertyStr(owned,builtin,"prototype");
            if (JS_IsException(prototype)) { JS_FreeValue(owned,builtin); JS_FreeValue(owned,global); detail::CaptureException(isolate,owned); detail::UnregisterContext(owned); return {}; }
            payload->builtins.emplace_back(std::string(name) + ".prototype",Root(owned,prototype));
            if ((!std::strcmp(name,"Object") || !std::strcmp(name,"Function")) && JS_IsObject(prototype)) {
                for (const char* method : {"toString","valueOf"}) {
                    JSValue function = JS_GetPropertyStr(owned,prototype,method);
                    if (JS_IsException(function)) { JS_FreeValue(owned,prototype); JS_FreeValue(owned,builtin); JS_FreeValue(owned,global); detail::CaptureException(isolate,owned); detail::UnregisterContext(owned); return {}; }
                    payload->builtins.emplace_back(std::string(name) + ".prototype." + method,Root(owned,function)); JS_FreeValue(owned,function);
                }
            }
            JS_FreeValue(owned,prototype);
        }
        if (!std::strcmp(name,"Reflect") || !std::strcmp(name,"Object")) {
            for (const char* method : {"set","freeze","seal"}) {
                if (!std::strcmp(method,"set") && std::strcmp(name,"Reflect")) continue;
                if (std::strcmp(method,"set") && std::strcmp(name,"Object")) continue;
                JSValue function = JS_GetPropertyStr(owned,builtin,method);
                if (JS_IsException(function)) { JS_FreeValue(owned,builtin); JS_FreeValue(owned,global); detail::CaptureException(isolate,owned); detail::UnregisterContext(owned); return {}; }
                payload->builtins.emplace_back(std::string(name) + "." + method,Root(owned,function)); JS_FreeValue(owned,function);
            }
        }
        JS_FreeValue(owned,builtin);
    }
    for (int index = int(kArrayProto_entries);index <= int(kSetIteratorPrototype);++index) {
        JSValue intrinsic = IntrinsicValue(owned,static_cast<Intrinsic>(index));
        if (JS_IsException(intrinsic)) { JS_FreeValue(owned,global); detail::CaptureException(isolate,owned); detail::UnregisterContext(owned); return {}; }
        payload->builtins.emplace_back(std::string("@intrinsic") + std::to_string(index),Root(owned,intrinsic)); JS_FreeValue(owned,intrinsic);
    }
    // The context owns and marks these metadata roots exactly once, independently of its globals.
    JS_SetContextNativeData(owned,new std::shared_ptr<ContextPayload>(payload),FinalizeContext,MarkContext);
    JS_FreeValue(owned,global); return context;
}
JSValue SavedBuiltin(JSContext* context,std::string_view name) {
    if (auto* view = detail::LookupContext(context)) if (auto realm = Realm(view)) {
        for (const auto& entry : realm->builtins) if (std::string_view(entry.first) == name) return JS_DupValue(context,entry.second.value);
    }
    return JS_UNDEFINED;
}
}
Local<Context> Context::AttachForQuickJS(Isolate* isolate,JSContext* context) { return context ? RegisterRealm(isolate,JS_DupContext(context)) : Local<Context>(); }
Local<Context> Context::New(Isolate* isolate,ExtensionConfiguration* extensions,MaybeLocal<ObjectTemplate> global_template,MaybeLocal<Value> global_object) {
    JSContext* raw = detail::ConsumePendingContext(isolate);
    if (!raw) raw = JS_NewContext(isolate->GetQuickJSRuntime());
    if (!raw) return {};
    auto context = RegisterRealm(isolate,raw); if (context.IsEmpty()) return {};
    Isolate::Scope current_isolate(isolate); Context::Scope entered(context);
    if (extensions && extensions->begin() != extensions->end()) {
        JS_ThrowTypeError(raw,"Native V8 extensions cannot be installed in a QuickJS realm"); detail::CaptureException(isolate,raw); detail::UnregisterContext(raw); return {};
    }
    Local<ObjectTemplate> object_template;
    if (!global_template.ToLocal(&object_template)) object_template = ObjectTemplate::New(isolate);
    if (object_template.IsEmpty()) return {};
    auto realm = Realm(*context); auto metadata = TemplateData(*object_template);
    JSValue previous = JS_GetGlobalObject(raw);
    JSValue previous_this = JS_GetGlobalThis(raw);
    JSValue inner = NewHost(raw,false);
    if (JS_IsException(inner)) { JS_FreeValue(raw,previous); JS_FreeValue(raw,previous_this); detail::CaptureException(isolate,raw); detail::UnregisterContext(raw); return {}; }
    auto* inner_state = State(inner); CopyConfiguration(inner_state,metadata.get());
    inner_state->brand = metadata->parent;
    if (CopyProperties(raw,previous,inner_state->properties.value) < 0) goto failed;
    {
        Local<Value> reused; JSValue proxy;
        if (global_object.ToLocal(&reused)) {
            auto* state = State(Raw(reused));
            if (!state || !state->detached || state->isolate != isolate) { JS_ThrowTypeError(raw,"Global proxy is not a detached facade realm"); goto failed; }
            proxy = JS_DupValue(raw,Raw(reused));
            if (JS_SetObjectContext(raw,proxy) < 0) { JS_FreeValue(raw,proxy); goto failed; }
            state->detached = false; state->origin = raw;
            state->global_target = Root(raw,inner); state->fields = inner_state->fields;
            JSValue storage = JS_NewObjectProto(raw,JS_NULL);
            if (JS_IsException(storage)) { JS_FreeValue(raw,proxy); goto failed; }
            state->properties = Root(raw,storage); JS_FreeValue(raw,storage);
        } else proxy = NewHost(raw,false);
        if (JS_IsException(proxy)) goto failed;
        auto* proxy_state = State(proxy); proxy_state->global_target = Root(raw,inner); proxy_state->brand = metadata->parent; proxy_state->fields.resize(metadata->internal_fields);
        realm->global_proxy = Root(raw,proxy); realm->global_inner = Root(raw,inner);
        // Identifier bindings use the inner Window; default this uses the stable proxy.
        if (JS_SetGlobalObject(raw,inner) < 0 || JS_SetGlobalThis(raw,proxy) < 0) { JS_FreeValue(raw,proxy); goto failed; }
        JS_FreeValue(raw,proxy);
    }
    if (!JS_IsUndefined(metadata->parent.value)) {
        auto function = InstantiateFunction(raw,metadata->parent.value); if (function.IsEmpty()) goto failed;
        JSValue prototype = JS_GetPropertyStr(raw,Raw(function),"prototype");
        if (JS_IsException(prototype)) goto failed;
        int result = JS_SetPrototypeForEmbedding(raw,inner,prototype);
        if (result >= 0) result = JS_SetPrototypeForEmbedding(raw,realm->global_proxy.value,prototype);
        JS_FreeValue(raw,prototype); if (result < 0) goto failed;
        if (InstallInstance(raw,inner,metadata->parent.value) < 0) goto failed;
    } else if (InstallEntries(raw,inner,metadata.get()) < 0) goto failed;
    metadata->instantiated = true;
    if (JS_DefinePropertyValueStr(raw,inner_state->properties.value,"globalThis",JS_DupValue(raw,realm->global_proxy.value),JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) < 0) goto failed;
    if (inner_state->immutable) {
        if (JS_SetImmutablePrototype(raw,inner) < 0 || JS_SetImmutablePrototype(raw,realm->global_proxy.value) < 0) goto failed;
        State(realm->global_proxy.value)->immutable = true;
    }
    realm->default_token = realm->global_inner; realm->security_token = realm->default_token;
    JS_FreeValue(raw,inner); JS_FreeValue(raw,previous); JS_FreeValue(raw,previous_this); return context;
failed:
    JS_SetGlobalObject(raw,previous); JS_SetGlobalThis(raw,previous_this);
    realm->global_proxy = Root(raw,previous_this); realm->global_inner = Root();
    JS_FreeValue(raw,inner); JS_FreeValue(raw,previous); JS_FreeValue(raw,previous_this);
    if (!detail::HasPendingException(isolate)) detail::CaptureException(isolate,raw);
    detail::UnregisterContext(raw); return {};
}
Local<Object> Context::Global() { auto realm = Realm(this); return ObjectView(realm->context,realm->global_proxy.value); }
void Context::DetachGlobal() {
    auto realm = Realm(this); auto* raw = realm->context;
    // Keep this realm's global_obj: reachable old closures still bind to its inner Window.
    if (auto* state = State(realm->global_proxy.value)) {
        if (state->wrapper) { mini_ui::ClearQuickJSWrapper(state->isolate,realm->global_proxy.value); state->wrapper = nullptr; }
        state->global_target = Root(); state->brand = Root(); state->detached = true;
        state->native_properties.clear(); state->properties = Root();
        for (auto& field : state->fields) field = Field();
        for (int index = 0;index < 2 && size_t(index) < state->fields.size();++index) detail::UpdateWeakInternalField(detail::IsolateOf(this),realm->global_proxy.value,index,nullptr);
        if (JS_SetPrototypeForEmbedding(raw,realm->global_proxy.value,JS_NULL) < 0) detail::CaptureException(detail::IsolateOf(this),raw);
    }
}
void Context::Enter() { detail::EnterContext(detail::IsolateOf(this),this); }
void Context::Exit() { detail::LeaveContext(detail::IsolateOf(this),this); }
Isolate* Context::GetIsolate() { return detail::IsolateOf(this); }
JSContext* Context::GetQuickJSContext() const { return detail::ContextOf(this); }
MicrotaskQueue* Context::GetMicrotaskQueue() {
    auto realm = Realm(this);
    return realm->microtask_queue ? realm->microtask_queue : detail::IsolateOf(this)->GetMicrotaskQueue();
}
void Context::SetMicrotaskQueue(MicrotaskQueue* queue) {
    if (queue && queue->GetIsolate() != detail::IsolateOf(this)) std::abort();
    Realm(this)->microtask_queue = queue;
}
void Context::SetSecurityToken(Local<Value> token) { Realm(this)->security_token = Hold(token); }
void Context::UseDefaultSecurityToken() { auto realm = Realm(this); realm->security_token = realm->default_token; }
Local<Value> Context::GetSecurityToken() { auto realm = Realm(this); return View(realm->context,realm->security_token.value); }
uint32_t Context::GetNumberOfEmbedderDataFields() { return static_cast<uint32_t>(Realm(this)->fields.size()); }
Local<Value> Context::GetEmbedderData(int index) { auto realm = Realm(this); if (index < 0 || size_t(index) >= realm->fields.size() || realm->fields[index].is_pointer) return {}; return View(realm->context,realm->fields[index].value.value); }
void Context::SetEmbedderData(int index,Local<Value> value) { if (index < 0) std::abort(); auto realm = Realm(this); if (size_t(index) >= realm->fields.size()) realm->fields.resize(size_t(index) + 1); auto& field = realm->fields[index]; field.value = Hold(value); field.pointer = nullptr; field.is_pointer = false; }
void* Context::GetAlignedPointerFromEmbedderData(int index) { auto realm = Realm(this); if (index < 0 || size_t(index) >= realm->fields.size()) return nullptr; auto& field = realm->fields[index]; return field.is_pointer ? field.pointer : nullptr; }
void Context::SetAlignedPointerInEmbedderData(int index,void* value) { if (index < 0 || (reinterpret_cast<uintptr_t>(value) & 1)) std::abort(); auto realm = Realm(this); if (size_t(index) >= realm->fields.size()) realm->fields.resize(size_t(index) + 1); auto& field = realm->fields[index]; field.value = Root(); field.pointer = value; field.is_pointer = true; }
Local<Object> Context::GetExtrasBindingObject() { auto realm = Realm(this); return ObjectView(realm->context,realm->extras.value); }
bool Context::HasTemplateLiteralObject(Local<Value> value) {
    if (value.IsEmpty() || detail::IsolateOf(*value) != detail::IsolateOf(this))
        return false;
    return JS_HasTemplateLiteralObject(detail::ContextOf(this),Raw(value));
}
void Context::AllowCodeGenerationFromStrings(bool allow) { auto realm = Realm(this); realm->code_generation_allowed = allow; JS_SetCodeGenerationAllowed(realm->context,allow); }
bool Context::IsCodeGenerationFromStringsAllowed() const { return Realm(this)->code_generation_allowed; }
void Context::SetErrorMessageForCodeGenerationFromStrings(Local<String> message) { auto realm = Realm(this); realm->code_generation_error = Hold(message); JS_SetCodeGenerationError(realm->context,Raw(message)); }
void FunctionTemplate::SetExceptionContext(ExceptionContext context) { auto metadata = TemplateData(this); CheckMutable(metadata); metadata->exception_context = static_cast<uint32_t>(context); }
MaybeLocal<Proxy> Proxy::New(Local<Context> context,Local<Object> target,Local<Object> handler) {
    auto* raw = CheckedContext(context,*target); if (!raw || !CheckedContext(context,*handler)) return {};
    JSValue constructor = SavedBuiltin(raw,"Proxy"); JSValue arguments[]{Raw(target),Raw(handler)};
    JSValue proxy = JS_CallConstructor(raw,constructor,2,arguments); JS_FreeValue(raw,constructor);
    auto value = detail::Adopt(raw,proxy); return value.IsEmpty() ? MaybeLocal<Proxy>() : MaybeLocal<Proxy>(value.As<Proxy>());
}
Local<Value> Proxy::GetTarget() { auto* context = detail::ContextOf(this); return detail::Adopt(context,JS_GetProxyTarget(context,Raw(this))); }
Local<Value> Proxy::GetHandler() { auto* context = detail::ContextOf(this); return detail::Adopt(context,JS_GetProxyHandler(context,Raw(this))); }
bool Proxy::IsRevoked() const { return JS_IsProxyRevoked(Raw(this)); }
void Proxy::Revoke() { auto* context = detail::ContextOf(this); if (JS_RevokeProxy(context,Raw(this)) < 0) detail::CaptureException(detail::IsolateOf(this),context); }
Local<FunctionTemplate> FunctionTemplate::NewWithCFunctionOverloads(Isolate* isolate,FunctionCallback callback,Local<Value> data,Local<Signature> signature,int length,ConstructorBehavior behavior,SideEffectType side_effect,const MemorySpan<const CFunction>&) {
    return New(isolate,callback,data,signature,length,behavior,side_effect);
}
Local<FunctionTemplate> FunctionTemplate::NewWithCache(Isolate* isolate,FunctionCallback callback,Local<Private> key,Local<Value> data,Local<Signature> signature,int length,SideEffectType side_effect) {
    auto result = New(isolate,callback,data,signature,length,ConstructorBehavior::kAllow,side_effect);
    if (!result.IsEmpty()) TemplateData(*result)->cache_property = Hold(key);
    return result;
}
Local<DictionaryTemplate> DictionaryTemplate::New(Isolate* isolate,MemorySpan<const std::string_view> names) {
    auto* context = detail::NeutralJSContext(isolate); auto metadata = std::make_shared<TemplatePayload>();
    metadata->entries.reserve(names.size());
    for (auto name : names) {
        JSValue key = JS_NewStringLen(context,name.data(),name.size());
        if (JS_IsException(key)) { detail::CaptureException(isolate,context); return {}; }
        TemplateEntry entry; entry.key = Root(context,key); JS_FreeValue(context,key); metadata->entries.push_back(std::move(entry));
    }
    return detail::MakeLocal<DictionaryTemplate>(isolate,context,NewTemplate(context,metadata),metadata);
}
Local<Object> DictionaryTemplate::NewInstance(Local<Context> context,MemorySpan<MaybeLocal<Value>> values) {
    auto* raw = CheckedContext(context,this); if (!raw) return {};
    auto metadata = TemplateData(this); if (values.size() != metadata->entries.size()) std::abort();
    JSValue object = NewHost(raw); if (JS_IsException(object)) { detail::CaptureException(detail::IsolateOf(this),raw); return {}; }
    for (size_t i = 0;i < values.size();++i) {
        Local<Value> value; if (!values[i].ToLocal(&value)) continue;
        Atom atom(raw,metadata->entries[i].key.value);
        if (!atom.value || JS_DefinePropertyValue(raw,State(object)->properties.value,atom.value,JS_DupValue(raw,Raw(value)),JS_PROP_C_W_E) < 0) {
            JS_FreeValue(raw,object); detail::CaptureException(detail::IsolateOf(this),raw); return {};
        }
    }
    return detail::Adopt(raw,object).As<Object>();
}
} // namespace v8
