// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/public/common/content_switch_dependent_feature_overrides.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "base/base_switches.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/test/scoped_feature_list.h"
#include "services/network/public/cpp/features.h"
#include "services/network/public/cpp/network_switches.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace content {
namespace {

bool HasEnableOverride(
    const std::vector<base::FeatureList::FeatureOverrideInfo>& overrides,
    const base::Feature& feature) {
  return std::ranges::any_of(overrides, [&feature](const auto& override_info) {
    return &override_info.first.get() == &feature &&
           override_info.second == base::FeatureList::OVERRIDE_ENABLE_FEATURE;
  });
}

// Builds a FeatureList the way the browser does: explicit --enable-features and
// --disable-features first, then the switch-dependent overrides. That ordering
// is what lets an explicit flag beat a switch-implied one, so the tests below
// have to reproduce it rather than register the overrides on their own.
std::unique_ptr<base::FeatureList> BuildFeatureList(
    const base::CommandLine& command_line) {
  auto feature_list = std::make_unique<base::FeatureList>();
  feature_list->InitFromCommandLine(
      command_line.GetSwitchValueASCII(::switches::kEnableFeatures),
      command_line.GetSwitchValueASCII(::switches::kDisableFeatures));
  feature_list->RegisterExtraFeatureOverrides(
      GetSwitchDependentFeatureOverrides(command_line));
  return feature_list;
}

TEST(ContentSwitchDependentFeatureOverridesTest, NoWarcOutputOverridesNothing) {
  base::CommandLine command_line(base::CommandLine::NO_PROGRAM);

  EXPECT_FALSE(
      HasEnableOverride(GetSwitchDependentFeatureOverrides(command_line),
                        network::features::kRendererSideContentDecoding));
}

TEST(ContentSwitchDependentFeatureOverridesTest,
     WarcOutputEnablesRendererSideContentDecoding) {
  // A WARC response record has to hold the body still in its transfer encoding
  // so that it agrees with the Content-Encoding stored beside it, and the
  // network service only leaves bodies encoded when the client will decode
  // them. Recording without this would silently produce unfaithful archives.
  base::CommandLine command_line(base::CommandLine::NO_PROGRAM);
  command_line.AppendSwitchASCII(network::switches::kWarcOutput, "out.warc.gz");

  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatureList(BuildFeatureList(command_line));

  EXPECT_TRUE(base::FeatureList::IsEnabled(
      network::features::kRendererSideContentDecoding));
}

TEST(ContentSwitchDependentFeatureOverridesTest,
     ExplicitDisableBeatsWarcOutput) {
  // Turning the feature off has to keep working. The recorder then falls back
  // to storing decoded bodies with their encoding headers rewritten, which is
  // the documented behaviour; the switch must not quietly override the flag.
  base::CommandLine command_line(base::CommandLine::NO_PROGRAM);
  command_line.AppendSwitchASCII(network::switches::kWarcOutput, "out.warc.gz");
  command_line.AppendSwitchASCII(
      ::switches::kDisableFeatures,
      network::features::kRendererSideContentDecoding.name);

  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatureList(BuildFeatureList(command_line));

  EXPECT_FALSE(base::FeatureList::IsEnabled(
      network::features::kRendererSideContentDecoding));
}

}  // namespace
}  // namespace content
