// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef RUNTIME_ELECTRON_RENDERER_AUTHORIZED_FILE_H_
#define RUNTIME_ELECTRON_RENDERER_AUTHORIZED_FILE_H_

#include <string>

#include "v8.h"

namespace atom {


// Synchronous webUtils accessor. It accepts only File objects materialized from
// an opaque browser-authorized chooser capability; File.name is never used.
bool GetAuthorizedPathForFile(v8::Local<v8::Context> context,
    v8::Local<v8::Value> file, std::string* authorized_path);

} // namespace atom

#endif // RUNTIME_ELECTRON_RENDERER_AUTHORIZED_FILE_H_
