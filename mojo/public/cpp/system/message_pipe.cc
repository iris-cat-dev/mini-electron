// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/public/cpp/system/message_pipe.h"

#include <cstring>

#include <limits>

namespace mojo {

MojoResult WriteMessageRaw(
    MessagePipeHandle message_pipe, const void* bytes, size_t num_bytes, const MojoHandle* handles, size_t num_handles, MojoWriteMessageFlags flags)
{
    if ((num_bytes && !bytes) || (num_handles && !handles))
        return MOJO_RESULT_INVALID_ARGUMENT;
    if (num_bytes > std::numeric_limits<uint32_t>::max() || num_handles > std::numeric_limits<uint32_t>::max())
        return MOJO_RESULT_RESOURCE_EXHAUSTED;
    ScopedMessageHandle message;
    MojoResult result = CreateMessage(&message, MOJO_CREATE_MESSAGE_FLAG_NONE);
    if (result != MOJO_RESULT_OK)
        return result;
    const MojoAppendMessageDataOptions options = { sizeof(options), MOJO_APPEND_MESSAGE_DATA_FLAG_COMMIT_SIZE };
    void* buffer = nullptr;
    result = MojoAppendMessageData(message->value(), static_cast<uint32_t>(num_bytes), handles, static_cast<uint32_t>(num_handles), &options, &buffer, nullptr);
    if (result != MOJO_RESULT_OK)
        return result;
    if (num_bytes)
        memcpy(buffer, bytes, num_bytes);
    return WriteMessageNew(message_pipe, std::move(message), flags);
}

MojoResult ReadMessageRaw(MessagePipeHandle message_pipe, std::vector<uint8_t>* payload, std::vector<ScopedHandle>* handles, MojoReadMessageFlags flags)
{
    ScopedMessageHandle message;
    MojoResult result = ReadMessageNew(message_pipe, &message, flags);
    if (result != MOJO_RESULT_OK)
        return result;
    result = MojoSerializeMessage(message->value(), nullptr);
    if (result != MOJO_RESULT_OK && result != MOJO_RESULT_FAILED_PRECONDITION)
        return MOJO_RESULT_ABORTED;
    void* buffer = nullptr;
    uint32_t num_bytes = 0;
    std::vector<ScopedHandle> discarded_handles;
    result = GetMessageData(message.get(), &buffer, &num_bytes, handles ? handles : &discarded_handles, MOJO_GET_MESSAGE_DATA_FLAG_NONE);
    if (result != MOJO_RESULT_OK)
        return result;
    if (payload) {
        payload->resize(num_bytes);
        if (num_bytes)
            memcpy(payload->data(), buffer, num_bytes);
    }
    return MOJO_RESULT_OK;
}

} // namespace mojo
