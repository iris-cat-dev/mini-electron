// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/webcrypto/algorithms/asymmetric_key_util.h"

#include <climits>
#include <memory>
#include <utility>

#include "components/webcrypto/algorithms/util.h"
#include "components/webcrypto/blink_key_handle.h"
#include "components/webcrypto/status.h"
#include "crypto/openssl_util.h"
#include "third_party/openssl/openssl/include/openssl/evp.h"
#include "third_party/openssl/openssl/include/openssl/x509.h"

namespace webcrypto {

Status ExportPKeySpki(EVP_PKEY* key, std::vector<uint8_t>* buffer)
{
    crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);
    const int length = i2d_PUBKEY(key, nullptr);
    if (length <= 0)
        return Status::OperationError();
    buffer->resize(length);
    uint8_t* output = buffer->data();
    if (i2d_PUBKEY(key, &output) != length)
        return Status::OperationError();
    return Status::Success();
}

Status ExportPKeyPkcs8(EVP_PKEY* key, std::vector<uint8_t>* buffer)
{
    crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);
    std::unique_ptr<PKCS8_PRIV_KEY_INFO, decltype(&PKCS8_PRIV_KEY_INFO_free)> info(EVP_PKEY2PKCS8(key), PKCS8_PRIV_KEY_INFO_free);
    if (!info)
        return Status::OperationError();
    const int length = i2d_PKCS8_PRIV_KEY_INFO(info.get(), nullptr);
    if (length <= 0)
        return Status::OperationError();
    buffer->resize(length);
    uint8_t* output = buffer->data();
    if (i2d_PKCS8_PRIV_KEY_INFO(info.get(), &output) != length)
        return Status::OperationError();
    return Status::Success();
}

Status CreateWebCryptoPublicKey(bssl::UniquePtr<EVP_PKEY> public_key, const blink::WebCryptoKeyAlgorithm& algorithm, bool extractable,
    blink::WebCryptoKeyUsageMask usages, blink::WebCryptoKey* key)
{
    *key = blink::WebCryptoKey::Create(CreateAsymmetricKeyHandle(std::move(public_key)), blink::kWebCryptoKeyTypePublic, extractable, algorithm, usages);
    return Status::Success();
}

Status CreateWebCryptoPrivateKey(bssl::UniquePtr<EVP_PKEY> private_key, const blink::WebCryptoKeyAlgorithm& algorithm, bool extractable,
    blink::WebCryptoKeyUsageMask usages, blink::WebCryptoKey* key)
{
    *key = blink::WebCryptoKey::Create(CreateAsymmetricKeyHandle(std::move(private_key)), blink::kWebCryptoKeyTypePrivate, extractable, algorithm, usages);
    return Status::Success();
}

Status ImportUnverifiedPkeyFromSpki(base::span<const uint8_t> key_data, int expected_pkey_id, bssl::UniquePtr<EVP_PKEY>* out_pkey)
{
    crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);
    if (key_data.empty() || key_data.size() > LONG_MAX)
        return Status::DataError();
    const uint8_t* input = key_data.data();
    bssl::UniquePtr<EVP_PKEY> pkey(d2i_PUBKEY(nullptr, &input, key_data.size()));
    if (!pkey || input != key_data.data() + key_data.size() || EVP_PKEY_id(pkey.get()) != expected_pkey_id)
        return Status::DataError();
    *out_pkey = std::move(pkey);
    return Status::Success();
}

Status ImportUnverifiedPkeyFromPkcs8(base::span<const uint8_t> key_data, int expected_pkey_id, bssl::UniquePtr<EVP_PKEY>* out_pkey)
{
    crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);
    if (key_data.empty() || key_data.size() > LONG_MAX)
        return Status::DataError();
    const uint8_t* input = key_data.data();
    std::unique_ptr<PKCS8_PRIV_KEY_INFO, decltype(&PKCS8_PRIV_KEY_INFO_free)> info(
        d2i_PKCS8_PRIV_KEY_INFO(nullptr, &input, key_data.size()), PKCS8_PRIV_KEY_INFO_free);
    if (!info || input != key_data.data() + key_data.size())
        return Status::DataError();
    bssl::UniquePtr<EVP_PKEY> pkey(EVP_PKCS82PKEY(info.get()));
    if (!pkey || EVP_PKEY_id(pkey.get()) != expected_pkey_id)
        return Status::DataError();
    *out_pkey = std::move(pkey);
    return Status::Success();
}

Status GetUsagesForGenerateAsymmetricKey(blink::WebCryptoKeyUsageMask combined_usages, blink::WebCryptoKeyUsageMask all_public_usages,
    blink::WebCryptoKeyUsageMask all_private_usages, blink::WebCryptoKeyUsageMask* public_usages, blink::WebCryptoKeyUsageMask* private_usages)
{
    Status status = CheckKeyCreationUsages(all_public_usages | all_private_usages, combined_usages);
    if (status.IsError())
        return status;
    *public_usages = combined_usages & all_public_usages;
    *private_usages = combined_usages & all_private_usages;
    // Empty private usages are rejected by the higher-level operation.
    return Status::Success();
}

} // namespace webcrypto
