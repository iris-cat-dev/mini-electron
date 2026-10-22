/*
 * Copyright (C) 2009 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_NON_COMPOSITED_WIDGET_CLIENT_H_
#define THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_NON_COMPOSITED_WIDGET_CLIENT_H_

#include <utility>

#include "base/functional/callback.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "third_party/blink/public/mojom/input/pointer_lock_context.mojom-blink-forward.h"
#include "third_party/blink/public/mojom/input/pointer_lock_result.mojom-blink.h"

#if defined(MINI_UI_QUICKJS)
namespace gfx {
class Rect;
}
#endif

namespace blink {

#if defined(MINI_UI_QUICKJS)
class WebPagePopup;
#endif

class WebNonCompositedWidgetClient {
public:
    virtual ~WebNonCompositedWidgetClient() = default;

    using PointerLockRequestCallback = base::OnceCallback<void(
        mojom::blink::PointerLockResult, mojo::PendingRemote<mojom::blink::PointerLockContext>)>;

    // A successful native host supplies a real context pipe. Closing either end
    // releases the native lock; this is independent of Widget/presentation IPC.
    virtual void RequestNonCompositedPointerLock(bool has_user_activation,
        bool unadjusted_movement, PointerLockRequestCallback callback)
    {
        std::move(callback).Run(mojom::blink::PointerLockResult::kUnsupportedOptions, mojo::NullRemote());
    }

    // Called to request a BeginMainFrame from the compositor. This is only
    // called for widget's WebViews that do not composite.
    virtual void ScheduleNonCompositedAnimation()
    {
    }

#if defined(MINI_UI_QUICKJS)
    // The popup is borrowed and paints through its real software WebWidget API.
    // Bounds are screen DIPs; apply accepted bounds using SetScreenRects/Resize.
    virtual void DidOpenNonCompositedPopup(WebPagePopup*, const gfx::Rect& bounds) = 0;
    virtual void DidChangeNonCompositedPopupBounds(WebPagePopup*, const gfx::Rect& bounds) = 0;
    // Invalidates the borrowed popup pointer before native ownership is dropped.
    virtual void DidCloseNonCompositedPopup(WebPagePopup*) = 0;
#endif
};

} // namespace blink

#endif // THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_NON_COMPOSITED_WIDGET_CLIENT_H_
