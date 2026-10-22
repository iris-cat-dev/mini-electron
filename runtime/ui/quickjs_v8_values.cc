// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "runtime/ui/quickjs_v8_values.h"
#include "runtime/ui/quickjs_v8_scripts.h"
#include "runtime/ui/quickjs_v8_internal.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

namespace v8 {
namespace {
using detail::Borrow;
using detail::ContextOf;
using detail::IsolateOf;
JSContext* Ctx(Local<Context> context) { return ContextOf(*context); }
void Failed(JSContext* ctx) { detail::CaptureException(detail::IsolateFor(ctx), ctx); }
struct Owned {
    JSContext* ctx; JSValue value;
    Owned(JSContext* c, JSValue v) : ctx(c), value(v) {}
    ~Owned() { JS_FreeValue(ctx, value); }
    Owned(const Owned&) = delete;
    JSValue release() { return std::exchange(value, JS_UNDEFINED); }
};
template<class T> Local<T> AdoptAs(JSContext* ctx, JSValue value) {
    auto local = detail::Adopt(ctx, value);
    return local.IsEmpty() ? Local<T>() : local.template As<T>();
}
JSValue CallIntrinsic(JSContext* ctx, const char* owner, const char* method,
    JSValueConst receiver, int argc = 0, JSValueConst* args = nullptr) {
    if (!*method) {
        Owned callable(ctx, JS_GetIntrinsic(ctx, owner));
        if (JS_IsException(callable.value)) return JS_EXCEPTION;
        return JS_Call(ctx, callable.value, receiver, argc, args);
    }
    return JS_CallIntrinsic(ctx, owner, method, receiver, argc, args);
}
JSValue Construct(JSContext* ctx, const char* name, int argc, JSValueConst* args) {
    Owned constructor(ctx, JS_GetIntrinsic(ctx, name));
    if (JS_IsException(constructor.value)) return JS_EXCEPTION;
    return JS_CallConstructor(ctx, constructor.value, argc, args);
}
bool ClassIs(const Data* data, JSBuiltinType type) {
    return JS_GetBuiltinType(Borrow(data)) == type;
}
struct Registry {
    JSContext* context;
    JSRuntime* runtime;
    std::unordered_map<JSAtom, JSValue> symbols;
    std::unordered_map<JSAtom, JSValue> privates;
    explicit Registry(JSContext* c) : context(c), runtime(JS_GetRuntime(c)) {}
    ~Registry() {
        for (auto& item : symbols) { JS_FreeValueRT(runtime, item.second); JS_FreeAtomRT(runtime, item.first); }
        for (auto& item : privates) { JS_FreeValueRT(runtime, item.second); JS_FreeAtomRT(runtime, item.first); }
    }
};
// Initialized before BlinkProcess's function-static construction; its isolate
// teardown can release registries before this process-lifetime map is destroyed.
std::unordered_map<Isolate*, std::unique_ptr<Registry>> registries;
const auto detached_backing_store = std::make_shared<BackingStore>(
    nullptr, 0, false, BackingStore::EmptyDeleter, nullptr);
Registry& GetRegistry(Isolate* isolate) {
    auto& entry = registries[isolate];
    if (!entry) entry = std::make_unique<Registry>(detail::NeutralJSContext(isolate));
    return *entry;
}
JSClassID external_class = 0;
struct ExternalPointer { void* pointer; };
void FreeExternal(JSRuntime*, JSValue value) {
    delete static_cast<ExternalPointer*>(JS_GetOpaque(value, external_class));
}
struct PrimitiveArrayPayload : detail::DataPayload {
    JSRuntime* runtime;
    std::vector<JSValue> values;
    PrimitiveArrayPayload(JSContext* ctx, size_t count)
        : runtime(JS_GetRuntime(ctx)), values(count, JS_UNDEFINED) {}
    ~PrimitiveArrayPayload() override {
        for (auto value : values) JS_FreeValueRT(runtime, value);
    }
};
struct Chars {
    const void* data; size_t length; JS_BOOL wide;
    explicit Chars(JSValueConst value) : data(JS_GetStringChars(value, &length, &wide)) {}
    uint16_t operator[](size_t index) const {
        return wide ? static_cast<const uint16_t*>(data)[index] : static_cast<const uint8_t*>(data)[index];
    }
};
int EncodeUTF8(uint32_t c, char* out) {
    if (c < 0x80) { out[0] = static_cast<char>(c); return 1; }
    if (c < 0x800) { out[0] = static_cast<char>(0xc0 | (c >> 6)); out[1] = static_cast<char>(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { out[0] = static_cast<char>(0xe0 | (c >> 12)); out[1] = static_cast<char>(0x80 | ((c >> 6) & 63)); out[2] = static_cast<char>(0x80 | (c & 63)); return 3; }
    out[0] = static_cast<char>(0xf0 | (c >> 18)); out[1] = static_cast<char>(0x80 | ((c >> 12) & 63));
    out[2] = static_cast<char>(0x80 | ((c >> 6) & 63)); out[3] = static_cast<char>(0x80 | (c & 63)); return 4;
}
}
namespace detail {
void DisposeValuesState(Isolate* isolate) { registries.erase(isolate); }
}

#define QJS_PRED(NAME, EXPR) bool Value::Is##NAME() const { return (EXPR); }
QJS_PRED(Undefined, JS_IsUndefined(Borrow(this)))
QJS_PRED(Null, JS_IsNull(Borrow(this)))
QJS_PRED(NullOrUndefined, IsNull() || IsUndefined())
QJS_PRED(True, JS_IsBool(Borrow(this)) && JS_VALUE_GET_BOOL(Borrow(this)))
QJS_PRED(False, JS_IsBool(Borrow(this)) && !JS_VALUE_GET_BOOL(Borrow(this)))
QJS_PRED(Name, IsString() || IsSymbol())
QJS_PRED(String, JS_IsString(Borrow(this)))
QJS_PRED(Symbol, JS_IsSymbol(Borrow(this)))
QJS_PRED(Function, JS_IsFunction(ContextOf(this), Borrow(this)))
QJS_PRED(Array, !JS_IsProxy(Borrow(this)) && JS_IsArray(ContextOf(this), Borrow(this)) > 0)
QJS_PRED(Object, JS_IsObject(Borrow(this)))
QJS_PRED(BigInt, JS_IsBigInt(ContextOf(this), Borrow(this)))
QJS_PRED(Boolean, JS_IsBool(Borrow(this)))
QJS_PRED(Number, JS_IsNumber(Borrow(this)))
QJS_PRED(External, external_class && JS_GetClassID(Borrow(this)) == external_class)
QJS_PRED(Date, ClassIs(this, JS_BUILTIN_DATE))
QJS_PRED(ArgumentsObject, ClassIs(this, JS_BUILTIN_ARGUMENTS))
QJS_PRED(BigIntObject, ClassIs(this, JS_BUILTIN_BIGINT_OBJECT))
QJS_PRED(BooleanObject, ClassIs(this, JS_BUILTIN_BOOLEAN_OBJECT))
QJS_PRED(NumberObject, ClassIs(this, JS_BUILTIN_NUMBER_OBJECT))
QJS_PRED(StringObject, ClassIs(this, JS_BUILTIN_STRING_OBJECT))
QJS_PRED(SymbolObject, ClassIs(this, JS_BUILTIN_SYMBOL_OBJECT))
QJS_PRED(NativeError, JS_IsError(ContextOf(this), Borrow(this)))
QJS_PRED(RegExp, JS_IsRegExp(Borrow(this)))
QJS_PRED(AsyncFunction, JS_IsAsyncFunction(Borrow(this)))
QJS_PRED(GeneratorFunction, JS_IsGeneratorFunction(Borrow(this)))
QJS_PRED(GeneratorObject, ClassIs(this, JS_BUILTIN_GENERATOR))
QJS_PRED(Promise, JS_IsPromise(Borrow(this)))
QJS_PRED(Map, JS_IsMap(Borrow(this)))
QJS_PRED(Set, JS_IsSet(Borrow(this)))
QJS_PRED(MapIterator, JS_IsMapIterator(Borrow(this)))
QJS_PRED(SetIterator, JS_IsSetIterator(Borrow(this)))
QJS_PRED(WeakMap, JS_IsWeakMap(Borrow(this)))
QJS_PRED(WeakSet, JS_IsWeakSet(Borrow(this)))
// No language-visible WeakRef, Float16Array or WebAssembly is installed.
// Core's native weak-reference tokens are internal metadata, not JS WeakRefs.
QJS_PRED(WeakRef, false)
QJS_PRED(Float16Array, false)
QJS_PRED(WasmMemoryObject, false)
QJS_PRED(WasmModuleObject, false)
QJS_PRED(WasmNull, false)
QJS_PRED(ArrayBuffer, JS_IsArrayBuffer(Borrow(this)))
QJS_PRED(SharedArrayBuffer, JS_IsSharedArrayBuffer(Borrow(this)))
QJS_PRED(ArrayBufferView, JS_IsArrayBufferView(Borrow(this)))
QJS_PRED(TypedArray, JS_IsTypedArray(Borrow(this)))
QJS_PRED(DataView, ClassIs(this, JS_BUILTIN_DATA_VIEW))
QJS_PRED(Proxy, JS_IsProxy(Borrow(this)))
QJS_PRED(ModuleNamespaceObject, ClassIs(this, JS_BUILTIN_MODULE_NAMESPACE))
#define QJS_TYPED_PRED(NAME, TYPE) QJS_PRED(NAME, JS_IsWhatTypedArray(Borrow(this), TYPE))
QJS_TYPED_PRED(Uint8Array, JS_UINT8_TYPED_ARRAY)
QJS_TYPED_PRED(Uint8ClampedArray, JS_UINT8C_TYPED_ARRAY)
QJS_TYPED_PRED(Int8Array, JS_INT8_TYPED_ARRAY)
QJS_TYPED_PRED(Uint16Array, JS_UINT16_TYPED_ARRAY)
QJS_TYPED_PRED(Int16Array, JS_INT16_TYPED_ARRAY)
QJS_TYPED_PRED(Uint32Array, JS_UINT32_TYPED_ARRAY)
QJS_TYPED_PRED(Int32Array, JS_INT32_TYPED_ARRAY)
QJS_TYPED_PRED(Float32Array, JS_FLOAT32_TYPED_ARRAY)
QJS_TYPED_PRED(Float64Array, JS_FLOAT64_TYPED_ARRAY)
QJS_TYPED_PRED(BigInt64Array, JS_BIG_INT64_TYPED_ARRAY)
QJS_TYPED_PRED(BigUint64Array, JS_BIG_UINT64_TYPED_ARRAY)
#undef QJS_TYPED_PRED
#undef QJS_PRED
bool Value::IsInt32() const {
    if (!IsNumber()) return false;
    double n; JS_ToFloat64(ContextOf(this), &n, Borrow(this));
    return n >= INT32_MIN && n <= INT32_MAX && std::trunc(n) == n && !(n == 0 && std::signbit(n));
}
bool Value::IsUint32() const {
    if (!IsNumber()) return false;
    double n; JS_ToFloat64(ContextOf(this), &n, Borrow(this));
    return n >= 0 && n <= UINT32_MAX && std::trunc(n) == n && !std::signbit(n);
}
MaybeLocal<Primitive> Value::ToPrimitive(Local<Context> context) const {
    return AdoptAs<Primitive>(Ctx(context), JS_ToPrimitive(Ctx(context), Borrow(this), 0));
}
MaybeLocal<Numeric> Value::ToNumeric(Local<Context> context) const {
    JSContext* ctx = Ctx(context);
    Owned primitive(ctx, JS_ToPrimitive(ctx, Borrow(this), 1));
    if (JS_IsException(primitive.value)) { Failed(ctx); return {}; }
    if (JS_IsBigInt(ctx, primitive.value)) return AdoptAs<BigInt>(ctx, primitive.release());
    double number;
    if (JS_ToFloat64(ctx, &number, primitive.value) < 0) { Failed(ctx); return {}; }
    return detail::MakeLocal<Number>(detail::IsolateFor(ctx), ctx, JS_NewFloat64(ctx, number));
}
MaybeLocal<BigInt> Value::ToBigInt(Local<Context> context) const {
    JSContext* ctx = Ctx(context);
    Owned primitive(ctx, JS_ToPrimitive(ctx, Borrow(this), 1));
    if (JS_IsException(primitive.value)) { Failed(ctx); return {}; }
    // V8's ToBigInt is the abstract conversion, not the BigInt constructor:
    // Number inputs (including a Number returned by ToPrimitive) must throw.
    if (JS_IsNumber(primitive.value)) {
        JS_ThrowTypeError(ctx, "Cannot convert a Number value to a BigInt");
        Failed(ctx); return {};
    }
    if (JS_IsBigInt(ctx, primitive.value))
        return detail::MakeLocal<BigInt>(detail::IsolateFor(ctx), ctx, primitive.release());
    if (JS_IsBool(primitive.value))
        return detail::MakeLocal<BigInt>(detail::IsolateFor(ctx), ctx, JS_NewBigInt64(ctx, JS_VALUE_GET_BOOL(primitive.value)));
    JSValueConst arg = primitive.value;
    return AdoptAs<BigInt>(ctx, CallIntrinsic(ctx, "BigInt", "", JS_UNDEFINED, 1, &arg));
}
MaybeLocal<Number> Value::ToNumber(Local<Context> context) const {
    JSContext* ctx = Ctx(context); double number;
    if (JS_ToFloat64(ctx, &number, Borrow(this)) < 0) { Failed(ctx); return {}; }
    return detail::MakeLocal<Number>(detail::IsolateFor(ctx), ctx, JS_NewFloat64(ctx, number));
}
MaybeLocal<String> Value::ToString(Local<Context> context) const { return AdoptAs<String>(Ctx(context), JS_ToString(Ctx(context), Borrow(this))); }
MaybeLocal<String> Value::ToDetailString(Local<Context> context) const {
    JSContext* ctx = Ctx(context);
    if (!IsObject()) {
        if (IsSymbol()) return AdoptAs<String>(ctx, CallIntrinsic(ctx, "Symbol.prototype", "toString", Borrow(this)));
        return ToString(context);
    }
    const char* name = "Object";
    switch (JS_GetBuiltinType(Borrow(this))) {
    case JS_BUILTIN_ARRAY: name = "Array"; break;
    case JS_BUILTIN_FUNCTION: name = "Function"; break;
    case JS_BUILTIN_DATE: name = "Date"; break;
    case JS_BUILTIN_REGEXP: name = "RegExp"; break;
    case JS_BUILTIN_MAP: name = "Map"; break;
    case JS_BUILTIN_SET: name = "Set"; break;
    case JS_BUILTIN_PROMISE: name = "Promise"; break;
    case JS_BUILTIN_ARRAY_BUFFER: name = "ArrayBuffer"; break;
    case JS_BUILTIN_SHARED_ARRAY_BUFFER: name = "SharedArrayBuffer"; break;
    case JS_BUILTIN_DATA_VIEW: name = "DataView"; break;
    case JS_BUILTIN_BOOLEAN_OBJECT: name = "Boolean"; break;
    case JS_BUILTIN_NUMBER_OBJECT: name = "Number"; break;
    case JS_BUILTIN_STRING_OBJECT: name = "String"; break;
    case JS_BUILTIN_SYMBOL_OBJECT: name = "Symbol"; break;
    case JS_BUILTIN_BIGINT_OBJECT: name = "BigInt"; break;
    case JS_BUILTIN_ARGUMENTS: name = "Arguments"; break;
    default: break;
    }
    std::string description = "[object "; description += name ? name : "Object"; description += ']';
    return AdoptAs<String>(ctx, JS_NewStringLen(ctx, description.data(), description.size()));
}
MaybeLocal<Object> Value::ToObject(Local<Context> context) const { return AdoptAs<Object>(Ctx(context), JS_ToObject(Ctx(context), Borrow(this))); }
MaybeLocal<Integer> Value::ToInteger(Local<Context> context) const {
    JSContext* ctx = Ctx(context); double n;
    if (JS_ToFloat64(ctx, &n, Borrow(this)) < 0) { Failed(ctx); return {}; }
    if (std::isnan(n) || n == 0) n = 0; else n = std::trunc(n);
    return detail::MakeLocal<Integer>(detail::IsolateFor(ctx), ctx, JS_NewFloat64(ctx, n));
}
MaybeLocal<Int32> Value::ToInt32(Local<Context> context) const {
    JSContext* ctx = Ctx(context); int32_t n;
    if (JS_ToInt32(ctx, &n, Borrow(this)) < 0) { Failed(ctx); return {}; }
    return detail::MakeLocal<Int32>(detail::IsolateFor(ctx), ctx, JS_NewInt32(ctx, n));
}
MaybeLocal<Uint32> Value::ToUint32(Local<Context> context) const {
    JSContext* ctx = Ctx(context); uint32_t n;
    if (JS_ToUint32(ctx, &n, Borrow(this)) < 0) { Failed(ctx); return {}; }
    return detail::MakeLocal<Uint32>(detail::IsolateFor(ctx), ctx, JS_NewUint32(ctx, n));
}
Local<Boolean> Value::ToBoolean(Isolate* isolate) const { return Boolean::New(isolate, BooleanValue(isolate)); }
bool Value::BooleanValue(Isolate*) const { return JS_ToBool(ContextOf(this), Borrow(this)) > 0; }
Maybe<double> Value::NumberValue(Local<Context> context) const {
    double n; if (JS_ToFloat64(Ctx(context), &n, Borrow(this)) < 0) { Failed(Ctx(context)); return Nothing<double>(); } return Just(n);
}
Maybe<int64_t> Value::IntegerValue(Local<Context> context) const {
    auto maybe = NumberValue(context); if (maybe.IsNothing()) return Nothing<int64_t>();
    double n = maybe.FromJust();
    if (std::isnan(n)) return Just(int64_t{0});
    if (n >= 9223372036854775808.0) return Just(INT64_MAX);
    if (n <= -9223372036854775808.0) return Just(INT64_MIN);
    return Just(static_cast<int64_t>(n));
}
Maybe<int32_t> Value::Int32Value(Local<Context> context) const {
    int32_t n; if (JS_ToInt32(Ctx(context), &n, Borrow(this)) < 0) { Failed(Ctx(context)); return Nothing<int32_t>(); } return Just(n);
}
Maybe<uint32_t> Value::Uint32Value(Local<Context> context) const {
    uint32_t n; if (JS_ToUint32(Ctx(context), &n, Borrow(this)) < 0) { Failed(Ctx(context)); return Nothing<uint32_t>(); } return Just(n);
}
MaybeLocal<Uint32> Value::ToArrayIndex(Local<Context> context) const {
    JSContext* ctx = Ctx(context);
    if (IsNumber()) {
        double n; JS_ToFloat64(ctx, &n, Borrow(this));
        if (n < 0 || n >= UINT32_MAX || n != std::trunc(n)) return {};
        return detail::MakeLocal<Uint32>(detail::IsolateFor(ctx), ctx, JS_NewUint32(ctx, static_cast<uint32_t>(n)));
    }
    Owned string(ctx, JS_ToString(ctx, Borrow(this)));
    if (JS_IsException(string.value)) { Failed(ctx); return {}; }
    Chars chars(string.value);
    if (!chars.length || chars.length > 10 || (chars.length > 1 && chars[0] == '0')) return {};
    uint64_t index = 0;
    for (size_t i = 0; i < chars.length; ++i) {
        if (chars[i] < '0' || chars[i] > '9') return {};
        index = index * 10 + chars[i] - '0';
    }
    if (index >= UINT32_MAX) return {};
    return detail::MakeLocal<Uint32>(detail::IsolateFor(ctx), ctx, JS_NewUint32(ctx, static_cast<uint32_t>(index)));
}
Maybe<bool> Value::Equals(Local<Context> context, Local<Value> that) const {
    int result = JS_IsValueEq(Ctx(context), Borrow(this), Borrow(*that), false);
    if (result < 0) { Failed(Ctx(context)); return Nothing<bool>(); } return Just(result != 0);
}
bool Value::StrictEquals(Local<Value> that) const { return JS_IsValueEq(ContextOf(this), Borrow(this), Borrow(*that), true) > 0; }
bool Value::SameValue(Local<Value> that) const {
    if (IsNumber() && that->IsNumber()) {
        double a, b; JS_ToFloat64(ContextOf(this), &a, Borrow(this)); JS_ToFloat64(ContextOf(this), &b, Borrow(*that));
        if (std::isnan(a) && std::isnan(b)) return true;
        if (a == 0 && b == 0) return std::signbit(a) == std::signbit(b);
        return a == b;
    }
    return StrictEquals(that);
}
Local<String> Value::TypeOf(Isolate* isolate) {
    // QuickJS's only falsy object is the native Annex B [[IsHTMLDDA]] object.
    const char* type = (IsUndefined() || (IsObject() && !BooleanValue(isolate))) ? "undefined" : IsBoolean() ? "boolean" : IsNumber() ? "number" : IsBigInt() ? "bigint" : IsString() ? "string" : IsSymbol() ? "symbol" : IsFunction() ? "function" : "object";
    return String::NewFromUtf8(isolate, type).ToLocalChecked();
}
Maybe<bool> Value::InstanceOf(Local<Context> context, Local<Object> object) {
    int result = JS_IsInstanceOf(Ctx(context), Borrow(this), Borrow(*object));
    if (result < 0) { Failed(Ctx(context)); return Nothing<bool>(); } return Just(result != 0);
}
TypecheckWitness::TypecheckWitness(Isolate* isolate) : isolate_(isolate) {}
TypecheckWitness::~TypecheckWitness() = default;
bool TypecheckWitness::Matches(Local<Value> candidate) const {
    return native_type_ && !candidate.IsEmpty()
        && native_type_ == mini_ui::GetQuickJSWrapperTypeInfo(isolate_, Borrow(*candidate));
}
void TypecheckWitness::Update(Local<Value> baseline) {
    native_type_ = baseline.IsEmpty() ? nullptr
        : mini_ui::GetQuickJSWrapperTypeInfo(isolate_, Borrow(*baseline));
}
Local<Primitive> Undefined(Isolate* isolate) { return Local<Primitive>(static_cast<Primitive*>(detail::ReadOnlyPrimitiveRoot(isolate, detail::PrimitiveRoot::kUndefined))); }
Local<Primitive> Null(Isolate* isolate) { return Local<Primitive>(static_cast<Primitive*>(detail::ReadOnlyPrimitiveRoot(isolate, detail::PrimitiveRoot::kNull))); }
Local<Boolean> True(Isolate* isolate) { return Local<Boolean>(static_cast<Boolean*>(detail::ReadOnlyPrimitiveRoot(isolate, detail::PrimitiveRoot::kTrue))); }
Local<Boolean> False(Isolate* isolate) { return Local<Boolean>(static_cast<Boolean*>(detail::ReadOnlyPrimitiveRoot(isolate, detail::PrimitiveRoot::kFalse))); }
bool Boolean::Value() const { return JS_VALUE_GET_BOOL(Borrow(this)) != 0; }
Local<Boolean> Boolean::New(Isolate* isolate, bool n) { return n ? True(isolate) : False(isolate); }
double Number::Value() const { double n; JS_ToFloat64(ContextOf(this), &n, Borrow(this)); return n; }
Local<Number> Number::New(Isolate* isolate, double n) { auto* ctx = detail::CurrentJSContext(isolate); return detail::MakeLocal<Number>(isolate, ctx, JS_NewFloat64(ctx, n)); }
int64_t Integer::Value() const { double n = Number::Value(); if (std::isnan(n)) return 0; if (n >= 9223372036854775808.0) return INT64_MAX; if (n <= -9223372036854775808.0) return INT64_MIN; return static_cast<int64_t>(n); }
int32_t Int32::Value() const { int32_t n; JS_ToInt32(ContextOf(this), &n, Borrow(this)); return n; }
uint32_t Uint32::Value() const { uint32_t n; JS_ToUint32(ContextOf(this), &n, Borrow(this)); return n; }
Local<Integer> Integer::New(Isolate* isolate, int32_t n) { auto* ctx = detail::CurrentJSContext(isolate); return detail::MakeLocal<Int32>(isolate, ctx, JS_NewInt32(ctx, n)); }
Local<Integer> Integer::NewFromUnsigned(Isolate* isolate, uint32_t n) { auto* ctx = detail::CurrentJSContext(isolate); return detail::MakeLocal<Uint32>(isolate, ctx, JS_NewUint32(ctx, n)); }
Local<BigInt> BigInt::New(Isolate* isolate, int64_t n) { auto* ctx = detail::CurrentJSContext(isolate); return AdoptAs<BigInt>(ctx, JS_NewBigInt64(ctx, n)); }
Local<BigInt> BigInt::NewFromUnsigned(Isolate* isolate, uint64_t n) { auto* ctx = detail::CurrentJSContext(isolate); return AdoptAs<BigInt>(ctx, JS_NewBigUint64(ctx, n)); }
MaybeLocal<BigInt> BigInt::NewFromWords(Local<Context> context, int sign, int count, const uint64_t* words) {
    JSContext* ctx = Ctx(context);
    if (count < 0 || (count && !words)) { JS_ThrowRangeError(ctx, "Invalid BigInt word count"); Failed(ctx); return {}; }
    return AdoptAs<BigInt>(ctx, JS_NewBigIntWords(ctx, sign != 0, static_cast<size_t>(count), words));
}
int BigInt::WordCount() const { size_t count = 0; int sign; if (JS_GetBigIntWords(ContextOf(this), Borrow(this), &sign, &count, nullptr) < 0) { Failed(ContextOf(this)); return 0; } return static_cast<int>(count); }
void BigInt::ToWordsArray(int* sign, int* count, uint64_t* words) const {
    size_t capacity = *count > 0 ? static_cast<size_t>(*count) : 0;
    if (JS_GetBigIntWords(ContextOf(this), Borrow(this), sign, &capacity, words) < 0) { Failed(ContextOf(this)); return; }
    *count = static_cast<int>(capacity);
}
uint64_t BigInt::Uint64Value(bool* lossless) const {
    size_t count = 1; int sign; uint64_t word = 0;
    if (JS_GetBigIntWords(ContextOf(this), Borrow(this), &sign, &count, &word) < 0) { Failed(ContextOf(this)); if (lossless) *lossless = false; return 0; }
    if (lossless) *lossless = !sign && count <= 1;
    return sign ? uint64_t{0} - word : word;
}
int64_t BigInt::Int64Value(bool* lossless) const {
    size_t count = 1; int sign; uint64_t word = 0;
    if (JS_GetBigIntWords(ContextOf(this), Borrow(this), &sign, &count, &word) < 0) { Failed(ContextOf(this)); if (lossless) *lossless = false; return 0; }
    if (lossless) *lossless = count <= 1 && (sign ? word <= (uint64_t{1} << 63) : word < (uint64_t{1} << 63));
    uint64_t bits = sign ? uint64_t{0} - word : word; int64_t result; std::memcpy(&result, &bits, sizeof(result)); return result;
}
int Name::GetIdentityHash() {
    uint32_t hash = 2166136261u;
    if (JS_IsString(Borrow(this))) {
        Chars chars(Borrow(this));
        for (size_t i = 0; i < chars.length; ++i) {
            hash = (hash ^ chars[i]) * 16777619u;
        }
    } else {
        uintptr_t identity = reinterpret_cast<uintptr_t>(JS_VALUE_GET_PTR(Borrow(this)));
        for (size_t i = 0; i < sizeof(identity); ++i) {
            hash = (hash ^ static_cast<uint8_t>(identity)) * 16777619u;
            identity >>= 8;
        }
    }
    hash &= 0x7fffffffu;
    return static_cast<int>(hash ? hash : 1);
}
int String::Length() const { return static_cast<int>(JS_GetStringSize(ContextOf(this), Borrow(this))); }
bool String::IsOneByte() const { Chars chars(Borrow(this)); return !chars.wide; }
bool String::ContainsOnlyOneByte() const { return JS_ContainsOnlyOneByte(ContextOf(this), Borrow(this)) != 0; }
int String::Utf8Length(Isolate*) const {
    Chars chars(Borrow(this)); int bytes = 0; char encoded[4];
    for (size_t i = 0; i < chars.length; ++i) {
        uint32_t c = chars[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < chars.length && chars[i + 1] >= 0xdc00 && chars[i + 1] <= 0xdfff) { c = 0x10000 + ((c - 0xd800) << 10) + chars[++i] - 0xdc00; }
        bytes += EncodeUTF8(c, encoded);
    }
    return bytes;
}
int String::Write(Isolate*, uint16_t* buffer, int start, int length, int options) const {
    Chars chars(Borrow(this)); if (start < 0 || static_cast<size_t>(start) > chars.length || length < -1) return 0;
    size_t available = chars.length - start, count = length < 0 ? available : std::min(available, static_cast<size_t>(length));
    if (chars.wide && count) std::memcpy(buffer, static_cast<const uint16_t*>(chars.data) + start, count * sizeof(uint16_t));
    else for (size_t i = 0; i < count; ++i) buffer[i] = chars[start + i];
    if (!(options & NO_NULL_TERMINATION) && (length < 0 || count < static_cast<size_t>(length))) buffer[count] = 0;
    return static_cast<int>(count);
}
int String::WriteOneByte(Isolate*, uint8_t* buffer, int start, int length, int options) const {
    Chars chars(Borrow(this)); if (start < 0 || static_cast<size_t>(start) > chars.length || length < -1) return 0;
    size_t available = chars.length - start, count = length < 0 ? available : std::min(available, static_cast<size_t>(length));
    if (!chars.wide && count) std::memcpy(buffer, static_cast<const uint8_t*>(chars.data) + start, count);
    else for (size_t i = 0; i < count; ++i) buffer[i] = static_cast<uint8_t>(chars[start + i]);
    if (!(options & NO_NULL_TERMINATION) && (length < 0 || count < static_cast<size_t>(length))) buffer[count] = 0;
    return static_cast<int>(count);
}
int String::WriteUtf8(Isolate*, char* buffer, int length, int* nchars, int options) const {
    Chars chars(Borrow(this)); size_t consumed = 0; int written = 0;
    int capacity = length < 0 ? INT32_MAX : length;
    while (consumed < chars.length) {
        uint32_t c = chars[consumed]; size_t units = 1;
        if (c >= 0xd800 && c <= 0xdbff && consumed + 1 < chars.length && chars[consumed + 1] >= 0xdc00 && chars[consumed + 1] <= 0xdfff) { c = 0x10000 + ((c - 0xd800) << 10) + chars[consumed + 1] - 0xdc00; units = 2; }
        else if (c >= 0xd800 && c <= 0xdfff && (options & REPLACE_INVALID_UTF8)) c = 0xfffd;
        char bytes[4]; int count = EncodeUTF8(c, bytes);
        if (count > capacity - written) break;
        std::memcpy(buffer + written, bytes, count); written += count; consumed += units;
    }
    if (nchars) *nchars = static_cast<int>(consumed);
    if (!(options & NO_NULL_TERMINATION) && written < capacity) buffer[written++] = 0;
    return written;
}
Local<String> String::Empty(Isolate* isolate) { return NewFromUtf8(isolate, "", NewStringType::kNormal, 0).ToLocalChecked(); }
MaybeLocal<String> String::NewFromUtf8(Isolate* isolate, const char* data, NewStringType type, int length) {
    if (length < 0) length = data ? static_cast<int>(std::strlen(data)) : 0;
    if (length > kMaxLength) return {};
    JSContext* ctx = detail::CurrentJSContext(isolate);
    auto value = AdoptAs<String>(ctx, JS_NewStringLen(ctx, data ? data : "", length));
    return type == NewStringType::kInternalized && !value.IsEmpty() ? value->InternalizeString(isolate) : value;
}
MaybeLocal<String> String::NewFromOneByte(Isolate* isolate, const uint8_t* data, NewStringType type, int length) {
    if (length < 0) length = data ? static_cast<int>(std::strlen(reinterpret_cast<const char*>(data))) : 0;
    if (length > kMaxLength) return {};
    JSContext* ctx = detail::CurrentJSContext(isolate);
    auto value = AdoptAs<String>(ctx, JS_NewStringLatin1(ctx, data, length));
    return type == NewStringType::kInternalized && !value.IsEmpty() ? value->InternalizeString(isolate) : value;
}
MaybeLocal<String> String::NewFromTwoByte(Isolate* isolate, const uint16_t* data, NewStringType type, int length) {
    if (length < 0) { length = 0; if (data) while (data[length]) ++length; }
    if (length > kMaxLength) return {};
    JSContext* ctx = detail::CurrentJSContext(isolate);
    auto value = AdoptAs<String>(ctx, JS_NewStringUTF16(ctx, data, length));
    return type == NewStringType::kInternalized && !value.IsEmpty() ? value->InternalizeString(isolate) : value;
}
Local<String> String::InternalizeString(Isolate*) {
    JSContext* ctx = ContextOf(this); JSAtom atom = JS_ValueToAtom(ctx, Borrow(this));
    if (!atom) { Failed(ctx); return {}; } JSValue string = JS_AtomToString(ctx, atom); JS_FreeAtom(ctx, atom); return AdoptAs<String>(ctx, string);
}
Local<String> String::Concat(Isolate* isolate, Local<String> left, Local<String> right) {
    JSContext* ctx = detail::CurrentJSContext(isolate); JSValueConst arg = Borrow(*right);
    return AdoptAs<String>(ctx, CallIntrinsic(ctx, "String.prototype", "concat", Borrow(*left), 1, &arg));
}
bool String::StringEquals(Local<String> that) const { return JS_IsValueEq(ContextOf(this), Borrow(this), Borrow(*that), true) > 0; }
String::Utf8Value::Utf8Value(Isolate* isolate, Local<v8::Value> value, WriteOptions options) {
    JSContext* ctx = detail::CurrentJSContext(isolate); auto string = AdoptAs<String>(ctx, JS_ToString(ctx, Borrow(*value)));
    if (string.IsEmpty()) return;
    length_ = string->Utf8Length(isolate); str_ = new char[static_cast<size_t>(length_) + 1];
    string->WriteUtf8(isolate, str_, length_ + 1, nullptr, options);
}
String::Utf8Value::~Utf8Value() { delete[] str_; }
String::Value::Value(Isolate* isolate, Local<v8::Value> value) {
    JSContext* ctx = detail::CurrentJSContext(isolate); auto string = AdoptAs<String>(ctx, JS_ToString(ctx, Borrow(*value)));
    if (string.IsEmpty()) return;
    length_ = string->Length(); str_ = new uint16_t[static_cast<size_t>(length_) + 1]; string->Write(isolate, str_);
}
String::Value::~Value() { delete[] str_; }
String::ValueView::ValueView(Isolate*, Local<String> value)
    : context_(JS_DupContext(ContextOf(*value))), string_(JS_DupValue(context_, Borrow(*value))) {
    size_t length; JS_BOOL wide; data_ = JS_GetStringChars(string_, &length, &wide); length_ = static_cast<int>(length); one_byte_ = !wide;
}
String::ValueView::~ValueView() { JS_FreeValue(context_, string_); JS_FreeContext(context_); }

Local<Value> Symbol::Description(Isolate*) const { return detail::Adopt(ContextOf(this), CallIntrinsic(ContextOf(this), "Symbol.prototype", "description", Borrow(this))); }
Local<Symbol> Symbol::New(Isolate* isolate, Local<String> description) {
    JSContext* ctx = detail::CurrentJSContext(isolate); JSValueConst arg = description.IsEmpty() ? JS_UNDEFINED : Borrow(*description);
    return AdoptAs<Symbol>(ctx, CallIntrinsic(ctx, "Symbol", "", JS_UNDEFINED, description.IsEmpty() ? 0 : 1, &arg));
}
Local<Symbol> Symbol::For(Isolate* isolate, Local<String> description) {
    JSContext* ctx = detail::CurrentJSContext(isolate); JSValueConst arg = Borrow(*description);
    return AdoptAs<Symbol>(ctx, CallIntrinsic(ctx, "Symbol", "for", JS_UNDEFINED, 1, &arg));
}
Local<Symbol> Symbol::ForApi(Isolate* isolate, Local<String> description) {
    auto& registry = GetRegistry(isolate);
    JSAtom key = JS_ValueToAtom(registry.context, Borrow(*description));
    if (!key) { Failed(registry.context); return {}; }
    auto found = registry.symbols.find(key);
    if (found != registry.symbols.end()) {
        JS_FreeAtom(registry.context, key);
        return AdoptAs<Symbol>(registry.context, JS_DupValue(registry.context, found->second));
    }
    JSValueConst description_value = Borrow(*description);
    auto symbol = detail::MakeLocal<Symbol>(isolate, registry.context,
        CallIntrinsic(registry.context, "Symbol", "", JS_UNDEFINED, 1, &description_value));
    if (symbol.IsEmpty()) JS_FreeAtom(registry.context, key);
    else registry.symbols.emplace(key, JS_DupValue(registry.context, Borrow(*symbol)));
    return symbol;
}
#define QJS_SYMBOL(NAME, KEY) Local<Symbol> Symbol::Get##NAME(Isolate* isolate) { JSContext* ctx = detail::CurrentJSContext(isolate); Owned constructor(ctx, JS_GetIntrinsic(ctx, "Symbol")); if (JS_IsException(constructor.value)) { Failed(ctx); return {}; } return AdoptAs<Symbol>(ctx, JS_GetPropertyStr(ctx, constructor.value, KEY)); }
QJS_SYMBOL(AsyncIterator, "asyncIterator")
QJS_SYMBOL(HasInstance, "hasInstance")
QJS_SYMBOL(IsConcatSpreadable, "isConcatSpreadable")
QJS_SYMBOL(Iterator, "iterator")
QJS_SYMBOL(Match, "match")
QJS_SYMBOL(Replace, "replace")
QJS_SYMBOL(Search, "search")
QJS_SYMBOL(Split, "split")
QJS_SYMBOL(ToPrimitive, "toPrimitive")
QJS_SYMBOL(ToStringTag, "toStringTag")
QJS_SYMBOL(Unscopables, "unscopables")
#undef QJS_SYMBOL
Local<Value> Private::Name() const { return detail::Adopt(ContextOf(this), CallIntrinsic(ContextOf(this), "Symbol.prototype", "description", Borrow(this))); }
Local<Private> Private::New(Isolate* isolate, Local<String> name) {
    JSContext* ctx = detail::CurrentJSContext(isolate);
    return detail::MakeLocal<Private>(isolate, ctx,
        JS_NewPrivateSymbol(ctx, name.IsEmpty() ? JS_UNDEFINED : Borrow(*name)));
}
Local<Private> Private::ForApi(Isolate* isolate, Local<String> name) {
    auto& registry = GetRegistry(isolate);
    JSAtom key = JS_ValueToAtom(registry.context, Borrow(*name));
    if (!key) { Failed(registry.context); return {}; }
    auto found = registry.privates.find(key);
    if (found != registry.privates.end()) {
        JS_FreeAtom(registry.context, key);
        return detail::MakeLocal<Private>(isolate, registry.context, JS_DupValue(registry.context, found->second));
    }
    auto symbol = detail::MakeLocal<Private>(isolate, registry.context,
        JS_NewPrivateSymbol(registry.context, Borrow(*name)));
    if (symbol.IsEmpty()) JS_FreeAtom(registry.context, key);
    else registry.privates.emplace(key, JS_DupValue(registry.context, Borrow(*symbol)));
    return symbol;
}
Local<External> External::New(Isolate* isolate, void* pointer) {
    JSContext* ctx = detail::NeutralJSContext(isolate); JSRuntime* rt = JS_GetRuntime(ctx);
    if (!external_class) JS_NewClassID(&external_class);
    if (!JS_IsRegisteredClass(rt, external_class)) { JSClassDef def{}; def.class_name = "External"; def.finalizer = FreeExternal; if (JS_NewClass(rt, external_class, &def) < 0) { JS_ThrowInternalError(ctx, "Cannot register External class"); Failed(ctx); return {}; } }
    Owned value(ctx, JS_NewObjectClass(ctx, external_class)); if (JS_IsException(value.value)) { Failed(ctx); return {}; }
    JS_SetOpaque(value.value, new ExternalPointer{pointer}); return detail::MakeLocal<External>(isolate, ctx, value.release());
}
void* External::Value() const { auto* value = static_cast<ExternalPointer*>(JS_GetOpaque(Borrow(this), external_class)); return value ? value->pointer : nullptr; }
Local<PrimitiveArray> PrimitiveArray::New(Isolate* isolate, int length) {
    JSContext* ctx = detail::NeutralJSContext(isolate);
    if (length < 0) { JS_ThrowRangeError(ctx, "Invalid PrimitiveArray length"); Failed(ctx); return {}; }
    return detail::MakeLocal<PrimitiveArray>(isolate, ctx, JS_UNDEFINED, std::make_shared<PrimitiveArrayPayload>(ctx, length));
}
int PrimitiveArray::Length() const { return static_cast<int>(std::static_pointer_cast<PrimitiveArrayPayload>(detail::Payload(this))->values.size()); }
void PrimitiveArray::Set(Isolate*, int index, Local<Primitive> value) {
    auto payload = std::static_pointer_cast<PrimitiveArrayPayload>(detail::Payload(this));
    if (index < 0 || static_cast<size_t>(index) >= payload->values.size()) std::abort();
    JSValue next = JS_DupValueRT(payload->runtime, Borrow(*value)); JS_FreeValueRT(payload->runtime, payload->values[index]); payload->values[index] = next;
}
Local<Primitive> PrimitiveArray::Get(Isolate* isolate, int index) {
    auto payload = std::static_pointer_cast<PrimitiveArrayPayload>(detail::Payload(this));
    if (index < 0 || static_cast<size_t>(index) >= payload->values.size()) std::abort();
    return AdoptAs<Primitive>(detail::NeutralJSContext(isolate), JS_DupValueRT(payload->runtime, payload->values[index]));
}

uint32_t Array::Length() const {
    JSContext* ctx = ContextOf(this); Owned length(ctx, JS_GetPropertyStr(ctx, Borrow(this), "length"));
    uint32_t n = 0; if (JS_IsException(length.value) || JS_ToUint32(ctx, &n, length.value) < 0) Failed(ctx); return n;
}
Local<Array> Array::New(Isolate* isolate, int length) {
    JSContext* ctx = detail::CurrentJSContext(isolate); Owned array(ctx, JS_NewArray(ctx));
    if (JS_IsException(array.value)) { Failed(ctx); return {}; }
    if (length > 0 && JS_SetPropertyStr(ctx, array.value, "length", JS_NewInt32(ctx, length)) < 0) { Failed(ctx); return {}; }
    return AdoptAs<Array>(ctx, array.release());
}
Local<Array> Array::New(Isolate* isolate, Local<Value>* elements, size_t length) {
    JSContext* ctx = detail::CurrentJSContext(isolate);
    if (length > UINT32_MAX) { JS_ThrowRangeError(ctx, "Invalid array length"); Failed(ctx); return {}; }
    Owned array(ctx, JS_NewArray(ctx)); if (JS_IsException(array.value)) { Failed(ctx); return {}; }
    for (size_t i = 0; i < length; ++i)
        if (JS_DefinePropertyValueUint32(ctx, array.value, static_cast<uint32_t>(i), JS_DupValue(ctx, Borrow(*elements[i])), JS_PROP_C_W_E) < 0) { Failed(ctx); return {}; }
    return AdoptAs<Array>(ctx, array.release());
}
MaybeLocal<Array> Array::New(Local<Context> context, size_t length, std::function<MaybeLocal<Value>()> next) {
    JSContext* ctx = Ctx(context);
    if (length > UINT32_MAX) { JS_ThrowRangeError(ctx, "Invalid array length"); Failed(ctx); return {}; }
    Owned array(ctx, JS_NewArray(ctx)); if (JS_IsException(array.value)) { Failed(ctx); return {}; }
    for (size_t i = 0; i < length; ++i) {
        Local<Value> value; if (!next().ToLocal(&value)) return {};
        if (JS_DefinePropertyValueUint32(ctx, array.value, static_cast<uint32_t>(i), JS_DupValue(ctx, Borrow(*value)), JS_PROP_C_W_E) < 0) { Failed(ctx); return {}; }
    }
    return AdoptAs<Array>(ctx, array.release());
}
Maybe<void> Array::Iterate(Local<Context> context, IterationCallback callback, void* data) {
    JSContext* ctx = Ctx(context); uint32_t length = Length();
    for (uint32_t i = 0; i < length; ++i) {
        HandleScope scope(IsolateOf(this));
        auto value = detail::Adopt(ctx, JS_GetPropertyUint32(ctx, Borrow(this), i));
        if (value.IsEmpty()) return Nothing<void>();
        auto result = callback(i, value, data);
        if (result == CallbackResult::kException) return Nothing<void>();
        if (result == CallbackResult::kBreak) break;
    }
    return JustVoid();
}
namespace {
size_t CollectionSize(const Data* value, const char* owner) {
    JSContext* ctx = ContextOf(value); Owned size(ctx, CallIntrinsic(ctx, owner, "size", Borrow(value))); int64_t n = 0;
    if (JS_IsException(size.value) || JS_ToInt64(ctx, &n, size.value) < 0) { Failed(ctx); return 0; }
    return static_cast<size_t>(n);
}
void CollectionClear(const Data* value, const char* owner) {
    JSContext* ctx = ContextOf(value); Owned result(ctx, CallIntrinsic(ctx, owner, "clear", Borrow(value)));
    if (JS_IsException(result.value)) Failed(ctx);
}
Maybe<bool> CollectionBoolean(const Data* value, Local<Context> context, const char* owner, const char* method, Local<Value> key) {
    JSContext* ctx = Ctx(context); JSValueConst arg = Borrow(*key);
    Owned result(ctx, CallIntrinsic(ctx, owner, method, Borrow(value), 1, &arg));
    if (JS_IsException(result.value)) { Failed(ctx); return Nothing<bool>(); }
    return Just(JS_ToBool(ctx, result.value) > 0);
}
Local<Array> CollectionArray(const Data* value, bool map) {
    JSContext* ctx = ContextOf(value);
    return AdoptAs<Array>(ctx, map ? JS_GetMapEntries(ctx, Borrow(value)) : JS_GetSetValues(ctx, Borrow(value)));
}
}
size_t Map::Size() const { return CollectionSize(this, "Map.prototype"); }
void Map::Clear() { CollectionClear(this, "Map.prototype"); }
Local<Map> Map::New(Isolate* isolate) { auto* ctx = detail::CurrentJSContext(isolate); return AdoptAs<Map>(ctx, JS_NewMap(ctx)); }
MaybeLocal<Value> Map::Get(Local<Context> context, Local<Value> key) {
    JSContext* ctx = Ctx(context); JSValueConst arg = Borrow(*key);
    return detail::Adopt(ctx, CallIntrinsic(ctx, "Map.prototype", "get", Borrow(this), 1, &arg));
}
MaybeLocal<Map> Map::Set(Local<Context> context, Local<Value> key, Local<Value> value) {
    JSContext* ctx = Ctx(context); JSValueConst args[] = {Borrow(*key), Borrow(*value)};
    return AdoptAs<Map>(ctx, CallIntrinsic(ctx, "Map.prototype", "set", Borrow(this), 2, args));
}
Maybe<bool> Map::Has(Local<Context> context, Local<Value> key) { return CollectionBoolean(this, context, "Map.prototype", "has", key); }
Maybe<bool> Map::Delete(Local<Context> context, Local<Value> key) { return CollectionBoolean(this, context, "Map.prototype", "delete", key); }
Local<Array> Map::AsArray() const { return CollectionArray(this, true); }
size_t Set::Size() const { return CollectionSize(this, "Set.prototype"); }
void Set::Clear() { CollectionClear(this, "Set.prototype"); }
Local<Set> Set::New(Isolate* isolate) { auto* ctx = detail::CurrentJSContext(isolate); return AdoptAs<Set>(ctx, Construct(ctx, "Set", 0, nullptr)); }
MaybeLocal<Set> Set::Add(Local<Context> context, Local<Value> key) {
    JSContext* ctx = Ctx(context); JSValueConst arg = Borrow(*key);
    return AdoptAs<Set>(ctx, CallIntrinsic(ctx, "Set.prototype", "add", Borrow(this), 1, &arg));
}
Maybe<bool> Set::Has(Local<Context> context, Local<Value> key) { return CollectionBoolean(this, context, "Set.prototype", "has", key); }
Maybe<bool> Set::Delete(Local<Context> context, Local<Value> key) { return CollectionBoolean(this, context, "Set.prototype", "delete", key); }
Local<Array> Set::AsArray() const { return CollectionArray(this, false); }
MaybeLocal<Value> Date::New(Local<Context> context, double time) {
    JSContext* ctx = Ctx(context); JSValueConst arg = JS_NewFloat64(ctx, time); return detail::Adopt(ctx, Construct(ctx, "Date", 1, &arg));
}
MaybeLocal<Value> Date::Parse(Local<Context> context, Local<String> string) {
    JSContext* ctx = Ctx(context); JSValueConst arg = Borrow(*string); return detail::Adopt(ctx, CallIntrinsic(ctx, "Date", "parse", JS_UNDEFINED, 1, &arg));
}
double Date::ValueOf() const {
    JSContext* ctx = ContextOf(this); Owned result(ctx, CallIntrinsic(ctx, "Date.prototype", "valueOf", Borrow(this))); double n = std::numeric_limits<double>::quiet_NaN();
    if (JS_IsException(result.value) || JS_ToFloat64(ctx, &n, result.value) < 0) Failed(ctx); return n;
}
Local<String> Date::ToISOString() const { return AdoptAs<String>(ContextOf(this), CallIntrinsic(ContextOf(this), "Date.prototype", "toISOString", Borrow(this))); }
Local<String> Date::ToUTCString() const { return AdoptAs<String>(ContextOf(this), CallIntrinsic(ContextOf(this), "Date.prototype", "toUTCString", Borrow(this))); }
MaybeLocal<RegExp> RegExp::New(Local<Context> context, Local<String> pattern, Flags flags) {
    JSContext* ctx = Ctx(context);
    if (flags & kLinear) { JS_ThrowSyntaxError(ctx, "QuickJS has no linear RegExp engine"); Failed(ctx); return {}; }
    std::string text;
    if (flags & kGlobal) text += 'g'; if (flags & kIgnoreCase) text += 'i'; if (flags & kMultiline) text += 'm';
    if (flags & kSticky) text += 'y'; if (flags & kUnicode) text += 'u'; if (flags & kDotAll) text += 's';
    if (flags & kHasIndices) text += 'd'; if (flags & kUnicodeSets) text += 'v';
    Owned flag_string(ctx, JS_NewStringLen(ctx, text.data(), text.size()));
    if (JS_IsException(flag_string.value)) { Failed(ctx); return {}; }
    JSValueConst args[] = {Borrow(*pattern), flag_string.value}; return AdoptAs<RegExp>(ctx, Construct(ctx, "RegExp", 2, args));
}
MaybeLocal<RegExp> RegExp::NewWithBacktrackLimit(Local<Context> context, Local<String> pattern, Flags flags, uint32_t limit) {
    Local<RegExp> regexp;
    if (!New(context, pattern, flags).ToLocal(&regexp)) return {};
    if (JS_SetRegExpBacktrackLimit(Ctx(context), Borrow(*regexp), limit) < 0) {
        Failed(Ctx(context)); return {};
    }
    return regexp;
}
MaybeLocal<Object> RegExp::Exec(Local<Context> context, Local<String> subject) {
    JSContext* ctx = Ctx(context); JSValueConst arg = Borrow(*subject);
    return AdoptAs<Object>(ctx, CallIntrinsic(ctx, "RegExp.prototype", "exec", Borrow(this), 1, &arg));
}
Local<String> RegExp::GetSource() const { return AdoptAs<String>(ContextOf(this), CallIntrinsic(ContextOf(this), "RegExp.prototype", "source", Borrow(this))); }
RegExp::Flags RegExp::GetFlags() const {
    JSContext* ctx = ContextOf(this);
    static constexpr struct { const char* name; Flags flag; } getters[] = {
        {"global", kGlobal}, {"ignoreCase", kIgnoreCase}, {"multiline", kMultiline},
        {"sticky", kSticky}, {"unicode", kUnicode}, {"dotAll", kDotAll}
    };
    int flags = 0;
    for (const auto& getter : getters) {
        Owned value(ctx, CallIntrinsic(ctx, "RegExp.prototype", getter.name, Borrow(this)));
        if (JS_IsException(value.value)) { Failed(ctx); return kNone; }
        if (JS_ToBool(ctx, value.value)) flags |= getter.flag;
    }
    return static_cast<Flags>(flags);
}
namespace {
Local<Value> Box(Isolate* isolate, JSValue primitive) {
    JSContext* ctx = detail::CurrentJSContext(isolate); Owned value(ctx, primitive);
    if (JS_IsException(value.value)) { Failed(ctx); return {}; }
    return detail::Adopt(ctx, JS_ToObject(ctx, value.value));
}
}
Local<Value> NumberObject::New(Isolate* isolate, double n) { return Box(isolate, JS_NewFloat64(detail::CurrentJSContext(isolate), n)); }
double NumberObject::ValueOf() const {
    JSContext* ctx = ContextOf(this); Owned value(ctx, CallIntrinsic(ctx, "Number.prototype", "valueOf", Borrow(this))); double n = std::numeric_limits<double>::quiet_NaN();
    if (JS_IsException(value.value) || JS_ToFloat64(ctx, &n, value.value) < 0) Failed(ctx); return n;
}
Local<Value> BooleanObject::New(Isolate* isolate, bool n) { return Box(isolate, JS_NewBool(detail::CurrentJSContext(isolate), n)); }
bool BooleanObject::ValueOf() const {
    JSContext* ctx = ContextOf(this); Owned value(ctx, CallIntrinsic(ctx, "Boolean.prototype", "valueOf", Borrow(this)));
    if (JS_IsException(value.value)) { Failed(ctx); return false; } return JS_ToBool(ctx, value.value) > 0;
}
Local<Value> StringObject::New(Isolate* isolate, Local<String> value) { return Box(isolate, JS_DupValue(detail::CurrentJSContext(isolate), Borrow(*value))); }
Local<String> StringObject::ValueOf() const { return AdoptAs<String>(ContextOf(this), CallIntrinsic(ContextOf(this), "String.prototype", "valueOf", Borrow(this))); }
Local<Value> SymbolObject::New(Isolate* isolate, Local<Symbol> value) { return Box(isolate, JS_DupValue(detail::CurrentJSContext(isolate), Borrow(*value))); }
Local<Symbol> SymbolObject::ValueOf() const { return AdoptAs<Symbol>(ContextOf(this), CallIntrinsic(ContextOf(this), "Symbol.prototype", "valueOf", Borrow(this))); }
Local<Value> BigIntObject::New(Isolate* isolate, int64_t n) { return Box(isolate, JS_NewBigInt64(detail::CurrentJSContext(isolate), n)); }
Local<BigInt> BigIntObject::ValueOf() const { return AdoptAs<BigInt>(ContextOf(this), CallIntrinsic(ContextOf(this), "BigInt.prototype", "valueOf", Borrow(this))); }

namespace {
struct ExternalStringOwner {
    String::ExternalStringResourceBase* resource;
    String::Encoding encoding;
    ExternalStringOwner(String::ExternalStringResourceBase* value, String::Encoding type)
        : resource(value), encoding(type) {}
};
struct BackingHolder {
    std::shared_ptr<BackingStore> store;
};
void FreeBacking(JSRuntime*, void* opaque, void*) { delete static_cast<BackingHolder*>(opaque); }
struct OriginalBackingOwner {
    JSArrayBufferReleaseData previous{};
    bool transferred = false;
    OriginalBackingOwner() = default;
    OriginalBackingOwner(const OriginalBackingOwner&) = delete;
    OriginalBackingOwner& operator=(const OriginalBackingOwner&) = delete;
};
void FreeOriginalBacking(void* data, size_t, void* opaque) {
    std::unique_ptr<OriginalBackingOwner> owner(static_cast<OriginalBackingOwner*>(opaque));
    if (owner->transferred) JS_ReleaseArrayBufferData(&owner->previous, data);
}
void FreeMallocBacking(void* data, size_t, void*) { std::free(data); }
std::unique_ptr<BackingStore> AllocateBacking(JSContext* ctx, size_t length, bool shared, BackingStoreInitializationMode mode) {
    // A non-null zero-sized allocation is needed by QuickJS's buffer constructor.
    void* data = mode == BackingStoreInitializationMode::kZeroInitialized ? std::calloc(length ? length : 1, 1) : std::malloc(length ? length : 1);
    if (!data) { JS_ThrowOutOfMemory(ctx); Failed(ctx); return {}; }
    return std::make_unique<BackingStore>(data, length, shared, FreeMallocBacking, nullptr);
}
std::shared_ptr<BackingStore> BufferBacking(const Data* value) {
    JSContext* ctx = ContextOf(value);
    if (JS_IsDetachedArrayBuffer(Borrow(value))) {
        return detached_backing_store;
    }
    uint8_t* data; size_t length; JSFreeArrayBufferDataFunc* free; void* opaque;
    if (JS_GetArrayBufferOwner(ctx, Borrow(value), &data, &length, &free, &opaque) < 0) { Failed(ctx); return {}; }
    if (free == FreeBacking) return static_cast<BackingHolder*>(opaque)->store;
    auto original = std::make_unique<OriginalBackingOwner>();
    auto store = std::make_shared<BackingStore>(data, length,
        JS_IsSharedArrayBuffer(Borrow(value)) != 0, FreeOriginalBacking, original.get());
    // From here the store owns the descriptor even if allocating the holder
    // fails. Until transfer commits its deleter must not free engine-owned data.
    auto* release = original.release();
    auto holder = std::make_unique<BackingHolder>(BackingHolder{store});
    if (JS_TransferArrayBufferOwner(ctx, Borrow(value), FreeBacking,
            holder.get(), &release->previous) < 0) {
        Failed(ctx); return {};
    }
    release->transferred = true;
    holder.release();
    return store;
}
JSValue BufferFromBacking(JSContext* ctx, std::shared_ptr<BackingStore> store, bool shared) {
    if (!store || store->IsShared() != shared) return JS_ThrowTypeError(ctx, "Backing store sharedness does not match buffer");
    if (store->ByteLength() > INT32_MAX) return JS_ThrowRangeError(ctx, "QuickJS ArrayBuffer exceeds its 2 GiB length limit");
    auto holder = std::make_unique<BackingHolder>(BackingHolder{std::move(store)});
    JSValue value = JS_NewArrayBuffer(ctx, static_cast<uint8_t*>(holder->store->Data()),
        holder->store->ByteLength(), FreeBacking, holder.get(), shared);
    if (!JS_IsException(value)) holder.release();
    return value;
}
struct ViewInfo {
    // The VM fills these outputs while initializing buffer.
    JSContext* ctx; size_t offset = 0; size_t length = 0; size_t element = 1; JSValue buffer;
    explicit ViewInfo(const Data* value) : ctx(ContextOf(value)),
        buffer(JS_GetArrayBufferViewInfo(ctx, Borrow(value), &offset, &length, &element)) {}
    ~ViewInfo() { JS_FreeValue(ctx, buffer); }
};
template<class T, class B> Local<T> NewView(Local<B> buffer, size_t offset, size_t length, JS_TYPED_ARRAY type) {
    JSContext* ctx = ContextOf(*buffer);
    if (offset > TypedArray::kMaxByteLength || length > T::kMaxLength) {
        JS_ThrowRangeError(ctx, "Typed array offset or length exceeds maximum"); Failed(ctx); return {};
    }
    return AdoptAs<T>(ctx, JS_NewTypedArray(ctx, type, Borrow(*buffer), offset, length));
}
template<class B> Local<DataView> NewDataView(Local<B> buffer, size_t offset, size_t length) {
    JSContext* ctx = ContextOf(*buffer);
    if (offset > TypedArray::kMaxByteLength || length > TypedArray::kMaxByteLength) {
        JS_ThrowRangeError(ctx, "DataView offset or length exceeds maximum"); Failed(ctx); return {};
    }
    JSValueConst arguments[] = {Borrow(*buffer),
        JS_NewFloat64(ctx, static_cast<double>(offset)),
        JS_NewFloat64(ctx, static_cast<double>(length))};
    return AdoptAs<DataView>(ctx, Construct(ctx, "DataView", 3, arguments));
}
}
MaybeLocal<String> String::NewExternalTwoByte(Isolate* isolate, ExternalStringResource* resource) {
    JSContext* ctx = detail::CurrentJSContext(isolate);
    if (resource->length() > static_cast<size_t>(kMaxLength)) return {};
    auto owner = std::make_unique<ExternalStringOwner>(resource, TWO_BYTE_ENCODING);
    JSValue value = JS_NewExternalString(ctx, resource->data(), resource->length(), true, [](void* opaque) {
        std::unique_ptr<ExternalStringOwner> owner(static_cast<ExternalStringOwner*>(opaque));
        owner->resource->Dispose();
    }, owner.get());
    if (!JS_IsException(value)) owner.release();
    return AdoptAs<String>(ctx, value);
}
MaybeLocal<String> String::NewExternalOneByte(Isolate* isolate, ExternalOneByteStringResource* resource) {
    JSContext* ctx = detail::CurrentJSContext(isolate);
    if (resource->length() > static_cast<size_t>(kMaxLength)) return {};
    auto owner = std::make_unique<ExternalStringOwner>(resource, ONE_BYTE_ENCODING);
    JSValue value = JS_NewExternalString(ctx, resource->data(), resource->length(), false, [](void* opaque) {
        std::unique_ptr<ExternalStringOwner> owner(static_cast<ExternalStringOwner*>(opaque));
        owner->resource->Dispose();
    }, owner.get());
    if (!JS_IsException(value)) owner.release();
    return AdoptAs<String>(ctx, value);
}
bool String::MakeExternal(ExternalStringResource* resource) { return MakeExternal(IsolateOf(this), resource); }
bool String::MakeExternal(ExternalOneByteStringResource* resource) { return MakeExternal(IsolateOf(this), resource); }
bool String::MakeExternal(Isolate*, ExternalStringResource* resource) {
    auto owner = std::make_unique<ExternalStringOwner>(resource, TWO_BYTE_ENCODING);
    int result = JS_MakeExternalString(ContextOf(this), Borrow(this), resource->data(), resource->length(), true, [](void* opaque) {
        std::unique_ptr<ExternalStringOwner> owner(static_cast<ExternalStringOwner*>(opaque));
        owner->resource->Dispose();
    }, owner.get());
    if (result < 0) Failed(ContextOf(this)); if (result > 0) owner.release(); return result > 0;
}
bool String::MakeExternal(Isolate*, ExternalOneByteStringResource* resource) {
    auto owner = std::make_unique<ExternalStringOwner>(resource, ONE_BYTE_ENCODING);
    int result = JS_MakeExternalString(ContextOf(this), Borrow(this), resource->data(), resource->length(), false, [](void* opaque) {
        std::unique_ptr<ExternalStringOwner> owner(static_cast<ExternalStringOwner*>(opaque));
        owner->resource->Dispose();
    }, owner.get());
    if (result < 0) Failed(ContextOf(this)); if (result > 0) owner.release(); return result > 0;
}
bool String::CanMakeExternal(Encoding encoding) const {
    return (encoding == TWO_BYTE_ENCODING || encoding == ONE_BYTE_ENCODING)
        && JS_CanMakeExternalString(Borrow(this), encoding == TWO_BYTE_ENCODING);
}
bool String::IsExternal() const { return JS_IsExternalString(Borrow(this)) != 0; }
bool String::IsExternalTwoByte() const { return IsExternal() && !IsOneByte(); }
bool String::IsExternalOneByte() const { return IsExternal() && IsOneByte(); }
String::ExternalStringResourceBase* String::GetExternalStringResourceBase(Isolate*, Encoding* encoding) const { return GetExternalStringResourceBase(encoding); }
String::ExternalStringResourceBase* String::GetExternalStringResourceBase(Encoding* encoding) const {
    if (!IsExternal()) { *encoding = UNKNOWN_ENCODING; return nullptr; }
    auto* owner = static_cast<ExternalStringOwner*>(JS_GetExternalStringOpaque(Borrow(this)));
    if (!owner) { *encoding = UNKNOWN_ENCODING; return nullptr; }
    *encoding = owner->encoding; return owner->resource;
}
String::ExternalStringResource* String::GetExternalStringResource() const {
    Encoding encoding; auto* resource = GetExternalStringResourceBase(&encoding);
    return encoding == TWO_BYTE_ENCODING ? static_cast<ExternalStringResource*>(resource) : nullptr;
}
const String::ExternalOneByteStringResource* String::GetExternalOneByteStringResource() const {
    Encoding encoding; auto* resource = GetExternalStringResourceBase(&encoding);
    return encoding == ONE_BYTE_ENCODING ? static_cast<ExternalOneByteStringResource*>(resource) : nullptr;
}
BackingStore::BackingStore(void* data, size_t length, bool shared, DeleterCallback deleter, void* opaque)
    : data_(data), length_(length), shared_(shared), deleter_(deleter), deleter_data_(opaque) {}
BackingStore::~BackingStore() { if (deleter_) deleter_(data_, length_, deleter_data_); }
void* BackingStore::Data() const { return data_; }
size_t BackingStore::ByteLength() const { return length_; }
size_t BackingStore::MaxByteLength() const { return length_; }
bool BackingStore::IsShared() const { return shared_; }
bool BackingStore::IsResizableByUserJavaScript() const { return false; }
void BackingStore::EmptyDeleter(void*, size_t, void*) {}
void* ArrayBuffer::Allocator::Reallocate(void* data, size_t old_length, size_t new_length) {
    void* replacement = Allocate(new_length); if (!replacement && new_length) return nullptr;
    if (std::min(old_length, new_length)) std::memcpy(replacement, data, std::min(old_length, new_length)); Free(data, old_length); return replacement;
}
namespace {
class DefaultBufferAllocator final : public ArrayBuffer::Allocator {
public:
    void* Allocate(size_t length) override { return std::calloc(length ? length : 1, 1); }
    void* AllocateUninitialized(size_t length) override { return std::malloc(length ? length : 1); }
    void Free(void* data, size_t) override { std::free(data); }
};
}
ArrayBuffer::Allocator* ArrayBuffer::Allocator::NewDefaultAllocator() { return new DefaultBufferAllocator; }
size_t ArrayBuffer::ByteLength() const {
    if (WasDetached()) return 0; size_t length = 0; JS_GetArrayBuffer(ContextOf(this), &length, Borrow(this)); return length;
}
size_t ArrayBuffer::MaxByteLength() const { return ByteLength(); }
void* ArrayBuffer::Data() const { if (WasDetached()) return nullptr; size_t length; return JS_GetArrayBuffer(ContextOf(this), &length, Borrow(this)); }
bool ArrayBuffer::IsDetachable() const { return JS_IsArrayBuffer(Borrow(this)) != 0; }
bool ArrayBuffer::WasDetached() const { return JS_IsDetachedArrayBuffer(Borrow(this)) != 0; }
bool ArrayBuffer::IsResizableByUserJavaScript() const { return false; }
MaybeLocal<ArrayBuffer> ArrayBuffer::MaybeNew(Isolate* isolate, size_t length, BackingStoreInitializationMode mode) {
    JSContext* ctx = detail::CurrentJSContext(isolate);
    if (length > INT32_MAX) { JS_ThrowRangeError(ctx, "QuickJS ArrayBuffer exceeds its 2 GiB length limit"); Failed(ctx); return {}; }
    auto backing = AllocateBacking(ctx, length, false, mode);
    if (!backing) return {}; return New(isolate, std::shared_ptr<BackingStore>(std::move(backing)));
}
Local<ArrayBuffer> ArrayBuffer::New(Isolate* isolate, size_t length, BackingStoreInitializationMode mode) {
    return MaybeNew(isolate, length, mode).ToLocalChecked();
}
Local<ArrayBuffer> ArrayBuffer::New(Isolate* isolate, std::shared_ptr<BackingStore> store) {
    JSContext* ctx = detail::CurrentJSContext(isolate); return AdoptAs<ArrayBuffer>(ctx, BufferFromBacking(ctx, std::move(store), false));
}
std::unique_ptr<BackingStore> ArrayBuffer::NewBackingStore(Isolate* isolate, size_t length, BackingStoreInitializationMode mode) {
    return AllocateBacking(detail::CurrentJSContext(isolate), length, false, mode);
}
std::unique_ptr<BackingStore> ArrayBuffer::NewBackingStore(void* data, size_t length, BackingStore::DeleterCallback free, void* opaque) {
    return std::make_unique<BackingStore>(data, length, false, free, opaque);
}
std::shared_ptr<BackingStore> ArrayBuffer::GetBackingStore() { return BufferBacking(this); }
void ArrayBuffer::Detach() { (void)Detach(Local<Value>()); }
Maybe<bool> ArrayBuffer::Detach(Local<Value> key) {
    JSContext* ctx = ContextOf(this);
    int result = JS_DetachArrayBufferWithKey(ctx, Borrow(this), key.IsEmpty() ? JS_UNDEFINED : Borrow(*key));
    if (result < 0) { Failed(ctx); return Nothing<bool>(); } return Just(result != 0);
}
void ArrayBuffer::SetDetachKey(Local<Value> key) {
    JSContext* ctx = ContextOf(this);
    if (JS_SetArrayBufferDetachKey(ctx, Borrow(this), key.IsEmpty() ? JS_UNDEFINED : Borrow(*key)) < 0) Failed(ctx);
}
size_t SharedArrayBuffer::ByteLength() const { size_t length = 0; JS_GetArrayBuffer(ContextOf(this), &length, Borrow(this)); return length; }
size_t SharedArrayBuffer::MaxByteLength() const { return ByteLength(); }
void* SharedArrayBuffer::Data() const { size_t length; return JS_GetArrayBuffer(ContextOf(this), &length, Borrow(this)); }
Local<SharedArrayBuffer> SharedArrayBuffer::New(Isolate* isolate, size_t length, BackingStoreInitializationMode mode) {
    JSContext* ctx = detail::CurrentJSContext(isolate);
    if (length > INT32_MAX) { JS_ThrowRangeError(ctx, "QuickJS SharedArrayBuffer exceeds its 2 GiB length limit"); Failed(ctx); return {}; }
    auto backing = AllocateBacking(ctx, length, true, mode); if (!backing) return {};
    return New(isolate, std::shared_ptr<BackingStore>(std::move(backing)));
}
Local<SharedArrayBuffer> SharedArrayBuffer::New(Isolate* isolate, std::shared_ptr<BackingStore> store) {
    JSContext* ctx = detail::CurrentJSContext(isolate); return AdoptAs<SharedArrayBuffer>(ctx, BufferFromBacking(ctx, std::move(store), true));
}
std::unique_ptr<BackingStore> SharedArrayBuffer::NewBackingStore(Isolate* isolate, size_t length, BackingStoreInitializationMode mode) {
    return AllocateBacking(detail::CurrentJSContext(isolate), length, true, mode);
}
std::unique_ptr<BackingStore> SharedArrayBuffer::NewBackingStore(void* data, size_t length, BackingStore::DeleterCallback free, void* opaque) {
    return std::make_unique<BackingStore>(data, length, true, free, opaque);
}
std::shared_ptr<BackingStore> SharedArrayBuffer::GetBackingStore() { return BufferBacking(this); }
Local<ArrayBuffer> ArrayBufferView::Buffer() {
    ViewInfo view(this); return AdoptAs<ArrayBuffer>(view.ctx, std::exchange(view.buffer, JS_UNDEFINED));
}
size_t ArrayBufferView::ByteOffset() {
    ViewInfo view(this); if (JS_IsException(view.buffer)) { Failed(view.ctx); return 0; } return view.offset;
}
size_t ArrayBufferView::ByteLength() {
    ViewInfo view(this); if (JS_IsException(view.buffer)) { Failed(view.ctx); return 0; } return view.length;
}
bool ArrayBufferView::HasBuffer() const {
    ViewInfo view(this); if (JS_IsException(view.buffer)) { Failed(view.ctx); return false; } return JS_IsObject(view.buffer);
}
MemorySpan<uint8_t> ArrayBufferView::GetContents(MemorySpan<uint8_t>) {
    ViewInfo view(this); if (JS_IsException(view.buffer)) { Failed(view.ctx); return {}; }
    if (JS_IsDetachedArrayBuffer(view.buffer)) return {};
    size_t length; auto* bytes = JS_GetArrayBuffer(view.ctx, &length, view.buffer);
    return MemorySpan<uint8_t>(bytes ? bytes + view.offset : nullptr, view.length);
}
size_t ArrayBufferView::CopyContents(void* destination, size_t capacity) {
    auto bytes = GetContents({}); size_t length = std::min(capacity, bytes.size());
    if (length) std::memcpy(destination, bytes.data(), length); return length;
}
size_t TypedArray::Length() {
    ViewInfo view(this); if (JS_IsException(view.buffer)) { Failed(view.ctx); return 0; } return view.element ? view.length / view.element : 0;
}
Local<DataView> DataView::New(Local<ArrayBuffer> buffer, size_t offset, size_t length) { return NewDataView(buffer, offset, length); }
Local<DataView> DataView::New(Local<SharedArrayBuffer> buffer, size_t offset, size_t length) { return NewDataView(buffer, offset, length); }
#define QJS_NEW_TYPED(T, TYPE) \
Local<T> T::New(Local<ArrayBuffer> buffer, size_t offset, size_t length) { return NewView<T>(buffer, offset, length, TYPE); } \
Local<T> T::New(Local<SharedArrayBuffer> buffer, size_t offset, size_t length) { return NewView<T>(buffer, offset, length, TYPE); }
QJS_NEW_TYPED(Uint8Array, JS_UINT8_TYPED_ARRAY)
QJS_NEW_TYPED(Uint8ClampedArray, JS_UINT8C_TYPED_ARRAY)
QJS_NEW_TYPED(Int8Array, JS_INT8_TYPED_ARRAY)
QJS_NEW_TYPED(Uint16Array, JS_UINT16_TYPED_ARRAY)
QJS_NEW_TYPED(Int16Array, JS_INT16_TYPED_ARRAY)
QJS_NEW_TYPED(Uint32Array, JS_UINT32_TYPED_ARRAY)
QJS_NEW_TYPED(Int32Array, JS_INT32_TYPED_ARRAY)
QJS_NEW_TYPED(Float32Array, JS_FLOAT32_TYPED_ARRAY)
QJS_NEW_TYPED(Float64Array, JS_FLOAT64_TYPED_ARRAY)
QJS_NEW_TYPED(BigInt64Array, JS_BIG_INT64_TYPED_ARRAY)
QJS_NEW_TYPED(BigUint64Array, JS_BIG_UINT64_TYPED_ARRAY)
#undef QJS_NEW_TYPED

namespace detail {
Local<Value> ClassifyAndAdopt(Isolate* isolate, JSContext* ctx, JSValue value) {
    if (JS_IsException(value)) { CaptureException(isolate, ctx); return {}; }
    if (JS_IsString(value)) return MakeLocal<String>(isolate, ctx, value);
    if (JS_IsSymbol(value)) return MakeLocal<Symbol>(isolate, ctx, value);
    if (JS_IsBool(value)) return MakeLocal<Boolean>(isolate, ctx, value);
    if (JS_IsBigInt(ctx, value)) return MakeLocal<BigInt>(isolate, ctx, value);
    if (JS_IsNumber(value)) {
        double n; JS_ToFloat64(ctx, &n, value);
        if (!(n == 0 && std::signbit(n)) && n == std::trunc(n)) {
            if (n >= INT32_MIN && n <= INT32_MAX) return MakeLocal<Int32>(isolate, ctx, value);
            if (n >= 0 && n <= UINT32_MAX) return MakeLocal<Uint32>(isolate, ctx, value);
            if (std::isfinite(n)) return MakeLocal<Integer>(isolate, ctx, value);
        }
        return MakeLocal<Number>(isolate, ctx, value);
    }
    if (!JS_IsObject(value)) return MakeLocal<Primitive>(isolate, ctx, value);
    if (external_class && JS_GetClassID(value) == external_class) return MakeLocal<External>(isolate, ctx, value);
    if (JS_IsProxy(value)) return MakeLocal<Proxy>(isolate, ctx, value);
    if (JS_IsFunction(ctx, value)) return MakeLocal<Function>(isolate, ctx, value);
    if (JS_IsPromise(value)) return MakeLocal<Promise>(isolate, ctx, value);
    if (JS_IsArray(ctx, value) > 0) return MakeLocal<Array>(isolate, ctx, value);
    if (JS_IsMap(value)) return MakeLocal<Map>(isolate, ctx, value);
    if (JS_IsSet(value)) return MakeLocal<Set>(isolate, ctx, value);
    if (JS_IsRegExp(value)) return MakeLocal<RegExp>(isolate, ctx, value);
    if (JS_IsArrayBuffer(value)) return MakeLocal<ArrayBuffer>(isolate, ctx, value);
    if (JS_IsSharedArrayBuffer(value)) return MakeLocal<SharedArrayBuffer>(isolate, ctx, value);
#define QJS_CLASSIFY_TYPED(T, TYPE) if (JS_IsWhatTypedArray(value, TYPE)) return MakeLocal<T>(isolate, ctx, value);
    QJS_CLASSIFY_TYPED(Uint8Array, JS_UINT8_TYPED_ARRAY)
    QJS_CLASSIFY_TYPED(Uint8ClampedArray, JS_UINT8C_TYPED_ARRAY)
    QJS_CLASSIFY_TYPED(Int8Array, JS_INT8_TYPED_ARRAY)
    QJS_CLASSIFY_TYPED(Uint16Array, JS_UINT16_TYPED_ARRAY)
    QJS_CLASSIFY_TYPED(Int16Array, JS_INT16_TYPED_ARRAY)
    QJS_CLASSIFY_TYPED(Uint32Array, JS_UINT32_TYPED_ARRAY)
    QJS_CLASSIFY_TYPED(Int32Array, JS_INT32_TYPED_ARRAY)
    QJS_CLASSIFY_TYPED(Float32Array, JS_FLOAT32_TYPED_ARRAY)
    QJS_CLASSIFY_TYPED(Float64Array, JS_FLOAT64_TYPED_ARRAY)
    QJS_CLASSIFY_TYPED(BigInt64Array, JS_BIG_INT64_TYPED_ARRAY)
    QJS_CLASSIFY_TYPED(BigUint64Array, JS_BIG_UINT64_TYPED_ARRAY)
#undef QJS_CLASSIFY_TYPED
    switch (JS_GetBuiltinType(value)) {
    case JS_BUILTIN_DATE: return MakeLocal<Date>(isolate, ctx, value);
    case JS_BUILTIN_DATA_VIEW: return MakeLocal<DataView>(isolate, ctx, value);
    case JS_BUILTIN_BOOLEAN_OBJECT: return MakeLocal<BooleanObject>(isolate, ctx, value);
    case JS_BUILTIN_NUMBER_OBJECT: return MakeLocal<NumberObject>(isolate, ctx, value);
    case JS_BUILTIN_STRING_OBJECT: return MakeLocal<StringObject>(isolate, ctx, value);
    case JS_BUILTIN_SYMBOL_OBJECT: return MakeLocal<SymbolObject>(isolate, ctx, value);
    case JS_BUILTIN_BIGINT_OBJECT: return MakeLocal<BigIntObject>(isolate, ctx, value);
    default: break;
    }
    return MakeLocal<Object>(isolate, ctx, value);
}
} // namespace detail
} // namespace v8
