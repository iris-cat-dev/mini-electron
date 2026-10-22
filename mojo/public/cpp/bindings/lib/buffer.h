// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MOJO_PUBLIC_CPP_BINDINGS_LIB_BUFFER_H_
#define MOJO_PUBLIC_CPP_BINDINGS_LIB_BUFFER_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "base/component_export.h"
#include "mojo/public/cpp/system/handle.h"
#include "mojo/public/cpp/system/message.h"

namespace mojo {
namespace internal {

// Buffer owns its lite-transport storage and allocates 8-byte aligned blocks.
// Moving a Buffer transfers its storage without copying.
//
// A Buffer may be moved around. A moved-from Buffer is reset and may no longer
// be used to Allocate memory unless re-Initialized.
class /*COMPONENT_EXPORT(MOJO_CPP_BINDINGS_BASE)*/ Buffer {
public:
    // Constructs an invalid Buffer. May not call Allocate().
    Buffer();

    // Takes allocator-owned storage of |size| bytes and resumes allocation at
    // |cursor|. Storage may grow as further blocks are allocated.
    //
    // |data| must be storage allocated by the lite transport allocator.
    Buffer(void* data, size_t size, size_t cursor);

    // Takes storage from a lite message while remembering its non-owning
    // message ID. The storage must not remain owned by the raw message table.
    Buffer(MessageHandle message, size_t message_payload_size, void* data, size_t size);

    Buffer(Buffer&& other);

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    ~Buffer();

    Buffer& operator=(Buffer&& other);

    void* data() const
    {
        return data_;
    }
    size_t size() const
    {
        return size_;
    }
    size_t cursor() const
    {
        return cursor_;
    }

    bool is_valid() const
    {
        return data_ != nullptr || (size_ == 0 && !message_.is_valid());
    }

    // Allocates |num_bytes| from the buffer and returns an index to the start of
    // the allocated block. The resulting index is 8-byte aligned and can be
    // resolved to an address using Get<T>() below.
    size_t Allocate(size_t num_bytes, bool align = true);

    // Returns a typed address within the Buffer corresponding to |index|. Note
    // that this address is NOT stable across calls to |Allocate()| and thus must
    // not be cached accordingly.
    template <typename T> T* Get(size_t index)
    {
        DCHECK_LT(index, cursor_);
        return reinterpret_cast<T*>(static_cast<uint8_t*>(data_) + index);
    }

    // A template helper combining Allocate() and Get<T>() above to allocate and
    // return a block of size |sizeof(T)|.
    template <typename T> T* AllocateAndGet()
    {
        return Get<T>(Allocate(sizeof(T)));
    }

    // A helper which combines Allocate() and Get<void>() for a specified number
    // of bytes.
    void* AllocateAndGet(size_t num_bytes)
    {
        return Get<void>(Allocate(num_bytes));
    }

    // Serializes |handles| into the buffer object. Only valid to call when this
    // Buffer is backed by a message object.
    [[nodiscard]] bool AttachHandles(std::vector<ScopedHandle>* handles);

    // Seals this Buffer so it can no longer be used for allocation, and ensures
    // the backing message object has a complete accounting of the size of the
    // meaningful payload bytes.
    void Seal();

    // Resets the buffer to an invalid state. Can no longer be used to Allocate().
    void Reset();

private:
    MessageHandle message_;

    // The payload size from the message's internal perspective. This differs from
    // |size_| as Mojo may intentionally over-allocate space to account for future
    // growth. It differs from |cursor_| because we don't push payload size
    // updates to the message object as frequently as we update |cursor_|, for
    // performance.
    size_t message_payload_size_ = 0;

    // The storage and capacity owned by this Buffer.
    void* data_ = nullptr;
    size_t size_ = 0;

    // The current write offset into |data_| if this Buffer is being used for
    // message creation.
    size_t cursor_ = 0;
};

} // namespace internal
} // namespace mojo

#endif // MOJO_PUBLIC_CPP_BINDINGS_LIB_BUFFER_H_
