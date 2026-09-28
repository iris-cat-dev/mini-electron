// ui/gfx/mojom/ca_layer_result.mojom-shared.h is generated from
// ui/gfx/mojom/ca_layer_result.mojom for the macOS build.

#ifndef UI_GFX_MOJOM_CA_LAYER_RESULT_MOJOM_SHARED_H_
#define UI_GFX_MOJOM_CA_LAYER_RESULT_MOJOM_SHARED_H_

#include <cstdint>

namespace gfx::mojom {

enum class CALayerResult : int32_t {
    kCALayerSuccess = 0,
    kCALayerFailedUnknown = 1,
    kCALayerFailedTextureNotCandidate = 5,
    kCALayerFailedTileNotCandidate = 7,
    kCALayerFailedQuadBlendMode = 8,
    kCALayerFailedQuadClipping = 10,
    kCALayerFailedDebugBoarder = 11,
    kCALayerFailedPictureContent = 12,
    kCALayerFailedSurfaceContent = 14,
    kCALayerFailedDifferentClipSettings = 16,
    kCALayerFailedRenderPassBackdropFilters = 19,
    kCALayerFailedRenderPassPassMask = 20,
    kCALayerFailedRenderPassFilterOperation = 21,
    kCALayerFailedRenderPassSortingContextId = 22,
    kCALayerFailedTooManyRenderPassDrawQuads = 23,
    kCALayerFailedQuadRoundedCornerNotUniform = 26,
    kCALayerFailedTooManyQuads = 27,
    kCALayerFailedCopyRequests = 31,
    kCALayerFailedOverlayDisabled = 32,
    kCALayerFailedVideoCaptureEnabled = 33,
    kMinValue = 0,
    kMaxValue = 33,
};

}  // namespace gfx::mojom

#endif  // UI_GFX_MOJOM_CA_LAYER_RESULT_MOJOM_SHARED_H_
