// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "runtime/electron/renderer/authorized_file.h"

#include "third_party/blink/renderer/core/fileapi/file.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_file.h"
#include "runtime/engine/renderer/brokered_file_registry.h"

namespace atom {

bool GetAuthorizedPathForFile(v8::Local<v8::Context> context,
    v8::Local<v8::Value> file, std::string* authorized_path)
{
    if (!authorized_path
        || !blink::V8File::HasInstance(context->GetIsolate(), file))
        return false;
    blink::File* blink_file =
        blink::V8File::ToWrappable(context->GetIsolate(), file);
    return blink_file
        && content::GetAuthorizedPathForBrokeredFile(
            *blink_file, authorized_path);
}

} // namespace atom
