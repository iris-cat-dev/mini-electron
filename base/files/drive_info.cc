// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/files/drive_info.h"

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_MAC)
#include <IOKit/IOKitLib.h>
#endif

namespace base {

DriveInfo::DriveInfo() = default;
DriveInfo::~DriveInfo() = default;
DriveInfo::DriveInfo(DriveInfo&&) = default;
DriveInfo& DriveInfo::operator=(DriveInfo&&) = default;


} // namespace base
