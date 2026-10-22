// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <limits>

#include "ipc/ipc_message.h"
#include "ipc/ipc_message_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace IPC {

// Tests nesting of messages as parameters to other messages.
TEST(IPCMessageUtilsTest, NestedMessages)
{
    int32 nested_routing = 12;
    uint32 nested_type = 78;
    int nested_content = 456789;
    Message::PriorityValue nested_priority = Message::PRIORITY_HIGH;
    Message nested_msg(nested_routing, nested_type, nested_priority);
    nested_msg.set_sync();
    ParamTraits<int>::Write(&nested_msg, nested_content);

    // Outer message contains the nested one as its parameter.
    int32 outer_routing = 91;
    uint32 outer_type = 88;
    Message::PriorityValue outer_priority = Message::PRIORITY_NORMAL;
    Message outer_msg(outer_routing, outer_type, outer_priority);
    ParamTraits<Message>::Write(&outer_msg, nested_msg);

    // Read back the nested message.
    base::PickleIterator iter(outer_msg);
    IPC::Message result_msg;
    ASSERT_TRUE(ParamTraits<Message>::Read(&outer_msg, &iter, &result_msg));

    // Verify nested message headers.
    EXPECT_EQ(nested_msg.routing_id(), result_msg.routing_id());
    EXPECT_EQ(nested_msg.type(), result_msg.type());
    EXPECT_EQ(nested_msg.priority(), result_msg.priority());
    EXPECT_EQ(nested_msg.flags(), result_msg.flags());

    // Verify nested message content
    base::PickleIterator nested_iter(nested_msg);
    int result_content = 0;
    ASSERT_TRUE(ParamTraits<int>::Read(&nested_msg, &nested_iter, &result_content));
    EXPECT_EQ(nested_content, result_content);

    // Try reading past the ends for both messages and make sure it fails.
    IPC::Message dummy;
    ASSERT_FALSE(ParamTraits<Message>::Read(&outer_msg, &iter, &dummy));
    ASSERT_FALSE(ParamTraits<int>::Read(&nested_msg, &nested_iter, &result_content));
}

TEST(IPCMessageUtilsTest, LongRoundTrip)
{
    const long values[] = { std::numeric_limits<long>::min(), -1, 0, 1, std::numeric_limits<long>::max() };
    for (long value : values) {
        Message message;
        WriteParam(&message, value);
        base::PickleIterator iter(message);
        long result = 0;
        ASSERT_TRUE(ReadParam(&message, &iter, &result));
        EXPECT_EQ(value, result);
        EXPECT_FALSE(ReadParam(&message, &iter, &result));
    }
}

TEST(IPCMessageUtilsTest, UnsignedLongRoundTrip)
{
    const unsigned long values[] = { 0, 1, static_cast<unsigned long>(std::numeric_limits<long>::max()),
        static_cast<unsigned long>(std::numeric_limits<long>::max()) + 1, std::numeric_limits<unsigned long>::max() };
    for (unsigned long value : values) {
        Message message;
        WriteParam(&message, value);
        base::PickleIterator iter(message);
        unsigned long result = 0;
        ASSERT_TRUE(ReadParam(&message, &iter, &result));
        EXPECT_EQ(value, result);
        EXPECT_FALSE(ReadParam(&message, &iter, &result));
    }
}

TEST(IPCMessageUtilsTest, BinaryValuesRoundTrip)
{
    base::Value::List values;
    values.Append(base::Value(base::Value::BlobStorage()));
    values.Append(base::Value(base::Value::BlobStorage { 0, 1, 127, 128, 255 }));
    base::Value::Dict dictionary;
    dictionary.Set("blobs", std::move(values));
    Message message;
    WriteParam(&message, dictionary);
    base::PickleIterator iter(message);
    base::Value::Dict result;
    ASSERT_TRUE(ReadParam(&message, &iter, &result));
    EXPECT_EQ(dictionary, result);
}

TEST(IPCMessageUtilsTest, TruncatedBinaryValue)
{
    Message message;
    message.WriteInt(static_cast<int>(base::Value::Type::LIST));
    message.WriteInt(1);
    message.WriteInt(static_cast<int>(base::Value::Type::BINARY));
    message.WriteInt(8);
    message.WriteInt(0);
    base::PickleIterator iter(message);
    base::Value::List result;
    EXPECT_FALSE(ReadParam(&message, &iter, &result));
}

} // namespace IPC
