// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/webcrypto/algorithms/util.h"

#include "base/check_op.h"
#include "components/webcrypto/status.h"
#include "crypto/openssl_util.h"
#include <climits>
#include "third_party/openssl/openssl/include/openssl/cipher.h"
#include "third_party/openssl/openssl/include/openssl/bn.h"
#include "third_party/openssl/openssl/include/openssl/digest.h"

namespace webcrypto {

const EVP_MD* GetDigest(const blink::WebCryptoAlgorithm& hash_algorithm)
{
    return GetDigest(hash_algorithm.Id());
}

const EVP_MD* GetDigest(blink::WebCryptoAlgorithmId id)
{
    switch (id) {
    case blink::kWebCryptoAlgorithmIdSha1:
        return EVP_sha1();
    case blink::kWebCryptoAlgorithmIdSha256:
        return EVP_sha256();
    case blink::kWebCryptoAlgorithmIdSha384:
        return EVP_sha384();
    case blink::kWebCryptoAlgorithmIdSha512:
        return EVP_sha512();
    default:
        return nullptr;
    }
}

void TruncateToBitLength(size_t length_bits, std::vector<uint8_t>* bytes)
{
    size_t length_bytes = NumBitsToBytes(length_bits);

    if (bytes->size() != length_bytes) {
        CHECK_LT(length_bytes, bytes->size());
        bytes->resize(length_bytes);
    }

    size_t remainder_bits = length_bits % 8;

    // Zero any "unused bits" in the final byte.
    if (remainder_bits)
        bytes->back() &= ~((0xFF) >> remainder_bits);
}

Status CheckKeyCreationUsages(blink::WebCryptoKeyUsageMask all_possible_usages, blink::WebCryptoKeyUsageMask actual_usages)
{
    if (!ContainsKeyUsages(all_possible_usages, actual_usages))
        return Status::ErrorCreateKeyBadUsages();
    return Status::Success();
}

bool ContainsKeyUsages(blink::WebCryptoKeyUsageMask a, blink::WebCryptoKeyUsageMask b)
{
    return (a & b) == b;
}

Status AeadEncryptDecrypt(EncryptOrDecrypt mode, base::span<const uint8_t> raw_key, base::span<const uint8_t> data, unsigned int tag_length_bytes,
    base::span<const uint8_t> iv, base::span<const uint8_t> additional_data, const EVP_CIPHER* aead_alg, std::vector<uint8_t>* buffer)
{
    crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);
    if (!aead_alg)
        return Status::ErrorUnexpected();
    if (iv.empty() || iv.size() > INT_MAX || additional_data.size() > INT_MAX || data.size() > INT_MAX)
        return Status::OperationError();
    if (mode == DECRYPT && data.size() < tag_length_bytes)
        return Status::ErrorDataTooSmall();

    bssl::ScopedEVP_CIPHER_CTX ctx;
    if (!EVP_CipherInit_ex(ctx.get(), aead_alg, nullptr, nullptr, nullptr, mode)
        || !EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr)
        || !EVP_CipherInit_ex(ctx.get(), nullptr, nullptr, raw_key.data(), iv.data(), mode))
        return Status::OperationError();

    int written = 0;
    if (!additional_data.empty()
        && !EVP_CipherUpdate(ctx.get(), nullptr, &written, additional_data.data(), static_cast<int>(additional_data.size())))
        return Status::OperationError();
    const size_t payload_size = mode == DECRYPT ? data.size() - tag_length_bytes : data.size();
    // Reserve a final-block scratch area even for an empty plaintext.
    buffer->resize(payload_size + 16);
    if (!EVP_CipherUpdate(ctx.get(), buffer->data(), &written, data.data(), static_cast<int>(payload_size))) {
        buffer->clear();
        return Status::OperationError();
    }
    if (mode == DECRYPT
        && !EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, tag_length_bytes, const_cast<uint8_t*>(data.data() + payload_size))) {
        buffer->clear();
        return Status::OperationError();
    }
    int final_bytes = 0;
    if (!EVP_CipherFinal_ex(ctx.get(), buffer->data() + written, &final_bytes)) {
        buffer->clear();
        return Status::OperationError();
    }
    const size_t output_size = written + final_bytes;
    if (mode == ENCRYPT
        && !EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, tag_length_bytes, buffer->data() + output_size)) {
        buffer->clear();
        return Status::OperationError();
    }
    buffer->resize(output_size + (mode == ENCRYPT ? tag_length_bytes : 0));
    return Status::Success();
}

} // namespace webcrypto
