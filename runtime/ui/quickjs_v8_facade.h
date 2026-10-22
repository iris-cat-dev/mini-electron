// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef MINI_UI_RUNTIME_UI_QUICKJS_V8_FACADE_H_
#define MINI_UI_RUNTIME_UI_QUICKJS_V8_FACADE_H_

#include "runtime/ui/quickjs_v8_core.h"
#include "runtime/ui/quickjs_v8_values.h"
#include "runtime/ui/quickjs_v8_objects.h"
#include "runtime/ui/quickjs_v8_scripts.h"
#include "v8/include/cppgc/visitor.h"

namespace cppgc {
// Standalone cppgc visitors need no V8 visitor downcast. The edge registry
// participates in QuickJS's external-edge SCAN/MARK/SWEEP protocol.
template<class T> struct TraceTrait<v8::TracedReference<T>> {
    static TraceDescriptor GetTraceDescriptor(const void*) { return {nullptr, Trace}; }
    static void Trace(Visitor*, const void* self) {
        static_cast<const v8::TracedReference<T>*>(self)->Trace();
    }
};
} // namespace cppgc
#endif
