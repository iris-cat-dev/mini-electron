// Copyright (c) 2015 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "runtime/electron/common/api/event_emitter_caller.h"

#include "runtime/electron/common/gin_helper/wrappable.h"
#include "runtime/electron/common/gin_helper/object_template_builder.h"
#include "runtime/electron/common/gin_helper/dictionary.h"
#include "runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "runtime/electron/common/gin_helper/public/wrapper_info.h"
#include "third_party/blink/public/common/messaging/cloneable_message.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/libnode/src/node_version.h"
#include "third_party/libuv/include/uv.h"
#include "base/values.h"


namespace mate {

namespace internal {

v8::Local<v8::Value> callEmitWithArgs(v8::Isolate* isolate, v8::Local<v8::Object> obj, ValueVector* args)
{
#if V8_MAJOR_VERSION == 5
    // Perform microtask checkpoint after running JavaScript.
    v8::MicrotasksScope script_scope(isolate, v8::MicrotasksScope::kRunMicrotasks);
#endif
    // Use node::MakeCallback to call the callback, and it will also run pending
    // tasks in Node.js.
    return node::MakeCallback(isolate, obj, "emit", args->size(), &args->front());
}

v8::Local<v8::Value> emitEventImpl(
    v8::Isolate* isolate,
    v8::Local<v8::Object> obj,
    internal::ValueVector& converted_args,
    v8::Local<v8::Object> event,
    const std::vector<blink::CloneableMessage>& args)
{
    for (size_t i = 0; i < args.size(); ++i) {
        v8::Local<v8::Value> val = gin_helper::ConvertToV8(isolate, args[i]);
        if (val.IsEmpty())
            return v8::Local<v8::Value>();
        converted_args.push_back(val);
    }
    return callEmitWithArgs(isolate, obj, &converted_args);
}

v8::Local<v8::Value> emitEventImpl(
    v8::Isolate* isolate, v8::Local<v8::Object> obj, internal::ValueVector& converted_args, v8::Local<v8::Object> event, const base::Value::List& args)
{
    for (const base::Value& value : args) {
        v8::Local<v8::Value> converted = gin_helper::ConvertToV8(isolate, value);
        if (converted.IsEmpty())
            return v8::Local<v8::Value>();
        converted_args.push_back(converted);
    }

    return callEmitWithArgs(isolate, obj, &converted_args);
}

} // namespace internal

} // namespace mate
