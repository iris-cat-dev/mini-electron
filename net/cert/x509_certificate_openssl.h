// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_CERT_X509_CERTIFICATE_OPENSSL_H_
#define NET_CERT_X509_CERTIFICATE_OPENSSL_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/gtest_prod_util.h"
#include "base/memory/ref_counted.h"
#include "base/time/time.h"
#include "net/base/hash_value.h"
#include "net/base/net_export.h"
#include "net/cert/x509_cert_types.h"

namespace base {
class Pickle;
class PickleIterator;
}

namespace net {

class X509Certificate;
using CertificateList = std::vector<scoped_refptr<X509Certificate>>;

// An immutable DER certificate and its associated, untrusted intermediates.
// Parsing is not verification: callers must retain the TLS backend's validation
// result separately in SSLInfo. This implementation uses the active OpenSSL
// backend, not BoringSSL's CRYPTO_BUFFER or certificate verifier.
class NET_EXPORT X509Certificate : public base::RefCountedThreadSafe<X509Certificate> {
public:
    enum PublicKeyType {
        kPublicKeyTypeUnknown,
        kPublicKeyTypeRSA,
        kPublicKeyTypeECDSA,
    };

    // The first element is the leaf. Every element must contain exactly one
    // DER certificate; malformed input returns nullptr, never a partial chain.
    static scoped_refptr<X509Certificate> CreateFromDERCertChain(const std::vector<std::string_view>& der_certs);
    static scoped_refptr<X509Certificate> CreateFromBytes(base::span<const uint8_t> data);
    static scoped_refptr<X509Certificate> CreateFromPickle(base::PickleIterator* pickle_iter);

    X509Certificate(const X509Certificate&) = delete;
    X509Certificate& operator=(const X509Certificate&) = delete;

    void Persist(base::Pickle* pickle) const;
    const std::string& serial_number() const { return serial_number_; }
    const CertPrincipal& subject() const { return subject_; }
    const CertPrincipal& issuer() const { return issuer_; }
    const base::Time& valid_start() const { return valid_start_; }
    const base::Time& valid_expiry() const { return valid_expiry_; }

    // IP addresses are raw 4-byte or 16-byte network-order values, not text.
    // Both outputs are cleared even when no supported SAN is present.
    bool GetSubjectAltName(std::vector<std::string>* dns_names, std::vector<std::string>* ip_addrs) const;
    bool HasExpired() const;
    bool EqualsExcludingChain(const X509Certificate* other) const;
    bool EqualsIncludingChain(const X509Certificate* other) const;
    bool IsIssuedByEncoded(const std::vector<std::string>& valid_issuers) const;

    // SAN-only Chromium name matching, not chain or validity verification.
    bool VerifyNameMatch(std::string_view hostname) const;
    static bool GetPEMEncodedFromDER(std::string_view der_encoded, std::string* pem_encoded);
    bool GetPEMEncodedChain(std::vector<std::string>* pem_encoded) const;
    static void GetPublicKeyInfo(std::string_view der_encoded, size_t* size_bits, PublicKeyType* type);
    static SHA256HashValue CalculateFingerprint256(std::string_view der_encoded);
    SHA256HashValue CalculateChainFingerprint256() const;
    static bool IsSelfSigned(std::string_view der_encoded);

    base::span<const uint8_t> cert_span() const;
    // Excludes the leaf; preserves the caller's chain order and DER bytes.
    size_t intermediate_count() const { return intermediate_der_.size(); }
    base::span<const uint8_t> intermediate_span(size_t index) const;

private:
    friend class base::RefCountedThreadSafe<X509Certificate>;
    FRIEND_TEST_ALL_PREFIXES(X509CertificateNameVerifyTest, VerifyHostname);

    X509Certificate();
    ~X509Certificate();
    bool Initialize(std::string_view der);
    static bool VerifyHostname(std::string_view hostname,
        const std::vector<std::string>& cert_san_dns_names,
        const std::vector<std::string>& cert_san_ip_addrs);

    CertPrincipal subject_;
    CertPrincipal issuer_;
    base::Time valid_start_;
    base::Time valid_expiry_;
    std::string serial_number_;
    std::string cert_der_;
    std::vector<std::string> intermediate_der_;
    std::vector<std::string> dns_names_;
    std::vector<std::string> ip_addrs_;
};

} // namespace net

#endif // NET_CERT_X509_CERTIFICATE_OPENSSL_H_
