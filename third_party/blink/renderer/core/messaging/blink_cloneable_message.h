// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MESSAGING_BLINK_CLONEABLE_MESSAGE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MESSAGING_BLINK_CLONEABLE_MESSAGE_H_

#include <optional>

#include "base/unguessable_token.h"
#include "third_party/blink/renderer/bindings/core/v8/serialization/serialized_script_value.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#if !defined(MINI_UI_QUICKJS)
#include "v8/include/v8-inspector.h"
#endif

namespace blink {

// This struct represents messages as they are posted over a broadcast channel.
// This type can be serialized as a blink::mojom::CloneableMessage struct.
// This is the renderer-side equivalent of blink::MessagePortMessage, where this
// struct uses blink types, while the other struct uses std:: types.
struct CORE_EXPORT BlinkCloneableMessage {
    BlinkCloneableMessage();
    BlinkCloneableMessage(BlinkCloneableMessage&&);
    BlinkCloneableMessage& operator=(BlinkCloneableMessage&&);
    ~BlinkCloneableMessage();

    scoped_refptr<blink::SerializedScriptValue> message;
    scoped_refptr<const blink::SecurityOrigin> sender_origin;
#if defined(MINI_UI_QUICKJS)
    // Transport metadata only; no inspector or debugger executes in this profile.
    uint64_t stack_trace_id = 0;
    int64_t stack_trace_debugger_id_first = 0;
    int64_t stack_trace_debugger_id_second = 0;
    bool stack_trace_should_pause = false;
#else
    v8_inspector::V8StackTraceId sender_stack_trace_id;
#endif
    base::UnguessableToken sender_agent_cluster_id;
    bool locked_to_sender_agent_cluster = false;
    uint64_t trace_id;
};

} // namespace blink

#endif // THIRD_PARTY_BLINK_RENDERER_CORE_MESSAGING_BLINK_CLONEABLE_MESSAGE_H_
