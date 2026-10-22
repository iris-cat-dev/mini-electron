// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_CRYPTO_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_CRYPTO_H_

#include "base/containers/span.h"
#include "third_party/blink/renderer/platform/platform_export.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"
#include "third_party/blink/renderer/platform/wtf/forward.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"
#if !defined(MINI_UI_QUICKJS)
#include "third_party/boringssl/src/include/openssl/digest.h"
#endif

namespace blink {

static const size_t kMaxDigestSize = 64;
typedef Vector<uint8_t, kMaxDigestSize> DigestValue;

enum HashAlgorithm { kHashAlgorithmSha1, kHashAlgorithmSha256, kHashAlgorithmSha384, kHashAlgorithmSha512 };

PLATFORM_EXPORT bool ComputeDigest(HashAlgorithm, base::span<const uint8_t> digestable, DigestValue& digest_result);

PLATFORM_EXPORT bool ComputeDigest(HashAlgorithm, const SegmentedBuffer* buffer, DigestValue& digest_result);

class PLATFORM_EXPORT Digestor {
public:
    explicit Digestor(HashAlgorithm);
    ~Digestor();
#if defined(MINI_UI_QUICKJS)
    Digestor(Digestor&& other) noexcept;
    Digestor& operator=(Digestor&& other) noexcept;
#else
    Digestor(Digestor&& other) = default;
    Digestor& operator=(Digestor&& other) = default;
#endif

    // Starts a new digest, discarding prior input and clearing any failure.
    void Reset(HashAlgorithm);

    bool has_failed() const
    {
        return has_failed_;
    }

    // Return false on failure. These do nothing once the |has_failed_| flag is
    // set. Call Reset before reusing this object after Finish or a failure.
    bool Update(base::span<const uint8_t>);
    bool UpdateUtf8(const String&, WTF::UTF8ConversionMode = WTF::kLenientUTF8Conversion);
    bool Finish(DigestValue&);

private:
#if defined(MINI_UI_QUICKJS)
    void ReleaseHandles();

    // Opaque CNG handles keep Windows and OpenSSL headers out of UI consumers.
    // The hash must be destroyed before its algorithm provider is closed.
    void* algorithm_handle_ = nullptr;
    void* hash_handle_ = nullptr;
    size_t digest_size_ = 0;
#else
    bssl::ScopedEVP_MD_CTX digest_context_;
#endif
    bool has_failed_ = false;
};

} // namespace blink

#endif // THIRD_PARTY_BLINK_RENDERER_PLATFORM_CRYPTO_H_
