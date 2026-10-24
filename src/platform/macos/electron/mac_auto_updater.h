// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef PLATFORM_MACOS_ELECTRON_MAC_AUTO_UPDATER_H_
#define PLATFORM_MACOS_ELECTRON_MAC_AUTO_UPDATER_H_

namespace node {
class Environment;
}

namespace mini_electron::electron {

void InstallMacAutoUpdaterBinding(node::Environment* environment);
void ShutdownMacAutoUpdaterBinding();

}  // namespace mini_electron::electron

#endif  // PLATFORM_MACOS_ELECTRON_MAC_AUTO_UPDATER_H_
