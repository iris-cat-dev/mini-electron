
#ifndef content_browser_LocalMainFrameHostImpl_h
#define content_browser_LocalMainFrameHostImpl_h

#include "third_party/blink/public/mojom/frame/frame.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/text_autosizer_page_info.mojom-blink-forward.h"
#include "third_party/blink/public/mojom/frame/text_autosizer_page_info.mojom-blink.h"
#include "third_party/blink/public/mojom/window_features/window_features.mojom-blink.h"
#include "third_party/blink/public/mojom/page/draggable_region.mojom-blink.h"
#include "runtime/engine/common/common.h"
#include "base/functional/callback.h"
#include "base/values.h"

namespace content {

class WebLocalFrameClientImpl;
void requestNativeWindowCommand(WebLocalFrameClientImpl* frame_client,
    base::Value::Dict command, base::OnceCallback<void(bool)> reply);

class LocalMainFrameHostImpl : public ::blink::mojom::blink::LocalMainFrameHost {
public:
    LocalMainFrameHostImpl(WebLocalFrameClientImpl* frameClient);

    virtual void ScaleFactorChanged(float scale) override
    {
        // Blink has applied the page scale. There is no browser zoom indicator;
        // do not feed this back as a native display/device scale change.
    }

    virtual void ContentsPreferredSizeChanged(const ::gfx::Size& pref_size) override
    {
        // Native window bounds are application-owned; this embedder has not
        // opted into content-size-driven automatic window resizing.
    }

    virtual void TextAutosizerPageInfoChanged(::blink::mojom::blink::TextAutosizerPageInfoPtr page_info) override
    {
        // Text autosizing already runs in the local Page. The IPC exists to
        // replicate its metrics to other renderers; all child frames are local.
    }

    virtual void FocusPage() override;
    virtual void TakeFocus(bool reverse) override;
    virtual void UpdateTargetURL(const ::blink::KURL& url, UpdateTargetURLCallback callback) override;

    virtual void RequestClose() override;

    virtual void ShowCreatedWindow(const ::blink::LocalFrameToken& opener_frame_token, ::ui::mojom::blink::WindowOpenDisposition disposition,
        ::blink::mojom::blink::WindowFeaturesPtr window_features, bool opened_by_user_gesture, ShowCreatedWindowCallback callback) override;
    virtual void SetWindowRect(const ::gfx::Rect& bounds, SetWindowRectCallback callback) override;

    virtual void DidFirstVisuallyNonEmptyPaint() override;

    virtual void DidAccessInitialMainDocument() override
    {
        // There is no pending-navigation address bar to invalidate. Exposed
        // WebContents URLs come from committed-frame navigation notifications.
    }

    virtual void Maximize() override;
    virtual void Minimize() override;
    virtual void Restore() override;
    virtual void SetResizable(bool resizable) override;

    virtual void DraggableRegionsChanged(WTF::Vector<::blink::mojom::blink::DraggableRegionPtr> regions) override;

    virtual void OnFirstContentfulPaint() override
    {
        // Paint presentation/ready-to-show is driven by the compositor frame
        // callback, not this optional browser paint-metrics notification.
    }

private:
    WebLocalFrameClientImpl* m_frameClient = nullptr;
};

}

#endif