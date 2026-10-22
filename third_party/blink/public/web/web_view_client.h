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

#ifndef THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_VIEW_CLIENT_H_
#define THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_VIEW_CLIENT_H_

#include "ui/gfx/geometry/size.h"
#if defined(MINI_UI_QUICKJS)
#include "ui/gfx/geometry/rect.h"
#include "ui/base/ime/text_input_mode.h"
#include "ui/base/ime/text_input_type.h"
namespace ui {
class Cursor;
}
#endif

namespace blink {

#if defined(MINI_UI_QUICKJS)
class WebPagePopup;
class WebString;
#endif

class WebViewClient {
public:
    virtual ~WebViewClient() = default;
#if defined(MINI_UI_QUICKJS)
    // The popup is borrowed and paints through its real software WebWidget API.
    // Bounds are screen DIPs; apply accepted bounds using SetScreenRects/Resize.
    virtual void DidOpenNonCompositedPopup(WebPagePopup*, const gfx::Rect& bounds) = 0;
    virtual void DidChangeNonCompositedPopupBounds(WebPagePopup*, const gfx::Rect& bounds) = 0;
    // Invalidates the borrowed popup pointer before native ownership is dropped.
    virtual void DidCloseNonCompositedPopup(WebPagePopup*) = 0;

    // Native requests return false when no host or suitable native target exists,
    // or when the platform operation fails. DidFocus() remains a notification.
    virtual bool RequestFocus() = 0;
    virtual bool CanTakeFocus(bool reverse) const = 0;
    virtual bool RequestTakeFocus(bool reverse) = 0;
    // Requests host-managed closing; must not destroy a borrowed native window.
    virtual bool RequestClose() = 0;
    // Applies the cursor to the borrowed native host, including custom pixels.
    // Offscreen views or failed platform requests return false.
    virtual bool RequestSetCursor(const ui::Cursor&) = 0;
    // Updates the host's input scope and default IME composition/candidate
    // position. Caret bounds are relative to the view in DIPs.
    virtual bool RequestTextInputState(ui::TextInputType, ui::TextInputMode,
        const gfx::Rect& caret_bounds) = 0;
    virtual bool RequestTooltip(const WebString&, const gfx::Rect& bounds,
        bool keyboard, bool right_to_left) = 0;
    virtual bool RequestClearKeyboardTooltip() = 0;

    // Screen DIPs: outer window bounds and the view's client bounds respectively.
    // Offscreen views report their known viewport, not another native window.
    virtual gfx::Rect WindowRect() const = 0;
    virtual gfx::Rect ViewRect() const = 0;
    // Attempts the requested outer bounds synchronously. Refresh the real widget
    // screen/client rects before returning, even if the request cannot be applied.
    virtual bool RequestSetWindowRect(const gfx::Rect& bounds) = 0;
#endif

    // Factory methods -----------------------------------------------------

    // Misc ----------------------------------------------------------------

    // Called when a region of the WebView needs to be re-painted. This is only
    // for non-composited WebViews that exist to contribute to a "parent" WebView
    // painting. Otherwise invalidations are transmitted to the compositor through
    // the layers.
    virtual void InvalidateContainer()
    {
    }

    // UI ------------------------------------------------------------------

    // Called when the View has changed size as a result of an auto-resize.
    virtual void DidAutoResize(const gfx::Size& new_size)
    {
    }

    // Called when the View acquires focus.
    virtual void DidFocus()
    {
    }

    // Called when the WebView is destroying itself.
    virtual void OnDestruct()
    {
    }
};

} // namespace blink

#endif // THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_VIEW_CLIENT_H_
