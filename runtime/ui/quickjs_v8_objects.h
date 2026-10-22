// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef MINI_UI_RUNTIME_UI_QUICKJS_V8_OBJECTS_H_
#define MINI_UI_RUNTIME_UI_QUICKJS_V8_OBJECTS_H_
#include "runtime/ui/quickjs_v8_core.h"
#define MINI_UI_QUICKJS_V8_VALUES_BASE_ONLY
#include "runtime/ui/quickjs_v8_values.h"
#undef MINI_UI_QUICKJS_V8_VALUES_BASE_ONLY
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>

namespace v8 {
class Object;
class Array;
class Function;
class FunctionTemplate;
class ScriptOrigin;
class ObjectTemplate;
class Signature;
class MicrotaskQueue;
class CFunction;
enum class ExceptionContext : uint32_t;
class PropertyDescriptor;
template<class T> class FunctionCallbackInfo;
template<class T> class PropertyCallbackInfo;

enum PropertyAttribute { None = 0, ReadOnly = 1, DontEnum = 2, DontDelete = 4 };
enum AccessControl { DEFAULT = 0 };
enum PropertyFilter { ALL_PROPERTIES = 0, ONLY_WRITABLE = 1, ONLY_ENUMERABLE = 2, ONLY_CONFIGURABLE = 4, SKIP_STRINGS = 8, SKIP_SYMBOLS = 16 };
enum class SideEffectType { kHasSideEffect, kHasNoSideEffect, kHasSideEffectToReceiver };
enum class ConstructorBehavior { kThrow, kAllow };
enum class KeyCollectionMode { kOwnOnly, kIncludePrototypes };
enum class IndexFilter { kIncludeIndices, kSkipIndices };
enum class KeyConversionMode { kConvertToString, kKeepNumbers, kNoNumbers };
enum class IntegrityLevel { kFrozen, kSealed };
enum class Intercepted : uint8_t { kNo, kYes };
enum class PropertyHandlerFlags { kNone = 0, kNonMasking = 1, kOnlyInterceptStrings = 2, kHasNoSideEffect = 4 };
enum Intrinsic { kArrayProto_entries, kArrayProto_forEach, kArrayProto_keys, kArrayProto_values, kArrayPrototype, kAsyncIteratorPrototype, kErrorPrototype, kIteratorPrototype, kMapIteratorPrototype, kObjProto_valueOf, kSetIteratorPrototype };
enum class CppHeapPointerTag : uint16_t { kFirstTag = 0, kNullTag = 0, kDefaultTag = 0x7000, kZappedEntryTag = 0x7ffd, kEvacuationEntryTag = 0x7ffe, kFreeEntryTag = 0x7fff, kLastTag = 0x7fff };
struct CppHeapPointerTagRange {
    constexpr CppHeapPointerTagRange(CppHeapPointerTag lower, CppHeapPointerTag upper) : lower_bound(lower), upper_bound(upper) {}
    constexpr explicit CppHeapPointerTagRange(CppHeapPointerTag tag) : lower_bound(tag), upper_bound(tag) {}
    CppHeapPointerTag lower_bound, upper_bound;
};
inline constexpr CppHeapPointerTagRange kAnyCppHeapPointer(CppHeapPointerTag::kFirstTag,CppHeapPointerTag::kLastTag);
namespace detail {
struct CallbackState {
    Isolate* isolate;
    JSContext* context;
    Local<Context> entered_context;
    Local<Object> receiver;
    Local<Object> holder;
    Local<Value> data;
    Local<Value> new_target;
    Local<Value>* arguments = nullptr;
    int argument_count = 0;
    JSValue result = JS_UNDEFINED;
    bool result_set = false;
    bool construct = false;
    bool should_throw = false;
    CallbackState(Isolate*, JSContext*);
    ~CallbackState();
    CallbackState(const CallbackState&) = delete;
    CallbackState& operator=(const CallbackState&) = delete;
    void SetResult(JSValue owned);
};
bool IsHostObject(JSContext*, JSValueConst);
Local<Context> ContextForQuickJS(JSContext*);
JSContext* JSContextOf(Local<Context>);
Local<Object> LegacyPropertyHolder(Local<Object>);
void* UnwrapObject(Isolate*, const Object*, CppHeapPointerTagRange);
void WrapObject(Isolate*, Object*, void*, CppHeapPointerTag);
}

template<class T> class ReturnValue {
public:
    explicit ReturnValue(detail::CallbackState* state) : state_(state) {}
    template<class S> ReturnValue(const ReturnValue<S>& value) : state_(value.state_) {
        static_assert(std::is_base_of<T,S>::value,"incompatible callback return value");
    }
    template<class S> void Set(Local<S> value) const { state_->SetResult(value.IsEmpty() ? JS_UNDEFINED : JS_DupValue(state_->context, detail::Borrow(*value))); }
    template<class S> void Set(const Global<S>& value) const { Set(value.Get(state_->isolate)); }
    template<class S> void Set(const BasicTracedReference<S>& value) const { Set(value.Get(state_->isolate)); }
    template<class S> void SetNonEmpty(const S& value) const { Set(value); }
    template<class S> void Set(S*) const = delete;
    void Set(bool value) const { state_->SetResult(JS_NewBool(state_->context, value)); }
    void Set(double value) const { state_->SetResult(JS_NewFloat64(state_->context, value)); }
    void Set(int16_t value) const { Set(int32_t(value)); }
    void Set(uint16_t value) const { Set(uint32_t(value)); }
    void Set(int32_t value) const { state_->SetResult(JS_NewInt32(state_->context, value)); }
    void Set(uint32_t value) const { state_->SetResult(JS_NewUint32(state_->context, value)); }
    void Set(int64_t value) const { Set(double(value)); }
    void Set(uint64_t value) const { Set(double(value)); }
    void SetNull() const { state_->SetResult(JS_NULL); }
    void SetUndefined() const { state_->SetResult(JS_UNDEFINED); }
    void SetFalse() const { Set(false); }
    void SetEmptyString() const { state_->SetResult(JS_NewStringLen(state_->context, "", 0)); }
    Local<Value> Get() const { return detail::Adopt(state_->context, JS_DupValue(state_->context, state_->result)); }
    Isolate* GetIsolate() const { return state_->isolate; }
private:
    template<class> friend class ReturnValue;
    detail::CallbackState* state_;
};
template<class T> class FunctionCallbackInfo {
public:
    explicit FunctionCallbackInfo(detail::CallbackState& state) : state_(&state) {}
    int Length() const { return state_->argument_count; }
    Local<Value> operator[](int index) const { return index >= 0 && index < Length() ? state_->arguments[index] : detail::Adopt(state_->context, JS_UNDEFINED); }
    Local<Object> This() const { return state_->receiver; }
    Local<Object> Holder() const { return state_->holder; }
    Local<Object> HolderSoonToBeDeprecated() const { return Holder(); }
    Local<Value> NewTarget() const { return state_->new_target; }
    bool IsConstructCall() const { return state_->construct; }
    Local<Value> Data() const { return state_->data; }
    Isolate* GetIsolate() const { return state_->isolate; }
    ReturnValue<T> GetReturnValue() const { return ReturnValue<T>(state_); }
private:
    detail::CallbackState* state_;
};
template<class T> class PropertyCallbackInfo {
public:
    explicit PropertyCallbackInfo(detail::CallbackState& state) : state_(&state) {
        if (!state.result_set) {
            JS_FreeValue(state.context,state.result);
            if constexpr (std::is_same<T,void>::value || std::is_same<T,Boolean>::value) state.result = JS_TRUE;
            else if constexpr (std::is_same<T,Integer>::value) state.result = JS_NewInt32(state.context,0);
            else state.result = JS_UNDEFINED;
        }
    }
    Isolate* GetIsolate() const { return state_->isolate; }
    Local<Value> Data() const { return state_->data; }
    Local<Object> This() const { return state_->receiver; }
    Local<Object> Holder() const { return detail::LegacyPropertyHolder(state_->holder); }
    Local<Object> HolderV2() const { return state_->holder; }
    ReturnValue<T> GetReturnValue() const { return ReturnValue<T>(state_); }
    bool ShouldThrowOnError() const { return state_->should_throw; }
private:
    detail::CallbackState* state_;
};
using FunctionCallback = void (*)(const FunctionCallbackInfo<Value>&);
using AccessorNameGetterCallback = void (*)(Local<Name>, const PropertyCallbackInfo<Value>&);
using AccessorNameSetterCallback = void (*)(Local<Name>, Local<Value>, const PropertyCallbackInfo<void>&);
using NamedPropertyGetterCallback = Intercepted (*)(Local<Name>, const PropertyCallbackInfo<Value>&);
using NamedPropertySetterCallback = Intercepted (*)(Local<Name>, Local<Value>, const PropertyCallbackInfo<void>&);
using NamedPropertyQueryCallback = Intercepted (*)(Local<Name>, const PropertyCallbackInfo<Integer>&);
using NamedPropertyDeleterCallback = Intercepted (*)(Local<Name>, const PropertyCallbackInfo<Boolean>&);
using NamedPropertyEnumeratorCallback = void (*)(const PropertyCallbackInfo<Array>&);
using NamedPropertyDefinerCallback = Intercepted (*)(Local<Name>, const PropertyDescriptor&, const PropertyCallbackInfo<void>&);
using NamedPropertyDescriptorCallback = Intercepted (*)(Local<Name>, const PropertyCallbackInfo<Value>&);
using IndexedPropertyGetterCallbackV2 = Intercepted (*)(uint32_t, const PropertyCallbackInfo<Value>&);
using IndexedPropertySetterCallbackV2 = Intercepted (*)(uint32_t, Local<Value>, const PropertyCallbackInfo<void>&);
using IndexedPropertyQueryCallbackV2 = Intercepted (*)(uint32_t, const PropertyCallbackInfo<Integer>&);
using IndexedPropertyDeleterCallbackV2 = Intercepted (*)(uint32_t, const PropertyCallbackInfo<Boolean>&);
using IndexedPropertyEnumeratorCallback = void (*)(const PropertyCallbackInfo<Array>&);
using IndexedPropertyDefinerCallbackV2 = Intercepted (*)(uint32_t, const PropertyDescriptor&, const PropertyCallbackInfo<void>&);
using IndexedPropertyDescriptorCallbackV2 = Intercepted (*)(uint32_t, const PropertyCallbackInfo<Value>&);
using AccessCheckCallback = bool (*)(Local<Context>, Local<Object>, Local<Value>);

struct NamedPropertyHandlerConfiguration {
    NamedPropertyGetterCallback getter = nullptr;
    NamedPropertySetterCallback setter = nullptr;
    NamedPropertyQueryCallback query = nullptr;
    NamedPropertyDeleterCallback deleter = nullptr;
    NamedPropertyEnumeratorCallback enumerator = nullptr;
    NamedPropertyDefinerCallback definer = nullptr;
    NamedPropertyDescriptorCallback descriptor = nullptr;
    Local<Value> data;
    PropertyHandlerFlags flags = PropertyHandlerFlags::kNone;
    NamedPropertyHandlerConfiguration() = default;
    NamedPropertyHandlerConfiguration(NamedPropertyGetterCallback g, NamedPropertySetterCallback s, NamedPropertyQueryCallback q, NamedPropertyDeleterCallback d, NamedPropertyEnumeratorCallback e, NamedPropertyDefinerCallback f, NamedPropertyDescriptorCallback r, Local<Value> v = {}, PropertyHandlerFlags flags = PropertyHandlerFlags::kNone) : getter(g), setter(s), query(q), deleter(d), enumerator(e), definer(f), descriptor(r), data(v), flags(flags) {}
    explicit NamedPropertyHandlerConfiguration(NamedPropertyGetterCallback g, NamedPropertySetterCallback s = nullptr, NamedPropertyQueryCallback q = nullptr, NamedPropertyDeleterCallback d = nullptr, NamedPropertyEnumeratorCallback e = nullptr, Local<Value> v = {}, PropertyHandlerFlags flags = PropertyHandlerFlags::kNone) : NamedPropertyHandlerConfiguration(g,s,q,d,e,nullptr,nullptr,v,flags) {}
    NamedPropertyHandlerConfiguration(NamedPropertyGetterCallback g, NamedPropertySetterCallback s, NamedPropertyDescriptorCallback r, NamedPropertyDeleterCallback d, NamedPropertyEnumeratorCallback e, NamedPropertyDefinerCallback f, Local<Value> v = {}, PropertyHandlerFlags flags = PropertyHandlerFlags::kNone) : NamedPropertyHandlerConfiguration(g,s,nullptr,d,e,f,r,v,flags) {}
};
struct IndexedPropertyHandlerConfiguration {
    IndexedPropertyGetterCallbackV2 getter = nullptr;
    IndexedPropertySetterCallbackV2 setter = nullptr;
    IndexedPropertyQueryCallbackV2 query = nullptr;
    IndexedPropertyDeleterCallbackV2 deleter = nullptr;
    IndexedPropertyEnumeratorCallback enumerator = nullptr;
    IndexedPropertyDefinerCallbackV2 definer = nullptr;
    IndexedPropertyDescriptorCallbackV2 descriptor = nullptr;
    Local<Value> data;
    PropertyHandlerFlags flags = PropertyHandlerFlags::kNone;
    IndexedPropertyHandlerConfiguration() = default;
    IndexedPropertyHandlerConfiguration(IndexedPropertyGetterCallbackV2 g, IndexedPropertySetterCallbackV2 s, IndexedPropertyQueryCallbackV2 q, IndexedPropertyDeleterCallbackV2 d, IndexedPropertyEnumeratorCallback e, IndexedPropertyDefinerCallbackV2 f, IndexedPropertyDescriptorCallbackV2 r, Local<Value> v = {}, PropertyHandlerFlags flags = PropertyHandlerFlags::kNone) : getter(g), setter(s), query(q), deleter(d), enumerator(e), definer(f), descriptor(r), data(v), flags(flags) {}
    explicit IndexedPropertyHandlerConfiguration(IndexedPropertyGetterCallbackV2 g, IndexedPropertySetterCallbackV2 s = nullptr, IndexedPropertyQueryCallbackV2 q = nullptr, IndexedPropertyDeleterCallbackV2 d = nullptr, IndexedPropertyEnumeratorCallback e = nullptr, Local<Value> v = {}, PropertyHandlerFlags flags = PropertyHandlerFlags::kNone) : IndexedPropertyHandlerConfiguration(g,s,q,d,e,nullptr,nullptr,v,flags) {}
    IndexedPropertyHandlerConfiguration(IndexedPropertyGetterCallbackV2 g, IndexedPropertySetterCallbackV2 s, IndexedPropertyDescriptorCallbackV2 r, IndexedPropertyDeleterCallbackV2 d, IndexedPropertyEnumeratorCallback e, IndexedPropertyDefinerCallbackV2 f, Local<Value> v = {}, PropertyHandlerFlags flags = PropertyHandlerFlags::kNone) : IndexedPropertyHandlerConfiguration(g,s,nullptr,d,e,f,r,v,flags) {}
};

class PropertyDescriptor {
public:
    PropertyDescriptor();
    explicit PropertyDescriptor(Local<Value>);
    PropertyDescriptor(Local<Value>, bool writable);
    PropertyDescriptor(Local<Value> getter, Local<Value> setter);
    ~PropertyDescriptor();
    PropertyDescriptor(const PropertyDescriptor&) = delete;
    PropertyDescriptor& operator=(const PropertyDescriptor&) = delete;
    Local<Value> value() const; bool has_value() const;
    Local<Value> get() const; bool has_get() const;
    Local<Value> set() const; bool has_set() const;
    void set_enumerable(bool); bool enumerable() const; bool has_enumerable() const;
    void set_configurable(bool); bool configurable() const; bool has_configurable() const;
    bool writable() const; bool has_writable() const;
    struct PrivateData {
        Local<Value> value,getter,setter;
        int flags = 0;
    };
    PrivateData* get_private() const { return &private_; }
private:
    mutable PrivateData private_;
};

class Object : public Value {
public:
    using Value::Value;
    static Object* Cast(Value* value) { return detail::RebindView<Object>(detail::CellOf(value)); }
    static Local<Object> New(Isolate*);
    static Local<Object> New(Isolate*, Local<Value>, Local<Name>*, Local<Value>*, size_t);
    Maybe<bool> Set(Local<Context>, Local<Value>, Local<Value>);
    Maybe<bool> Set(Local<Context>, Local<Value>, Local<Value>, MaybeLocal<Object>);
    Maybe<bool> Set(Local<Context>, uint32_t, Local<Value>);
    MaybeLocal<Value> Get(Local<Context>, Local<Value>);
    MaybeLocal<Value> Get(Local<Context>, Local<Value>, MaybeLocal<Object>);
    MaybeLocal<Value> Get(Local<Context>, uint32_t);
    Maybe<bool> Has(Local<Context>, Local<Value>);
    Maybe<bool> Has(Local<Context>, uint32_t);
    Maybe<bool> Delete(Local<Context>, Local<Value>);
    Maybe<bool> Delete(Local<Context>, uint32_t);
    Maybe<bool> CreateDataProperty(Local<Context>, Local<Name>, Local<Value>);
    Maybe<bool> CreateDataProperty(Local<Context>, uint32_t, Local<Value>);
    Maybe<bool> DefineOwnProperty(Local<Context>, Local<Name>, Local<Value>, PropertyAttribute = None);
    Maybe<bool> DefineProperty(Local<Context>, Local<Name>, PropertyDescriptor&);
    Maybe<PropertyAttribute> GetPropertyAttributes(Local<Context>, Local<Value>);
    MaybeLocal<Value> GetOwnPropertyDescriptor(Local<Context>, Local<Name>);
    void SetAccessorProperty(Local<Name>, Local<Function>, Local<Function> = {}, PropertyAttribute = None);
    Maybe<bool> SetNativeDataProperty(Local<Context>, Local<Name>, AccessorNameGetterCallback, AccessorNameSetterCallback = nullptr, Local<Value> = {}, PropertyAttribute = None, SideEffectType = SideEffectType::kHasSideEffect, SideEffectType = SideEffectType::kHasSideEffect);
    Maybe<bool> SetLazyDataProperty(Local<Context>, Local<Name>, AccessorNameGetterCallback, Local<Value> = {}, PropertyAttribute = None, SideEffectType = SideEffectType::kHasSideEffect, SideEffectType = SideEffectType::kHasSideEffect);
    Maybe<bool> HasPrivate(Local<Context>, Local<Private>);
    Maybe<bool> SetPrivate(Local<Context>, Local<Private>, Local<Value>);
    Maybe<bool> DeletePrivate(Local<Context>, Local<Private>);
    MaybeLocal<Value> GetPrivate(Local<Context>, Local<Private>);
    MaybeLocal<Array> GetPropertyNames(Local<Context>);
    MaybeLocal<Array> GetPropertyNames(Local<Context>, KeyCollectionMode, PropertyFilter, IndexFilter, KeyConversionMode = KeyConversionMode::kKeepNumbers);
    MaybeLocal<Array> GetOwnPropertyNames(Local<Context>);
    MaybeLocal<Array> GetOwnPropertyNames(Local<Context>, PropertyFilter, KeyConversionMode = KeyConversionMode::kKeepNumbers);
    Local<Value> GetPrototype();
    Local<Value> GetPrototypeV2();
    Maybe<bool> SetPrototype(Local<Context>, Local<Value>);
    Maybe<bool> SetPrototypeV2(Local<Context>, Local<Value>);
    Local<Object> FindInstanceInPrototypeChain(Local<FunctionTemplate>);
    MaybeLocal<String> ObjectProtoToString(Local<Context>);
    Local<String> GetConstructorName();
    Maybe<bool> SetIntegrityLevel(Local<Context>, IntegrityLevel);
    int InternalFieldCount() const;
    Local<Data> GetInternalField(int);
    void SetInternalField(int, Local<Data>);
    void* GetAlignedPointerFromInternalField(int);
    void* GetAlignedPointerFromInternalField(Isolate*, int index) { return GetAlignedPointerFromInternalField(index); }
    void SetAlignedPointerInInternalField(int, void*);
    void SetAlignedPointerInInternalFields(int, int[], void*[]);
    template<CppHeapPointerTag tag, class T = void> static T* Unwrap(Isolate* isolate, const Local<Object>& wrapper) { return Unwrap<T>(isolate,wrapper,CppHeapPointerTagRange(tag)); }
    template<class T = void> static T* Unwrap(Isolate* isolate, const Local<Object>& wrapper, CppHeapPointerTagRange range) { return wrapper.IsEmpty() ? nullptr : static_cast<T*>(detail::UnwrapObject(isolate,*wrapper,range)); }
    template<CppHeapPointerTag tag> static void Wrap(Isolate* isolate, const Local<Object>& wrapper, void* pointer) { Wrap(isolate,wrapper,pointer,tag); }
    static void Wrap(Isolate* isolate, const Local<Object>& wrapper, void* pointer, CppHeapPointerTag tag) { detail::WrapObject(isolate,*wrapper,pointer,tag); }
    template<CppHeapPointerTag tag, class T = void> static T* Unwrap(Isolate* isolate, const PersistentBase<Object>& wrapper) { return Unwrap<T>(isolate,wrapper.Get(isolate),CppHeapPointerTagRange(tag)); }
    template<CppHeapPointerTag tag, class T = void> static T* Unwrap(Isolate* isolate, const BasicTracedReference<Object>& wrapper) { return Unwrap<T>(isolate,wrapper.Get(isolate),CppHeapPointerTagRange(tag)); }
    template<class T = void> static T* Unwrap(Isolate* isolate, const PersistentBase<Object>& wrapper, CppHeapPointerTagRange range) { return Unwrap<T>(isolate,wrapper.Get(isolate),range); }
    template<class T = void> static T* Unwrap(Isolate* isolate, const BasicTracedReference<Object>& wrapper, CppHeapPointerTagRange range) { return Unwrap<T>(isolate,wrapper.Get(isolate),range); }
    template<CppHeapPointerTag tag> static void Wrap(Isolate* isolate, const PersistentBase<Object>& wrapper, void* pointer) { Wrap(isolate,wrapper.Get(isolate),pointer,tag); }
    template<CppHeapPointerTag tag> static void Wrap(Isolate* isolate, const BasicTracedReference<Object>& wrapper, void* pointer) { Wrap(isolate,wrapper.Get(isolate),pointer,tag); }
    static void Wrap(Isolate* isolate, const PersistentBase<Object>& wrapper, void* pointer, CppHeapPointerTag tag) { Wrap(isolate,wrapper.Get(isolate),pointer,tag); }
    static void Wrap(Isolate* isolate, const BasicTracedReference<Object>& wrapper, void* pointer, CppHeapPointerTag tag) { Wrap(isolate,wrapper.Get(isolate),pointer,tag); }
    Maybe<bool> HasOwnProperty(Local<Context>, Local<Name>);
    Maybe<bool> HasOwnProperty(Local<Context>, uint32_t);
    Maybe<bool> HasRealNamedProperty(Local<Context>, Local<Name>);
    Maybe<bool> HasRealIndexedProperty(Local<Context>, uint32_t);
    Maybe<bool> HasRealNamedCallbackProperty(Local<Context>, Local<Name>);
    MaybeLocal<Value> GetRealNamedProperty(Local<Context>, Local<Name>);
    MaybeLocal<Value> GetRealNamedPropertyInPrototypeChain(Local<Context>, Local<Name>);
    Maybe<PropertyAttribute> GetRealNamedPropertyAttributes(Local<Context>, Local<Name>);
    Maybe<PropertyAttribute> GetRealNamedPropertyAttributesInPrototypeChain(Local<Context>, Local<Name>);
    bool HasNamedLookupInterceptor() const;
    bool HasIndexedLookupInterceptor() const;
    int GetIdentityHash();
    Local<Object> Clone();
    Local<Object> Clone(Isolate*);
    MaybeLocal<Context> GetCreationContext();
    MaybeLocal<Context> GetCreationContext(Isolate*);
    Local<Context> GetCreationContextChecked();
    Local<Context> GetCreationContextChecked(Isolate* isolate) { return GetCreationContext(isolate).ToLocalChecked(); }
    void* GetAlignedPointerFromEmbedderDataInCreationContext(Isolate*, int);
    void* GetAlignedPointerFromEmbedderDataInCreationContext(int);
    bool IsCallable() const;
    bool IsConstructor() const;
    bool IsApiWrapper() const;
    bool IsUndetectable() const;
    bool IsCodeLike(Isolate*) const;
    MaybeLocal<Value> CallAsFunction(Local<Context>, Local<Value>, int, Local<Value>[]);
    MaybeLocal<Value> CallAsConstructor(Local<Context>, int, Local<Value>[]);
    Isolate* GetIsolate();
};

class Function : public Object {
public:
    using Object::Object;
    static Function* Cast(Value* value) { return detail::RebindView<Function>(detail::CellOf(value)); }
    static MaybeLocal<Function> New(Local<Context>, FunctionCallback, Local<Value> = {}, int = 0, ConstructorBehavior = ConstructorBehavior::kAllow, SideEffectType = SideEffectType::kHasSideEffect);
    MaybeLocal<Object> NewInstance(Local<Context>, int, Local<Value>[]) const;
    MaybeLocal<Object> NewInstance(Local<Context> context) const { return NewInstance(context,0,nullptr); }
    MaybeLocal<Object> NewInstanceWithSideEffectType(Local<Context> context,int argc,Local<Value> argv[],SideEffectType = SideEffectType::kHasSideEffect) const { return NewInstance(context,argc,argv); }
    MaybeLocal<Value> Call(Local<Context>, Local<Value>, int, Local<Value>[]);
    MaybeLocal<Value> Call(Isolate*, Local<Context> context, Local<Value> receiver, int argc, Local<Value> argv[]) { return Call(context,receiver,argc,argv); }
    void SetName(Local<String>);
    Local<Value> GetName() const;
    Local<Value> GetInferredName() const;
    Local<Value> GetDebugName() const;
    ScriptOrigin GetScriptOrigin() const;
    int GetScriptLineNumber() const;
    int GetScriptColumnNumber() const;
    int ScriptId() const;
    Local<Value> GetBoundFunction() const;
    MaybeLocal<String> FunctionProtoToString(Local<Context>);
};

class Proxy : public Object {
public:
    using Object::Object;
    static Proxy* Cast(Value* value) { return detail::RebindView<Proxy>(detail::CellOf(value)); }
    static MaybeLocal<Proxy> New(Local<Context>, Local<Object>, Local<Object>);
    Local<Value> GetTarget();
    Local<Value> GetHandler();
    bool IsRevoked() const;
    void Revoke();
};

class Template : public Data {
public:
    using Data::Data;
    void Set(Local<Name>, Local<Data>, PropertyAttribute = None);
    void Set(Isolate*, const char*, Local<Data>, PropertyAttribute = None);
    void SetPrivate(Local<Private>, Local<Data>, PropertyAttribute = None);
    void SetAccessorProperty(Local<Name>, Local<FunctionTemplate> = {}, Local<FunctionTemplate> = {}, PropertyAttribute = None);
    void SetNativeDataProperty(Local<Name>, AccessorNameGetterCallback, AccessorNameSetterCallback = nullptr, Local<Value> = {}, PropertyAttribute = None, SideEffectType = SideEffectType::kHasSideEffect, SideEffectType = SideEffectType::kHasSideEffect);
    void SetLazyDataProperty(Local<Name>, AccessorNameGetterCallback, Local<Value> = {}, PropertyAttribute = None, SideEffectType = SideEffectType::kHasSideEffect, SideEffectType = SideEffectType::kHasSideEffect);
    void SetIntrinsicDataProperty(Local<Name>, Intrinsic, PropertyAttribute = None);
};
class FunctionTemplate : public Template {
public:
    using Template::Template;
    static FunctionTemplate* Cast(Data* value) { return detail::RebindView<FunctionTemplate>(detail::CellOf(value)); }
    static Local<FunctionTemplate> New(Isolate*, FunctionCallback = nullptr, Local<Value> = {}, Local<Signature> = {}, int = 0, ConstructorBehavior = ConstructorBehavior::kAllow, SideEffectType = SideEffectType::kHasSideEffect, const CFunction* = nullptr, uint16_t = 0, uint16_t = 0, uint16_t = 0);
    static Local<FunctionTemplate> NewWithCFunctionOverloads(Isolate*, FunctionCallback = nullptr, Local<Value> = {}, Local<Signature> = {}, int = 0, ConstructorBehavior = ConstructorBehavior::kAllow, SideEffectType = SideEffectType::kHasSideEffect, const MemorySpan<const CFunction>& = {});
    static Local<FunctionTemplate> NewWithCache(Isolate*, FunctionCallback, Local<Private>, Local<Value> = {}, Local<Signature> = {}, int = 0, SideEffectType = SideEffectType::kHasSideEffect);
    MaybeLocal<Function> GetFunction(Local<Context>);
    void SetCallHandler(FunctionCallback, Local<Value> = {}, SideEffectType = SideEffectType::kHasSideEffect, const MemorySpan<const CFunction>& = {});
    void SetLength(int);
    Local<ObjectTemplate> InstanceTemplate();
    Local<ObjectTemplate> PrototypeTemplate();
    void Inherit(Local<FunctionTemplate>);
    void SetPrototypeProviderTemplate(Local<FunctionTemplate>);
    void SetClassName(Local<String>);
    void SetInterfaceName(Local<String>);
    void SetExceptionContext(ExceptionContext);
    void SetAcceptAnyReceiver(bool);
    void ReadOnlyPrototype();
    void RemovePrototype();
    bool HasInstance(Local<Value>);
    bool IsLeafTemplateForApiObject(Local<Value>) const;
};
class ObjectTemplate : public Template {
public:
    using Template::Template;
    static ObjectTemplate* Cast(Data* value) { return detail::RebindView<ObjectTemplate>(detail::CellOf(value)); }
    static Local<ObjectTemplate> New(Isolate*, Local<FunctionTemplate> = {});
    MaybeLocal<Object> NewInstance(Local<Context>);
    void SetHandler(const NamedPropertyHandlerConfiguration&);
    void SetHandler(const IndexedPropertyHandlerConfiguration&);
    void SetCallAsFunctionHandler(FunctionCallback, Local<Value> = {});
    void SetAccessCheckCallback(AccessCheckCallback, Local<Value> = {});
    void SetAccessCheckCallbackAndHandler(AccessCheckCallback, const NamedPropertyHandlerConfiguration&, const IndexedPropertyHandlerConfiguration&, Local<Value> = {});
    void MarkAsUndetectable();
    int InternalFieldCount() const;
    void SetInternalFieldCount(int);
    bool IsImmutableProto() const;
    void SetImmutableProto();
    void SetCodeLike();
    bool IsCodeLike() const;
};
class DictionaryTemplate : public Data {
public:
    using Data::Data;
    static DictionaryTemplate* Cast(Data* data) { return detail::RebindView<DictionaryTemplate>(detail::CellOf(data)); }
    static Local<DictionaryTemplate> New(Isolate*, MemorySpan<const std::string_view>);
    Local<Object> NewInstance(Local<Context>, MemorySpan<MaybeLocal<Value>>);
};
class Signature : public Data {
public:
    using Data::Data;
    static Signature* Cast(Data* value) { return detail::RebindView<Signature>(detail::CellOf(value)); }
    static Local<Signature> New(Isolate*, Local<FunctionTemplate> = {});
};
class ExtensionConfiguration {
public:
    ExtensionConfiguration() = default;
    ExtensionConfiguration(int count,const char* names[]) : count_(count),names_(names) {}
    const char* const* begin() const { return names_; }
    const char* const* end() const { return names_ ? names_ + count_ : names_; }
private:
    int count_ = 0;
    const char* const* names_ = nullptr;
};
class Context : public Data {
public:
    using Data::Data;
    static Local<Context> New(Isolate*, ExtensionConfiguration* = nullptr, MaybeLocal<ObjectTemplate> = {}, MaybeLocal<Value> = {});
    static Local<Context> AttachForQuickJS(Isolate*, JSContext*);
    JSContext* GetQuickJSContext() const;
    MicrotaskQueue* GetMicrotaskQueue();
    void SetMicrotaskQueue(MicrotaskQueue*);
    void AllowCodeGenerationFromStrings(bool);
    bool IsCodeGenerationFromStringsAllowed() const;
    void SetErrorMessageForCodeGenerationFromStrings(Local<String>);
    Local<Object> Global();
    void DetachGlobal();
    void Enter(); void Exit();
    Isolate* GetIsolate();
    void SetSecurityToken(Local<Value>);
    void UseDefaultSecurityToken();
    Local<Value> GetSecurityToken();
    uint32_t GetNumberOfEmbedderDataFields();
    Local<Value> GetEmbedderData(int);
    void SetEmbedderData(int, Local<Value>);
    void* GetAlignedPointerFromEmbedderData(int);
    void* GetAlignedPointerFromEmbedderData(Isolate*, int index) { return GetAlignedPointerFromEmbedderData(index); }
    void SetAlignedPointerInEmbedderData(int, void*);
    Local<Object> GetExtrasBindingObject();
    bool HasTemplateLiteralObject(Local<Value>);
    class Scope {
    public:
        explicit Scope(Local<Context> context) : context_(context) { context_->Enter(); }
        ~Scope() { context_->Exit(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        Local<Context> context_;
    };
    class BackupIncumbentScope final {
    public:
        explicit BackupIncumbentScope(Local<Context>);
        ~BackupIncumbentScope();
        BackupIncumbentScope(const BackupIncumbentScope&) = delete;
        BackupIncumbentScope& operator=(const BackupIncumbentScope&) = delete;
    private:
        friend struct detail::CoreAccess;
        Isolate* isolate_ = nullptr;
        detail::Cell* context_cell_ = nullptr;
        const BackupIncumbentScope* previous_ = nullptr;
        int execution_depth_ = 0;
    };
};
} // namespace v8
#include "runtime/ui/quickjs_v8_values.h"
#endif
