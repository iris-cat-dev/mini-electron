// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/common/display/use_layered_window.h"

#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include "ui/base/win/internal_constants.h"
#endif

namespace viz {

bool NeedsToUseLayerWindow(HWND hwnd)
{
#if BUILDFLAG(IS_WIN)
    return GetPropW(hwnd, ui::kWindowTranslucent);
#else
    return false;
#endif
}

} // namespace viz
