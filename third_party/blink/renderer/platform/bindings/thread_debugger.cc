// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/bindings/thread_debugger.h"

#include "third_party/blink/renderer/platform/bindings/v8_per_isolate_data.h"

namespace blink {

// static
ThreadDebugger* ThreadDebugger::From(v8::Isolate* isolate)
{
    if (!isolate)
        return nullptr;
    V8PerIsolateData* data = V8PerIsolateData::From(isolate);
    return data ? data->GetThreadDebugger() : nullptr;
}

// static
void ThreadDebugger::IdleStarted(v8::Isolate* isolate)
{
    if (ThreadDebugger* debugger = ThreadDebugger::From(isolate)) {
        if (v8_inspector::V8Inspector* inspector = debugger->GetV8Inspector())
            inspector->idleStarted();
    }
}

// static
void ThreadDebugger::IdleFinished(v8::Isolate* isolate)
{
    if (ThreadDebugger* debugger = ThreadDebugger::From(isolate)) {
        if (v8_inspector::V8Inspector* inspector = debugger->GetV8Inspector())
            inspector->idleFinished();
    }
}

} // namespace blink
