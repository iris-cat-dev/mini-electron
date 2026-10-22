// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef MINI_UI_RUNTIME_UI_QUICKJS_BINDINGS_H_
#define MINI_UI_RUNTIME_UI_QUICKJS_BINDINGS_H_

#include <cstddef>
#include <cstdint>

#include "third_party/quickjs/quickjs.h"
#include "v8/include/cppgc/common.h"
#include "v8/include/cppgc/heap-handle.h"

namespace v8 {
class Isolate;
}

namespace blink {
class ScriptWrappable;
}

namespace mini_ui {

// Opaque generation-tagged handle: high 32 bits generation, low 32 bits slot+1.
// Released slots are reused; exhausted generations retire rather than wrap.
using QuickJSEdgeId = std::uint64_t;
inline constexpr QuickJSEdgeId kInvalidQuickJSEdge = 0;

// Internal counters used by lifetime tests. They intentionally expose no object
// addresses and are not production telemetry.
struct QuickJSGCStats {
    std::size_t wrapper_count = 0;
    std::size_t edge_count = 0;
    std::size_t live_wrapper_count = 0;
    std::size_t live_edge_count = 0;
    std::uint64_t epoch = 0;
};

// A facade isolate is explicitly associated with its QuickJS runtime. These
// calls do not inspect, reinterpret, or depend on a V8 runtime object layout.
void RegisterQuickJSIsolate(v8::Isolate*, JSRuntime*);
void UnregisterQuickJSIsolate(v8::Isolate*);

// Called by Blink ThreadState's MINI_UI_QUICKJS attachment path.
void AttachQuickJSCppgcHeap(v8::Isolate*, cppgc::HeapHandle&);
void DetachQuickJSCppgcHeap(v8::Isolate*);
// Drop cross-heap records before a native heap destroys its objects/pages.
void DetachQuickJSCppgcHeap(cppgc::HeapHandle&);
// Independent native collections need the same pending-activity refresh.
void RecomputeQuickJSActiveWrappables(cppgc::HeapHandle&);
// Coordinate the JS joint-root scan with the caller's native stack contract.
// Ordinary collections must use kMayContainHeapPointers.
void CollectQuickJSGarbage(v8::Isolate*, cppgc::EmbedderStackState);

// Native-to-JS edges are reference-counted by QuickJS outside a collection.
// During cycle collection they participate in the three-phase external-edge
// protocol implemented by QuickJSRuntime.
// Attached Blink heaps refresh ActiveScriptWrappable pending-activity roots at
// each joint collection's start, before JS trial marking and native root scans.
// All bridge access and native tracing must run stop-the-world on the owning
// Blink UI thread; this registry does not support concurrent tracing.
QuickJSEdgeId RegisterQuickJSStrongEdge(v8::Isolate*, JSValueConst);
void ResetQuickJSStrongEdge(v8::Isolate*, QuickJSEdgeId);
// Liveness is independent of the stored value: a live edge can hold undefined.
bool IsQuickJSStrongEdgeAlive(v8::Isolate*, QuickJSEdgeId);
JSValueConst GetQuickJSStrongEdge(v8::Isolate*, QuickJSEdgeId);
void TraceQuickJSStrongEdge(v8::Isolate*, QuickJSEdgeId);

// The source-level facade calls these when Object::Wrap associates a Blink
// native object and when QuickJS scans a reachable wrapper object.
// Registration roots the native object across independent native collections.
// Joint GC drops bridge-owned roots before scanning real host roots, restoring
// only proven-live wrappers afterward; clearing a wrapper releases its root.
void RegisterQuickJSWrapper(v8::Isolate*, JSValueConst,
    blink::ScriptWrappable*);
// Exact native identity only; unknown values and nonwrappers return nullptr.
const void* GetQuickJSWrapperTypeInfo(v8::Isolate*, JSValueConst);
void ClearQuickJSWrapper(v8::Isolate*, JSValueConst);
// The actual child-edge marker distinguishes live tracing from the collector's
// separate physical-reference restoration pass over dead cycles.
void MarkQuickJSWrapper(v8::Isolate*, JSValueConst, JS_MarkFunc*);

QuickJSGCStats GetQuickJSGCStats(v8::Isolate*);

} // namespace mini_ui

#endif // MINI_UI_RUNTIME_UI_QUICKJS_BINDINGS_H_
