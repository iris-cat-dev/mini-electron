// Copyright 2011 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPC_IPC_EXPORT_H_
#define IPC_IPC_EXPORT_H_

// Defines IPC_EXPORT so that functionality implemented by the IPC module can
// be exported to consumers.
#if defined(COMPONENT_BUILD)
#if defined(WIN32)
#if defined(IPC_IMPLEMENTATION)
#define IPC_EXPORT __declspec(dllexport)
#else
#define IPC_EXPORT __declspec(dllimport)
#endif
#else
#if defined(IPC_IMPLEMENTATION)
#define IPC_EXPORT __attribute__((visibility("default")))
#else
#define IPC_EXPORT
#endif
#endif
#else
#define IPC_EXPORT
#endif

#endif  // IPC_IPC_EXPORT_H_
