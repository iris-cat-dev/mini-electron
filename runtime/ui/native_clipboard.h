// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef MINI_UI_RUNTIME_UI_NATIVE_CLIPBOARD_H_
#define MINI_UI_RUNTIME_UI_NATIVE_CLIPBOARD_H_

#include <memory>

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/browser_interface_broker.mojom.h"

namespace mini_ui {

class LocalResources;

// Each binding is one document's capability group. Closing its broker pipe
// closes its ClipboardHost pipes and cancels their uncommitted writes. Both
// interfaces dispatch on one process-stable native UI/STA thread, independently
// of the renderer sequence. Call on the native UI thread after Blink/Mojo
// initialization, outside DllMain/loader lock.
void BindLocalBrowserInterfaceBroker(
    mojo::PendingReceiver<blink::mojom::BrowserInterfaceBroker> receiver,
    std::shared_ptr<LocalResources> resources);

// Terminal and idempotent; does not start a service that was never bound.
// Call on the binding UI thread immediately after the BlinkProcess destructor's
// thread check, before isolate/heap/scheduler/Mojo teardown. Closes native groups,
// cancels pending writers and destroys ClipboardWin, then stops/joins and
// releases the native thread and runner without a caller-runner task.
void ShutdownNativeClipboard();

} // namespace mini_ui

#endif // MINI_UI_RUNTIME_UI_NATIVE_CLIPBOARD_H_
