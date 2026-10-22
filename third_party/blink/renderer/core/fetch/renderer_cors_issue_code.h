// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_FETCH_RENDERER_CORS_ISSUE_CODE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_FETCH_RENDERER_CORS_ISSUE_CODE_H_

namespace blink {

enum class RendererCorsIssueCode {
    kDisallowedByMode,
    kCorsDisabledScheme,
    kNoCorsRedirectModeNotFollow,
};

} // namespace blink

#endif // THIRD_PARTY_BLINK_RENDERER_CORE_FETCH_RENDERER_CORS_ISSUE_CODE_H_
