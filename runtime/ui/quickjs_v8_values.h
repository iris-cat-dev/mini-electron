// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
// Two independent guards allow Object and Value to include each other's API.
#ifndef MINI_UI_QUICKJS_V8_VALUES_BASE_DEFINED
#define MINI_UI_QUICKJS_V8_VALUES_BASE_DEFINED
#include "runtime/ui/quickjs_v8_core.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <limits>
#include <vector>

namespace v8 {
class Primitive; class Numeric; class Boolean; class Number; class Integer;
class Int32; class Uint32; class BigInt; class Name; class String; class Symbol;
class Private; class External; class Object; class Context; class Array;
class ArrayBuffer; class SharedArrayBuffer;
#define MINI_QJS_CAST(T) static T* Cast(Data* value) { return detail::RebindView<T>(detail::CellOf(value)); }
class Value : public Data {
public:
    using Data::Data;
    MINI_QJS_CAST(Value)
    bool IsUndefined() const; bool IsNull() const; bool IsNullOrUndefined() const;
    bool IsTrue() const; bool IsFalse() const; bool IsName() const;
    bool IsString() const; bool IsSymbol() const; bool IsFunction() const;
    bool IsArray() const; bool IsObject() const; bool IsBigInt() const;
    bool IsBoolean() const; bool IsNumber() const; bool IsExternal() const;
    bool IsInt32() const; bool IsUint32() const; bool IsDate() const;
    bool IsArgumentsObject() const; bool IsBigIntObject() const;
    bool IsBooleanObject() const; bool IsNumberObject() const;
    bool IsStringObject() const; bool IsSymbolObject() const; bool IsNativeError() const;
    bool IsRegExp() const; bool IsAsyncFunction() const; bool IsGeneratorFunction() const;
    bool IsGeneratorObject() const; bool IsPromise() const; bool IsMap() const;
    bool IsSet() const; bool IsMapIterator() const; bool IsSetIterator() const;
    bool IsWeakMap() const; bool IsWeakSet() const; bool IsWeakRef() const;
    bool IsArrayBuffer() const; bool IsSharedArrayBuffer() const;
    bool IsArrayBufferView() const; bool IsTypedArray() const; bool IsDataView() const;
    bool IsUint8Array() const; bool IsUint8ClampedArray() const; bool IsInt8Array() const;
    bool IsUint16Array() const; bool IsInt16Array() const; bool IsUint32Array() const;
    bool IsInt32Array() const; bool IsFloat16Array() const; bool IsFloat32Array() const;
    bool IsFloat64Array() const; bool IsBigInt64Array() const; bool IsBigUint64Array() const;
    bool IsProxy() const; bool IsWasmMemoryObject() const; bool IsWasmModuleObject() const;
    bool IsWasmNull() const; bool IsModuleNamespaceObject() const;
    MaybeLocal<Primitive> ToPrimitive(Local<Context>) const;
    MaybeLocal<Numeric> ToNumeric(Local<Context>) const;
    MaybeLocal<BigInt> ToBigInt(Local<Context>) const;
    MaybeLocal<Number> ToNumber(Local<Context>) const;
    MaybeLocal<String> ToString(Local<Context>) const;
    MaybeLocal<String> ToDetailString(Local<Context>) const;
    MaybeLocal<Object> ToObject(Local<Context>) const;
    MaybeLocal<Integer> ToInteger(Local<Context>) const;
    MaybeLocal<Uint32> ToUint32(Local<Context>) const;
    MaybeLocal<Int32> ToInt32(Local<Context>) const;
    Local<Boolean> ToBoolean(Isolate*) const;
    MaybeLocal<Uint32> ToArrayIndex(Local<Context>) const;
    bool BooleanValue(Isolate*) const;
    Maybe<double> NumberValue(Local<Context>) const;
    Maybe<int64_t> IntegerValue(Local<Context>) const;
    Maybe<uint32_t> Uint32Value(Local<Context>) const;
    Maybe<int32_t> Int32Value(Local<Context>) const;
    Maybe<bool> Equals(Local<Context>, Local<Value>) const;
    bool StrictEquals(Local<Value>) const; bool SameValue(Local<Value>) const;
    Local<String> TypeOf(Isolate*);
    Maybe<bool> InstanceOf(Local<Context>, Local<Object>);
};
class TypecheckWitness {
public:
    explicit TypecheckWitness(Isolate*);
    ~TypecheckWitness();
    TypecheckWitness(const TypecheckWitness&) = delete;
    TypecheckWitness& operator=(const TypecheckWitness&) = delete;
    bool Matches(Local<Value>) const;
    void Update(Local<Value>);
private:
    Isolate* isolate_;
    const void* native_type_ = nullptr;
};
class Primitive : public Value { public: using Value::Value; MINI_QJS_CAST(Primitive) };
class Boolean : public Primitive {
public: using Primitive::Primitive; MINI_QJS_CAST(Boolean)
    bool Value() const; static Local<Boolean> New(Isolate*, bool);
};
class Numeric : public Primitive { public: using Primitive::Primitive; MINI_QJS_CAST(Numeric) };
class Number : public Numeric {
public: using Numeric::Numeric; MINI_QJS_CAST(Number)
    double Value() const; static Local<Number> New(Isolate*, double);
};
class Integer : public Number {
public: using Number::Number; MINI_QJS_CAST(Integer)
    int64_t Value() const; static Local<Integer> New(Isolate*, int32_t);
    static Local<Integer> NewFromUnsigned(Isolate*, uint32_t);
};
class Int32 : public Integer { public: using Integer::Integer; MINI_QJS_CAST(Int32) int32_t Value() const; };
class Uint32 : public Integer { public: using Integer::Integer; MINI_QJS_CAST(Uint32) uint32_t Value() const; };
class BigInt : public Numeric {
public: using Numeric::Numeric; MINI_QJS_CAST(BigInt)
    static Local<BigInt> New(Isolate*, int64_t);
    static Local<BigInt> NewFromUnsigned(Isolate*, uint64_t);
    static MaybeLocal<BigInt> NewFromWords(Local<Context>, int, int, const uint64_t*);
    uint64_t Uint64Value(bool* lossless = nullptr) const;
    int64_t Int64Value(bool* lossless = nullptr) const;
    int WordCount() const; void ToWordsArray(int*, int*, uint64_t*) const;
};
class Name : public Primitive {
public: using Primitive::Primitive; MINI_QJS_CAST(Name) int GetIdentityHash();
};
enum class NewStringType { kNormal, kInternalized };
class String : public Name {
public:
    using Name::Name; MINI_QJS_CAST(String)
    static constexpr int kMaxLength = sizeof(void*) == 4 ? (1 << 28) - 16 : (1 << 29) - 24;
    enum Encoding { UNKNOWN_ENCODING = 1, TWO_BYTE_ENCODING = 0, ONE_BYTE_ENCODING = 8 };
    enum WriteOptions { NO_OPTIONS = 0, HINT_MANY_WRITES_EXPECTED = 1,
        NO_NULL_TERMINATION = 2, PRESERVE_ONE_BYTE_NULL = 4, REPLACE_INVALID_UTF8 = 8 };
    int Length() const; int Utf8Length(Isolate*) const;
    bool IsOneByte() const; bool ContainsOnlyOneByte() const;
    int Write(Isolate*, uint16_t*, int start = 0, int length = -1, int options = NO_OPTIONS) const;
    int WriteOneByte(Isolate*, uint8_t*, int start = 0, int length = -1, int options = NO_OPTIONS) const;
    int WriteUtf8(Isolate*, char*, int length = -1, int* nchars_ref = nullptr, int options = NO_OPTIONS) const;
    static Local<String> Empty(Isolate*);
    static MaybeLocal<String> NewFromUtf8(Isolate*, const char*, NewStringType = NewStringType::kNormal, int length = -1);
    static MaybeLocal<String> NewFromOneByte(Isolate*, const uint8_t*, NewStringType = NewStringType::kNormal, int length = -1);
    static MaybeLocal<String> NewFromTwoByte(Isolate*, const uint16_t*, NewStringType = NewStringType::kNormal, int length = -1);
    template<int N> static Local<String> NewFromUtf8Literal(Isolate* isolate, const char (&s)[N], NewStringType type = NewStringType::kNormal) {
        return NewFromUtf8(isolate, s, type, N - 1).ToLocalChecked();
    }
    static Local<String> Concat(Isolate*, Local<String>, Local<String>);
    Local<String> InternalizeString(Isolate*); bool StringEquals(Local<String>) const;
    class ExternalStringResourceBase {
    public:
        virtual ~ExternalStringResourceBase() = default;
        virtual bool IsCacheable() const { return true; }
        virtual void Unaccount(Isolate*) {}
        ExternalStringResourceBase(const ExternalStringResourceBase&) = delete;
        ExternalStringResourceBase& operator=(const ExternalStringResourceBase&) = delete;
    protected:
        ExternalStringResourceBase() = default;
        virtual void Dispose() { delete this; }
        virtual void Lock() const {} virtual void Unlock() const {}
        friend class String;
    };
    class ExternalStringResource : public ExternalStringResourceBase {
    public:
        virtual const uint16_t* data() const = 0; virtual size_t length() const = 0;
        const uint16_t* cached_data() const { return cached_data_; }
        void UpdateDataCache() { cached_data_ = data(); }
    private: const uint16_t* cached_data_ = nullptr;
    };
    class ExternalOneByteStringResource : public ExternalStringResourceBase {
    public:
        virtual const char* data() const = 0; virtual size_t length() const = 0;
        const char* cached_data() const { return cached_data_; }
        void UpdateDataCache() { cached_data_ = data(); }
    private: const char* cached_data_ = nullptr;
    };
    static MaybeLocal<String> NewExternalTwoByte(Isolate*, ExternalStringResource*);
    static MaybeLocal<String> NewExternalOneByte(Isolate*, ExternalOneByteStringResource*);
    bool MakeExternal(ExternalStringResource*); bool MakeExternal(ExternalOneByteStringResource*);
    bool MakeExternal(Isolate*, ExternalStringResource*); bool MakeExternal(Isolate*, ExternalOneByteStringResource*);
    bool CanMakeExternal(Encoding) const;
    bool IsExternal() const; bool IsExternalTwoByte() const; bool IsExternalOneByte() const;
    ExternalStringResourceBase* GetExternalStringResourceBase(Isolate*, Encoding*) const;
    ExternalStringResourceBase* GetExternalStringResourceBase(Encoding*) const;
    ExternalStringResource* GetExternalStringResource() const;
    const ExternalOneByteStringResource* GetExternalOneByteStringResource() const;
    class Utf8Value {
    public:
        Utf8Value(Isolate*, Local<v8::Value>, WriteOptions = REPLACE_INVALID_UTF8);
        ~Utf8Value(); char* operator*() { return str_; } const char* operator*() const { return str_; }
        int length() const { return length_; }
        Utf8Value(const Utf8Value&) = delete; Utf8Value& operator=(const Utf8Value&) = delete;
    private: char* str_ = nullptr; int length_ = 0;
    };
    class Value {
    public:
        Value(Isolate*, Local<v8::Value>); ~Value();
        uint16_t* operator*() { return str_; } const uint16_t* operator*() const { return str_; }
        int length() const { return length_; }
        Value(const Value&) = delete; Value& operator=(const Value&) = delete;
    private: uint16_t* str_ = nullptr; int length_ = 0;
    };
    class ValueView {
    public:
        ValueView(Isolate*, Local<String>); ~ValueView();
        const uint8_t* data8() const { return static_cast<const uint8_t*>(data_); }
        const uint16_t* data16() const { return static_cast<const uint16_t*>(data_); }
        int length() const { return length_; } bool is_one_byte() const { return one_byte_; }
        ValueView(const ValueView&) = delete; ValueView& operator=(const ValueView&) = delete;
    private: JSContext* context_; JSValue string_; const void* data_; int length_; bool one_byte_;
    };
};
class Symbol : public Name {
public: using Name::Name; MINI_QJS_CAST(Symbol)
    Local<Value> Description(Isolate*) const;
    static Local<Symbol> New(Isolate*, Local<String> description = {});
    static Local<Symbol> For(Isolate*, Local<String>); static Local<Symbol> ForApi(Isolate*, Local<String>);
    static Local<Symbol> GetAsyncIterator(Isolate*); static Local<Symbol> GetHasInstance(Isolate*);
    static Local<Symbol> GetIsConcatSpreadable(Isolate*); static Local<Symbol> GetIterator(Isolate*);
    static Local<Symbol> GetMatch(Isolate*); static Local<Symbol> GetReplace(Isolate*);
    static Local<Symbol> GetSearch(Isolate*); static Local<Symbol> GetSplit(Isolate*);
    static Local<Symbol> GetToPrimitive(Isolate*); static Local<Symbol> GetToStringTag(Isolate*);
    static Local<Symbol> GetUnscopables(Isolate*);
};
class Private : public Data {
public: using Data::Data; MINI_QJS_CAST(Private)
    Local<Value> Name() const; static Local<Private> New(Isolate*, Local<String> name = {});
    static Local<Private> ForApi(Isolate*, Local<String>);
};
class External : public Value {
public: using Value::Value; MINI_QJS_CAST(External)
    static Local<External> New(Isolate*, void*); void* Value() const;
};
class PrimitiveArray : public Data {
public: using Data::Data; MINI_QJS_CAST(PrimitiveArray)
    static Local<PrimitiveArray> New(Isolate*, int); int Length() const;
    void Set(Isolate*, int, Local<Primitive>); Local<Primitive> Get(Isolate*, int);
};
Local<Primitive> Undefined(Isolate*); Local<Primitive> Null(Isolate*);
Local<Boolean> True(Isolate*); Local<Boolean> False(Isolate*);
namespace detail {
Local<Value> ClassifyAndAdopt(Isolate*, JSContext*, JSValue);
void DisposeValuesState(Isolate*);
}
} // namespace v8
#endif // MINI_UI_QUICKJS_V8_VALUES_BASE_DEFINED

#ifndef MINI_UI_QUICKJS_V8_VALUES_BASE_ONLY
#include "runtime/ui/quickjs_v8_objects.h"
#ifndef MINI_UI_QUICKJS_V8_VALUES_OBJECTS_DEFINED
#define MINI_UI_QUICKJS_V8_VALUES_OBJECTS_DEFINED
namespace v8 {
class Array : public Object {
public: using Object::Object; MINI_QJS_CAST(Array)
    uint32_t Length() const; static Local<Array> New(Isolate*, int length = 0);
    static Local<Array> New(Isolate*, Local<Value>*, size_t);
    static MaybeLocal<Array> New(Local<Context>, size_t, std::function<MaybeLocal<Value>()>);
    enum class CallbackResult { kException, kBreak, kContinue };
    using IterationCallback = CallbackResult (*)(uint32_t, Local<Value>, void*);
    Maybe<void> Iterate(Local<Context>, IterationCallback, void*);
};
class Map : public Object {
public: using Object::Object; MINI_QJS_CAST(Map)
    size_t Size() const; void Clear(); static Local<Map> New(Isolate*);
    MaybeLocal<Value> Get(Local<Context>, Local<Value>);
    MaybeLocal<Map> Set(Local<Context>, Local<Value>, Local<Value>);
    Maybe<bool> Has(Local<Context>, Local<Value>); Maybe<bool> Delete(Local<Context>, Local<Value>);
    Local<Array> AsArray() const;
};
class Set : public Object {
public: using Object::Object; MINI_QJS_CAST(Set)
    size_t Size() const; void Clear(); static Local<Set> New(Isolate*);
    MaybeLocal<Set> Add(Local<Context>, Local<Value>);
    Maybe<bool> Has(Local<Context>, Local<Value>); Maybe<bool> Delete(Local<Context>, Local<Value>);
    Local<Array> AsArray() const;
};
class Date : public Object {
public: using Object::Object; MINI_QJS_CAST(Date)
    static MaybeLocal<Value> New(Local<Context>, double);
    static MaybeLocal<Value> Parse(Local<Context>, Local<String>);
    double ValueOf() const; Local<String> ToISOString() const; Local<String> ToUTCString() const;
};
class RegExp : public Object {
public: using Object::Object; MINI_QJS_CAST(RegExp)
    enum Flags { kNone = 0, kGlobal = 1, kIgnoreCase = 2, kMultiline = 4, kSticky = 8,
        kUnicode = 16, kDotAll = 32, kLinear = 64, kHasIndices = 128, kUnicodeSets = 256 };
    static constexpr int kFlagCount = 9;
    static MaybeLocal<RegExp> New(Local<Context>, Local<String>, Flags);
    static MaybeLocal<RegExp> NewWithBacktrackLimit(Local<Context>, Local<String>, Flags, uint32_t);
    MaybeLocal<Object> Exec(Local<Context>, Local<String>);
    Local<String> GetSource() const; Flags GetFlags() const;
};
enum class ArrayBufferCreationMode { kInternalized, kExternalized };
enum class BackingStoreInitializationMode { kZeroInitialized, kUninitialized };
class BackingStore {
public:
    using DeleterCallback = void (*)(void*, size_t, void*);
    BackingStore(void*, size_t, bool, DeleterCallback, void*);
    ~BackingStore();
    BackingStore(const BackingStore&) = delete; BackingStore& operator=(const BackingStore&) = delete;
    void* Data() const; size_t ByteLength() const; size_t MaxByteLength() const;
    bool IsShared() const; bool IsResizableByUserJavaScript() const;
    static void EmptyDeleter(void*, size_t, void*);
private:
    void* data_; size_t length_; bool shared_; DeleterCallback deleter_; void* deleter_data_;
};
using BackingStoreDeleterCallback = BackingStore::DeleterCallback;
class ArrayBuffer : public Object {
public: using Object::Object; MINI_QJS_CAST(ArrayBuffer)
    class Allocator {
    public:
        virtual ~Allocator() = default; virtual void* Allocate(size_t) = 0;
        virtual void* AllocateUninitialized(size_t) = 0; virtual void Free(void*, size_t) = 0;
        virtual void* Reallocate(void*, size_t, size_t); static Allocator* NewDefaultAllocator();
        enum class AllocationMode { kNormal, kReservation };
    };
    size_t ByteLength() const; size_t MaxByteLength() const; void* Data() const;
    static MaybeLocal<ArrayBuffer> MaybeNew(Isolate*, size_t, BackingStoreInitializationMode = BackingStoreInitializationMode::kZeroInitialized);
    static Local<ArrayBuffer> New(Isolate*, size_t, BackingStoreInitializationMode = BackingStoreInitializationMode::kZeroInitialized);
    static Local<ArrayBuffer> New(Isolate*, std::shared_ptr<BackingStore>);
    static std::unique_ptr<BackingStore> NewBackingStore(Isolate*, size_t, BackingStoreInitializationMode = BackingStoreInitializationMode::kZeroInitialized);
    static std::unique_ptr<BackingStore> NewBackingStore(void*, size_t, BackingStore::DeleterCallback, void*);
    bool IsDetachable() const; bool WasDetached() const; void Detach(); Maybe<bool> Detach(Local<Value>);
    void SetDetachKey(Local<Value>); std::shared_ptr<BackingStore> GetBackingStore();
    bool IsResizableByUserJavaScript() const;
    static constexpr int kInternalFieldCount = 2, kEmbedderFieldCount = 2;
};
class SharedArrayBuffer : public Object {
public: using Object::Object; MINI_QJS_CAST(SharedArrayBuffer)
    size_t ByteLength() const; size_t MaxByteLength() const; void* Data() const;
    static Local<SharedArrayBuffer> New(Isolate*, size_t, BackingStoreInitializationMode = BackingStoreInitializationMode::kZeroInitialized);
    static Local<SharedArrayBuffer> New(Isolate*, std::shared_ptr<BackingStore>);
    static std::unique_ptr<BackingStore> NewBackingStore(Isolate*, size_t, BackingStoreInitializationMode = BackingStoreInitializationMode::kZeroInitialized);
    static std::unique_ptr<BackingStore> NewBackingStore(void*, size_t, BackingStore::DeleterCallback, void*);
    std::shared_ptr<BackingStore> GetBackingStore(); static constexpr int kInternalFieldCount = 2;
};
class ArrayBufferView : public Object {
public: using Object::Object; MINI_QJS_CAST(ArrayBufferView)
    Local<ArrayBuffer> Buffer(); size_t ByteOffset(); size_t ByteLength();
    size_t CopyContents(void*, size_t); MemorySpan<uint8_t> GetContents(MemorySpan<uint8_t>);
    bool HasBuffer() const; static constexpr int kInternalFieldCount = 2, kEmbedderFieldCount = 2;
};
class DataView : public ArrayBufferView {
public: using ArrayBufferView::ArrayBufferView; MINI_QJS_CAST(DataView)
    static Local<DataView> New(Local<ArrayBuffer>, size_t, size_t);
    static Local<DataView> New(Local<SharedArrayBuffer>, size_t, size_t);
};
class TypedArray : public ArrayBufferView {
public: using ArrayBufferView::ArrayBufferView; MINI_QJS_CAST(TypedArray)
    static constexpr size_t kMaxByteLength = sizeof(size_t) == 4 ? static_cast<size_t>(std::numeric_limits<int>::max()) : static_cast<size_t>((uint64_t{1} << 53) - 1);
    size_t Length();
};
#define MINI_QJS_TYPED_ARRAY(T, SIZE) \
class T : public TypedArray { public: using TypedArray::TypedArray; MINI_QJS_CAST(T) \
    static constexpr size_t kMaxLength = TypedArray::kMaxByteLength / SIZE; \
    static Local<T> New(Local<ArrayBuffer>, size_t, size_t); \
    static Local<T> New(Local<SharedArrayBuffer>, size_t, size_t); };
MINI_QJS_TYPED_ARRAY(Uint8Array, 1)
MINI_QJS_TYPED_ARRAY(Uint8ClampedArray, 1)
MINI_QJS_TYPED_ARRAY(Int8Array, 1)
MINI_QJS_TYPED_ARRAY(Uint16Array, 2)
MINI_QJS_TYPED_ARRAY(Int16Array, 2)
MINI_QJS_TYPED_ARRAY(Uint32Array, 4)
MINI_QJS_TYPED_ARRAY(Int32Array, 4)
MINI_QJS_TYPED_ARRAY(Float32Array, 4)
MINI_QJS_TYPED_ARRAY(Float64Array, 8)
MINI_QJS_TYPED_ARRAY(BigInt64Array, 8)
MINI_QJS_TYPED_ARRAY(BigUint64Array, 8)
#undef MINI_QJS_TYPED_ARRAY
class NumberObject : public Object {
public: using Object::Object; MINI_QJS_CAST(NumberObject)
    static Local<Value> New(Isolate*, double); double ValueOf() const;
};
class BooleanObject : public Object {
public: using Object::Object; MINI_QJS_CAST(BooleanObject)
    static Local<Value> New(Isolate*, bool); bool ValueOf() const;
};
class StringObject : public Object {
public: using Object::Object; MINI_QJS_CAST(StringObject)
    static Local<Value> New(Isolate*, Local<String>); Local<String> ValueOf() const;
};
class SymbolObject : public Object {
public: using Object::Object; MINI_QJS_CAST(SymbolObject)
    static Local<Value> New(Isolate*, Local<Symbol>); Local<Symbol> ValueOf() const;
};
class BigIntObject : public Object {
public: using Object::Object; MINI_QJS_CAST(BigIntObject)
    static Local<Value> New(Isolate*, int64_t); Local<BigInt> ValueOf() const;
};
} // namespace v8
#endif // MINI_UI_QUICKJS_V8_VALUES_OBJECTS_DEFINED
#endif // !MINI_UI_QUICKJS_V8_VALUES_BASE_ONLY
