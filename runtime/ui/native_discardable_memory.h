// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef RUNTIME_UI_NATIVE_DISCARDABLE_MEMORY_H_
#define RUNTIME_UI_NATIVE_DISCARDABLE_MEMORY_H_

namespace mini_ui {

// Installs the native discardable-memory allocator unless the host has already
// installed one. Must be called during single-threaded process startup.
void InitializeNativeDiscardableMemory();

} // namespace mini_ui

#endif // RUNTIME_UI_NATIVE_DISCARDABLE_MEMORY_H_
