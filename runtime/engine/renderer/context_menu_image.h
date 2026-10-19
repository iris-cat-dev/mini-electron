// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef MINI_ELECTRON_RUNTIME_ENGINE_RENDERER_CONTEXT_MENU_IMAGE_H_
#define MINI_ELECTRON_RUNTIME_ENGINE_RENDERER_CONTEXT_MENU_IMAGE_H_

#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/frame/visual_viewport.h"
#include "third_party/blink/renderer/core/input/event_handler.h"
#include "third_party/blink/renderer/core/layout/hit_test_location.h"
#include "third_party/blink/renderer/core/layout/hit_test_result.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/core/loader/resource/image_resource_content.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/core/style/fill_layer.h"
#include "third_party/blink/renderer/core/style/style_image.h"
#include "third_party/blink/renderer/platform/graphics/image.h"
#include "third_party/blink/renderer/core/page/page.h"

namespace content {

struct ContextMenuBackgroundImage {
    scoped_refptr<blink::Image> image;
    blink::KURL url;
};

// RN Web images can paint a URL background while their sizing <img> is hidden.
// Share the same native hit test between the menu and Copy Image. Only inspect
// the frontmost background layer of the hit node; never walk through another
// element, a generated gradient, or a frame boundary to expose a hidden image.
inline ContextMenuBackgroundImage GetContextMenuBackgroundImage(
    blink::WebLocalFrame& frame, const gfx::Point& point_in_blink)
{
    blink::LocalFrame* local_frame =
        blink::LocalFrame::FromFrameToken(frame.GetLocalFrameToken());
    if (!local_frame || !local_frame->View() || !local_frame->GetPage())
        return {};
    const gfx::Point root_point(
        local_frame->GetPage()->GetVisualViewport().ViewportToRootFrame(point_in_blink));
    const blink::HitTestLocation location(local_frame->View()->ConvertFromRootFrame(root_point));
    blink::HitTestResult result = local_frame->GetEventHandler().HitTestResultAtLocation(location);
    result.SetToShadowHostIfInUAShadowRoot();
    if (!result.AbsoluteImageURL().IsEmpty()
        || !result.AbsoluteMediaURL().IsEmpty())
        return {};
    blink::Node* node = result.InnerNodeOrImageMapImage();
    const blink::LayoutObject* layout = node ? node->GetLayoutObject() : nullptr;
    if (!layout || layout->IsCanvas() || layout->IsImage() || layout->IsSVGImage())
        return {};
    const blink::StyleImage* background =
        layout->StyleRef().BackgroundLayers().GetImage();
    const blink::ImageResourceContent* resource =
        background ? background->CachedImage() : nullptr;
    if (!resource || resource->ErrorOccurred())
        return {};
    blink::Image* image = resource->GetImage();
    if (!image || image->IsNull())
        return {};
    return { image, resource->Url() };
}

} // namespace content

#endif // MINI_ELECTRON_RUNTIME_ENGINE_RENDERER_CONTEXT_MENU_IMAGE_H_
