// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_BINDINGS_CORE_V8_REJECTED_PROMISES_H_
#define THIRD_PARTY_BLINK_RENDERER_BINDINGS_CORE_V8_REJECTED_PROMISES_H_

#include <memory>
#include "third_party/blink/renderer/bindings/core/v8/sanitize_script_errors.h"
#include "third_party/blink/renderer/platform/bindings/source_location.h"
#include "third_party/blink/renderer/platform/wtf/forward.h"
#include "third_party/blink/renderer/platform/wtf/ref_counted.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace v8 {
template <class T>
class Local;
class Promise;
class Value;
#if !defined(MINI_UI_QUICKJS)
class PromiseRejectMessage;
#endif
}

namespace blink {

class ScriptState;

class RejectedPromises final : public RefCounted<RejectedPromises> {
    USING_FAST_MALLOC(RejectedPromises);

public:
    static scoped_refptr<RejectedPromises> Create()
    {
        return base::AdoptRef(new RejectedPromises());
    }

    ~RejectedPromises();
    void Dispose();

#if defined(MINI_UI_QUICKJS)
    void RejectedWithNoHandler(
        ScriptState*, v8::Local<v8::Promise>, v8::Local<v8::Value>,
        SanitizeScriptErrors);
    void HandlerAdded(v8::Local<v8::Promise>);
#else
    void RejectedWithNoHandler(ScriptState*, v8::PromiseRejectMessage, const String& error_message,
        std::unique_ptr<SourceLocation>, SanitizeScriptErrors);
    void HandlerAdded(v8::PromiseRejectMessage);
#endif

    void ProcessQueue();

private:
    class Message;

    RejectedPromises();

    using MessageQueue = Vector<std::unique_ptr<Message>>;

    void ProcessQueueNow(MessageQueue);
    void RevokeNow(std::unique_ptr<Message>);
    void HandlerAddedInternal(v8::Local<v8::Promise>);

    MessageQueue queue_;
    Vector<std::unique_ptr<Message>> reported_as_errors_;
};


} // namespace blink

#endif // THIRD_PARTY_BLINK_RENDERER_BINDINGS_CORE_V8_REJECTED_PROMISES_H_
