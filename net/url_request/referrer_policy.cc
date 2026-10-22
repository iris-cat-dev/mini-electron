// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/url_request/referrer_policy.h"

#include <string>

#include "base/containers/adapters.h"
#include "base/containers/fixed_flat_map.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/feature_list.h"
#include "base/notreached.h"
#include "net/base/features.h"
#include "net/url_request/url_request_job.h"
#include "url/origin.h"

namespace net {
namespace {

// Assuming |url| has already been stripped for use as a referrer, if
// |should_strip_to_origin| is true, this method returns the output of the
// "Strip `url` for use as a referrer" algorithm from the Referrer Policy spec
// with its "origin-only" flag set to true:
// https://w3c.github.io/webappsec-referrer-policy/#strip-url
GURL MaybeStripToOrigin(GURL url, bool should_strip_to_origin)
{
    if (!should_strip_to_origin)
        return url;

    return url.DeprecatedGetOriginAsURL();
}

} // namespace

// static
GURL URLRequestJob::ComputeReferrerForPolicy(ReferrerPolicy policy, const GURL& original_referrer, const GURL& destination, bool* same_origin_out_for_metrics)
{
    // Here and below, numbered lines are from the Referrer Policy spec's
    // "Determine request's referrer" algorithm:
    // https://w3c.github.io/webappsec-referrer-policy/#determine-requests-referrer
    //
    // 4. Let referrerURL be the result of stripping referrerSource for use as a
    // referrer.
    GURL stripped_referrer = original_referrer.GetAsReferrer();

    // 5. Let referrerOrigin be the result of stripping referrerSource for use as
    // a referrer, with the origin-only flag set to true.
    //
    // (We use a boolean instead of computing the URL right away in order to avoid
    // constructing a new GURL when it's not necessary.)
    bool should_strip_to_origin = false;

    // 6. If the result of serializing referrerURL is a string whose length is
    // greater than 4096, set referrerURL to referrerOrigin.
    if (stripped_referrer.spec().size() > 4096)
        should_strip_to_origin = true;

    bool same_origin = url::IsSameOriginWith(original_referrer, destination);

    if (same_origin_out_for_metrics)
        *same_origin_out_for_metrics = same_origin;

    // 7. The user agent MAY alter referrerURL or referrerOrigin at this point to
    // enforce arbitrary policy considerations in the interests of minimizing data
    // leakage. For example, the user agent could strip the URL down to an origin,
    // modify its host, replace it with an empty string, etc.
    if (base::FeatureList::IsEnabled(features::kCapReferrerToOriginOnCrossOrigin) && !same_origin) {
        should_strip_to_origin = true;
    }

    bool secure_referrer_but_insecure_destination = original_referrer.SchemeIsCryptographic() && !destination.SchemeIsCryptographic();

    switch (policy) {
    case ReferrerPolicy::CLEAR_ON_TRANSITION_FROM_SECURE_TO_INSECURE:
        if (secure_referrer_but_insecure_destination)
            return GURL();
        return MaybeStripToOrigin(std::move(stripped_referrer), should_strip_to_origin);

    case ReferrerPolicy::REDUCE_GRANULARITY_ON_TRANSITION_CROSS_ORIGIN:
        if (secure_referrer_but_insecure_destination)
            return GURL();
        if (!same_origin)
            should_strip_to_origin = true;
        return MaybeStripToOrigin(std::move(stripped_referrer), should_strip_to_origin);

    case ReferrerPolicy::ORIGIN_ONLY_ON_TRANSITION_CROSS_ORIGIN:
        if (!same_origin)
            should_strip_to_origin = true;
        return MaybeStripToOrigin(std::move(stripped_referrer), should_strip_to_origin);

    case ReferrerPolicy::NEVER_CLEAR:
        return MaybeStripToOrigin(std::move(stripped_referrer), should_strip_to_origin);

    case ReferrerPolicy::ORIGIN:
        should_strip_to_origin = true;
        return MaybeStripToOrigin(std::move(stripped_referrer), should_strip_to_origin);

    case ReferrerPolicy::CLEAR_ON_TRANSITION_CROSS_ORIGIN:
        if (!same_origin)
            return GURL();
        return MaybeStripToOrigin(std::move(stripped_referrer), should_strip_to_origin);

    case ReferrerPolicy::ORIGIN_CLEAR_ON_TRANSITION_FROM_SECURE_TO_INSECURE:
        if (secure_referrer_but_insecure_destination)
            return GURL();
        should_strip_to_origin = true;
        return MaybeStripToOrigin(std::move(stripped_referrer), should_strip_to_origin);

    case ReferrerPolicy::NO_REFERRER:
        return GURL();
    }

    NOTREACHED();
}

std::optional<ReferrerPolicy> ReferrerPolicyFromHeader(std::string_view referrer_policy_header_value)
{
    using enum ReferrerPolicy;
    const auto policy_tokens = base::SplitStringPiece(referrer_policy_header_value, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);

    // Map from lower-cased token to ReferrerPolicy. It's good for compile speed
    // to keep this sorted.
    static constexpr auto kTokenToReferrerPolicy = base::MakeFixedFlatMap<std::string_view, ReferrerPolicy>(
        { { "no-referrer", NO_REFERRER }, { "no-referrer-when-downgrade", CLEAR_ON_TRANSITION_FROM_SECURE_TO_INSECURE }, { "origin", ORIGIN },
            { "origin-when-cross-origin", ORIGIN_ONLY_ON_TRANSITION_CROSS_ORIGIN }, { "same-origin", CLEAR_ON_TRANSITION_CROSS_ORIGIN },
            { "strict-origin", ORIGIN_CLEAR_ON_TRANSITION_FROM_SECURE_TO_INSECURE },
            { "strict-origin-when-cross-origin", REDUCE_GRANULARITY_ON_TRANSITION_CROSS_ORIGIN }, { "unsafe-url", NEVER_CLEAR } });

    // Per https://w3c.github.io/webappsec-referrer-policy/#unknown-policy-values,
    // use the last recognized policy value, and ignore unknown policies.
    for (const auto& token : base::Reversed(policy_tokens)) {
        const std::string lowered_token = base::ToLowerASCII(token);
        const auto it = kTokenToReferrerPolicy.find(lowered_token);
        if (it != kTokenToReferrerPolicy.end()) {
            return it->second;
        }
    }

    return std::nullopt;
}

} // namespace net
