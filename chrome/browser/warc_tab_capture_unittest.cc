// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/warc_tab_capture.h"

#include "base/test/scoped_command_line.h"
#include "base/unguessable_token.h"
#include "chrome/browser/chrome_content_browser_client.h"
#include "chrome/browser/wacz_replay.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/public/test/browser_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

class WarcTabCaptureTest : public testing::Test {
 protected:
  content::BrowserTaskEnvironment task_environment_;
  TestingProfile profile_;
};

// A tab is archived from an empty state, so its partition is not the profile's
// and keeps nothing once the browser is gone.
TEST_F(WarcTabCaptureTest, CapturePartitionIsSeparateAndInMemory) {
  const content::StoragePartitionConfig partition =
      WarcTabCapture::CapturePartition(&profile_,
                                       base::UnguessableToken::Create());
  EXPECT_FALSE(partition.is_default());
  EXPECT_TRUE(partition.in_memory());
}

// Each capture has a partition of its own, and the partition says which
// capture it is: that is what lets a worker, which has no tab to ask about,
// be recorded into the capture of the tab it works for.
TEST_F(WarcTabCaptureTest, CapturePartitionNamesItsCapture) {
  const auto capture = base::UnguessableToken::Create();
  const auto other = base::UnguessableToken::Create();

  EXPECT_NE(WarcTabCapture::CapturePartition(&profile_, capture),
            WarcTabCapture::CapturePartition(&profile_, other));
  EXPECT_EQ(capture, WarcTabCapture::CaptureIdOfPartition(
                         WarcTabCapture::CapturePartition(&profile_, capture)));

  // And it is the browser's answer for what the partition loads.
  ChromeContentBrowserClient client;
  EXPECT_EQ(capture,
            client.GetWarcCaptureIdForPartition(
                &profile_, WarcTabCapture::CapturePartition(&profile_, capture)));
}

TEST_F(WarcTabCaptureTest, OnlyACapturePartitionNamesACapture) {
  const auto token = base::UnguessableToken::Create();

  EXPECT_EQ(std::nullopt,
            WarcTabCapture::CaptureIdOfPartition(
                content::StoragePartitionConfig::CreateDefault(&profile_)));

  // Somebody else's partition is not a capture's for being named like one.
  EXPECT_EQ(std::nullopt, WarcTabCapture::CaptureIdOfPartition(
                              content::StoragePartitionConfig::Create(
                                  &profile_, "someone-else", token.ToString(),
                                  /*in_memory=*/true)));

  // Nor is a replay's, though it too is separate and in memory.
  base::test::ScopedCommandLine command_line;
  command_line.GetProcessCommandLine()->AppendSwitchASCII(
      switches::kWaczReplay, "/tmp/capture.wacz");
  EXPECT_EQ(std::nullopt,
            WarcTabCapture::CaptureIdOfPartition(
                wacz_replay::ReplayPartition(&profile_, "20260301120000")));

  // A capture partition's name that is not a capture's id names none.
  EXPECT_EQ(std::nullopt, WarcTabCapture::CaptureIdOfPartition(
                              content::StoragePartitionConfig::Create(
                                  &profile_, "warc-capture", "not-a-token",
                                  /*in_memory=*/true)));

  ChromeContentBrowserClient client;
  EXPECT_EQ(std::nullopt,
            client.GetWarcCaptureIdForPartition(
                &profile_,
                content::StoragePartitionConfig::CreateDefault(&profile_)));
}

}  // namespace
