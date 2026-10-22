// Copyright 2011 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/ssl/ssl_cipher_suite_names.h"

#include <ostream>

#include "base/notreached.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "net/ssl/ssl_connection_status_flags.h"
#if defined(MINI_ELECTRON_USE_OPENSSL)
#include <openssl/ssl.h>
#else
#include "third_party/boringssl/src/include/openssl/ssl.h"
#endif

namespace net {

namespace {

const SSL_CIPHER* CipherByValue(uint16_t cipher_suite)
{
#if defined(MINI_ELECTRON_USE_OPENSSL)
    // OpenSSL's lookup needs an SSL method, not a live connection.
    struct CipherLookup {
        CipherLookup()
            : context(SSL_CTX_new(TLS_method()))
            , ssl(context ? SSL_new(context) : nullptr)
        {
        }
        SSL_CTX* context;
        SSL* ssl;
    };
    static const CipherLookup lookup;
    if (!lookup.ssl)
        return nullptr;
    const unsigned char wire_value[] = {
        static_cast<unsigned char>(cipher_suite >> 8),
        static_cast<unsigned char>(cipher_suite),
    };
    return SSL_CIPHER_find(lookup.ssl, wire_value);
#else
    return SSL_get_cipher_by_value(cipher_suite);
#endif
}

int ObsoleteSSLStatusForProtocol(int ssl_version)
{
    int obsolete_ssl = OBSOLETE_SSL_NONE;
    if (ssl_version < SSL_CONNECTION_VERSION_TLS1_2)
        obsolete_ssl |= OBSOLETE_SSL_MASK_PROTOCOL;
    return obsolete_ssl;
}

int ObsoleteSSLStatusForCipherSuite(uint16_t cipher_suite)
{
    int obsolete_ssl = OBSOLETE_SSL_NONE;

    const SSL_CIPHER* cipher = CipherByValue(cipher_suite);
    if (!cipher) {
        // Cannot determine/unknown cipher suite. Err on the side of caution.
        obsolete_ssl |= OBSOLETE_SSL_MASK_KEY_EXCHANGE;
        obsolete_ssl |= OBSOLETE_SSL_MASK_CIPHER;
        return obsolete_ssl;
    }

    if (SSL_CIPHER_get_kx_nid(cipher) == NID_kx_rsa) {
        obsolete_ssl |= OBSOLETE_SSL_MASK_KEY_EXCHANGE;
    }

    if (!SSL_CIPHER_is_aead(cipher)) {
        obsolete_ssl |= OBSOLETE_SSL_MASK_CIPHER;
    }

    return obsolete_ssl;
}

int ObsoleteSSLStatusForSignature(uint16_t signature_algorithm)
{
    switch (signature_algorithm) {
    case 0x0203: // ecdsa_sha1
    case 0xff01: // rsa_pkcs1_md5_sha1 (legacy internal value)
    case 0x0201: // rsa_pkcs1_sha1
        return OBSOLETE_SSL_MASK_SIGNATURE;
    default:
        return OBSOLETE_SSL_NONE;
    }
}

} // namespace

void SSLCipherSuiteToStrings(const char** key_exchange_str, const char** cipher_str, const char** mac_str, bool* is_aead, bool* is_tls13, uint16_t cipher_suite)
{
    *key_exchange_str = *cipher_str = *mac_str = "???";
    *is_aead = false;
    *is_tls13 = false;

    const SSL_CIPHER* cipher = CipherByValue(cipher_suite);
    if (!cipher)
        return;

    switch (SSL_CIPHER_get_kx_nid(cipher)) {
    case NID_kx_any:
        *key_exchange_str = nullptr;
        *is_tls13 = true;
        break;
    case NID_kx_rsa:
        *key_exchange_str = "RSA";
        break;
    case NID_kx_ecdhe:
        switch (SSL_CIPHER_get_auth_nid(cipher)) {
        case NID_auth_rsa:
            *key_exchange_str = "ECDHE_RSA";
            break;
        case NID_auth_ecdsa:
            *key_exchange_str = "ECDHE_ECDSA";
            break;
        }
        break;
    }

    switch (SSL_CIPHER_get_cipher_nid(cipher)) {
    case NID_aes_128_gcm:
        *cipher_str = "AES_128_GCM";
        break;
    case NID_aes_256_gcm:
        *cipher_str = "AES_256_GCM";
        break;
    case NID_chacha20_poly1305:
        *cipher_str = "CHACHA20_POLY1305";
        break;
    case NID_aes_128_cbc:
        *cipher_str = "AES_128_CBC";
        break;
    case NID_aes_256_cbc:
        *cipher_str = "AES_256_CBC";
        break;
    case NID_des_ede3_cbc:
        *cipher_str = "3DES_EDE_CBC";
        break;
    }

    if (SSL_CIPHER_is_aead(cipher)) {
        *is_aead = true;
        *mac_str = nullptr;
    } else {
        switch (SSL_CIPHER_get_digest_nid(cipher)) {
        case NID_sha1:
            *mac_str = "HMAC-SHA1";
            break;
        case NID_sha256:
            *mac_str = "HMAC-SHA256";
            break;
        case NID_sha384:
            *mac_str = "HMAC-SHA384";
            break;
        }
    }
}

void SSLVersionToString(const char** name, int ssl_version)
{
    switch (ssl_version) {
    case SSL_CONNECTION_VERSION_SSL2:
        *name = "SSL 2.0";
        break;
    case SSL_CONNECTION_VERSION_SSL3:
        *name = "SSL 3.0";
        break;
    case SSL_CONNECTION_VERSION_TLS1:
        *name = "TLS 1.0";
        break;
    case SSL_CONNECTION_VERSION_TLS1_1:
        *name = "TLS 1.1";
        break;
    case SSL_CONNECTION_VERSION_TLS1_2:
        *name = "TLS 1.2";
        break;
    case SSL_CONNECTION_VERSION_TLS1_3:
        *name = "TLS 1.3";
        break;
    case SSL_CONNECTION_VERSION_QUIC:
        *name = "QUIC";
        break;
    default:
        NOTREACHED() << ssl_version;
    }
}

const char* SSLGroupToName(uint16_t group)
{
    // IANA TLS Supported Groups registry values (not OpenSSL NIDs).
    switch (group) {
    case 1: return "sect163k1";
    case 2: return "sect163r1";
    case 3: return "sect163r2";
    case 4: return "sect193r1";
    case 5: return "sect193r2";
    case 6: return "sect233k1";
    case 7: return "sect233r1";
    case 8: return "sect239k1";
    case 9: return "sect283k1";
    case 10: return "sect283r1";
    case 11: return "sect409k1";
    case 12: return "sect409r1";
    case 13: return "sect571k1";
    case 14: return "sect571r1";
    case 15: return "secp160k1";
    case 16: return "secp160r1";
    case 17: return "secp160r2";
    case 18: return "secp192k1";
    case 19: return "secp192r1";
    case 20: return "secp224k1";
    case 21: return "secp224r1";
    case 22: return "secp256k1";
    case 23: return "P-256";
    case 24: return "P-384";
    case 25: return "P-521";
    case 26: return "brainpoolP256r1";
    case 27: return "brainpoolP384r1";
    case 28: return "brainpoolP512r1";
    case 29: return "X25519";
    case 30: return "X448";
    case 31: return "brainpoolP256r1tls13";
    case 32: return "brainpoolP384r1tls13";
    case 33: return "brainpoolP512r1tls13";
    case 34: return "GC256A";
    case 35: return "GC256B";
    case 36: return "GC256C";
    case 37: return "GC256D";
    case 38: return "GC512A";
    case 39: return "GC512B";
    case 40: return "GC512C";
    case 41: return "curveSM2";
    case 256: return "ffdhe2048";
    case 257: return "ffdhe3072";
    case 258: return "ffdhe4096";
    case 259: return "ffdhe6144";
    case 260: return "ffdhe8192";
    case 4588: return "X25519MLKEM768";
    case 4587: return "SecP256r1MLKEM768";
    case 4589: return "SecP384r1MLKEM1024";
    case 25497: return "X25519Kyber768Draft00";
    default: return nullptr;
    }
}

bool ParseSSLCipherString(const std::string& cipher_string, uint16_t* cipher_suite)
{
    int value = 0;
    if (cipher_string.size() == 6 && base::StartsWith(cipher_string, "0x", base::CompareCase::INSENSITIVE_ASCII)
        && base::HexStringToInt(cipher_string, &value)) {
        *cipher_suite = static_cast<uint16_t>(value);
        return true;
    }
    return false;
}

int ObsoleteSSLStatus(int connection_status, uint16_t signature_algorithm)
{
    int obsolete_ssl = OBSOLETE_SSL_NONE;

    int ssl_version = SSLConnectionStatusToVersion(connection_status);
    obsolete_ssl |= ObsoleteSSLStatusForProtocol(ssl_version);

    uint16_t cipher_suite = SSLConnectionStatusToCipherSuite(connection_status);
    obsolete_ssl |= ObsoleteSSLStatusForCipherSuite(cipher_suite);

    obsolete_ssl |= ObsoleteSSLStatusForSignature(signature_algorithm);

    return obsolete_ssl;
}

bool IsTLSCipherSuiteAllowedByHTTP2(uint16_t cipher_suite)
{
    return ObsoleteSSLStatusForCipherSuite(cipher_suite) == OBSOLETE_SSL_NONE;
}

} // namespace net
