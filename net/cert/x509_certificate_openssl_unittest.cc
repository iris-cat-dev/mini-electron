// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/cert/x509_certificate.h"

#include <memory>
#include <string>
#include <vector>

#include "base/pickle.h"
#include "net/ssl/ssl_info.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/openssl/openssl/include/openssl/ec.h"
#include "third_party/openssl/openssl/include/openssl/evp.h"
#include "third_party/openssl/openssl/include/openssl/x509v3.h"

namespace net {
namespace {

// Generate and sign actual X.509 data: no synthetic metadata or test-only
// certificate parser. The cert is self-signed and is never treated as trusted.
class X509CertificateOpenSSLTest : public testing::Test {
protected:
    void SetUp() override
    {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
            EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
        ASSERT_TRUE(context);
        ASSERT_EQ(1, EVP_PKEY_keygen_init(context.get()));
        ASSERT_EQ(1, EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), NID_X9_62_prime256v1));
        EVP_PKEY* key = nullptr;
        ASSERT_EQ(1, EVP_PKEY_keygen(context.get(), &key));
        key_.reset(key);
    }

    void MakeCertificate(const char* common_name, const char* san, std::string* der)
    {
        std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
        ASSERT_TRUE(certificate);
        ASSERT_EQ(1, X509_set_version(certificate.get(), 2));
        ASSERT_EQ(1, ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 128));
        ASSERT_EQ(1, ASN1_TIME_set_string_X509(X509_getm_notBefore(certificate.get()), "20240101000000Z"));
        ASSERT_EQ(1, ASN1_TIME_set_string_X509(X509_getm_notAfter(certificate.get()), "20300101000000Z"));
        X509_NAME* subject = X509_get_subject_name(certificate.get());
        ASSERT_EQ(1, X509_NAME_add_entry_by_NID(subject, NID_commonName, MBSTRING_UTF8,
                         reinterpret_cast<const unsigned char*>(common_name), -1, -1, 0));
        ASSERT_EQ(1, X509_set_issuer_name(certificate.get(), subject));
        ASSERT_EQ(1, X509_set_pubkey(certificate.get(), key_.get()));
        if (san) {
            std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
                X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, san), X509_EXTENSION_free);
            ASSERT_TRUE(extension);
            ASSERT_EQ(1, X509_add_ext(certificate.get(), extension.get(), -1));
        }
        ASSERT_GT(X509_sign(certificate.get(), key_.get(), EVP_sha256()), 0);
        int length = i2d_X509(certificate.get(), nullptr);
        ASSERT_GT(length, 0);
        der->resize(length);
        auto* output = reinterpret_cast<unsigned char*>(der->data());
        ASSERT_EQ(length, i2d_X509(certificate.get(), &output));
    }

    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key_{nullptr, EVP_PKEY_free};
};

TEST_F(X509CertificateOpenSSLTest, RealMetadataAndBinarySubjectAltNames)
{
    std::string der;
    ASSERT_NO_FATAL_FAILURE(MakeCertificate("display.example", "DNS:example.test,IP:127.0.0.1,IP:2001:db8::1", &der));
    auto certificate = X509Certificate::CreateFromBytes(base::as_byte_span(der));
    ASSERT_TRUE(certificate);
    EXPECT_EQ("display.example", certificate->subject().common_name);
    EXPECT_EQ("display.example", certificate->issuer().common_name);
    EXPECT_EQ(std::string("\0\x80", 2), certificate->serial_number());
    EXPECT_EQ(1704067200, certificate->valid_start().InSecondsFSinceUnixEpoch());
    EXPECT_EQ(1893456000, certificate->valid_expiry().InSecondsFSinceUnixEpoch());
    EXPECT_EQ(der, base::as_string_view(certificate->cert_span()));
    std::vector<std::string> dns, ips;
    ASSERT_TRUE(certificate->GetSubjectAltName(&dns, &ips));
    EXPECT_EQ((std::vector<std::string>{"example.test"}), dns);
    ASSERT_EQ(2u, ips.size());
    EXPECT_EQ(std::string("\x7f\0\0\1", 4), ips[0]);
    EXPECT_EQ(16u, ips[1].size());
    EXPECT_TRUE(certificate->VerifyNameMatch("example.test"));
    EXPECT_TRUE(certificate->VerifyNameMatch("127.0.0.1"));
    EXPECT_TRUE(certificate->VerifyNameMatch("2001:db8::1"));
    EXPECT_FALSE(certificate->VerifyNameMatch("display.example"));
    EXPECT_FALSE(certificate->VerifyNameMatch("wrong.example"));
    EXPECT_TRUE(X509Certificate::IsSelfSigned(der));
}

TEST_F(X509CertificateOpenSSLTest, ChainSerializationAndSSLInfoOwnership)
{
    std::string leaf, intermediate;
    ASSERT_NO_FATAL_FAILURE(MakeCertificate("leaf", "DNS:example.test", &leaf));
    ASSERT_NO_FATAL_FAILURE(MakeCertificate("intermediate", nullptr, &intermediate));
    auto certificate = X509Certificate::CreateFromDERCertChain({leaf, intermediate});
    ASSERT_TRUE(certificate);
    ASSERT_EQ(1u, certificate->intermediate_count());
    EXPECT_EQ(intermediate, base::as_string_view(certificate->intermediate_span(0)));
    base::Pickle pickle;
    certificate->Persist(&pickle);
    base::PickleIterator iterator(pickle);
    auto decoded = X509Certificate::CreateFromPickle(&iterator);
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(certificate->EqualsIncludingChain(decoded.get()));
    std::vector<std::string> pem;
    ASSERT_TRUE(decoded->GetPEMEncodedChain(&pem));
    ASSERT_EQ(2u, pem.size());
    EXPECT_TRUE(pem[0].starts_with("-----BEGIN CERTIFICATE-----"));
    SSLInfo info;
    EXPECT_FALSE(info.is_valid());
    info.cert = certificate;
    SSLInfo copy(info);
    info.Reset();
    EXPECT_FALSE(info.is_valid());
    ASSERT_TRUE(copy.is_valid());
    EXPECT_TRUE(copy.cert->EqualsIncludingChain(certificate.get()));
    EXPECT_FALSE(copy.is_issued_by_known_root);
}

TEST_F(X509CertificateOpenSSLTest, RejectsMalformedOrPartialChains)
{
    std::string der;
    ASSERT_NO_FATAL_FAILURE(MakeCertificate("leaf", nullptr, &der));
    EXPECT_FALSE(X509Certificate::CreateFromDERCertChain({}));
    EXPECT_FALSE(X509Certificate::CreateFromBytes(base::as_byte_span(std::string_view())));
    EXPECT_FALSE(X509Certificate::CreateFromBytes(base::as_byte_span(der).first(der.size() - 1)));
    EXPECT_FALSE(X509Certificate::CreateFromBytes(base::as_byte_span(der + "trailing data")));
    EXPECT_FALSE(X509Certificate::CreateFromDERCertChain({der, "not a certificate"}));
    base::Pickle incomplete;
    incomplete.WriteInt(2);
    incomplete.WriteString(der);
    base::PickleIterator iterator(incomplete);
    EXPECT_FALSE(X509Certificate::CreateFromPickle(&iterator));
}

TEST_F(X509CertificateOpenSSLTest, NoCommonNameFallbackOrPublicSuffixWildcard)
{
    std::string der;
    ASSERT_NO_FATAL_FAILURE(MakeCertificate("example.test", nullptr, &der));
    auto certificate = X509Certificate::CreateFromBytes(base::as_byte_span(der));
    ASSERT_TRUE(certificate);
    std::vector<std::string> dns{"stale"}, ips{"stale"};
    EXPECT_FALSE(certificate->GetSubjectAltName(&dns, &ips));
    EXPECT_TRUE(dns.empty());
    EXPECT_TRUE(ips.empty());
    EXPECT_FALSE(certificate->VerifyNameMatch("example.test"));
    ASSERT_NO_FATAL_FAILURE(MakeCertificate("wildcard", "DNS:*.com,DNS:*.example.com", &der));
    certificate = X509Certificate::CreateFromBytes(base::as_byte_span(der));
    ASSERT_TRUE(certificate);
    EXPECT_FALSE(certificate->VerifyNameMatch("example.com"));
    EXPECT_TRUE(certificate->VerifyNameMatch("www.example.com"));
    EXPECT_FALSE(certificate->VerifyNameMatch("a.b.example.com"));
}

} // namespace
} // namespace net
