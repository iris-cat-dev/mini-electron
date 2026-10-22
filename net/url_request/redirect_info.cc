// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/url_request/redirect_info.h"
#if !defined(MINI_UI_IMPLEMENTATION)
#include "net/url_request/url_request_job.h"
#endif

namespace net {
#if !defined(MINI_UI_IMPLEMENTATION)
namespace {

std::string ComputeMethodForRedirect(const std::string& method, int http_status_code)
{
    if ((http_status_code == 303 && method != "HEAD") || ((http_status_code == 301 || http_status_code == 302) && method == "POST")) {
        return "GET";
    }
    return method;
}

ReferrerPolicy ProcessReferrerPolicyHeaderOnRedirect(
    ReferrerPolicy original_referrer_policy, const std::optional<std::string>& referrer_policy_header)
{
    if (referrer_policy_header) {
        return ReferrerPolicyFromHeader(referrer_policy_header.value()).value_or(original_referrer_policy);
    }
    return original_referrer_policy;
}

} // namespace
#endif

RedirectInfo::RedirectInfo() = default;

RedirectInfo::RedirectInfo(const RedirectInfo& other) = default;

RedirectInfo::~RedirectInfo() = default;
#if !defined(MINI_UI_IMPLEMENTATION)
RedirectInfo RedirectInfo::ComputeRedirectInfo(
    const std::string& original_method, const GURL& original_url, const SiteForCookies& original_site_for_cookies,
    RedirectInfo::FirstPartyURLPolicy original_first_party_url_policy, ReferrerPolicy original_referrer_policy,
    const std::string& original_referrer, int http_status_code, const GURL& new_location,
    const std::optional<std::string>& referrer_policy_header, bool insecure_scheme_was_upgraded,
    bool copy_fragment, bool is_signed_exchange_fallback_redirect)
{
    RedirectInfo redirect_info;
    redirect_info.status_code = http_status_code;
    redirect_info.new_method = ComputeMethodForRedirect(original_method, http_status_code);

    if (original_url.is_valid() && original_url.has_ref() && !new_location.has_ref() && copy_fragment) {
        GURL::Replacements replacements;
        replacements.SetRefStr(original_url.ref_piece());
        redirect_info.new_url = new_location.ReplaceComponents(replacements);
    } else {
        redirect_info.new_url = new_location;
    }

    redirect_info.insecure_scheme_was_upgraded = insecure_scheme_was_upgraded;
    redirect_info.is_signed_exchange_fallback_redirect = is_signed_exchange_fallback_redirect;
    if (original_first_party_url_policy == FirstPartyURLPolicy::UPDATE_URL_ON_REDIRECT) {
        redirect_info.new_site_for_cookies = SiteForCookies::FromUrl(redirect_info.new_url);
    } else {
        redirect_info.new_site_for_cookies = original_site_for_cookies;
    }

    redirect_info.new_referrer_policy =
        ProcessReferrerPolicyHeaderOnRedirect(original_referrer_policy, referrer_policy_header);
    redirect_info.new_referrer =
        URLRequestJob::ComputeReferrerForPolicy(redirect_info.new_referrer_policy, GURL(original_referrer), redirect_info.new_url).spec();
    return redirect_info;
}
#endif

} // namespace net
