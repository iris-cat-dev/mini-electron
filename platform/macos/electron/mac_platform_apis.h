// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef PLATFORM_MACOS_ELECTRON_MAC_PLATFORM_APIS_H_
#define PLATFORM_MACOS_ELECTRON_MAC_PLATFORM_APIS_H_

namespace node {
class Environment;
}

namespace mini_electron::electron {

// Adds AppKit implementations for the Electron linked bindings that cannot use
// the Win32 browser API sources.
void InstallMacPlatformApiBindings(node::Environment* environment);

}  // namespace mini_electron::electron

#endif  // PLATFORM_MACOS_ELECTRON_MAC_PLATFORM_APIS_H_
