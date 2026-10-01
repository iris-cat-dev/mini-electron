// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/common/display/de_jelly.h"

#include "base/command_line.h"
#include "components/viz/common/features.h"
#include "components/viz/common/switches.h"

namespace viz {

bool DeJellyEnabled()
{
//     static bool enabled
//         = !base::FeatureList::IsEnabled(features::kDisableDeJelly) && base::CommandLine::ForCurrentProcess()->HasSwitch(switches::kEnableDeJelly);
//     return enabled;
    return false;
}

bool DeJellyActive()
{
    if (!DeJellyEnabled())
        return false;

    return true;
}

float DeJellyScreenWidth()
{
//     std::string value = base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(switches::kDeJellyScreenWidth);
//     if (!value.empty())
//         return std::atoi(value.c_str());

    return 1440.0f;
}

float MaxDeJellyHeight()
{
    // Not currently configurable.
    return 30.0f;
}

} // namespace viz
