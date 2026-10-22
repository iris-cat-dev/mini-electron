// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef MINI_UI_RUNTIME_UI_QUICKJS_V8_SCRIPTS_H_
#define MINI_UI_RUNTIME_UI_QUICKJS_V8_SCRIPTS_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <utility>
#include "runtime/ui/quickjs_v8_objects.h"

namespace v8 {
class Script;
class UnboundScript;
class UnboundModuleScript;
class Module;
class Message;
class StackTrace;
class StackFrame;

class ScriptOriginOptions {
public:
    ScriptOriginOptions(bool shared = false, bool opaque = false, bool wasm = false, bool module = false)
        : flags_((shared ? 1 : 0) | (opaque ? 4 : 0) | (wasm ? 2 : 0) | (module ? 8 : 0)) {}
    explicit ScriptOriginOptions(int flags) : flags_(flags & 15) {}
    bool IsSharedCrossOrigin() const { return flags_ & 1; }
    bool IsOpaque() const { return flags_ & 4; }
    bool IsWasm() const { return flags_ & 2; }
    bool IsModule() const { return flags_ & 8; }
    int Flags() const { return flags_; }
private:
    int flags_;
};

// Handle-scope metadata only: compilation chooses its realm from Context/Isolate
// entry, never from a primitive resource-name handle's neutral context.
class ScriptOrigin {
public:
    ScriptOrigin(Local<Value> name, int line = 0, int column = 0, bool shared = false,
        int id = -1, Local<Value> source_map = {}, bool opaque = false, bool wasm = false,
        bool module = false, Local<Data> host_options = {})
        : name_(name), line_(line), column_(column), options_(shared, opaque, wasm, module),
          id_(id), source_map_(source_map), host_options_(host_options) {}
    Local<Value> ResourceName() const { return name_; }
    int LineOffset() const { return line_; }
    int ColumnOffset() const { return column_; }
    int ScriptId() const { return id_; }
    Local<Value> SourceMapUrl() const { return source_map_; }
    Local<Data> GetHostDefinedOptions() const { return host_options_; }
    ScriptOriginOptions Options() const { return options_; }
private:
    Local<Value> name_;
    int line_, column_;
    ScriptOriginOptions options_;
    int id_;
    Local<Value> source_map_;
    Local<Data> host_options_;
};

class Location {
public:
    Location(int line, int column) : line_(line), column_(column) {}
    int GetLineNumber() const { return line_; }
    int GetColumnNumber() const { return column_; }
private:
    int line_, column_;
};

class ScriptOrModule : public Data {
public:
    using Data::Data;
    Local<Value> GetResourceName();
    Local<Data> HostDefinedOptions();
};
class UnboundScript : public Data {
public:
    using Data::Data;
    Local<Script> BindToCurrentContext();
    int GetId() const;
    Local<Value> GetScriptName();
    Local<Value> GetSourceURL();
    Local<Value> GetSourceMappingURL();
    int GetLineNumber(int position = 0);
    int GetColumnNumber(int position = 0);
    static constexpr int kNoScriptId = 0;
};
class UnboundModuleScript : public Data {
public:
    using Data::Data;
    Local<Value> GetSourceURL();
    Local<Value> GetSourceMappingURL();
};
class ModuleRequest : public Data {
public:
    using Data::Data;
    Local<String> GetSpecifier() const;
    int GetSourceOffset() const;
    Local<FixedArray> GetImportAttributes() const;
    static ModuleRequest* Cast(Data* value) { return detail::RebindView<ModuleRequest>(detail::CellOf(value)); }
};
class Module : public Data {
public:
    using Data::Data;
    enum Status { kUninstantiated, kInstantiating, kInstantiated, kEvaluating, kEvaluated, kErrored };
    using ResolveModuleCallback = MaybeLocal<Module> (*)(Local<Context>, Local<String>, Local<FixedArray>, Local<Module>);
    using ResolveSourceCallback = MaybeLocal<Object> (*)(Local<Context>, Local<String>, Local<FixedArray>, Local<Module>);
    using SyntheticModuleEvaluationSteps = MaybeLocal<Value> (*)(Local<Context>, Local<Module>);
    Status GetStatus() const;
    Local<Value> GetException() const;
    Local<FixedArray> GetModuleRequests() const;
    Location SourceOffsetToLocation(int offset) const;
    int GetIdentityHash() const;
    Maybe<bool> InstantiateModule(Local<Context>, ResolveModuleCallback, ResolveSourceCallback = nullptr);
    MaybeLocal<Value> Evaluate(Local<Context>);
    Local<Value> GetModuleNamespace();
    Local<UnboundModuleScript> GetUnboundModuleScript();
    int ScriptId() const;
    bool IsGraphAsync() const;
    bool HasTopLevelAwait() const;
    bool IsSourceTextModule() const;
    bool IsSyntheticModule() const;
    static Local<Module> CreateSyntheticModule(Isolate*, Local<String>, const MemorySpan<const Local<String>>&, SyntheticModuleEvaluationSteps);
    Maybe<bool> SetSyntheticModuleExport(Isolate*, Local<String>, Local<Value>);
    static Module* Cast(Data* value) { return detail::RebindView<Module>(detail::CellOf(value)); }
};
class Script : public Data {
public:
    using Data::Data;
    static MaybeLocal<Script> Compile(Local<Context>, Local<String>, ScriptOrigin* = nullptr);
    MaybeLocal<Value> Run(Local<Context>);
    MaybeLocal<Value> Run(Local<Context>, Local<Data> host_options);
    Local<UnboundScript> GetUnboundScript();
    Local<Value> GetResourceName();
};
class ScriptCompiler {
public:
    enum CompileOptions { kNoCompileOptions = 0, kConsumeCodeCache = 1, kEagerCompile = 2,
        kProduceCompileHints = 4, kConsumeCompileHints = 8, kFollowCompileHintsMagicComment = 16 };
    enum NoCacheReason { kNoCacheNoReason, kNoCacheBecauseCachingDisabled, kNoCacheBecauseNoResource,
        kNoCacheBecauseInlineScript, kNoCacheBecauseModule, kNoCacheBecauseStreamingSource,
        kNoCacheBecauseInspector, kNoCacheBecauseScriptTooSmall, kNoCacheBecauseCacheTooCold,
        kNoCacheBecauseV8Extension, kNoCacheBecauseExtensionModule, kNoCacheBecausePacScript,
        kNoCacheBecauseInDocumentWrite, kNoCacheBecauseResourceWithNoCacheHandler, kNoCacheBecauseDeferredProduceCodeCache };
    class Source {
    public:
        explicit Source(Local<String> source) : source_(source), origin_({}) {}
        Source(Local<String> source, const ScriptOrigin& origin) : source_(source), origin_(origin) {}
        const ScriptOriginOptions GetResourceOptions() const { return origin_.Options(); }
    private:
        friend class ScriptCompiler;
        Local<String> source_;
        ScriptOrigin origin_;
    };
    static MaybeLocal<UnboundScript> CompileUnboundScript(Isolate*, Source*, CompileOptions = kNoCompileOptions, NoCacheReason = kNoCacheNoReason);
    static MaybeLocal<Script> Compile(Local<Context>, Source*, CompileOptions = kNoCompileOptions, NoCacheReason = kNoCacheNoReason);
    static MaybeLocal<Module> CompileModule(Isolate*, Source*, CompileOptions = kNoCompileOptions, NoCacheReason = kNoCacheNoReason);
    static MaybeLocal<Function> CompileFunction(Local<Context>, Source*, size_t = 0, Local<String>[] = nullptr,
        size_t = 0, Local<Object>[] = nullptr, CompileOptions = kNoCompileOptions, NoCacheReason = kNoCacheNoReason);
};

class Promise : public Object {
public:
    using Object::Object;
    enum PromiseState { kPending, kFulfilled, kRejected };
    class Resolver : public Object {
    public:
        using Object::Object;
        static MaybeLocal<Resolver> New(Local<Context>);
        Local<Promise> GetPromise();
        Maybe<bool> Resolve(Local<Context>, Local<Value>);
        Maybe<bool> Reject(Local<Context>, Local<Value>);
        static Resolver* Cast(Value* value) { return detail::RebindView<Resolver>(detail::CellOf(value)); }
    };
    MaybeLocal<Promise> Catch(Local<Context>, Local<Function>);
    MaybeLocal<Promise> Then(Local<Context>, Local<Function>);
    MaybeLocal<Promise> Then(Local<Context>, Local<Function>, Local<Function>);
    bool HasHandler() const;
    Local<Value> Result();
    PromiseState State();
    void MarkAsHandled();
    static Promise* Cast(Value* value) { return detail::RebindView<Promise>(detail::CellOf(value)); }
    static constexpr int kEmbedderFieldCount = 0;
};
enum PromiseRejectEvent {
    kPromiseRejectWithNoHandler = 0,
    kPromiseHandlerAddedAfterReject = 1,
    kPromiseRejectAfterResolved = 2,
    kPromiseResolveAfterResolved = 3
};
class PromiseRejectMessage {
public:
    PromiseRejectMessage(Local<Promise> promise, PromiseRejectEvent event, Local<Value> value)
        : promise_(promise), event_(event), value_(value) {}
    Local<Promise> GetPromise() const { return promise_; }
    PromiseRejectEvent GetEvent() const { return event_; }
    Local<Value> GetValue() const { return value_; }
private:
    Local<Promise> promise_;
    PromiseRejectEvent event_;
    Local<Value> value_;
};
using PromiseRejectCallback = void (*)(PromiseRejectMessage);
class JSON {
public:
    static MaybeLocal<Value> Parse(Local<Context>, Local<String>);
    static MaybeLocal<String> Stringify(Local<Context>, Local<Value>, Local<String> gap = {});
};

class StackFrame : public Data {
public:
    using Data::Data;
    Location GetLocation() const;
    int GetLineNumber() const { return GetLocation().GetLineNumber() + 1; }
    int GetColumn() const { return GetLocation().GetColumnNumber() + 1; }
    int GetScriptId() const;
    Local<String> GetScriptName() const;
    Local<String> GetScriptNameOrSourceURL() const;
    Local<String> GetScriptSource() const;
    Local<String> GetScriptSourceMappingURL() const;
    Local<String> GetFunctionName() const;
    bool IsEval() const;
    bool IsConstructor() const;
    bool IsWasm() const { return false; }
    bool IsUserJavaScript() const;
};
class StackTrace : public Data {
public:
    using Data::Data;
    enum StackTraceOptions { kLineNumber = 1, kColumnOffset = 3, kScriptName = 4, kFunctionName = 8,
        kIsEval = 16, kIsConstructor = 32, kScriptNameOrSourceURL = 64, kScriptId = 128,
        kExposeFramesAcrossSecurityOrigins = 256, kOverview = 15, kDetailed = 127 };
    int GetID() const;
    Local<StackFrame> GetFrame(Isolate*, uint32_t) const;
    int GetFrameCount() const;
    static Local<StackTrace> CurrentStackTrace(Isolate*, int, StackTraceOptions = kDetailed);
    static Local<String> CurrentScriptNameOrSourceURL(Isolate*);
};
class Message : public Data {
public:
    using Data::Data;
    Local<String> Get() const;
    Isolate* GetIsolate() const;
    MaybeLocal<String> GetSource(Local<Context>) const;
    MaybeLocal<String> GetSourceLine(Local<Context>) const;
    ScriptOrigin GetScriptOrigin() const;
    Local<Value> GetScriptResourceName() const;
    Local<StackTrace> GetStackTrace() const;
    Maybe<int> GetLineNumber(Local<Context>) const;
    int GetStartPosition() const;
    int GetEndPosition() const;
    int GetWasmFunctionIndex() const { return -1; }
    int ErrorLevel() const;
    int GetStartColumn() const;
    Maybe<int> GetStartColumn(Local<Context>) const;
    int GetEndColumn() const;
    Maybe<int> GetEndColumn(Local<Context>) const;
    bool IsSharedCrossOrigin() const;
    bool IsOpaque() const;
    static void PrintCurrentStackTrace(Isolate*, std::ostream&);
    static constexpr int kNoLineNumberInfo = 0, kNoColumnInfo = 0, kNoScriptIdInfo = 0, kNoWasmFunctionIndexInfo = -1;
};
class Exception {
public:
    static Local<Value> RangeError(Local<String>, Local<Value> = {});
    static Local<Value> ReferenceError(Local<String>, Local<Value> = {});
    static Local<Value> SyntaxError(Local<String>, Local<Value> = {});
    static Local<Value> TypeError(Local<String>, Local<Value> = {});
    static Local<Value> Error(Local<String>, Local<Value> = {});
    static Local<Message> CreateMessage(Isolate*, Local<Value>);
    static Local<StackTrace> GetStackTrace(Local<Value>);
    static Maybe<bool> CaptureStackTrace(Local<Context>, Local<Object>);
};
enum class ExceptionContext : uint32_t {
    kUnknown, kConstructor, kOperation, kAttributeGet, kAttributeSet,
    kIndexedQuery, kIndexedGetter, kIndexedDescriptor, kIndexedSetter,
    kIndexedDefiner, kIndexedDeleter, kNamedQuery, kNamedGetter,
    kNamedDescriptor, kNamedSetter, kNamedDefiner, kNamedDeleter, kNamedEnumerator
};
class ExceptionPropagationMessage {
public:
    ExceptionPropagationMessage(Isolate* isolate, Local<Object> exception,
        Local<String> interface_name, Local<String> property_name, ExceptionContext context)
        : isolate_(isolate), exception_(exception), interface_(interface_name),
          property_(property_name), context_(context) {}
    Isolate* GetIsolate() const { return isolate_; }
    Local<Object> GetException() const { return exception_; }
    Local<String> GetInterfaceName() const { return interface_; }
    Local<String> GetPropertyName() const { return property_; }
    ExceptionContext GetExceptionContext() const { return context_; }
private:
    Isolate* isolate_;
    Local<Object> exception_;
    Local<String> interface_, property_;
    ExceptionContext context_;
};
using ExceptionPropagationCallback = void (*)(ExceptionPropagationMessage);
class TryCatch {
public:
    explicit TryCatch(Isolate*);
    ~TryCatch();
    bool HasCaught() const;
    bool CanContinue() const;
    bool HasTerminated() const;
    Local<Value> ReThrow();
    Local<Value> Exception() const;
    static MaybeLocal<Value> StackTrace(Local<Context>, Local<Value>);
    MaybeLocal<Value> StackTrace(Local<Context>) const;
    Local<v8::Message> Message() const;
    void Reset();
    void SetVerbose(bool value) { verbose_ = value; }
    bool IsVerbose() const { return verbose_; }
    void SetCaptureMessage(bool value) { capture_message_ = value; }
    TryCatch(const TryCatch&) = delete;
    TryCatch& operator=(const TryCatch&) = delete;
    // Called only by Core's exception stack, with a borrowed exception value.
    void Capture(JSContext*, JSValueConst, bool terminated = false);
private:
    Isolate* isolate_;
    // Explicit stack-scoped catch lifetime, not an implicit Value Cell root.
    JSContext* context_ = nullptr;
    JSValue exception_ = JS_UNDEFINED;
    bool caught_ = false, terminated_ = false, rethrow_ = false;
    bool verbose_ = false, capture_message_ = true;
    detail::ExceptionMetadata metadata_;
    std::shared_ptr<detail::DataPayload> message_payload_;
};

class ValueSerializer {
public:
    class Delegate {
    public:
        virtual ~Delegate() = default;
        virtual void ThrowDataCloneError(Local<String>) = 0;
        virtual bool HasCustomHostObject(Isolate*);
        virtual Maybe<bool> IsHostObject(Isolate*, Local<Object>);
        virtual Maybe<bool> WriteHostObject(Isolate*, Local<Object>);
        virtual Maybe<uint32_t> GetSharedArrayBufferId(Isolate*, Local<SharedArrayBuffer>);
        virtual void* ReallocateBufferMemory(void*, size_t, size_t*);
        virtual void FreeBufferMemory(void*);
    };
    explicit ValueSerializer(Isolate*);
    ValueSerializer(Isolate*, Delegate*);
    ~ValueSerializer();
    void WriteHeader();
    Maybe<bool> WriteValue(Local<Context>, Local<Value>);
    std::pair<uint8_t*, size_t> Release();
    void TransferArrayBuffer(uint32_t, Local<ArrayBuffer>);
    void SetTreatArrayBufferViewsAsHostObjects(bool);
    void WriteUint32(uint32_t);
    void WriteUint64(uint64_t);
    void WriteDouble(double);
    void WriteRawBytes(const void*, size_t);
    ValueSerializer(const ValueSerializer&) = delete;
    ValueSerializer& operator=(const ValueSerializer&) = delete;
private:
    struct PrivateData;
    std::unique_ptr<PrivateData> private_;
};
class ValueDeserializer {
public:
    class Delegate {
    public:
        virtual ~Delegate() = default;
        virtual MaybeLocal<Object> ReadHostObject(Isolate*);
        virtual MaybeLocal<SharedArrayBuffer> GetSharedArrayBufferFromId(Isolate*, uint32_t);
    };
    ValueDeserializer(Isolate*, const uint8_t*, size_t);
    ValueDeserializer(Isolate*, const uint8_t*, size_t, Delegate*);
    ~ValueDeserializer();
    Maybe<bool> ReadHeader(Local<Context>);
    MaybeLocal<Value> ReadValue(Local<Context>);
    void TransferArrayBuffer(uint32_t, Local<ArrayBuffer>);
    void TransferSharedArrayBuffer(uint32_t, Local<SharedArrayBuffer>);
    void SetSupportsLegacyWireFormat(bool);
    uint32_t GetWireFormatVersion() const;
    bool ReadUint32(uint32_t*);
    bool ReadUint64(uint64_t*);
    bool ReadDouble(double*);
    bool ReadRawBytes(size_t, const void**);
    ValueDeserializer(const ValueDeserializer&) = delete;
    ValueDeserializer& operator=(const ValueDeserializer&) = delete;
private:
    struct PrivateData;
    std::unique_ptr<PrivateData> private_;
};
namespace detail {
void ForgetScriptContext(JSContext*);
// Called by ScriptRuntime's observer, not installed as a replacement tracker.
// Handles and entered context remain valid for the synchronous native reporter.
// False means the realm is not attached to this facade or no reporter exists.
bool ForwardPromiseRejection(JSContext*, JSValueConst promise, JSValueConst reason,
    bool handled, PromiseRejectCallback reporter);
}
} // namespace v8
#endif
