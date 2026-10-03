// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef RUNTIME_ENGINE_RENDERER_RENDERER_PERMISSION_BROKER_H_
#define RUNTIME_ENGINE_RENDERER_RENDERER_PERMISSION_BROKER_H_

#include <functional>
#include <string>

namespace content {

using RendererPermissionReply = std::function<void(bool)>;
using RendererPermissionBroker = std::function<void(
    const std::string&, bool, bool, RendererPermissionReply)>;

void SetRendererPermissionBroker(RendererPermissionBroker broker);
void RequestRendererPermission(const std::string& permission,
    bool user_gesture, bool is_request, RendererPermissionReply reply);
std::string RendererPermissionName(int permission_name);

} // namespace content

#endif // RUNTIME_ENGINE_RENDERER_RENDERER_PERMISSION_BROKER_H_
