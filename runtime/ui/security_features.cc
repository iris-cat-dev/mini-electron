// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "services/network/public/cpp/features.h"

// The local profile owns only the configuration used by retained header parsers.
// The normal profile supplies these identities from network's full producer.
namespace network::features {

BASE_FEATURE(kCrossOriginOpenerPolicy, "CrossOriginOpenerPolicy", base::FEATURE_ENABLED_BY_DEFAULT);
BASE_FEATURE(kCrossOriginOpenerPolicyByDefault, "CrossOriginOpenerPolicyByDefault", base::FEATURE_DISABLED_BY_DEFAULT);
BASE_FEATURE(kCoopNoopenerAllowPopups, "CoopNoopenerAllowPopups", base::FEATURE_ENABLED_BY_DEFAULT);
BASE_FEATURE(kCoopRestrictProperties, "CoopRestrictProperties", base::FEATURE_DISABLED_BY_DEFAULT);
BASE_FEATURE(kCoopRestrictPropertiesOriginTrial, "CoopRestrictPropertiesOriginTrial", base::FEATURE_ENABLED_BY_DEFAULT);
BASE_FEATURE(kReduceAcceptLanguage, "ReduceAcceptLanguage", base::FEATURE_DISABLED_BY_DEFAULT);

} // namespace network::features
