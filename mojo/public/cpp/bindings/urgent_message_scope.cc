// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/public/cpp/bindings/urgent_message_scope.h"

namespace mojo {
namespace {
// Scopes are stack allocated and never cross tasks. Each thread independently
// restores its previous state when nested scopes leave.
constinit thread_local bool is_in_urgent_message_scope = false;
}  // namespace

UrgentMessageScope::UrgentMessageScope()
    : resetter_(&is_in_urgent_message_scope, true) {}

UrgentMessageScope& UrgentMessageScope::operator=(UrgentMessageScope&& other) = default;
UrgentMessageScope::UrgentMessageScope(UrgentMessageScope&& other) = default;
UrgentMessageScope::~UrgentMessageScope() = default;

bool UrgentMessageScope::IsInUrgentScope() {
  return is_in_urgent_message_scope;
}

}  // namespace mojo
