// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file provides a C++ wrapping around the Mojo C API for message pipes,
// replacing the prefix of "Mojo" with a "mojo" namespace, and using more
// strongly-typed representations of |MojoHandle|s.
//
// Please see "mojo/public/c/system/message_pipe.h" for complete documentation
// of the API.

#ifndef MOJO_PUBLIC_CPP_SYSTEM_MESSAGE_PIPE_H_
#define MOJO_PUBLIC_CPP_SYSTEM_MESSAGE_PIPE_H_

#include <stdint.h>

#include <vector>

#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "mojo/public/c/system/message_pipe.h"
#include "mojo/public/cpp/system/handle.h"
#include "mojo/public/cpp/system/message.h"
#include "mojo/public/cpp/system/system_export.h"

namespace mojo {

#if 1
// A strongly-typed representation of a |MojoHandle| to one end of a message
// pipe.
class MessagePipeHandle : public Handle {
public:
    MessagePipeHandle()
    {
    }
    explicit MessagePipeHandle(MojoHandle value)
        : Handle(value)
    {
    }

    // Copying and assignment allowed.
};

static_assert(sizeof(MessagePipeHandle) == sizeof(Handle), "Bad size for C++ MessagePipeHandle");

typedef ScopedHandleBase<MessagePipeHandle> ScopedMessagePipeHandle;
static_assert(sizeof(ScopedMessagePipeHandle) == sizeof(MessagePipeHandle), "Bad size for C++ ScopedMessagePipeHandle");
#else
// class RawPtrMessagePipe
//     : public base::RefCountedThreadSafe<RawPtrMessagePipe>
//     , public Handle {
// public:
//     void* ptr;
// };
//
// typedef ScopedHandleBase<RawPtrMessagePipe> ScopedMessagePipeHandle;
// static_assert(sizeof(ScopedMessagePipeHandle) == sizeof(RawPtrMessagePipe), "Bad size for C++ ScopedMessagePipeHandle");
#endif

// Creates a message pipe. See |MojoCreateMessagePipe()| for complete
// documentation.
inline MojoResult CreateMessagePipe(const MojoCreateMessagePipeOptions* options, ScopedMessagePipeHandle* message_pipe0, ScopedMessagePipeHandle* message_pipe1)
{
    DCHECK(message_pipe0);
    DCHECK(message_pipe1);
    MessagePipeHandle handle0;
    MessagePipeHandle handle1;
    MojoResult rv = MojoCreateMessagePipe(options, handle0.mutable_value(), handle1.mutable_value());
    // Reset even on failure (reduces the chances that a "stale"/incorrect handle
    // will be used).
    message_pipe0->reset(handle0);
    message_pipe1->reset(handle1);
    return rv;
}

// Copying convenience wrappers around the raw message object API.
MOJO_CPP_SYSTEM_EXPORT MojoResult WriteMessageRaw(
    MessagePipeHandle message_pipe, const void* bytes, size_t num_bytes, const MojoHandle* handles, size_t num_handles, MojoWriteMessageFlags flags);
MOJO_CPP_SYSTEM_EXPORT MojoResult ReadMessageRaw(
    MessagePipeHandle message_pipe, std::vector<uint8_t>* payload, std::vector<ScopedHandle>* handles, MojoReadMessageFlags flags);

inline MojoResult WriteMessageNew(MessagePipeHandle message_pipe, ScopedMessageHandle message, MojoWriteMessageFlags flags)
{
    const MojoWriteMessageOptions options = { sizeof(options), flags };
    return MojoWriteMessage(message_pipe.value(), message.release().value(), &options);
}

inline MojoResult ReadMessageNew(MessagePipeHandle message_pipe, ScopedMessageHandle* message, MojoReadMessageFlags flags)
{
    if (!message)
        return MOJO_RESULT_INVALID_ARGUMENT;
    const MojoReadMessageOptions options = { sizeof(options), flags };
    MojoMessageHandle raw_message;
    const MojoResult result = MojoReadMessage(message_pipe.value(), &options, &raw_message);
    if (result == MOJO_RESULT_OK)
        message->reset(MessageHandle(raw_message));
    return result;
}
//
// // Fuses two message pipes together at the given handles. See
// // |MojoFuseMessagePipes()| for complete documentation.
// inline MojoResult FuseMessagePipes(ScopedMessagePipeHandle message_pipe0, ScopedMessagePipeHandle message_pipe1)
// {
//     return MojoFuseMessagePipes(message_pipe0.release().value(), message_pipe1.release().value(), nullptr);
// }

// A wrapper class that automatically creates a message pipe and owns both
// handles.
class MessagePipe {
public:
    MessagePipe();
    explicit MessagePipe(const MojoCreateMessagePipeOptions& options);
    ~MessagePipe();

    ScopedMessagePipeHandle handle0;
    ScopedMessagePipeHandle handle1;
};

inline MessagePipe::MessagePipe()
{
    MojoResult result = CreateMessagePipe(nullptr, &handle0, &handle1);
    DCHECK_EQ(MOJO_RESULT_OK, result);
    DCHECK(handle0.is_valid());
    DCHECK(handle1.is_valid());
}

inline MessagePipe::MessagePipe(const MojoCreateMessagePipeOptions& options)
{
    MojoResult result = CreateMessagePipe(&options, &handle0, &handle1);
    DCHECK_EQ(MOJO_RESULT_OK, result);
    DCHECK(handle0.is_valid());
    DCHECK(handle1.is_valid());
}

inline MessagePipe::~MessagePipe()
{
}

} // namespace mojo

#endif // MOJO_PUBLIC_CPP_SYSTEM_MESSAGE_PIPE_H_
