// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wacz_replay.h"

#include <memory>
#include <vector>

#include "base/test/scoped_command_line.h"
#include "base/test/test_future.h"
#include "base/unguessable_token.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/browser/back_forward_cache.h"
#include "content/public/browser/child_process_host.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/navigation_simulator.h"
#include "content/public/test/web_contents_tester.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_options.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

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
  const base::UnguessableToken tab_ = base::UnguessableToken::Create();
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
      ReplayPartition(&profile_, kMoment, tab_);
  EXPECT_FALSE(partition.is_default());
  EXPECT_TRUE(partition.in_memory());
  EXPECT_NE(partition, content::StoragePartitionConfig::CreateDefault(
                           &profile_));
}

TEST_F(WaczReplayTest, OnlyAReplayPartitionIsOne) {
  Replay("/tmp/capture.wacz");

  EXPECT_TRUE(IsReplayPartition(ReplayPartition(&profile_, kMoment, tab_)));
  EXPECT_FALSE(IsReplayPartition(
      content::StoragePartitionConfig::CreateDefault(&profile_)));
  // Somebody else's non-default partition is not one either -- a <webview>'s,
  // an extension's. Being partitioned is not what is being asked.
  EXPECT_FALSE(IsReplayPartition(content::StoragePartitionConfig::Create(
      &profile_, "someextensionid", "", /*in_memory=*/true)));
}

// One tab is one partition, whatever frames and workers it holds: they share
// what the tab stored, as the pages being replayed did when they were live.
TEST_F(WaczReplayTest, OneTabIsOnePartition) {
  Replay("/tmp/capture.wacz");
  EXPECT_EQ(ReplayPartition(&profile_, kMoment, tab_),
            ReplayPartition(&profile_, kMoment, tab_));
}

// Two tabs are two visits, even to one archive at one moment, as two captures
// of one site were: neither finds what the other stored.
TEST_F(WaczReplayTest, TwoTabsAreTwoPartitions) {
  Replay("/tmp/capture.wacz");
  const content::StoragePartitionConfig other =
      ReplayPartition(&profile_, kMoment, base::UnguessableToken::Create());

  EXPECT_NE(ReplayPartition(&profile_, kMoment, tab_), other);
  EXPECT_TRUE(IsReplayPartition(other));
}

// The moment is in the name too, which is how the partition says what it
// serves.
TEST_F(WaczReplayTest, TwoMomentsAreTwoPartitions) {
  Replay("/tmp/capture.wacz");
  const content::StoragePartitionConfig march =
      ReplayPartition(&profile_, "20260301120000", tab_);
  const content::StoragePartitionConfig april =
      ReplayPartition(&profile_, "20260401120000", tab_);

  EXPECT_NE(march, april);
  EXPECT_TRUE(IsReplayPartition(march));
  EXPECT_TRUE(IsReplayPartition(april));
}

// A page list and an index spell a time differently. The same moment however
// it is written is the same partition, or a tab opened from the page list and
// one opened from the index would not share what they stored.
TEST_F(WaczReplayTest, OneMomentHoweverItIsWritten) {
  Replay("/tmp/capture.wacz");
  EXPECT_EQ(ReplayPartition(&profile_, "2026-03-01T12:00:00Z", tab_),
            ReplayPartition(&profile_, "20260301120000", tab_));
}

// The partition is where a factory learns which moment to serve, so what goes
// in has to come back out -- for a frame, and for a worker that has only its
// process to go by.
TEST_F(WaczReplayTest, APartitionKnowsItsMoment) {
  Replay("/tmp/capture.wacz");
  EXPECT_EQ("20260301120000", ReplayMoment(ReplayPartition(
                                  &profile_, "2026-03-01T12:00:00Z", tab_)));
  // No moment is the earliest capture, and stays that.
  EXPECT_EQ("", ReplayMoment(ReplayPartition(&profile_, "", tab_)));
  // Nothing that is not a replay partition has a moment to give.
  EXPECT_EQ("", ReplayMoment(
                    content::StoragePartitionConfig::CreateDefault(&profile_)));
  EXPECT_EQ("", ReplayMoment(content::StoragePartitionConfig::Create(
                    &profile_, "someextensionid", "ab-20260301120000",
                    /*in_memory=*/true)));
}

// A replay renderer's clock starts at its partition's moment, so a page reads
// the time it was captured at.
TEST_F(WaczReplayTest, AReplayClockStartsAtTheMoment) {
  Replay("/tmp/capture.wacz");
  // 2026-10-30 22:56:29 UTC.
  const base::Time moment = base::Time::UnixEpoch() + base::Seconds(1793400989);
  EXPECT_EQ(moment, ReplayClockStart(ReplayPartition(
                        &profile_, "2026-10-30T22:56:29Z", tab_)));
  EXPECT_EQ(moment, ReplayClockStart(
                        ReplayPartition(&profile_, "20261030225629", tab_)));
  // The earliest capture of everything is no one time, so its clock is the
  // real one; and nothing outside a replay partition has its clock moved.
  EXPECT_EQ(std::nullopt,
            ReplayClockStart(ReplayPartition(&profile_, "", tab_)));
  EXPECT_EQ(std::nullopt,
            ReplayClockStart(
                content::StoragePartitionConfig::CreateDefault(&profile_)));
  EXPECT_EQ(std::nullopt,
            ReplayClockStart(content::StoragePartitionConfig::Create(
                &profile_, "someextensionid", "ab-20261030225629",
                /*in_memory=*/true)));
}

// Fail closed: asked about nothing in particular -- no frame, and a process
// that has gone or never was -- the answer is that it is not served an
// archive, never that it might as well be.
TEST_F(WaczReplayTest, NothingIsNotServedFromAnArchive) {
  Replay("/tmp/capture.wacz");
  EXPECT_FALSE(ServesFromArchive(/*frame=*/nullptr,
                                 content::ChildProcessHost::kInvalidUniqueID));
}

// Replay tabs made as OpenReplayTabs() makes them, and the cookies their pages
// find stored.
class WaczReplayVisitTest : public ChromeRenderViewHostTestHarness {
 protected:
  void TearDown() override {
    tabs_.clear();
    ChromeRenderViewHostTestHarness::TearDown();
  }

  content::WebContents* OpenReplayTab() {
    std::unique_ptr<content::WebContents> tab =
        content::WebContentsTester::CreateTestWebContents(
            profile(), content::SiteInstance::CreateForFixedStoragePartition(
                           profile(), page_,
                           ReplayPartition(profile(), kMoment,
                                           base::UnguessableToken::Create())));
    // Going back is told apart by what it is, not by the page being kept.
    tab->GetController().GetBackForwardCache().DisableForTesting(
        content::BackForwardCache::TEST_REQUIRES_NO_CACHING);
    content::NavigationSimulator::NavigateAndCommitFromBrowser(tab.get(),
                                                               page_);
    EXPECT_TRUE(IsReplayTab(tab.get()));
    tabs_.push_back(std::move(tab));
    return tabs_.back().get();
  }

  // Stores a cookie, as a page of the site would.
  void Store(content::WebContents* tab) {
    base::test::TestFuture<net::CookieAccessResult> stored;
    CookieManager(tab)->SetCanonicalCookie(
        *net::CanonicalCookie::CreateForTesting(page_, "visitor=1",
                                                base::Time::Now(),
                                                net::CookieSourceType::kOther),
        page_, net::CookieOptions::MakeAllInclusive(), stored.GetCallback());
    ASSERT_TRUE(stored.Get().status.IsInclude());
  }

  // Whether what Store() stored is still there.
  bool Stored(content::WebContents* tab) {
    base::test::TestFuture<std::vector<net::CanonicalCookie>> cookies;
    CookieManager(tab)->GetAllCookies(
        cookies.GetCallback<const std::vector<net::CanonicalCookie>&>());
    return !cookies.Get().empty();
  }

  const GURL page_{"https://archived.example.org/"};
  const GURL other_page_{"https://archived.example.org/other"};

 private:
  network::mojom::CookieManager* CookieManager(content::WebContents* tab) {
    return tab->GetPrimaryMainFrame()
        ->GetStoragePartition()
        ->GetCookieManagerForBrowserProcess();
  }

  std::vector<std::unique_ptr<content::WebContents>> tabs_;
};

// What a visit stored is gone when the next begins -- an address typed, a
// bookmark -- so the site is sent what it sent the capture: a first visit.
TEST_F(WaczReplayVisitTest, AVisitStartsWithNothingStored) {
  content::WebContents* tab = OpenReplayTab();
  Store(tab);
  ASSERT_TRUE(Stored(tab));

  content::NavigationSimulator::NavigateAndCommitFromBrowser(tab, other_page_);
  EXPECT_FALSE(Stored(tab));
}

// Reloading is seeing the page again as it was captured, so it is a visit too.
TEST_F(WaczReplayVisitTest, AReloadIsANewVisit) {
  content::WebContents* tab = OpenReplayTab();
  Store(tab);

  content::NavigationSimulator::Reload(tab);
  EXPECT_FALSE(Stored(tab));
}

// Following a link carries on the visit, with what it has stored so far, as
// it did when it was captured.
TEST_F(WaczReplayVisitTest, FollowingALinkCarriesOnTheVisit) {
  content::WebContents* tab = OpenReplayTab();
  Store(tab);

  content::NavigationSimulator::NavigateAndCommitFromDocument(
      other_page_, tab->GetPrimaryMainFrame());
  EXPECT_TRUE(Stored(tab));
}

TEST_F(WaczReplayVisitTest, GoingBackCarriesOnTheVisit) {
  content::WebContents* tab = OpenReplayTab();
  content::NavigationSimulator::NavigateAndCommitFromDocument(
      other_page_, tab->GetPrimaryMainFrame());
  Store(tab);

  content::NavigationSimulator::GoBack(tab);
  EXPECT_TRUE(Stored(tab));
}

// One tab's new visit is no other tab's: each stored only what its own pages
// did.
TEST_F(WaczReplayVisitTest, AnotherTabKeepsWhatItStored) {
  content::WebContents* tab = OpenReplayTab();
  content::WebContents* other = OpenReplayTab();
  Store(other);

  content::NavigationSimulator::NavigateAndCommitFromBrowser(tab, other_page_);
  EXPECT_TRUE(Stored(other));
}

// A tab browsing the web keeps what it stored, however it is navigated.
TEST_F(WaczReplayVisitTest, AnOrdinaryTabIsLeftAlone) {
  content::NavigationSimulator::NavigateAndCommitFromBrowser(web_contents(),
                                                             page_);
  ASSERT_FALSE(IsReplayTab(web_contents()));
  Store(web_contents());

  content::NavigationSimulator::NavigateAndCommitFromBrowser(web_contents(),
                                                             other_page_);
  EXPECT_TRUE(Stored(web_contents()));
}

}  // namespace
}  // namespace wacz_replay
