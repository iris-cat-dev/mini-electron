// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/public/cpp/bindings/lib/send_message_helper.h"

#include <cstring>
#include <tuple>

#include "mojo/public/cpp/bindings/mojo_buildflags.h"
#if BUILDFLAG(MOJO_TRACE_ENABLED)
#include "base/trace_event/typed_macros.h"
#endif
#include "mojo/public/cpp/bindings/message.h"

namespace mojo {
namespace internal {

void SendMojoMessage(MessageReceiver& receiver, Message& message) {
#if BUILDFLAG(MOJO_TRACE_ENABLED)
  uint64_t flow_id = message.GetTraceId();
  bool is_sync_non_response = message.has_flag(Message::kFlagIsSync) &&
                              !message.has_flag(Message::kFlagIsResponse);
  TRACE_EVENT_INSTANT("toplevel.flow", "Send mojo message",
                      perfetto::Flow::Global(flow_id));
#endif

  std::ignore = receiver.Accept(&message);

#if BUILDFLAG(MOJO_TRACE_ENABLED)
  // If this is a sync message which has just received a reply, connect the
  // point which received the sync reply (us) to the flow.
  if (is_sync_non_response) {
    TRACE_EVENT_INSTANT("toplevel.flow", "Receive mojo sync reply",
                        perfetto::Flow::Global(flow_id));
  }
#endif
}

void SendMojoMessage(MessageReceiverWithResponder& receiver,
                     Message& message,
                     std::unique_ptr<MessageReceiver> responder) {
#if BUILDFLAG(MOJO_TRACE_ENABLED)
  uint64_t flow_id = message.GetTraceId();
  bool is_sync_non_response = message.has_flag(Message::kFlagIsSync) &&
                              !message.has_flag(Message::kFlagIsResponse);
  TRACE_EVENT_INSTANT("toplevel.flow", "Send mojo message",
                      perfetto::Flow::Global(flow_id));
#endif

  std::ignore = receiver.AcceptWithResponder(&message, std::move(responder));

#if BUILDFLAG(MOJO_TRACE_ENABLED)
  // If this is a sync message which has just received a reply, connect the
  // point which received the sync reply (us) to the flow.
  if (is_sync_non_response) {
    TRACE_EVENT_INSTANT("toplevel.flow", "Receive mojo sync reply",
                        perfetto::Flow::Global(flow_id));
  }
#endif
}

}  // namespace internal
}  // namespace mojo
