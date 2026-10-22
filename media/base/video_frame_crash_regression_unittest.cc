// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/video_frame.h"

#include <array>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/hash/md5.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "media/video/fake_gpu_memory_buffer.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace media {

TEST(VideoFrameCrashRegression, HashIgnoresRowPadding)
{
    std::array<uint8_t, 16> pixels = { 1, 2, 3, 4, 99, 99, 99, 99, 5, 6, 7, 8, 99, 99, 99, 99 };
    const auto layout = VideoFrameLayout::CreateWithStrides(PIXEL_FORMAT_ARGB, gfx::Size(1, 2), { 8 });
    ASSERT_TRUE(layout);
    auto frame = VideoFrame::WrapExternalDataWithLayout(*layout, gfx::Rect(1, 2), gfx::Size(1, 2), pixels.data(), pixels.size(), base::TimeDelta());
    ASSERT_TRUE(frame);
    base::MD5Context context;
    base::MD5Init(&context);
    VideoFrame::HashFrameForTesting(&context, *frame);
    base::MD5Digest actual;
    base::MD5Final(&actual, &context);
    const std::array<uint8_t, 8> content = { 1, 2, 3, 4, 5, 6, 7, 8 };
    base::MD5Digest expected;
    base::MD5Sum(content, &expected);
    EXPECT_EQ(base::MD5DigestToBase16(expected), base::MD5DigestToBase16(actual));
}

TEST(VideoFrameCrashRegression, SharedImageRetainsAcquireTokenAndReleasesOnce)
{
    auto image = gpu::ClientSharedImage::CreateForTesting();
    const gpu::SyncToken acquire(gpu::CommandBufferNamespace::GPU_IO, gpu::CommandBufferId::FromUnsafeValue(1), 7);
    int releases = 0;
    auto frame = VideoFrame::WrapSharedImage(PIXEL_FORMAT_ARGB, image, acquire,
        base::BindOnce([](int* count, const gpu::SyncToken&) { ++*count; }, &releases),
        gfx::Size(4, 4), gfx::Rect(4, 4), gfx::Size(4, 4), base::Milliseconds(3));
    ASSERT_TRUE(frame);
    EXPECT_TRUE(frame->HasSharedImage());
    EXPECT_EQ(acquire, frame->acquire_sync_token());
    EXPECT_EQ(image->mailbox(), frame->shared_image()->mailbox());
    EXPECT_EQ(0, releases);
    frame.reset();
    EXPECT_EQ(1, releases);
}

TEST(VideoFrameCrashRegression, ExternalGpuBufferPreservesLayout)
{
    const gfx::Size size(16, 8);
    auto buffer = std::make_unique<FakeGpuMemoryBuffer>(size, gfx::BufferFormat::YUV_420_BIPLANAR, gfx::NativePixmapHandle::kNoModifier);
    auto* original_buffer = buffer.get();
    auto image = gpu::ClientSharedImage::CreateForTesting();
    auto frame = VideoFrame::WrapExternalGpuMemoryBuffer(gfx::Rect(size), size, std::move(buffer), image,
        gpu::SyncToken(), base::DoNothing(), base::Milliseconds(9));
    ASSERT_TRUE(frame);
    EXPECT_EQ(PIXEL_FORMAT_NV12, frame->format());
    EXPECT_EQ(2u, frame->layout().num_planes());
    EXPECT_EQ(size.width(), frame->layout().planes()[0].stride);
    EXPECT_EQ(size.width(), frame->layout().planes()[1].stride);
    EXPECT_EQ(original_buffer, frame->GetGpuMemoryBufferForTesting());
    EXPECT_EQ(base::Milliseconds(9), frame->timestamp());
    EXPECT_TRUE(frame->HasSharedImage());
}

} // namespace media
