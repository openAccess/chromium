// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wacz_replay.h"

#include "base/test/scoped_command_line.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/browser/child_process_host.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/public/test/browser_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace wacz_replay {
namespace {

constexpr char kMoment[] = "20260301120000";

class WaczReplayTest : public testing::Test {
 protected:
  // Starts this browser replaying `path`, as --wacz-replay would.
  void Replay(const char* path) {
    command_line_.GetProcessCommandLine()->AppendSwitchASCII(
        switches::kWaczReplay, path);
  }

  content::BrowserTaskEnvironment task_environment_;
  base::test::ScopedCommandLine command_line_;
  TestingProfile profile_;
};

TEST_F(WaczReplayTest, NotReplayingUntilAnArchiveIsNamed) {
  EXPECT_FALSE(IsEnabled());
  Replay("/tmp/capture.wacz");
  EXPECT_TRUE(IsEnabled());
}

// The two things that make the partition an isolating one: it is not the
// profile's own, so nothing a replayed page reads or writes is the profile's;
// and it is in memory, so nothing it wrote -- a cookie, an HSTS entry -- is
// still there for the next browser.
TEST_F(WaczReplayTest, ReplayPartitionIsSeparateAndInMemory) {
  Replay("/tmp/capture.wacz");

  const content::StoragePartitionConfig partition =
      ReplayPartition(&profile_, kMoment);
  EXPECT_FALSE(partition.is_default());
  EXPECT_TRUE(partition.in_memory());
  EXPECT_NE(partition, content::StoragePartitionConfig::CreateDefault(
                           &profile_));
}

TEST_F(WaczReplayTest, OnlyAReplayPartitionIsOne) {
  Replay("/tmp/capture.wacz");

  EXPECT_TRUE(IsReplayPartition(ReplayPartition(&profile_, kMoment)));
  EXPECT_FALSE(IsReplayPartition(
      content::StoragePartitionConfig::CreateDefault(&profile_)));
  // Somebody else's non-default partition is not one either -- a <webview>'s,
  // an extension's. Being partitioned is not what is being asked.
  EXPECT_FALSE(IsReplayPartition(content::StoragePartitionConfig::Create(
      &profile_, "someextensionid", "", /*in_memory=*/true)));
}

// One archive at one moment is one partition, however many tabs or frames are
// replaying it: they were one capture of the web and share what it stored, as
// the pages being replayed did when they were live.
TEST_F(WaczReplayTest, OneMomentIsOnePartition) {
  Replay("/tmp/capture.wacz");
  EXPECT_EQ(ReplayPartition(&profile_, kMoment),
            ReplayPartition(&profile_, kMoment));
}

// The moment is what makes it a tab's: two tabs on one archive at two
// moments are two visits to the web, and keep apart what each stored.
TEST_F(WaczReplayTest, TwoMomentsAreTwoPartitions) {
  Replay("/tmp/capture.wacz");
  const content::StoragePartitionConfig march =
      ReplayPartition(&profile_, "20260301120000");
  const content::StoragePartitionConfig april =
      ReplayPartition(&profile_, "20260401120000");

  EXPECT_NE(march, april);
  EXPECT_TRUE(IsReplayPartition(march));
  EXPECT_TRUE(IsReplayPartition(april));
}

// A page list and an index spell a time differently. The same moment however
// it is written is the same partition, or a tab opened from the page list and
// one opened from the index would not share what they stored.
TEST_F(WaczReplayTest, OneMomentHoweverItIsWritten) {
  Replay("/tmp/capture.wacz");
  EXPECT_EQ(ReplayPartition(&profile_, "2026-03-01T12:00:00Z"),
            ReplayPartition(&profile_, "20260301120000"));
}

// The partition is where a factory learns which moment to serve, so what goes
// in has to come back out -- for a frame, and for a worker that has only its
// process to go by.
TEST_F(WaczReplayTest, APartitionKnowsItsMoment) {
  Replay("/tmp/capture.wacz");
  EXPECT_EQ("20260301120000",
            ReplayMoment(ReplayPartition(&profile_, "2026-03-01T12:00:00Z")));
  // No moment is the earliest capture, and stays that.
  EXPECT_EQ("", ReplayMoment(ReplayPartition(&profile_, "")));
  // Nothing that is not a replay partition has a moment to give.
  EXPECT_EQ("", ReplayMoment(
                    content::StoragePartitionConfig::CreateDefault(&profile_)));
  EXPECT_EQ("", ReplayMoment(content::StoragePartitionConfig::Create(
                    &profile_, "someextensionid", "ab-20260301120000",
                    /*in_memory=*/true)));
}

// Two archives are two captures, and what one stored is no more the other's
// than a live site's would be.
TEST_F(WaczReplayTest, TwoArchivesAreTwoPartitions) {
  Replay("/tmp/capture.wacz");
  const content::StoragePartitionConfig first =
      ReplayPartition(&profile_, kMoment);

  Replay("/tmp/another.wacz");
  const content::StoragePartitionConfig second =
      ReplayPartition(&profile_, kMoment);

  EXPECT_NE(first, second);
  EXPECT_TRUE(IsReplayPartition(second));
}

// Fail closed: asked about nothing in particular -- no frame, and a process
// that has gone or never was -- the answer is that it is not served an
// archive, never that it might as well be.
TEST_F(WaczReplayTest, NothingIsNotServedFromAnArchive) {
  Replay("/tmp/capture.wacz");
  EXPECT_FALSE(ServesFromArchive(/*frame=*/nullptr,
                                 content::ChildProcessHost::kInvalidUniqueID));
}

}  // namespace
}  // namespace wacz_replay
