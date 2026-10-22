// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>

#include <memory>

#include "components/webcrypto/algorithm_implementation.h"
#include "components/webcrypto/algorithms/secret_key_util.h"
#include "components/webcrypto/algorithms/util.h"
#include "components/webcrypto/blink_key_handle.h"
#include "components/webcrypto/status.h"
#include "crypto/openssl_util.h"
#include "third_party/blink/public/platform/web_crypto_algorithm_params.h"
#include "third_party/blink/public/platform/web_crypto_key_algorithm.h"
#include <climits>
#include "third_party/openssl/openssl/include/openssl/evp.h"
#include "third_party/openssl/openssl/include/openssl/kdf.h"

namespace webcrypto {

namespace {

const blink::WebCryptoKeyUsageMask kValidUsages = blink::kWebCryptoKeyUsageDeriveKey | blink::kWebCryptoKeyUsageDeriveBits;

class HkdfImplementation : public AlgorithmImplementation {
public:
    HkdfImplementation()
    {
    }

    Status ImportKey(blink::WebCryptoKeyFormat format, base::span<const uint8_t> key_data, const blink::WebCryptoAlgorithm& algorithm, bool extractable,
        blink::WebCryptoKeyUsageMask usages, blink::WebCryptoKey* key) const override
    {
        switch (format) {
        case blink::kWebCryptoKeyFormatRaw:
            return ImportKeyRaw(key_data, algorithm, extractable, usages, key);
        default:
            return Status::ErrorUnsupportedImportKeyFormat();
        }
    }

    Status ImportKeyRaw(base::span<const uint8_t> key_data, const blink::WebCryptoAlgorithm& algorithm, bool extractable, blink::WebCryptoKeyUsageMask usages,
        blink::WebCryptoKey* key) const
    {
        Status status = CheckKeyCreationUsages(kValidUsages, usages);
        if (status.IsError())
            return status;

        if (extractable)
            return Status::ErrorImportExtractableKdfKey();

        return CreateWebCryptoSecretKey(
            key_data, blink::WebCryptoKeyAlgorithm::CreateWithoutParams(blink::kWebCryptoAlgorithmIdHkdf), extractable, usages, key);
    }

    Status DeriveBits(const blink::WebCryptoAlgorithm& algorithm, const blink::WebCryptoKey& base_key, bool has_optional_length_bits,
        unsigned int optional_length_bits, std::vector<uint8_t>* derived_bytes) const override
    {
        crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);
        if (!has_optional_length_bits)
            return Status::ErrorHkdfDeriveBitsLengthNotSpecified();
        if (optional_length_bits % 8)
            return Status::ErrorHkdfLengthNotWholeByte();
        const auto* params = algorithm.HkdfParams();
        const EVP_MD* digest = GetDigest(params->GetHash());
        if (!digest)
            return Status::ErrorUnsupported();
        size_t length = optional_length_bits / 8;
        if (length > 255u * EVP_MD_size(digest))
            return Status::ErrorHkdfLengthTooLong();
        const auto& raw_key = GetSymmetricKeyData(base_key);
        if (raw_key.size() > INT_MAX || params->Salt().size() > INT_MAX || params->Info().size() > INT_MAX)
            return Status::OperationError();
        derived_bytes->resize(length);
        if (!length)
            return Status::Success();
        // OpenSSL distinguishes a missing key from a valid empty input key.
        const uint8_t empty_key = 0;
        const uint8_t* key = raw_key.empty() ? &empty_key : raw_key.data();
        bssl::UniquePtr<EVP_PKEY_CTX> ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr));
        if (!ctx || EVP_PKEY_derive_init(ctx.get()) <= 0
            || EVP_PKEY_CTX_set_hkdf_md(ctx.get(), digest) <= 0
            || EVP_PKEY_CTX_set1_hkdf_salt(ctx.get(), params->Salt().data(), params->Salt().size()) <= 0
            || EVP_PKEY_CTX_set1_hkdf_key(ctx.get(), key, raw_key.size()) <= 0
            || EVP_PKEY_CTX_add1_hkdf_info(ctx.get(), params->Info().data(), params->Info().size()) <= 0
            || EVP_PKEY_derive(ctx.get(), derived_bytes->data(), &length) <= 0)
            return Status::OperationError();
        return Status::Success();
    }

    Status DeserializeKeyForClone(const blink::WebCryptoKeyAlgorithm& algorithm, blink::WebCryptoKeyType type, bool extractable,
        blink::WebCryptoKeyUsageMask usages, base::span<const uint8_t> key_data, blink::WebCryptoKey* key) const override
    {
        if (algorithm.ParamsType() != blink::kWebCryptoKeyAlgorithmParamsTypeNone || type != blink::kWebCryptoKeyTypeSecret)
            return Status::ErrorUnexpected();

        // NOTE: Unlike ImportKeyRaw(), this does not enforce extractable==false.
        // This is intentional. Although keys cannot currently be created with
        // extractable==true, earlier implementations permitted this, so
        // de-serialization by structured clone should not reject them.
        return CreateWebCryptoSecretKey(key_data, algorithm, extractable, usages, key);
    }

    Status GetKeyLength(const blink::WebCryptoAlgorithm& key_length_algorithm, bool* has_length_bits, unsigned int* length_bits) const override
    {
        *has_length_bits = false;
        return Status::Success();
    }
};

} // namespace

std::unique_ptr<AlgorithmImplementation> CreateHkdfImplementation()
{
    return std::make_unique<HkdfImplementation>();
}

} // namespace webcrypto
