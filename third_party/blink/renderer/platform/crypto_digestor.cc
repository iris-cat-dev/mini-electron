// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/crypto.h"

#include "base/numerics/safe_conversions.h"
#if defined(MINI_UI_QUICKJS)
#include <windows.h>
#include <bcrypt.h>

#include <limits>
#include <utility>
#else
#include "crypto/openssl_util.h"
#endif
#include "third_party/blink/renderer/platform/wtf/shared_buffer.h"
#include "third_party/blink/renderer/platform/wtf/text/string_utf8_adaptor.h"

namespace blink {

Digestor::Digestor(HashAlgorithm algorithm)
{
    Reset(algorithm);
}

#if defined(MINI_UI_QUICKJS)
Digestor::~Digestor()
{
    ReleaseHandles();
}

Digestor::Digestor(Digestor&& other) noexcept
    : algorithm_handle_(std::exchange(other.algorithm_handle_, nullptr))
    , hash_handle_(std::exchange(other.hash_handle_, nullptr))
    , digest_size_(std::exchange(other.digest_size_, 0))
    , has_failed_(std::exchange(other.has_failed_, true))
{
}

Digestor& Digestor::operator=(Digestor&& other) noexcept
{
    if (this != &other) {
        ReleaseHandles();
        algorithm_handle_ = std::exchange(other.algorithm_handle_, nullptr);
        hash_handle_ = std::exchange(other.hash_handle_, nullptr);
        digest_size_ = std::exchange(other.digest_size_, 0);
        has_failed_ = std::exchange(other.has_failed_, true);
    }
    return *this;
}

void Digestor::ReleaseHandles()
{
    if (hash_handle_)
        BCryptDestroyHash(std::exchange(hash_handle_, nullptr));
    if (algorithm_handle_)
        BCryptCloseAlgorithmProvider(std::exchange(algorithm_handle_, nullptr), 0);
    digest_size_ = 0;
}
#else
Digestor::~Digestor() = default;
#endif

void Digestor::Reset(HashAlgorithm algorithm)
{
#if defined(MINI_UI_QUICKJS)
    ReleaseHandles();
    has_failed_ = true;

    LPCWSTR algorithm_id = nullptr;
    ULONG expected_size = 0;
    switch (algorithm) {
        case kHashAlgorithmSha1:
            algorithm_id = BCRYPT_SHA1_ALGORITHM;
            expected_size = 20;
            break;
        case kHashAlgorithmSha256:
            algorithm_id = BCRYPT_SHA256_ALGORITHM;
            expected_size = 32;
            break;
        case kHashAlgorithmSha384:
            algorithm_id = BCRYPT_SHA384_ALGORITHM;
            expected_size = 48;
            break;
        case kHashAlgorithmSha512:
            algorithm_id = BCRYPT_SHA512_ALGORITHM;
            expected_size = 64;
            break;
    }
    if (!algorithm_id)
        return;

    ULONG hash_size = 0;
    ULONG bytes_written = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm_handle_, algorithm_id, nullptr, 0) < 0
        || BCryptGetProperty(algorithm_handle_, BCRYPT_HASH_LENGTH,
               reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size), &bytes_written, 0) < 0
        || bytes_written != sizeof(hash_size) || hash_size != expected_size
        // Windows 7+ lets CNG allocate and own the hash object's backing storage.
        || BCryptCreateHash(algorithm_handle_, &hash_handle_, nullptr, 0, nullptr, 0, 0) < 0) {
        ReleaseHandles();
        return;
    }
    digest_size_ = hash_size;
    has_failed_ = false;
#else
    digest_context_.Reset();

    crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);

    const EVP_MD* evp_md = nullptr;
    switch (algorithm) {
        case kHashAlgorithmSha1:
            evp_md = EVP_sha1();
            break;
        case kHashAlgorithmSha256:
            evp_md = EVP_sha256();
            break;
        case kHashAlgorithmSha384:
            evp_md = EVP_sha384();
            break;
        case kHashAlgorithmSha512:
            evp_md = EVP_sha512();
            break;
    }

    has_failed_ = !evp_md || !EVP_DigestInit_ex(digest_context_.get(), evp_md, nullptr);
#endif
}

bool Digestor::Update(base::span<const uint8_t> data)
{
    if (has_failed_)
        return false;

#if defined(MINI_UI_QUICKJS)
    if (!hash_handle_) {
        has_failed_ = true;
        return false;
    }
    while (!data.empty()) {
        const ULONG chunk_size = static_cast<ULONG>(
            data.size() > (std::numeric_limits<ULONG>::max)()
                ? (std::numeric_limits<ULONG>::max)() : data.size());
        // CNG's input parameter is non-const, but BCryptHashData only reads it.
        if (BCryptHashData(hash_handle_, const_cast<PUCHAR>(data.data()), chunk_size, 0) < 0) {
            has_failed_ = true;
            ReleaseHandles();
            return false;
        }
        data = data.subspan(chunk_size);
    }
    return true;
#else
    crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);
    has_failed_ = !EVP_DigestUpdate(digest_context_.get(), data.data(), data.size());
    return !has_failed_;
#endif
}

bool Digestor::UpdateUtf8(const String& string, WTF::UTF8ConversionMode mode)
{
#if defined(MINI_UI_QUICKJS)
    if (has_failed_)
        return false;
#endif
    StringUTF8Adaptor utf8(string, mode);
#if defined(MINI_UI_QUICKJS)
    if (string.length() && !utf8.size()) {
        has_failed_ = true;
        ReleaseHandles();
        return false;
    }
#endif
    return Update(base::span<const uint8_t>((const uint8_t*)utf8.data(), utf8.size()));
}

bool Digestor::Finish(DigestValue& digest_result)
{
    if (has_failed_)
        return false;

#if defined(MINI_UI_QUICKJS)
    if (!hash_handle_) {
        has_failed_ = true;
        return false;
    }
    digest_result.resize(base::checked_cast<wtf_size_t>(digest_size_));
    has_failed_ = BCryptFinishHash(hash_handle_, digest_result.data(),
                      base::checked_cast<ULONG>(digest_size_), 0) < 0;
    ReleaseHandles();
    if (has_failed_)
        digest_result.clear();
    return !has_failed_;
#else
    crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);
    const size_t expected_size = EVP_MD_CTX_size(digest_context_.get());
    DCHECK_LE(expected_size, static_cast<size_t>(EVP_MAX_MD_SIZE));
    digest_result.resize(base::checked_cast<wtf_size_t>(expected_size));

    unsigned result_size;
    has_failed_ = !EVP_DigestFinal_ex(digest_context_.get(), digest_result.data(), &result_size) || result_size != expected_size;
    return !has_failed_;
#endif
}

bool ComputeDigest(HashAlgorithm algorithm, base::span<const uint8_t> digestable, DigestValue& digest_result)
{
    Digestor digestor(algorithm);
    digestor.Update(digestable);
    digestor.Finish(digest_result);
    return !digestor.has_failed();
}

bool ComputeDigest(HashAlgorithm algorithm, const SegmentedBuffer* buffer, DigestValue& digest_result)
{
    Digestor digestor(algorithm);
    if (buffer) {
        for (const auto& span : *buffer) {
            digestor.Update(base::as_bytes(span));
        }
    }
    digestor.Finish(digest_result);
    return !digestor.has_failed();
}

} // namespace blink
