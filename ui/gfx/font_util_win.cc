// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/gfx/font_util_win.h"
#include <algorithm>
#include <limits>
#include <dwrite.h>

#include <windows.h>

#include <wrl/client.h>

#include "base/files/file_path.h"
#include "third_party/skia/include/core/SkSurfaceProps.h"
#include "third_party/skia/include/core/SkTypes.h"
#include "ui/base/ui_base_features.h"

namespace gfx {

namespace {
struct TextParameters {
    float contrast;
    float gamma;
};

TextParameters GetTextParameters()
{
    static const TextParameters text_parameters = [] {
        TextParameters parameters { FontUtilWin::TextGammaContrast(), SK_GAMMA_EXPONENT };
        // DirectWrite defaults differ from Skia's. Apply user rendering
        // parameters only when the primary display has an explicit settings key.
        const base::win::RegKey key = FontUtilWin::GetTextSettingsRegistryKey();
        if (!key.Valid())
            return parameters;
        Microsoft::WRL::ComPtr<IDWriteFactory> factory;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.GetAddressOf()))))
            return parameters;
        Microsoft::WRL::ComPtr<IDWriteRenderingParams> rendering;
        if (SUCCEEDED(factory->CreateRenderingParams(&rendering))) {
            parameters.contrast = FontUtilWin::ClampContrast(rendering->GetEnhancedContrast());
            parameters.gamma = FontUtilWin::ClampGamma(rendering->GetGamma());
        }
        return parameters;
    }();
    return text_parameters;
}
} // namespace

// static
base::win::RegKey FontUtilWin::GetTextSettingsRegistryKey(REGSAM access)
{
    DISPLAY_DEVICE display_device = { sizeof(DISPLAY_DEVICE) };
    for (int i = 0; EnumDisplayDevices(nullptr, i, &display_device, 0); ++i) {
        // TODO(scottmg): We only support the primary device currently.
        if (display_device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            base::FilePath trimmed = base::FilePath(display_device.DeviceName).BaseName();
            return base::win::RegKey { HKEY_CURRENT_USER, (L"SOFTWARE\\Microsoft\\Avalon.Graphics\\" + trimmed.value()).c_str(), access };
        }
    }
    return base::win::RegKey {};
}

// static
float FontUtilWin::ClampContrast(float value)
{
    return std::clamp(value, SkSurfaceProps::kMinContrastInclusive, SkSurfaceProps::kMaxContrastInclusive);
}

// static
float FontUtilWin::ClampGamma(float value)
{
    // Handle the exclusive max by subtracting espilon.
    return std::clamp(value, SkSurfaceProps::kMinGammaInclusive, SkSurfaceProps::kMaxGammaExclusive - std::numeric_limits<float>::epsilon());
}

// static
float FontUtilWin::GetContrastFromRegistry()
{
    return GetTextParameters().contrast;
}

// static
float FontUtilWin::GetGammaFromRegistry()
{
    return GetTextParameters().gamma;
}

// static
float FontUtilWin::TextGammaContrast()
{
//     if (base::FeatureList::IsEnabled(features::kIncreaseWindowsTextContrast)) {
//         // On Windows, SK_GAMMA_CONTRAST is currently 0.5. This flag increases it
//         // to 1.0.
//         return 1.0f;
//     } else {
//         return SK_GAMMA_CONTRAST;
//     }
    return SK_GAMMA_CONTRAST;
}

} // namespace gfx
