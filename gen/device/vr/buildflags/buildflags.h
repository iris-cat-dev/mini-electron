// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef DEVICE_VR_BUILDFLAGS_BUILDFLAGS_H_
#define DEVICE_VR_BUILDFLAGS_BUILDFLAGS_H_

#include "build/buildflag.h"

// This runtime provides WebGL, not a WebXR device service.
#define BUILDFLAG_INTERNAL_ENABLE_VR() (0)
#define BUILDFLAG_INTERNAL_ENABLE_OPENXR() (0)

#endif  // DEVICE_VR_BUILDFLAGS_BUILDFLAGS_H_
