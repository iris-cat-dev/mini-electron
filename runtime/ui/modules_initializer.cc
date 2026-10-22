// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include "runtime/ui/bindings_initializer.h"

#include <memory>

#include "third_party/blink/renderer/bindings/core/v8/serialization/serialized_script_value_factory.h"
#include "third_party/blink/renderer/bindings/modules/v8/init_idl_interfaces.h"
#include "third_party/blink/renderer/bindings/modules/v8/properties_per_feature_installer.h"
#include "third_party/blink/renderer/core/core_initializer.h"
#include "third_party/blink/renderer/core/html/canvas/html_canvas_element.h"
#include "third_party/blink/renderer/core/offscreencanvas/offscreen_canvas.h"
#include "third_party/blink/renderer/core/scheduler/task_attribution_tracker_impl.h"
#include "third_party/blink/renderer/modules/canvas/canvas2d/canvas_rendering_context_2d.h"
#include "third_party/blink/renderer/modules/canvas/imagebitmap/image_bitmap_rendering_context.h"
#include "third_party/blink/renderer/modules/canvas/offscreencanvas2d/offscreen_canvas_rendering_context_2d.h"
#include "third_party/blink/renderer/platform/bindings/v8_per_isolate_data.h"
#include "third_party/blink/renderer/platform/bindings/origin_trial_features.h"

namespace mini_ui {
namespace {

// The core implementation owns real structured-clone handling for Blob,
// ImageBitmap, ImageData and the retained DOM geometry types.
struct CoreScriptValueFactory final : blink::SerializedScriptValueFactory { };

class RetainedModulesInitializer final : public blink::CoreInitializer {
public:
    void Initialize() override
    {
        blink::bindings::InitIDLInterfaces();
        auto* previous_installer = blink::SetInstallPropertiesPerFeatureFunc(
            blink::bindings::InstallPropertiesPerFeature);
        CHECK(!previous_installer);
        static CoreScriptValueFactory script_value_factory;
        blink::SerializedScriptValueFactory::Initialize(&script_value_factory);
        blink::CoreInitializer::Initialize();
        blink::HTMLCanvasElement::RegisterRenderingContextFactory(
            std::make_unique<blink::CanvasRenderingContext2D::Factory>());
        blink::HTMLCanvasElement::RegisterRenderingContextFactory(
            std::make_unique<blink::ImageBitmapRenderingContext::Factory>());
        blink::OffscreenCanvas::RegisterRenderingContextFactory(
            std::make_unique<blink::OffscreenCanvasRenderingContext2D::Factory>());
        blink::OffscreenCanvas::RegisterRenderingContextFactory(
            std::make_unique<blink::ImageBitmapRenderingContext::Factory>());
        blink::V8PerIsolateData::SetTaskAttributionTrackerFactory(
            &blink::scheduler::TaskAttributionTrackerImpl::Create);
    }
};

} // namespace

void InitializeRetainedModules()
{
    static RetainedModulesInitializer initializer;
    initializer.Initialize();
}

} // namespace mini_ui
