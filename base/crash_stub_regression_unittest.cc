// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/metrics/field_trial_param_associator.h"

#include "base/files/file_path.h"
#include "base/i18n/file_util_icu.h"
#include "base/test/scoped_feature_list.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/strings/str_format.h"

namespace base {
namespace {

TEST(CrashStubRegression, FieldTrialLookupAndClearPreserveOtherGroups)
{
    test::ScopedFeatureList feature_list;
    feature_list.InitWithEmptyFeatureAndFieldTrialLists();
    FieldTrialParamAssociator associator;
    const FieldTrialParams first { { "key", "first" } };
    const FieldTrialParams second { { "key", "second" } };
    ASSERT_TRUE(associator.AssociateFieldTrialParams("CrashRegression", "a", first));
    ASSERT_TRUE(associator.AssociateFieldTrialParams("CrashRegression", "b", second));
    FieldTrialParams result;
    ASSERT_TRUE(associator.GetFieldTrialParamsWithoutFallback("CrashRegression", "a", &result));
    EXPECT_EQ(first, result);
    associator.ClearParamsForTesting("CrashRegression", "a");
    EXPECT_FALSE(associator.GetFieldTrialParamsWithoutFallback("CrashRegression", "a", &result));
    EXPECT_EQ(first, result);
    ASSERT_TRUE(associator.GetFieldTrialParamsWithoutFallback("CrashRegression", "b", &result));
    EXPECT_EQ(second, result);
    EXPECT_FALSE(associator.GetFieldTrialParamsWithoutFallback("Missing", "b", &result));
    associator.ClearParamsForTesting("Missing", "b");
}

TEST(CrashStubRegression, UntypedFormatUsesCanonicalAbseilImplementation)
{
    std::string result;
    const absl::UntypedFormatSpec spec("%s:%04d:%.2f");
    ASSERT_TRUE(absl::FormatUntyped(&result, spec,
        { absl::FormatArg("value"), absl::FormatArg(7), absl::FormatArg(1.25) }));
    EXPECT_EQ("value:0007:1.25", result);
}

TEST(CrashStubRegression, FilenameCollationIsStrictOrdering)
{
    const FilePath first(FILE_PATH_LITERAL("a.txt"));
    const FilePath second(FILE_PATH_LITERAL("b.txt"));
    EXPECT_FALSE(i18n::LocaleAwareCompareFilenames(first, first));
    EXPECT_TRUE(i18n::LocaleAwareCompareFilenames(first, second));
    EXPECT_FALSE(i18n::LocaleAwareCompareFilenames(second, first));
}

} // namespace
} // namespace base
