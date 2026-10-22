// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string_view>
#include <unordered_set>

#include "components/shared_highlighting/core/common/disabled_sites.h"

#include "base/containers/fixed_flat_map.h"
#include "base/feature_list.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "components/shared_highlighting/core/common/shared_highlighting_features.h"
#include "third_party/re2/src/re2/re2.h"

namespace shared_highlighting {

bool ShouldOfferLinkToText(const GURL& url)
{
    struct PathPatterns {
        const re2::RE2 all_paths { ".*" };
        const re2::RE2 amp { R"(^\/amp\/.*)" };
        const re2::RE2 facebook_about { "about" };
        const re2::RE2 instagram_post { "/p/" };
        const re2::RE2 reddit_comments { "comments" };
        const re2::RE2 twitter_status { "status" };
        const re2::RE2 youtube_about_or_community { "(about|community)" };
    };
    using PathPattern = const re2::RE2 PathPatterns::*;

    // Match a blocked host's path, then optionally apply its refined allowlist.
    static constexpr auto kBlocklist = base::MakeFixedFlatMap<std::string_view, PathPattern>({
        { "facebook.com", &PathPatterns::all_paths },
        // TODO(crbug.com/1157981): Cover other Google TLDs.
        { "google.com", &PathPatterns::amp },
        { "instagram.com", &PathPatterns::all_paths },
        { "mail.google.com", &PathPatterns::all_paths },
        { "outlook.live.com", &PathPatterns::all_paths },
        { "reddit.com", &PathPatterns::all_paths },
        { "twitter.com", &PathPatterns::all_paths },
        { "web.whatsapp.com", &PathPatterns::all_paths },
        { "youtube.com", &PathPatterns::all_paths },
    });
    static constexpr auto kAllowlist = base::MakeFixedFlatMap<std::string_view, PathPattern>({
        { "facebook.com", &PathPatterns::facebook_about },
        { "instagram.com", &PathPatterns::instagram_post },
        { "reddit.com", &PathPatterns::reddit_comments },
        { "twitter.com", &PathPatterns::twitter_status },
        { "youtube.com", &PathPatterns::youtube_about_or_community },
    });

    std::string_view domain = url.host_piece();
    if (domain.starts_with("www.")) {
        domain.remove_prefix(4);
    } else if (domain.starts_with("m.")) {
        domain.remove_prefix(2);
    } else if (domain.starts_with("mobile.")) {
        domain.remove_prefix(7);
    }

    if (base::FeatureList::IsEnabled(kSharedHighlightingAmp) && domain == "google.com") {
        return true;
    }

    const auto block_list_it = kBlocklist.find(domain);
    if (block_list_it == kBlocklist.end()) {
        return true;
    }

    // Compile the constant expressions once and retain them for process lifetime.
    static const base::NoDestructor<PathPatterns> patterns;
    const auto path = url.path_piece();
    const re2::StringPiece re2_path(path.data(), path.size());
    if (!re2::RE2::FullMatch(re2_path, (*patterns).*(block_list_it->second))) {
        return true;
    }
    if (base::FeatureList::IsEnabled(kSharedHighlightingRefinedBlocklist)) {
        const auto allow_list_it = kAllowlist.find(domain);
        if (allow_list_it != kAllowlist.end()) {
            return re2::RE2::PartialMatch(re2_path, (*patterns).*(allow_list_it->second));
        }
    }
    return false;
}

bool SupportsLinkGenerationInIframe(GURL main_frame_url)
{
    const std::unordered_set<std::string> good_hosts
        = { "www.google.com", "m.google.com", "mobile.google.com", "www.bing.com", "m.bing.com", "mobile.bing.com" };

    return main_frame_url.SchemeIs(url::kHttpsScheme) && good_hosts.find(main_frame_url.host()) != good_hosts.end()
        && base::StartsWith(main_frame_url.path(), "/amp/");
}

} // namespace shared_highlighting
