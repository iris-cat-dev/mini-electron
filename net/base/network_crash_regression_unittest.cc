// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/proxy_chain.h"
#include "net/base/url_util.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_response_headers.h"
#include "net/http/structured_headers.h"
#include "net/ssl/ssl_cipher_suite_names.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/browsing_topics_parser.h"
#include "services/network/public/cpp/cors/cors.h"
#include "services/network/public/cpp/fence_event_reporting_parser.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {
namespace {

TEST(NetworkCrashRegression, StructuredHeadersRoundTripAndRejectMalformed)
{
    const structured_headers::Item bytes(std::string("\0\xff", 2), structured_headers::Item::kByteSequenceType);
    auto serialized = structured_headers::SerializeItem(bytes);
    ASSERT_TRUE(serialized);
    EXPECT_EQ(":AP8=:", *serialized);
    auto parsed = structured_headers::ParseBareItem(*serialized);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(bytes, *parsed);
    EXPECT_EQ("12345", structured_headers::SerializeItem(structured_headers::Item(int64_t{12345})).value());
    EXPECT_FALSE(structured_headers::ParseItem("retry;allowed-origin=\"unterminated"));
    EXPECT_FALSE(structured_headers::ParseBareItem("?1 trailing"));
}

TEST(NetworkCrashRegression, RedirectUpdatesMethodFragmentCookiesAndReferrer)
{
    const GURL original("https://example.com/form#fragment");
    const auto redirect = RedirectInfo::ComputeRedirectInfo("POST", original, SiteForCookies::FromUrl(original),
        RedirectInfo::FirstPartyURLPolicy::UPDATE_URL_ON_REDIRECT, ReferrerPolicy::NEVER_CLEAR,
        "https://example.com/form", 302, GURL("https://other.example/next"), std::string("no-referrer"), false, true, false);
    EXPECT_EQ(302, redirect.status_code);
    EXPECT_EQ("GET", redirect.new_method);
    EXPECT_EQ(GURL("https://other.example/next#fragment"), redirect.new_url);
    EXPECT_TRUE(redirect.new_referrer.empty());
    EXPECT_TRUE(redirect.new_site_for_cookies.IsFirstParty(redirect.new_url));
}

TEST(NetworkCrashRegression, StorageAccessHeaderRequiresValidAllowedOrigin)
{
    auto headers = HttpResponseHeaders::Builder(HttpVersion(1, 1), "200 OK")
        .AddHeader("Activate-Storage-Access", "retry;allowed-origin=\"https://example.com\"").Build();
    const std::string expected("https://example.com");
    const std::string other("https://other.example");
    EXPECT_TRUE(headers->HasStorageAccessRetryHeader(&expected));
    EXPECT_FALSE(headers->HasStorageAccessRetryHeader(&other));
    EXPECT_FALSE(headers->HasStorageAccessRetryHeader(nullptr));
    auto malformed = HttpResponseHeaders::Builder(HttpVersion(1, 1), "200 OK")
        .AddHeader("Activate-Storage-Access", "retry;allowed-origin=*").AddHeader("Activate-Storage-Access", "junk").Build();
    EXPECT_FALSE(malformed->HasStorageAccessRetryHeader(&expected));
}

TEST(NetworkCrashRegression, BooleanResponseHeadersAreStrict)
{
    auto headers = HttpResponseHeaders::Builder(HttpVersion(1, 1), "200 OK")
        .AddHeader("Observe-Browsing-Topics", "?1")
        .AddHeader("Allow-Cross-Origin-Event-Reporting", "true").Build();
    EXPECT_TRUE(network::ParseObserveBrowsingTopicsFromHeader(*headers));
    EXPECT_FALSE(network::ParseAllowCrossOriginEventReportingFromHeader(*headers));
}

TEST(NetworkCrashRegression, ProxyFormattingAndHostClassification)
{
    EXPECT_EQ("INVALID PROXY CHAIN", ProxyChain().ToDebugString());
    EXPECT_EQ("[direct://]", ProxyChain::Direct().ToDebugString());
    EXPECT_EQ("[https://proxy.example:443]", ProxyChain::FromSchemeHostAndPort(ProxyServer::SCHEME_HTTPS, "proxy.example", "443").ToDebugString());
    EXPECT_TRUE(IsHostnameNonUnique("intranet"));
    EXPECT_FALSE(IsHostnameNonUnique("example.com"));
}

TEST(NetworkCrashRegression, CorsSafelistDoesNotPermitUnsafeInputs)
{
    EXPECT_TRUE(network::cors::IsCorsSafelistedMethod("GET"));
    EXPECT_FALSE(network::cors::IsCorsSafelistedMethod("DELETE"));
    EXPECT_TRUE(network::cors::IsForbiddenMethod("CONNECT"));
    EXPECT_TRUE(network::cors::IsCorsSafelistedHeader("content-type", "text/plain"));
    EXPECT_FALSE(network::cors::IsCorsSafelistedHeader("content-type", "application/json"));
    EXPECT_FALSE(network::cors::IsCorsSafelistedHeader("accept", std::string(129, 'a')));
    EXPECT_FALSE(network::cors::IsNoCorsSafelistedHeaderName("authorization"));
}

TEST(NetworkCrashRegression, OpenSSLCipherDescriptionsUseWireIdentifiers)
{
    const char* key_exchange;
    const char* cipher;
    const char* mac;
    bool aead;
    bool tls13;
    SSLCipherSuiteToStrings(&key_exchange, &cipher, &mac, &aead, &tls13, 0x1301);
    EXPECT_STREQ("AES_128_GCM", cipher);
    EXPECT_EQ(nullptr, key_exchange);
    EXPECT_EQ(nullptr, mac);
    EXPECT_TRUE(aead);
    EXPECT_TRUE(tls13);
    EXPECT_STREQ("X25519", SSLGroupToName(29));
    EXPECT_EQ(nullptr, SSLGroupToName(0xffff));
}

} // namespace
} // namespace net
