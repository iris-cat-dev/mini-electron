// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/config/gpu_feature_info.h"

#include "base/containers/contains.h"
#if !defined(MINI_ELECTRON_IMPLEMENTATION)
#include "base/check.h"
#include "ui/gl/gl_context.h"
#endif

namespace gpu {

GpuFeatureInfo::GpuFeatureInfo()
{
    for (auto& status : status_values)
        status = kGpuFeatureStatusUndefined;
}

GpuFeatureInfo::GpuFeatureInfo(const GpuFeatureInfo&) = default;

GpuFeatureInfo::GpuFeatureInfo(GpuFeatureInfo&&) = default;

GpuFeatureInfo::~GpuFeatureInfo() = default;

GpuFeatureInfo& GpuFeatureInfo::operator=(const GpuFeatureInfo&) = default;

GpuFeatureInfo& GpuFeatureInfo::operator=(GpuFeatureInfo&&) = default;

#if !defined(MINI_ELECTRON_IMPLEMENTATION)
void GpuFeatureInfo::ApplyToGLContext(gl::GLContext* gl_context) const
{
    DCHECK(gl_context);
    // Driver workarounds are applied by ANGLE's backend, not GLContext.
    gl_context->SetDisabledGLExtensions(this->disabled_extensions);
}
#endif

bool GpuFeatureInfo::IsWorkaroundEnabled(int32_t workaround) const
{
    return base::Contains(this->enabled_gpu_driver_bug_workarounds, workaround);
}

bool GpuFeatureInfo::IsInitialized() const
{
    // Check if any feature status is undefined.
    return status_values[GPU_FEATURE_TYPE_ACCELERATED_GL] != kGpuFeatureStatusUndefined;
}

} // namespace gpu
