// Copyright 2026 The mini-electron Authors.
// Use of this source code is governed by the MIT license in LICENSE.

#include "runtime/electron/common/gin_helper/callback_converter.h"
#include "runtime/electron/common/gin_helper/converter.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "gin/test/v8_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/events/event_constants.h"
#include "third_party/blink/public/common/messaging/cloneable_message.h"
#include "v8/include/v8.h"

namespace gin_helper {

using RuntimeCrashContractTest = gin::V8Test;

TEST_F(RuntimeCrashContractTest, ValuesCallbacksAndGenericEvents)
{
    v8::Isolate* isolate = instance_->isolate();
    v8::HandleScope handles(isolate);
    auto context = context_.Get(isolate);
    v8::Context::Scope context_scope(context);
    auto evaluate = [&](const char* source) {
        return v8::Script::Compile(context, v8::String::NewFromUtf8(isolate, source).ToLocalChecked())
            .ToLocalChecked()->Run(context).ToLocalChecked();
    };

    base::Value::Dict dictionary;
    ASSERT_TRUE(Converter<base::Value::Dict>::FromV8(isolate,
        evaluate("({'': 'empty key', 42: 'numeric key', text: 'a\\0b', large: 4294967295, list: ['', null]})"), &dictionary));
    EXPECT_EQ(*dictionary.FindString(""), "empty key");
    EXPECT_EQ(*dictionary.FindString("42"), "numeric key");
    EXPECT_EQ(*dictionary.FindString("text"), std::string("a\0b", 3));
    EXPECT_EQ(dictionary.FindDouble("large"), 4294967295.0);
    auto round_trip = Converter<base::Value::Dict>::ToV8(isolate, dictionary).As<v8::Object>();
    EXPECT_TRUE(round_trip->HasOwnProperty(context, StringToV8(isolate, "")).FromJust());
    EXPECT_TRUE(ConvertToV8(isolate, base::Value())->IsNull());

    base::Value::Dict prototype_key;
    prototype_key.Set("__proto__", base::Value::Dict().Set("marker", true));
    auto prototype_result = Converter<base::Value::Dict>::ToV8(isolate, prototype_key).As<v8::Object>();
    EXPECT_TRUE(prototype_result->HasOwnProperty(context, StringToV8(isolate, "__proto__")).FromJust());
    EXPECT_TRUE(prototype_result->Get(context, StringToV8(isolate, "marker")).ToLocalChecked()->IsUndefined());

    base::Value converted;
    EXPECT_FALSE(Converter<base::Value>::FromV8(isolate, evaluate("({value: Symbol('unsupported')})"), &converted));
    EXPECT_FALSE(Converter<base::Value>::FromV8(isolate, evaluate("({value: 1n})"), &converted));
    EXPECT_FALSE(Converter<base::Value>::FromV8(isolate, evaluate("({value: Infinity})"), &converted));
    EXPECT_FALSE(Converter<base::Value>::FromV8(isolate, evaluate("(() => { const x = {}; x.self = x; return x; })()"), &converted));
    {
        v8::TryCatch exception(isolate);
        EXPECT_FALSE(Converter<base::Value>::FromV8(isolate,
            evaluate("({get value() { throw Error('getter failed'); }})"), &converted));
        EXPECT_TRUE(exception.HasCaught());
    }
    {
        v8::TryCatch exception(isolate);
        EXPECT_FALSE(Converter<base::Value>::FromV8(isolate,
            evaluate("new Proxy({}, {ownKeys() { throw Error('keys failed'); }})"), &converted));
        EXPECT_TRUE(exception.HasCaught());
    }

    std::vector<blink::CloneableMessage> messages;
    ASSERT_TRUE(Converter<std::vector<blink::CloneableMessage>>::FromV8(isolate, evaluate("['', null, {nested: 3}]"), &messages));
    EXPECT_EQ(messages.size(), 3u);
    auto messages_js = Converter<std::vector<blink::CloneableMessage>>::ToV8(isolate, messages).As<v8::Array>();
    EXPECT_EQ(messages_js->Length(), 3u);
    EXPECT_TRUE(messages_js->Get(context, 1).ToLocalChecked()->IsNull());
    std::unique_ptr<std::vector<blink::CloneableMessage>> owned_messages;
    ASSERT_TRUE(Converter<decltype(owned_messages)>::FromV8(isolate, messages_js, &owned_messages));
    EXPECT_EQ(owned_messages->size(), 3u);
    EXPECT_TRUE(Converter<decltype(owned_messages)>::ToV8(isolate, owned_messages)->IsArray());

    int calls = 0;
    auto repeating = base::BindRepeating([](int* count) { return ++*count; }, &calls);
    auto repeated = Converter<decltype(repeating)>::ToV8(isolate, repeating).As<v8::Function>();
    EXPECT_EQ(repeated->Call(context, context->Global(), 0, nullptr).ToLocalChecked().As<v8::Int32>()->Value(), 1);
    EXPECT_EQ(repeated->Call(context, context->Global(), 0, nullptr).ToLocalChecked().As<v8::Int32>()->Value(), 2);
    auto once = base::BindOnce([](int* count) { return ++*count; }, &calls);
    auto one_time = Converter<decltype(once)>::ToV8(isolate, std::move(once)).As<v8::Function>();
    EXPECT_EQ(one_time->Call(context, context->Global(), 0, nullptr).ToLocalChecked().As<v8::Int32>()->Value(), 3);
    {
        v8::TryCatch exception(isolate);
        EXPECT_TRUE(one_time->Call(context, context->Global(), 0, nullptr).IsEmpty());
        EXPECT_TRUE(exception.HasCaught());
    }
    EXPECT_EQ(calls, 3);

    auto event = mate::internal::createJSEvent(isolate, context->Global());
    EXPECT_FALSE(event->Has(context, StringToV8(isolate, "sendReply")).FromJust());
    auto prevent_default = event->Get(context, StringToV8(isolate, "preventDefault")).ToLocalChecked().As<v8::Function>();
    ASSERT_FALSE(prevent_default->Call(context, event, 0, nullptr).IsEmpty());
    EXPECT_TRUE(event->Get(context, StringToV8(isolate, "defaultPrevented")).ToLocalChecked()->IsTrue());
    auto modifiers = mate::internal::createEventFromFlags(isolate, ui::EF_SHIFT_DOWN | ui::EF_COMMAND_DOWN);
    EXPECT_TRUE(modifiers->Get(context, StringToV8(isolate, "shiftKey")).ToLocalChecked()->IsTrue());
    EXPECT_TRUE(modifiers->Get(context, StringToV8(isolate, "metaKey")).ToLocalChecked()->IsTrue());
    EXPECT_TRUE(modifiers->Get(context, StringToV8(isolate, "ctrlKey")).ToLocalChecked()->IsFalse());
}

} // namespace gin_helper
