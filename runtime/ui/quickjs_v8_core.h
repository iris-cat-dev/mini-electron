// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef MINI_UI_RUNTIME_UI_QUICKJS_V8_CORE_H_
#define MINI_UI_RUNTIME_UI_QUICKJS_V8_CORE_H_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <functional>
#include <initializer_list>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "third_party/quickjs/quickjs.h"
#include "runtime/ui/quickjs_bindings.h"
#include "v8/include/v8config.h"

namespace v8 {
class Data; class Value; class Primitive; class Boolean; class Name; class String;
class Symbol; class Private; class Number; class Integer; class Int32; class Uint32;
class Numeric; class External; class Proxy; class Date; class RegExp;
class Map; class Set; class ArrayBufferView; class DataView; class TypedArray;
class Uint8Array; class Uint8ClampedArray; class Int8Array; class Uint16Array;
class Int16Array; class Uint32Array; class Int32Array; class Float32Array;
class Float64Array; class BigInt64Array; class BigUint64Array;
class BooleanObject; class NumberObject; class StringObject; class SymbolObject; class BigIntObject;
class BigInt; class Object; class Array; class Function; class Context; class Template;
class ObjectTemplate; class FunctionTemplate; class Signature; class AccessorSignature;
class Script; class UnboundScript; class Module; class Promise; class Message;
class ScriptOrModule; class UnboundModuleScript; class DictionaryTemplate;
class StackTrace; class StackFrame; class FixedArray; class Isolate; class HandleScope;
template<class T> class WeakCallbackInfo;
class EscapableHandleScope; class MicrotaskQueue; class TryCatch; class CppHeap;
class EmbedderGraph; class HeapProfiler; class ArrayBuffer; class SharedArrayBuffer;
template<class T> class Local;
template<class T> class LocalVector;
template<class T> class MaybeLocal;
template<class T> class Global;
template<class T> class TracedReference;
template<class T> class PersistentBase;
template<class T> class ReturnValue;

namespace detail {
struct Cell;
struct CoreAccess;
struct DataPayload { virtual ~DataPayload() = default; };
struct WeakPersistent;
inline constexpr unsigned kPinnedContext = 1u << 7;
inline constexpr unsigned kIsolateData = 1u << 8;
using WeakSecondPass = std::function<void()>;
using WeakCallback = std::function<WeakSecondPass(Isolate*, void*, void**)>;
std::shared_ptr<WeakPersistent> MakeWeakPersistent(Cell*, void*, WeakCallback, bool internal_fields);
std::shared_ptr<WeakPersistent> CopyWeakPersistent(const std::shared_ptr<WeakPersistent>&);
void CancelWeakPersistent(const std::shared_ptr<WeakPersistent>&);
bool WeakPersistentEmpty(const std::shared_ptr<WeakPersistent>&);
Local<Data> LocalizeWeak(Isolate*, const std::shared_ptr<WeakPersistent>&);
Cell* StrengthenWeak(const std::shared_ptr<WeakPersistent>&);
void* WeakParameter(const std::shared_ptr<WeakPersistent>&);
Isolate* WeakIsolate(const std::shared_ptr<WeakPersistent>&);
void CollectWeakPersistents(Isolate*);
void UpdateWeakInternalField(Isolate*, JSValueConst object, int index, void*);
JSValueConst Borrow(const Data*);
JSContext* ContextOf(const Data*);
Isolate* IsolateOf(const Data*);
JSContext* CurrentJSContext(Isolate*);
JSContext* NeutralJSContext(Isolate*);
Isolate* IsolateFor(JSContext*);
Local<Value> Adopt(JSContext*, JSValue owned);
Local<Value> ClassifyAndAdopt(Isolate*, JSContext*, JSValue owned);
std::shared_ptr<DataPayload> Payload(const Data*);
void SetPayload(Data*, std::shared_ptr<DataPayload>);
Cell* CellOf(const Data*);
Data* ViewOf(Cell*);
void Retain(Cell*);
void Release(Cell*);
bool Equal(const Data*, const Data*);
void RegisterContext(Isolate*, JSContext*, Context*);
void UnregisterContext(JSContext*);
Context* LookupContext(JSContext*);
void EnterContext(Isolate*, Context*);
void LeaveContext(Isolate*, Context*);
JSContext* ConsumePendingContext(Isolate*); // Transfers one JSContext reference.
struct ExceptionMetadata {
    std::string resource_name;
    std::string source_line;
    std::string stack;
    int line_number = 0;
    int start_column = 0;
    int end_column = 0;
};
struct ExceptionSink {
    virtual ~ExceptionSink() = default;
    virtual void OnException(JSContext*, JSValueConst, const ExceptionMetadata&) = 0;
};
void PushExceptionSink(Isolate*, ExceptionSink*);
void PopExceptionSink(Isolate*, ExceptionSink*);
void PushTryCatch(Isolate*, TryCatch*);
void PopTryCatch(Isolate*, TryCatch*);
bool HasPendingException(Isolate*);
JSValueConst PeekPendingException(Isolate*);
JSValue TakePendingException(Isolate*);
void ClearPendingException(Isolate*);
const ExceptionMetadata& PendingExceptionMetadata(Isolate*);
void SetPendingExceptionMetadata(Isolate*, ExceptionMetadata);
void CaptureException(Isolate*, JSContext*);
void Throw(Isolate*, JSContext*, JSValue owned);
using ConstructView = Data* (*)(void*, Cell*);
using DestroyView = void (*)(Data*);
Data* RebindCellView(Cell*, ConstructView, DestroyView);
template<class T> Data* ConstructApiView(void* storage, Cell* cell) { return new (storage) T(cell); }
template<class T> void DestroyApiView(Data* view) { static_cast<T*>(view)->~T(); }
template<class T> T* RebindView(Cell* cell) {
    static_assert(sizeof(T) == sizeof(void*), "API views carry only a Cell pointer");
    return static_cast<T*>(RebindCellView(cell, ConstructApiView<T>, DestroyApiView<T>));
}
Cell* AllocateCell(Isolate*, JSContext*, JSValue, std::shared_ptr<DataPayload>,
    ConstructView, DestroyView, unsigned type_flags);
Local<Data> Localize(Isolate*, Cell*);
Local<Value> LocalizeEdge(Isolate*, mini_ui::QuickJSEdgeId);
void CheckIsolate(Isolate*);
class ExecutionScope {
public:
    explicit ExecutionScope(Isolate*);
    ~ExecutionScope();
    ExecutionScope(const ExecutionScope&) = delete;
    ExecutionScope& operator=(const ExecutionScope&) = delete;
private:
    Isolate* isolate_;
};
template<class T> Local<T> MakeLocal(Isolate*, JSContext*, JSValue owned,
    std::shared_ptr<DataPayload> payload = {});
}

template<class T> class MemorySpan {
public:
    constexpr MemorySpan() = default;
    constexpr MemorySpan(T* data, size_t size) : data_(data), size_(size) {}
    template<size_t N> constexpr MemorySpan(T (&data)[N]) : data_(data), size_(N) {}
    constexpr T* data() const { return data_; }
    constexpr size_t size() const { return size_; }
    constexpr bool empty() const { return size_ == 0; }
    constexpr T* begin() const { return data_; }
    constexpr T* end() const { return size_ ? data_ + size_ : data_; }
    constexpr T& operator[](size_t index) const { assert(index < size_); return data_[index]; }
private:
    T* data_ = nullptr;
    size_t size_ = 0;
};

class Data {
public:
    explicit Data(detail::Cell* cell) : cell_(cell) {}
    bool IsValue() const;
    bool IsModule() const;
    bool IsFixedArray() const;
    bool IsPrivate() const;
    bool IsObjectTemplate() const;
    bool IsFunctionTemplate() const;
    bool IsContext() const;
    friend detail::Cell* detail::CellOf(const Data*);
private:
    detail::Cell* cell_;
};

class FixedArray : public Data {
public:
    using Data::Data;
    static Local<FixedArray> New(Isolate*, int length);
    int Length() const;
    Local<Data> Get(Local<Context>, int index) const;
    void Set(int index, Local<Data>);
    static FixedArray* Cast(Data* data) { return data ? detail::RebindView<FixedArray>(detail::CellOf(data)) : nullptr; }
};

template<class T> class Local {
public:
    constexpr Local() = default;
    explicit constexpr Local(T* pointer) : pointer_(pointer) {}
    template<class S, std::enable_if_t<std::is_convertible<S*, T*>::value, int> = 0>
    Local(Local<S> other) : pointer_(*other) {}
    bool IsEmpty() const { return pointer_ == nullptr; }
    void Clear() { pointer_ = nullptr; }
    T* operator->() const { assert(pointer_); return pointer_; }
    T* operator*() const { return pointer_; }
    template<class S> Local<S> As() const { return Local<S>::Cast(*this); }
    template<class S> static Local<T> Cast(Local<S> other) {
        if constexpr (std::is_convertible<S*, T*>::value) return Local<T>(static_cast<T*>(*other));
        else return Local<T>(other.IsEmpty() ? nullptr : detail::RebindView<T>(detail::CellOf(*other)));
    }
    static Local<T> New(Isolate* isolate, Local<T> other) {
        return other.IsEmpty() ? Local<T>() : detail::Localize(isolate, detail::CellOf(*other)).template As<T>();
    }
    template<class H> static Local<T> New(Isolate* isolate, const H& handle) { return handle.Get(isolate).template As<T>(); }
    template<class S> bool operator==(Local<S> other) const { return detail::Equal(pointer_, *other); }
    template<class S> bool operator!=(Local<S> other) const { return !(*this == other); }
private:
    T* pointer_ = nullptr;
};

// Cells are rooted by their HandleScope, independently of this contiguous
// typed pointer storage. Copying or moving a vector does not extend that scope.
template<class T> class LocalVector {
    using Storage = std::vector<Local<T>>;
public:
    using value_type = Local<T>;
    using reference = value_type&;
    using const_reference = const value_type&;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using iterator = typename Storage::iterator;
    using const_iterator = typename Storage::const_iterator;

    explicit LocalVector(Isolate* isolate) { detail::CheckIsolate(isolate); }
    LocalVector(Isolate* isolate, size_t size) : LocalVector(isolate) { values_.resize(size); }
    explicit LocalVector(Isolate* isolate, std::initializer_list<Local<T>> values)
        : LocalVector(isolate) { values_.assign(values); }

    iterator begin() noexcept { return values_.begin(); }
    const_iterator begin() const noexcept { return values_.begin(); }
    iterator end() noexcept { return values_.end(); }
    const_iterator end() const noexcept { return values_.end(); }
    size_t size() const noexcept { return values_.size(); }
    bool empty() const noexcept { return values_.empty(); }
    void reserve(size_t size) { values_.reserve(size); }
    void shrink_to_fit() { values_.shrink_to_fit(); }
    reference operator[](size_t index) { return values_[index]; }
    const_reference operator[](size_t index) const { return values_[index]; }
    reference at(size_t index) { return values_.at(index); }
    const_reference at(size_t index) const { return values_.at(index); }
    reference front() { return values_.front(); }
    const_reference front() const { return values_.front(); }
    reference back() { return values_.back(); }
    const_reference back() const { return values_.back(); }
    Local<T>* data() noexcept { return values_.data(); }
    const Local<T>* data() const noexcept { return values_.data(); }
    iterator insert(const_iterator pos, const Local<T>& value) { return values_.insert(pos, value); }
    template<class InputIt> iterator insert(const_iterator pos, InputIt first, InputIt last) {
        return values_.insert(pos, first, last);
    }
    iterator insert(const_iterator pos, std::initializer_list<Local<T>> values) {
        return values_.insert(pos, values);
    }
    LocalVector& operator=(std::initializer_list<Local<T>> values) {
        values_.assign(values); return *this;
    }
    void push_back(const Local<T>& value) { values_.push_back(value); }
    void pop_back() { values_.pop_back(); }
    template<class... Args> void emplace_back(Args&&... args) {
        values_.push_back(Local<T>{std::forward<Args>(args)...});
    }
    void clear() noexcept { values_.clear(); }
    void resize(size_t size) { values_.resize(size); }
    void swap(LocalVector& other) { values_.swap(other.values_); }
    friend bool operator==(const LocalVector& left, const LocalVector& right) {
        return left.values_ == right.values_;
    }
    friend bool operator!=(const LocalVector& left, const LocalVector& right) { return !(left == right); }
private:
    Storage values_;
};

template<class T> class MaybeLocal {
public:
    constexpr MaybeLocal() = default;
    template<class S, std::enable_if_t<std::is_convertible<S*, T*>::value, int> = 0>
    MaybeLocal(Local<S> value) : value_(value) {}
    template<class S, std::enable_if_t<std::is_convertible<S*, T*>::value, int> = 0>
    MaybeLocal(MaybeLocal<S> value) { Local<S> local; if (value.ToLocal(&local)) value_ = local; }
    bool IsEmpty() const { return value_.IsEmpty(); }
    template<class S> static MaybeLocal<T> Cast(MaybeLocal<S> other) {
        Local<S> local;
        return other.ToLocal(&local) ? MaybeLocal<T>(Local<T>::Cast(local)) : MaybeLocal<T>();
    }
    template<class S> MaybeLocal<S> As() const { return MaybeLocal<S>::Cast(*this); }
    template<class S> bool ToLocal(Local<S>* out) const {
        if (IsEmpty()) { out->Clear(); return false; }
        *out = value_.template As<S>(); return true;
    }
    Local<T> ToLocalChecked() const { if (IsEmpty()) std::abort(); return value_; }
    Local<T> FromMaybe(Local<T> fallback) const { return IsEmpty() ? fallback : value_; }
private:
    Local<T> value_;
};

template<class T> class Maybe {
public:
    bool IsNothing() const { return !has_; }
    bool IsJust() const { return has_; }
    T FromJust() const { if (!has_) std::abort(); return value_; }
    T ToChecked() const { return FromJust(); }
    T FromMaybe(T fallback) const { return has_ ? value_ : fallback; }
    bool To(T* out) const { if (!has_) return false; *out = value_; return true; }
    void Check() const { if (!has_) std::abort(); }
    bool operator==(const Maybe& rhs) const { return has_ == rhs.has_ && (!has_ || value_ == rhs.value_); }
    bool operator!=(const Maybe& rhs) const { return !(*this == rhs); }
    static Maybe JustValue(T value) { return Maybe(true, std::move(value)); }
    static Maybe NothingValue() { return Maybe(false, T{}); }
private:
    Maybe(bool has, T value) : has_(has), value_(std::move(value)) {}
    bool has_;
    T value_;
};
template<> class Maybe<void> {
public:
    bool IsNothing() const { return !has_; }
    bool IsJust() const { return has_; }
    void FromJust() const { Check(); }
    void ToChecked() const { Check(); }
    void Check() const { if (!has_) std::abort(); }
    bool operator==(Maybe rhs) const { return has_ == rhs.has_; }
    bool operator!=(Maybe rhs) const { return has_ != rhs.has_; }
    static Maybe JustValue() { return Maybe(true); }
    static Maybe NothingValue() { return Maybe(false); }
private:
    explicit Maybe(bool has) : has_(has) {}
    bool has_;
};
inline Maybe<void> JustVoid() { return Maybe<void>::JustValue(); }
template<class T> Maybe<T> Just(T value) { return Maybe<T>::JustValue(std::move(value)); }
template<class T> Maybe<T> Nothing() { return Maybe<T>::NothingValue(); }

class HandleScope {
public:
    explicit HandleScope(Isolate*);
    ~HandleScope();
    HandleScope(const HandleScope&) = delete;
    HandleScope& operator=(const HandleScope&) = delete;
    static int NumberOfHandles(Isolate*);
    Isolate* GetIsolate() const { return isolate_; }
protected:
    detail::Cell* EscapeCell(detail::Cell*);
private:
    Isolate* isolate_;
    HandleScope* parent_ = nullptr;
    detail::Cell* head_ = nullptr;
    friend struct detail::Cell;
    friend class Isolate;
    friend Local<Data> detail::Localize(Isolate*, detail::Cell*);
    friend detail::Cell* detail::AllocateCell(Isolate*, JSContext*, JSValue,
        std::shared_ptr<detail::DataPayload>, detail::ConstructView, detail::DestroyView, unsigned);
};
class EscapableHandleScope : public HandleScope {
public:
    explicit EscapableHandleScope(Isolate* isolate) : HandleScope(isolate) {}
    template<class T> Local<T> Escape(Local<T> value) {
        if (escaped_) std::abort();
        escaped_ = true;
        if (!value.IsEmpty()) EscapeCell(detail::CellOf(*value));
        return value;
    }
    template<class T> MaybeLocal<T> EscapeMaybe(MaybeLocal<T> value) {
        Local<T> local; return value.ToLocal(&local) ? MaybeLocal<T>(Escape(local)) : MaybeLocal<T>();
    }
private:
    bool escaped_ = false;
};

inline constexpr int kInternalFieldsInWeakCallback = 2;
inline constexpr int kEmbedderFieldsInWeakCallback = 2;
enum class WeakCallbackType { kParameter, kInternalFields };
template<class T> class WeakCallbackInfo {
public:
    using Callback = void (*)(const WeakCallbackInfo&);
    WeakCallbackInfo(Isolate* isolate, T* parameter, void** fields, bool second = false)
        : isolate_(isolate), parameter_(parameter), fields_(fields), second_(second) {}
    Isolate* GetIsolate() const { return isolate_; }
    T* GetParameter() const { return parameter_; }
    void* GetInternalField(int index) const { if (index < 0 || index >= 2) std::abort(); return fields_[index]; }
    void SetSecondPassCallback(Callback callback) const { if (second_) std::abort(); second_pass_ = callback; }
    Callback SecondPassCallback() const { return second_pass_; }
private:
    Isolate* isolate_;
    T* parameter_;
    void** fields_;
    bool second_;
    mutable Callback second_pass_ = nullptr;
};

template<class T> class PersistentBase {
public:
    bool IsEmpty() const { return weak_ ? detail::WeakPersistentEmpty(weak_) : cell_ == nullptr; }
    bool IsWeak() const { return weak_ != nullptr; }
    void AnnotateStrongRetainer(const char* label) { retainer_label_ = label ? label : ""; }
    const char* GetStrongRetainerLabelForQuickJS() const { return retainer_label_.c_str(); }
    void Reset() {
        if (weak_) { detail::CancelWeakPersistent(weak_); weak_.reset(); }
        if (cell_) detail::Release(std::exchange(cell_, nullptr));
    }
    void SetWeak() { SetWeak<void>(nullptr, nullptr, WeakCallbackType::kParameter); }
    template<class P> void SetWeak(P* parameter, typename WeakCallbackInfo<P>::Callback callback,
        WeakCallbackType type = WeakCallbackType::kParameter) {
        if (!cell_ || weak_) std::abort();
        detail::WeakCallback erased;
        if (callback) erased = [callback](Isolate* isolate, void* raw, void** fields) -> detail::WeakSecondPass {
            WeakCallbackInfo<P> info(isolate, static_cast<P*>(raw), fields);
            callback(info);
            auto second = info.SecondPassCallback();
            if (!second) return {};
            auto first_field = fields[0], second_field = fields[1];
            return [isolate, raw, second, first_field, second_field] {
                void* saved[2] = {first_field, second_field};
                WeakCallbackInfo<P> info(isolate, static_cast<P*>(raw), saved, true);
                second(info);
            };
        };
        auto weak = detail::MakeWeakPersistent(cell_, parameter, std::move(erased), type == WeakCallbackType::kInternalFields);
        if (!weak) return;
        weak_ = std::move(weak);
        detail::Release(std::exchange(cell_, nullptr));
    }
    template<class P> P* ClearWeak() {
        if (!weak_) return nullptr;
        auto* parameter = static_cast<P*>(detail::WeakParameter(weak_));
        cell_ = detail::StrengthenWeak(weak_);
        detail::CancelWeakPersistent(weak_);
        weak_.reset();
        return parameter;
    }
    template<class S> void Reset(Isolate* isolate, Local<S> value) {
        static_assert(std::is_convertible<S*, T*>::value, "incompatible persistent type");
        detail::CheckIsolate(isolate);
        detail::Cell* next = value.IsEmpty() ? nullptr : detail::CellOf(*value);
        if (next) { if (detail::IsolateOf(*value) != isolate) std::abort(); detail::Retain(next); }
        Reset(); cell_ = next;
    }
    template<class S> void Reset(Isolate* isolate, const PersistentBase<S>& value) { Reset(isolate, value.Get(isolate)); }
    Local<T> Get(Isolate* isolate) const {
        return weak_ ? detail::LocalizeWeak(isolate, weak_).template As<T>()
            : (cell_ ? detail::Localize(isolate, cell_).template As<T>() : Local<T>());
    }
    template<class S> bool operator==(Local<S> rhs) const {
        if (weak_) return Get(detail::WeakIsolate(weak_)) == rhs;
        return detail::Equal(detail::ViewOf(cell_), *rhs);
    }
    template<class S> bool operator==(const PersistentBase<S>& rhs) const {
        if (!weak_ && !rhs.weak_) return detail::Equal(detail::ViewOf(cell_), detail::ViewOf(rhs.cell_));
        auto* isolate = weak_ ? detail::WeakIsolate(weak_)
            : (cell_ ? detail::IsolateOf(detail::ViewOf(cell_)) : (rhs.weak_ ? detail::WeakIsolate(rhs.weak_) : nullptr));
        if (!isolate) return !rhs.cell_;
        return Get(isolate) == rhs.Get(isolate);
    }
    template<class S> bool operator!=(Local<S> rhs) const { return !(*this == rhs); }
protected:
    PersistentBase() = default;
    ~PersistentBase() { if (weak_) detail::CancelWeakPersistent(weak_); }
    detail::Cell* cell_ = nullptr;
    std::shared_ptr<detail::WeakPersistent> weak_;
    std::string retainer_label_;
    template<class> friend class PersistentBase;
};
template<class T> class Global : public PersistentBase<T> {
public:
    Global() = default;
    template<class S> Global(Isolate* isolate, Local<S> value) { this->Reset(isolate, value); }
    ~Global() { this->Reset(); }
    Global(const Global&) = delete;
    Global& operator=(const Global&) = delete;
    Global(Global&& rhs) noexcept {
        this->cell_ = std::exchange(rhs.cell_, nullptr);
        this->weak_ = std::move(rhs.weak_);
        this->retainer_label_ = std::move(rhs.retainer_label_);
    }
    Global& operator=(Global&& rhs) noexcept {
        if (this != &rhs) {
            this->Reset(); this->cell_ = std::exchange(rhs.cell_, nullptr);
            this->weak_ = std::move(rhs.weak_);
            this->retainer_label_ = std::move(rhs.retainer_label_);
        }
        return *this;
    }
    Global Pass() { return std::move(*this); }
};
template<class T> using UniquePersistent = Global<T>;
template<class T> struct NonCopyablePersistentTraits { static constexpr bool kResetInDestructor = false; };
template<class T> struct CopyablePersistentTraits { static constexpr bool kResetInDestructor = true; };
template<class T, class M = NonCopyablePersistentTraits<T>> class Persistent : public PersistentBase<T> {
public:
    Persistent() = default;
    template<class S> Persistent(Isolate* isolate, Local<S> value) { this->Reset(isolate, value); }
    Persistent(const Persistent& rhs) {
        static_assert(M::kResetInDestructor, "noncopyable persistent");
        this->cell_ = rhs.cell_; if (this->cell_) detail::Retain(this->cell_);
        if (rhs.weak_) this->weak_ = detail::CopyWeakPersistent(rhs.weak_);
        this->retainer_label_ = rhs.retainer_label_;
    }
    Persistent& operator=(const Persistent& rhs) {
        static_assert(M::kResetInDestructor, "noncopyable persistent");
        if (this != &rhs) {
            auto* next = rhs.cell_; if (next) detail::Retain(next);
            auto weak = rhs.weak_ ? detail::CopyWeakPersistent(rhs.weak_) : nullptr;
            this->Reset(); this->cell_ = next; this->weak_ = std::move(weak);
            this->retainer_label_ = rhs.retainer_label_;
        }
        return *this;
    }
    ~Persistent() { if constexpr (M::kResetInDestructor) this->Reset(); }
};
template<class T> class Eternal {
public:
    Eternal() = default;
    Eternal(Isolate* isolate, Local<T> value) : value_(isolate, value) {}
    bool IsEmpty() const { return value_.IsEmpty(); }
    void Set(Isolate* isolate, Local<T> value) { if (!IsEmpty()) std::abort(); value_.Reset(isolate, value); }
    Local<T> Get(Isolate* isolate) const { return value_.Get(isolate); }
private:
    Global<T> value_;
};

class TracedReferenceBase {
public:
    constexpr TracedReferenceBase() = default;
    bool IsEmpty() const;
    bool IsEmptyThreadSafe() const { return IsEmpty(); }
    void Reset();
    Local<Data> Get(Isolate*) const;
    void Trace() const;
    template<class S> bool operator==(Local<S> rhs) const {
        return Equals(*rhs);
    }
    bool operator==(const TracedReferenceBase&) const;
    bool operator!=(const TracedReferenceBase& rhs) const { return !(*this == rhs); }
    bool Equals(const Data*) const;
protected:
    void Set(Isolate*, const Data*);
    void Copy(const TracedReferenceBase&);
    void Move(TracedReferenceBase&);
    Isolate* isolate_ = nullptr;
    mini_ui::QuickJSEdgeId edge_ = mini_ui::kInvalidQuickJSEdge;
};
template<class T> class BasicTracedReference : public TracedReferenceBase {
public:
    Local<T> Get(Isolate* isolate) const { return TracedReferenceBase::Get(isolate).template As<T>(); }
};
template<class T> class TracedReference : public BasicTracedReference<T> {
public:
    struct IsDroppable {};
    TracedReference() = default;
    template<class S> TracedReference(Isolate* isolate, Local<S> value) { Reset(isolate, value); }
    template<class S> TracedReference(Isolate* isolate, Local<S> value, IsDroppable) { Reset(isolate, value); }
    TracedReference(const TracedReference& rhs) { this->Copy(rhs); }
    TracedReference(TracedReference&& rhs) noexcept { this->Move(rhs); }
    TracedReference& operator=(const TracedReference& rhs) { if (this != &rhs) this->Copy(rhs); return *this; }
    TracedReference& operator=(TracedReference&& rhs) noexcept { if (this != &rhs) this->Move(rhs); return *this; }
    template<class S> TracedReference(const TracedReference<S>& rhs) { static_assert(std::is_convertible<S*, T*>::value, "incompatible traced type"); this->Copy(rhs); }
    using TracedReferenceBase::Reset;
    template<class S> void Reset(Isolate* isolate, Local<S> value) {
        static_assert(std::is_convertible<S*, T*>::value, "incompatible traced type"); this->Set(isolate, *value);
    }
    template<class S> TracedReference<S>& As() const {
        // The handle slot has a single common base representation; no JS object ABI.
        return reinterpret_cast<TracedReference<S>&>(const_cast<TracedReference&>(*this));
    }
};
static_assert(std::is_trivially_destructible<TracedReference<Value>>::value, "Blink traces explicit-reset slots");

enum class MicrotasksPolicy { kExplicit, kScoped, kAuto };
using MicrotaskCallback = void (*)(void*);
using MicrotasksCompletedCallbackWithData = void (*)(Isolate*, void*);
using MessageCallback = void (*)(Local<Message>, Local<Value>);
class Isolate {
public:
    static Isolate* CreateForQuickJS(JSRuntime* borrowed_runtime);
    void Dispose();
    JSRuntime* GetQuickJSRuntime() const;
    void SetPendingContextForQuickJS(JSContext*);
    void Enter();
    void Exit();
    static Isolate* GetCurrent();
    static Isolate* TryGetCurrent();
    bool IsCurrent() const;
    class Scope {
    public:
        explicit Scope(Isolate* isolate) : isolate_(isolate) { isolate_->Enter(); }
        ~Scope() { isolate_->Exit(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private: Isolate* isolate_;
    };
    void* GetData(uint32_t slot) const;
    void SetData(uint32_t slot, void*);
    static uint32_t GetNumberOfDataSlots() { return 64; }
    Local<Context> GetCurrentContext();
    Local<Context> GetEnteredOrMicrotaskContext();
    Local<Context> GetIncumbentContext();
    Local<Value> GetContinuationPreservedEmbedderData();
    void SetContinuationPreservedEmbedderData(Local<Value>);
    bool InContext() const;
    Local<Value> ThrowException(Local<Value>);
    bool HasPendingException() const;
    enum MessageErrorLevel {
        kMessageLog = 1, kMessageDebug = 2, kMessageInfo = 4,
        kMessageError = 8, kMessageWarning = 16, kMessageAll = 31
    };
    bool AddMessageListener(MessageCallback, Local<Value> data = {});
    bool AddMessageListenerWithErrorLevel(MessageCallback, int levels, Local<Value> data = {});
    void RemoveMessageListeners(MessageCallback);
    int64_t AdjustAmountOfExternalAllocatedMemory(int64_t delta);
    void TerminateExecution();
    void CancelTerminateExecution();
    bool IsExecutionTerminating() const;
    void PerformMicrotaskCheckpoint();
    void EnqueueMicrotask(Local<Function>);
    void EnqueueMicrotask(MicrotaskCallback, void* data = nullptr);
    void SetMicrotasksPolicy(MicrotasksPolicy);
    MicrotasksPolicy GetMicrotasksPolicy() const;
    MicrotaskQueue* GetMicrotaskQueue() const;
    void AddMicrotasksCompletedCallback(MicrotasksCompletedCallbackWithData, void* = nullptr);
    void RemoveMicrotasksCompletedCallback(MicrotasksCompletedCallbackWithData, void* = nullptr);
    void RequestGarbageCollectionForTesting(int);
    void LowMemoryNotification();
    enum GarbageCollectionType { kFullGarbageCollection, kMinorGarbageCollection };
private:
    Isolate();
    ~Isolate();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class HandleScope;
    friend class TracedReferenceBase;
    friend struct detail::Cell;
    friend struct detail::CoreAccess;
    friend void detail::CheckIsolate(Isolate*);
};

class MicrotaskQueue {
public:
    static std::unique_ptr<MicrotaskQueue> New(Isolate*, MicrotasksPolicy = MicrotasksPolicy::kAuto);
    // All facade queues schedule the borrowed runtime's native job FIFO.
    // New owns this adapter; a Context borrows it and must not outlive it.
    virtual ~MicrotaskQueue();
    void EnqueueMicrotask(Isolate*, Local<Function>);
    void EnqueueMicrotask(Isolate*, MicrotaskCallback, void* = nullptr);
    void AddMicrotasksCompletedCallback(MicrotasksCompletedCallbackWithData, void* = nullptr);
    void RemoveMicrotasksCompletedCallback(MicrotasksCompletedCallbackWithData, void* = nullptr);
    void PerformCheckpoint(Isolate*);
    bool IsRunningMicrotasks() const;
    int GetMicrotasksScopeDepth() const;
    Isolate* GetIsolate() const;
    MicrotaskQueue(const MicrotaskQueue&) = delete;
    MicrotaskQueue& operator=(const MicrotaskQueue&) = delete;
private:
    explicit MicrotaskQueue(Isolate* isolate) : isolate_(isolate) {}
    Isolate* isolate_;
    std::vector<std::pair<MicrotasksCompletedCallbackWithData, void*>> completed_callbacks_;
    static void DispatchCompleted(Isolate*, void*);
};
class MicrotasksScope {
public:
    enum Type { kRunMicrotasks, kDoNotRunMicrotasks };
    MicrotasksScope(Isolate*, Type);
    MicrotasksScope(Local<Context>, Type);
    MicrotasksScope(Isolate*, MicrotaskQueue*, Type);
    ~MicrotasksScope();
    MicrotasksScope(const MicrotasksScope&) = delete;
    MicrotasksScope& operator=(const MicrotasksScope&) = delete;
    static void PerformCheckpoint(Isolate*);
    static bool IsRunningMicrotasks(Isolate*);
    static int GetCurrentDepth(Isolate*);
private:
    Isolate* isolate_;
    Type type_;
};



namespace detail {
template<class T> Local<T> MakeLocal(Isolate* isolate, JSContext* context, JSValue owned,
    std::shared_ptr<DataPayload> payload) {
    static_assert(sizeof(T) == sizeof(Data), "API views carry only Data::Cell pointer");
    static_assert(alignof(T) <= alignof(std::max_align_t), "unsupported view alignment");
    if (JS_IsException(owned)) { CaptureException(isolate, context); return {}; }
    constexpr unsigned flags = (std::is_convertible<T*, Value*>::value ? 1u : 0u)
        | (std::is_same<T, Module>::value ? 2u : 0u)
        | (std::is_same<T, FixedArray>::value ? 4u : 0u)
        | (std::is_same<T, Private>::value ? 8u : 0u)
        | (std::is_same<T, ObjectTemplate>::value ? 16u : 0u)
        | (std::is_same<T, FunctionTemplate>::value ? 32u : 0u)
        | (std::is_same<T, Context>::value ? 64u : 0u)
        | ((std::is_same<T, Context>::value || std::is_same<T, Script>::value
            || std::is_same<T, Module>::value || std::is_same<T, UnboundScript>::value
            || std::is_same<T, UnboundModuleScript>::value || std::is_same<T, ScriptOrModule>::value)
            ? kPinnedContext : 0u)
        | ((std::is_same<T, Template>::value || std::is_same<T, ObjectTemplate>::value
            || std::is_same<T, FunctionTemplate>::value || std::is_same<T, Signature>::value
            || std::is_same<T, AccessorSignature>::value || std::is_same<T, DictionaryTemplate>::value
            || std::is_same<T, Private>::value) ? kIsolateData : 0u);
    auto* cell = AllocateCell(isolate, context, owned, std::move(payload),
        ConstructApiView<T>, DestroyApiView<T>, flags);
    return Local<T>(static_cast<T*>(ViewOf(cell)));
}
}
} // namespace v8
#endif
