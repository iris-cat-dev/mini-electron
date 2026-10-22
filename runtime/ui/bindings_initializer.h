// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef MINI_UI_BINDINGS_INITIALIZER_H_
#define MINI_UI_BINDINGS_INITIALIZER_H_

#include <memory>

#include "base/files/file.h"
#include "mini_ui/ui.h"

extern "C" {
#include "third_party/quickjs/quickjs.h"
}

namespace v8 {
class Isolate;
}

namespace blink {
class MojoBindingContext;
class Platform;
class WebLocalFrame;
namespace scheduler {
class WebThreadScheduler;
}
}

namespace mini_ui {
class LocalResources;
class ScriptRealm;

// The platform/heap portion precedes construction of Blink's main scheduler.
void PrepareBlinkForQuickJS();
// Completes native Blink and retained-interface initialization; no realm or
// JavaScript isolate is created by this call.
void InitializeBlinkForQuickJS(blink::Platform*,
    blink::scheduler::WebThreadScheduler*);
// Borrows the backend's facade isolate; the backend retains runtime ownership.
void InitializeQuickJSBlinkIsolate(v8::Isolate*);
// Requires every frame and document realm closed. Releases caches and callbacks,
// empties the bridge, then terminates the native heap while facade/runtime live.
// The backend unregisters and disposes the facade only after this returns.
void ShutdownQuickJSBlinkIsolate(v8::Isolate*);
// Installs the real main-world Window/Document bindings into the given realm.
// Must precede script execution for this frame/document.
bool BindQuickJSRealm(blink::WebLocalFrame&, ScriptRealm&, Error*);
// Uses only the provided context's successfully bound local-root capability.
// No entered/current realm fallback; unavailable or destroyed contexts fail.
bool GetLocalFileInfo(const blink::MojoBindingContext&, const base::FilePath&,
    base::File::Info*);
// Copies only the supplied live context's native grant, never GC/JS metadata.
std::shared_ptr<LocalResources> GetLocalResources(const blink::MojoBindingContext&);
// ScriptRuntime observer; raw values are borrowed for the duration of the call.
void NotifyQuickJSPromiseRejection(JSContext*, JSValueConst promise,
    JSValueConst reason, bool handled, void* isolate) noexcept;
void InitializeRetainedModules();

} // namespace mini_ui

#endif // MINI_UI_BINDINGS_INITIALIZER_H_
