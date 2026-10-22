// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/cert/x509_certificate.h"

#include <climits>
#include <ctime>
#include <memory>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/contains.h"
#include "base/pickle.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/base/url_util.h"
#include "third_party/openssl/openssl/include/openssl/evp.h"
#include "third_party/openssl/openssl/include/openssl/pem.h"
#include "third_party/openssl/openssl/include/openssl/sha.h"
#include "third_party/openssl/openssl/include/openssl/x509v3.h"
#include "url/url_canon.h"

namespace net {
namespace {

using CertificatePtr = std::unique_ptr<X509, decltype(&X509_free)>;

CertificatePtr ParseCertificate(std::string_view der)
{
    if (der.empty() || der.size() > LONG_MAX)
        return CertificatePtr(nullptr, X509_free);
    const auto* cursor = reinterpret_cast<const unsigned char*>(der.data());
    const auto* end = cursor + der.size();
    CertificatePtr certificate(d2i_X509(nullptr, &cursor, static_cast<long>(der.size())), X509_free);
    if (cursor != end)
        certificate.reset();
    return certificate;
}

bool ParseTime(const ASN1_TIME* encoded, base::Time* result)
{
    std::tm time = {};
    if (!encoded || ASN1_TIME_to_tm(encoded, &time) != 1)
        return false;
    base::Time::Exploded exploded = {};
    exploded.year = time.tm_year + 1900;
    exploded.month = time.tm_mon + 1;
    exploded.day_of_week = time.tm_wday;
    exploded.day_of_month = time.tm_mday;
    exploded.hour = time.tm_hour;
    exploded.minute = time.tm_min;
    exploded.second = time.tm_sec;
    return base::Time::FromUTCExploded(exploded, result);
}

void SplitOnChar(std::string_view src, char c, std::string_view* left, std::string_view* right)
{
    size_t pos = src.find(c);
    *left = src.substr(0, pos);
    *right = pos == std::string_view::npos ? std::string_view() : src.substr(pos);
}

} // namespace

X509Certificate::X509Certificate() = default;
X509Certificate::~X509Certificate() = default;

bool X509Certificate::Initialize(std::string_view der)
{
    auto certificate = ParseCertificate(der);
    if (!certificate || !subject_.ParseDistinguishedName(X509_get_subject_name(certificate.get()))
        || !issuer_.ParseDistinguishedName(X509_get_issuer_name(certificate.get()))
        || !ParseTime(X509_get0_notBefore(certificate.get()), &valid_start_)
        || !ParseTime(X509_get0_notAfter(certificate.get()), &valid_expiry_)) {
        return false;
    }

    // i2d retains the INTEGER's signed DER content, including a necessary
    // leading zero, unlike ASN1_STRING_get0_data on OpenSSL's internal integer.
    const ASN1_INTEGER* serial = X509_get0_serialNumber(certificate.get());
    int serial_length = i2d_ASN1_INTEGER(serial, nullptr);
    if (serial_length <= 0)
        return false;
    std::string encoded_serial(serial_length, '\0');
    auto* output = reinterpret_cast<unsigned char*>(encoded_serial.data());
    if (i2d_ASN1_INTEGER(serial, &output) != serial_length)
        return false;
    const auto* serial_content = reinterpret_cast<const unsigned char*>(encoded_serial.data());
    long content_length = 0;
    int tag = 0;
    int xclass = 0;
    if ((ASN1_get_object(&serial_content, &content_length, &tag, &xclass, serial_length) & 0x80)
        || tag != V_ASN1_INTEGER || content_length < 0) {
        return false;
    }
    serial_number_.assign(reinterpret_cast<const char*>(serial_content), content_length);

    int critical = -1;
    std::unique_ptr<GENERAL_NAMES, decltype(&GENERAL_NAMES_free)> names(
        static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(certificate.get(), NID_subject_alt_name, &critical, nullptr)), GENERAL_NAMES_free);
    // Missing SAN is valid metadata. A duplicate or malformed SAN is not.
    if (!names && (critical != -1 || X509_get_ext_by_NID(certificate.get(), NID_subject_alt_name, -1) >= 0))
        return false;
    if (names) {
        for (int i = 0; i < sk_GENERAL_NAME_num(names.get()); ++i) {
            const GENERAL_NAME* name = sk_GENERAL_NAME_value(names.get(), i);
            const ASN1_STRING* value = nullptr;
            if (name->type == GEN_DNS)
                value = name->d.dNSName;
            else if (name->type == GEN_IPADD)
                value = name->d.iPAddress;
            else
                continue;
            int length = ASN1_STRING_length(value);
            if (length < 0 || (name->type == GEN_IPADD && length != 4 && length != 16))
                return false;
            std::string bytes;
            if (length)
                bytes.assign(reinterpret_cast<const char*>(ASN1_STRING_get0_data(value)), length);
            if (name->type == GEN_DNS)
                dns_names_.push_back(std::move(bytes));
            else
                ip_addrs_.push_back(std::move(bytes));
        }
    }
    cert_der_.assign(der);
    return true;
}

scoped_refptr<X509Certificate> X509Certificate::CreateFromDERCertChain(const std::vector<std::string_view>& der_certs)
{
    if (der_certs.empty())
        return nullptr;
    auto result = base::WrapRefCounted(new X509Certificate());
    if (!result->Initialize(der_certs.front()))
        return nullptr;
    result->intermediate_der_.reserve(der_certs.size() - 1);
    for (size_t i = 1; i < der_certs.size(); ++i) {
        if (!ParseCertificate(der_certs[i]))
            return nullptr;
        result->intermediate_der_.emplace_back(der_certs[i]);
    }
    return result;
}

scoped_refptr<X509Certificate> X509Certificate::CreateFromBytes(base::span<const uint8_t> data)
{
    auto result = base::WrapRefCounted(new X509Certificate());
    if (!result->Initialize(base::as_string_view(data)))
        return nullptr;
    return result;
}

scoped_refptr<X509Certificate> X509Certificate::CreateFromPickle(base::PickleIterator* pickle_iter)
{
    size_t count = 0;
    if (!pickle_iter->ReadLength(&count) || !count)
        return nullptr;
    std::vector<std::string_view> chain;
    for (size_t i = 0; i < count; ++i) {
        const char* data = nullptr;
        size_t size = 0;
        if (!pickle_iter->ReadData(&data, &size))
            return nullptr;
        chain.emplace_back(data, size);
    }
    return CreateFromDERCertChain(chain);
}

void X509Certificate::Persist(base::Pickle* pickle) const
{
    CHECK_LT(intermediate_der_.size(), static_cast<size_t>(INT_MAX));
    pickle->WriteInt(static_cast<int>(intermediate_der_.size() + 1));
    pickle->WriteString(cert_der_);
    for (const auto& intermediate : intermediate_der_)
        pickle->WriteString(intermediate);
}

bool X509Certificate::GetSubjectAltName(std::vector<std::string>* dns_names, std::vector<std::string>* ip_addrs) const
{
    if (dns_names)
        *dns_names = dns_names_;
    if (ip_addrs)
        *ip_addrs = ip_addrs_;
    return !dns_names_.empty() || !ip_addrs_.empty();
}

bool X509Certificate::HasExpired() const
{
    return base::Time::Now() > valid_expiry_;
}

bool X509Certificate::EqualsExcludingChain(const X509Certificate* other) const
{
    return other && cert_der_ == other->cert_der_;
}

bool X509Certificate::EqualsIncludingChain(const X509Certificate* other) const
{
    return EqualsExcludingChain(other) && intermediate_der_ == other->intermediate_der_;
}

bool X509Certificate::IsIssuedByEncoded(const std::vector<std::string>& valid_issuers) const
{
    using NamePtr = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>;
    std::vector<NamePtr> names;
    names.reserve(valid_issuers.size());
    for (const auto& der_name : valid_issuers) {
        if (der_name.empty() || der_name.size() > LONG_MAX)
            continue;
        const auto* cursor = reinterpret_cast<const unsigned char*>(der_name.data());
        const auto* end = cursor + der_name.size();
        NamePtr name(d2i_X509_NAME(nullptr, &cursor, static_cast<long>(der_name.size())), X509_NAME_free);
        if (name && cursor == end)
            names.push_back(std::move(name));
    }
    if (names.empty())
        return false;
    auto matches = [&](std::string_view der) {
        auto certificate = ParseCertificate(der);
        if (!certificate)
            return false;
        for (const auto& name : names) {
            if (X509_NAME_cmp(name.get(), X509_get_issuer_name(certificate.get())) == 0)
                return true;
        }
        return false;
    };
    if (matches(cert_der_))
        return true;
    for (const auto& intermediate : intermediate_der_) {
        if (matches(intermediate))
            return true;
    }
    return false;
}

bool X509Certificate::VerifyNameMatch(std::string_view hostname) const
{
    return !hostname.empty() && VerifyHostname(hostname, dns_names_, ip_addrs_);
}

bool X509Certificate::GetPEMEncodedFromDER(std::string_view der_encoded, std::string* pem_encoded)
{
    if (der_encoded.empty() || der_encoded.size() > LONG_MAX)
        return false;
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    if (!bio || PEM_write_bio(bio.get(), "CERTIFICATE", "",
                    reinterpret_cast<const unsigned char*>(der_encoded.data()), static_cast<long>(der_encoded.size())) <= 0)
        return false;
    char* data = nullptr;
    long size = BIO_get_mem_data(bio.get(), &data);
    if (size <= 0)
        return false;
    pem_encoded->assign(data, size);
    return true;
}

bool X509Certificate::GetPEMEncodedChain(std::vector<std::string>* pem_encoded) const
{
    std::vector<std::string> result(intermediate_der_.size() + 1);
    if (!GetPEMEncodedFromDER(cert_der_, &result[0]))
        return false;
    for (size_t i = 0; i < intermediate_der_.size(); ++i) {
        if (!GetPEMEncodedFromDER(intermediate_der_[i], &result[i + 1]))
            return false;
    }
    *pem_encoded = std::move(result);
    return true;
}

void X509Certificate::GetPublicKeyInfo(std::string_view der_encoded, size_t* size_bits, PublicKeyType* type)
{
    *size_bits = 0;
    *type = kPublicKeyTypeUnknown;
    auto certificate = ParseCertificate(der_encoded);
    if (!certificate)
        return;
    EVP_PKEY* key = X509_get0_pubkey(certificate.get());
    if (!key)
        return;
    switch (EVP_PKEY_base_id(key)) {
    case EVP_PKEY_RSA:
    case EVP_PKEY_RSA_PSS:
        *type = kPublicKeyTypeRSA;
        break;
    case EVP_PKEY_EC:
        *type = kPublicKeyTypeECDSA;
        break;
    default:
        return;
    }
    int bits = EVP_PKEY_bits(key);
    if (bits > 0)
        *size_bits = bits;
}

SHA256HashValue X509Certificate::CalculateFingerprint256(std::string_view der_encoded)
{
    SHA256HashValue result = {};
    SHA256(reinterpret_cast<const unsigned char*>(der_encoded.data()), der_encoded.size(), result.data);
    return result;
}

SHA256HashValue X509Certificate::CalculateChainFingerprint256() const
{
    SHA256HashValue result = {};
    SHA256_CTX context;
    SHA256_Init(&context);
    SHA256_Update(&context, cert_der_.data(), cert_der_.size());
    for (const auto& intermediate : intermediate_der_)
        SHA256_Update(&context, intermediate.data(), intermediate.size());
    SHA256_Final(result.data, &context);
    return result;
}

bool X509Certificate::IsSelfSigned(std::string_view der_encoded)
{
    auto certificate = ParseCertificate(der_encoded);
    if (!certificate || X509_NAME_cmp(X509_get_subject_name(certificate.get()), X509_get_issuer_name(certificate.get())) != 0)
        return false;
    EVP_PKEY* key = X509_get0_pubkey(certificate.get());
    return key && X509_verify(certificate.get(), key) == 1;
}

bool X509Certificate::VerifyHostname(
    std::string_view hostname, const std::vector<std::string>& cert_san_dns_names, const std::vector<std::string>& cert_san_ip_addrs)
{
    DCHECK(!hostname.empty());

    if (cert_san_dns_names.empty() && cert_san_ip_addrs.empty()) {
        // Either a dNSName or iPAddress subjectAltName MUST be present in order
        // to match, so fail quickly if not.
        return false;
    }

    // Perform name verification following http://tools.ietf.org/html/rfc6125.
    // The terminology used in this method is as per that RFC:-
    // Reference identifier == the host the local user/agent is intending to
    //                         access, i.e. the thing displayed in the URL bar.
    // Presented identifier(s) == name(s) the server knows itself as, in its cert.

    // CanonicalizeHost requires surrounding brackets to parse an IPv6 address.
    const std::string host_or_ip = hostname.find(':') != std::string::npos ? base::StrCat({ "[", hostname, "]" }) : std::string(hostname);
    url::CanonHostInfo host_info;
    std::string reference_name = CanonicalizeHost(host_or_ip, &host_info);

    // If the host cannot be canonicalized, fail fast.
    if (reference_name.empty())
        return false;

    // Fully handle all cases where |hostname| contains an IP address.
    if (host_info.IsIPAddress()) {
        std::string_view ip_addr_string(reinterpret_cast<const char*>(host_info.address), host_info.AddressLength());
        return base::Contains(cert_san_ip_addrs, ip_addr_string);
    }

    // The host portion of a URL may support a variety of name resolution formats
    // and services. However, the only supported name types in this code are IP
    // addresses, which have been handled above via iPAddress subjectAltNames,
    // and DNS names, via dNSName subjectAltNames.
    // Validate that the host conforms to the DNS preferred name syntax, in
    // either relative or absolute form, and exclude the "root" label for DNS.
    if (reference_name == "." || !IsCanonicalizedHostCompliant(reference_name))
        return false;

    // CanonicalizeHost does not normalize absolute vs relative DNS names. If
    // the input name was absolute (included trailing .), normalize it as if it
    // was relative.
    if (reference_name.back() == '.')
        reference_name.pop_back();

    // |reference_domain| is the remainder of |host| after the leading host
    // component is stripped off, but includes the leading dot e.g.
    // "www.f.com" -> ".f.com".
    // If there is no meaningful domain part to |host| (e.g. it contains no dots)
    // then |reference_domain| will be empty.
    std::string_view reference_host, reference_domain;
    SplitOnChar(reference_name, '.', &reference_host, &reference_domain);
    bool allow_wildcards = false;
    if (!reference_domain.empty()) {
        DCHECK(reference_domain.starts_with("."));

        // Do not allow wildcards for public/ICANN registry controlled domains -
        // that is, prevent *.com or *.co.uk as valid presented names, but do not
        // prevent *.appspot.com (a private registry controlled domain).
        // In addition, unknown top-level domains (such as 'intranet' domains or
        // new TLDs/gTLDs not yet added to the registry controlled domain dataset)
        // are also implicitly prevented.
        // Because |reference_domain| must contain at least one name component that
        // is not registry controlled, this ensures that all reference domains
        // contain at least three domain components when using wildcards.
        size_t registry_length = registry_controlled_domains::GetCanonicalHostRegistryLength(
            reference_name, registry_controlled_domains::INCLUDE_UNKNOWN_REGISTRIES, registry_controlled_domains::EXCLUDE_PRIVATE_REGISTRIES);

        // Because |reference_name| was already canonicalized, the following
        // should never happen.
        CHECK_NE(std::string::npos, registry_length);

        // Account for the leading dot in |reference_domain|.
        bool is_registry_controlled = registry_length != 0 && registry_length == (reference_domain.size() - 1);

        // Additionally, do not attempt wildcard matching for purely numeric
        // hostnames.
        allow_wildcards = !is_registry_controlled && reference_name.find_first_not_of("0123456789.") != std::string::npos;
    }

    // Now step through the DNS names doing wild card comparison (if necessary)
    // on each against the reference name.
    for (const auto& cert_san_dns_name : cert_san_dns_names) {
        // Catch badly corrupt cert names up front.
        if (cert_san_dns_name.empty() || cert_san_dns_name.find('\0') != std::string::npos) {
            continue;
        }
        std::string presented_name(base::ToLowerASCII(cert_san_dns_name));

        // Remove trailing dot, if any.
        if (*presented_name.rbegin() == '.')
            presented_name.resize(presented_name.length() - 1);

        // The hostname must be at least as long as the cert name it is matching,
        // as we require the wildcard (if present) to match at least one character.
        if (presented_name.length() > reference_name.length())
            continue;

        std::string_view presented_host, presented_domain;
        SplitOnChar(presented_name, '.', &presented_host, &presented_domain);

        if (presented_domain != reference_domain)
            continue;

        if (presented_host != "*") {
            if (presented_host == reference_host)
                return true;
            continue;
        }

        if (!allow_wildcards)
            continue;

        return true;
    }
    return false;
}

base::span<const uint8_t> X509Certificate::cert_span() const
{
    return base::as_byte_span(cert_der_);
}

base::span<const uint8_t> X509Certificate::intermediate_span(size_t index) const
{
    CHECK_LT(index, intermediate_der_.size());
    return base::as_byte_span(intermediate_der_[index]);
}

} // namespace net
