// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/paint/paint_op_reader.h"
#include "cc/paint/paint_op_writer.h"
#include "cc/test/test_options_provider.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gfx/hdr_metadata.h"

namespace cc {
namespace {

TEST(GraphicsCrashRepairTest, HdrMetadataPaintRoundTrip)
{
    alignas(PaintOpWriter::kMaxAlignment) char buffer[1024] = {};
    TestOptionsProvider options;
    gfx::HDRMetadata input;
    input.cta_861_3 = gfx::HdrMetadataCta861_3(1000, 400);
    input.smpte_st_2086 = gfx::HdrMetadataSmpteSt2086(
        SkColorSpacePrimaries { 0.68f, 0.32f, 0.265f, 0.69f, 0.15f, 0.06f, 0.3127f, 0.329f }, 1000.f, 0.01f);
    input.ndwl = gfx::HdrMetadataNdwl(203.f);
    input.extended_range = gfx::HdrMetadataExtendedRange(2.f, 4.f);
    PaintOpWriter writer(buffer, sizeof(buffer), options.serialize_options(), true);
    writer.Write(input);
    ASSERT_GT(writer.size(), 0u);
    PaintOpReader reader(buffer, writer.size(), options.deserialize_options(), true);
    gfx::HDRMetadata output;
    reader.Read(&output);
    ASSERT_TRUE(reader.valid());
    EXPECT_EQ(input.cta_861_3, output.cta_861_3);
    EXPECT_EQ(input.ndwl, output.ndwl);
    EXPECT_EQ(input.extended_range, output.extended_range);
    EXPECT_EQ(input.smpte_st_2086, output.smpte_st_2086);
    EXPECT_EQ(0u, reader.remaining_bytes());
}

TEST(GraphicsCrashRepairTest, EmptyHdrMetadataPaintRoundTrip)
{
    alignas(PaintOpWriter::kMaxAlignment) char buffer[1024] = {};
    TestOptionsProvider options;
    PaintOpWriter writer(buffer, sizeof(buffer), options.serialize_options(), true);
    writer.Write(gfx::HDRMetadata());
    ASSERT_GT(writer.size(), 0u);
    PaintOpReader reader(buffer, writer.size(), options.deserialize_options(), true);
    gfx::HDRMetadata output;
    output.ndwl = gfx::HdrMetadataNdwl(500.f);
    reader.Read(&output);
    ASSERT_TRUE(reader.valid());
    EXPECT_FALSE(output.cta_861_3.has_value());
    EXPECT_FALSE(output.smpte_st_2086.has_value());
    EXPECT_FALSE(output.ndwl.has_value());
    EXPECT_FALSE(output.extended_range.has_value());
}

} // namespace
} // namespace cc
