// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "platform/macos/electron/node_runtime.h"

#include "third_party/libnode/src/node.h"

namespace mini_electron::electron {

int RunAsNode(int argc, char** argv) {
  return node::Start(argc, argv);
}

}  // namespace mini_electron::electron
