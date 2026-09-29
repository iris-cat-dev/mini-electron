// Copyright 2026 The miniblink132 Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef PLATFORM_MACOS_ELECTRON_NODE_RUNTIME_H_
#define PLATFORM_MACOS_ELECTRON_NODE_RUNTIME_H_

namespace miniblink::electron {

// Runs the executable as a Node.js process. Node and Blink link the same V8
// implementation, but only one runtime is initialized in a given process.
int RunAsNode(int argc, char** argv);

}  // namespace miniblink::electron

#endif  // PLATFORM_MACOS_ELECTRON_NODE_RUNTIME_H_
