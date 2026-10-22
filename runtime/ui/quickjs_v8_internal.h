// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef MINI_UI_RUNTIME_UI_QUICKJS_V8_INTERNAL_H_
#define MINI_UI_RUNTIME_UI_QUICKJS_V8_INTERNAL_H_
#include "runtime/ui/quickjs_v8_objects.h"


namespace v8::detail {
enum class PrimitiveRoot : unsigned { kUndefined, kNull, kTrue, kFalse };
Data* ReadOnlyPrimitiveRoot(Isolate*, PrimitiveRoot);
mini_ui::QuickJSEdgeId RegisterModuleEdge(Isolate*, JSContext*, JSModuleDef*);
JSContext* ModuleEdgeContext(Isolate*, mini_ui::QuickJSEdgeId);
Local<Module> ModuleView(JSContext*, JSModuleDef*);
void SetBuiltinWrapper(Isolate*, JSValueConst, void*, CppHeapPointerTag);
void* GetBuiltinWrapper(Isolate*, JSValueConst, CppHeapPointerTagRange);
// A host safe point must not interrupt JS, callback entry, weak notification,
// or a microtask checkpoint. Native stack roots remain conservative.
bool CanCollectAtHostIdle(Isolate*);
void EnterNativeCallback(Isolate*);
void LeaveNativeCallback(Isolate*);
std::size_t HandleArenaAllocatedBytes(Isolate*);
struct HandleArenaStats {
    std::size_t allocated_bytes;
    std::size_t cell_capacity;
    std::size_t view_capacity;
    std::size_t live_cells;
};
HandleArenaStats GetHandleArenaStats(Isolate*);
void TrimHandleArenas(Isolate*);

// The facade owns this representation; no field describes a V8 heap object.
struct ApiView {
    alignas(std::max_align_t) unsigned char storage[sizeof(Data)];
    ConstructView construct = nullptr;
    DestroyView destroy = nullptr;
    Data* view = nullptr;
    ApiView* next = nullptr;
};
struct Cell {
    Isolate* isolate = nullptr;
    // Owned only for explicitly realm-bound Context/compiled metadata.
    // Objects use their VM creation-context edge; primitives remain neutral.
    JSContext* context = nullptr;
    JSValue value = JS_UNDEFINED;
    std::shared_ptr<DataPayload> payload;
    ApiView primary;
    ApiView* extra_views = nullptr;
    std::size_t owners = 0;
    unsigned type_flags = 0;
    bool read_only_root = false;
    HandleScope* scope = nullptr;
    Cell* previous = nullptr;
    Cell* next = nullptr;
    Cell* free_next = nullptr;
};
} // namespace v8::detail
#endif
