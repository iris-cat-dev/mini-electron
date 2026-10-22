// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/ipc/common/mailbox_mojom_traits.h"

#include "base/containers/span.h"
#include "mojo/public/cpp/bindings/array_traits_span.h"

namespace mojo {

// static
bool StructTraits<gpu::mojom::MailboxDataView, gpu::Mailbox>::Read(gpu::mojom::MailboxDataView data, gpu::Mailbox* out)
{
    if (data.is_null())
        return false;
    base::span<int8_t> mailbox_name(out->name);
    mojo::ArrayDataView<int8_t> name;
    data.GetNameDataView(&name);
    if (name.is_null() || name.size() != mailbox_name.size())
        return false;
    return data.ReadName(&mailbox_name);
}

} // namespace mojo
