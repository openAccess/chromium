// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/warc_page_recorder.h"

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/strings/string_split.h"
#include "base/values.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/navigation_simulator.h"
#include "content/test/navigation_simulator_impl.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/mojom/navigation/navigation_params.mojom.h"
#include "url/gurl.h"

namespace {

class WarcPageRecorderTest : public ChromeRenderViewHostTestHarness {
 protected:
  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();
    ASSERT_TRUE(capture_.CreateUniqueTempDir());
    WarcPageRecorder::ResetFileSequenceForTesting();
    WarcPageRecorder::CreateForWebContents(web_contents());
    WarcPageRecorder::FromWebContents(web_contents())
        ->SetCaptureDirectory(capture_.GetPath());
  }

  // Lands on `url` and leaves it loading, as a page that sends the browser on
  // before it has finished does. Nothing but the next page writes it.
  void Land(const GURL& url) {
    std::unique_ptr<content::NavigationSimulator> navigation =
        content::NavigationSimulator::CreateBrowserInitiated(url,
                                                             web_contents());
    navigation->SetKeepLoading(true);
    navigation->Commit();
  }

  // The page in the tab reloads itself, as a bot check does once it is
  // satisfied, and leaves what it reloads into loading. A renderer asks for a
  // reload as a link that reloads, never with a reload's transition, which
  // only the browser may use.
  void ReloadFromThePage() {
    std::unique_ptr<content::NavigationSimulatorImpl> navigation =
        content::NavigationSimulatorImpl::CreateRendererInitiated(
            web_contents()->GetLastCommittedURL(), main_rfh());
    navigation->set_navigation_type(blink::mojom::NavigationType::RELOAD);
    navigation->SetKeepLoading(true);
    navigation->Commit();
  }

  void SetTitle(const std::u16string& title) {
    web_contents()->UpdateTitleForEntry(
        web_contents()->GetController().GetLastCommittedEntry(), title);
  }

  // The pages listed so far, header left out.
  std::vector<base::DictValue> Pages() {
    task_environment()->RunUntilIdle();
    std::string contents;
    base::ReadFileToString(capture_.GetPath().AppendASCII("pages.jsonl"),
                           &contents);
    std::vector<base::DictValue> pages;
    for (const std::string& line :
         base::SplitString(contents, "\n", base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_NONEMPTY)) {
      std::optional<base::DictValue> page =
          base::JSONReader::ReadDict(line, base::JSON_PARSE_RFC);
      if (page && !page->Find("format")) {
        pages.push_back(std::move(*page));
      }
    }
    return pages;
  }

  base::ScopedTempDir capture_;
};

// A page that sends the browser on before it has a title -- a redirect done
// by script, a bot check -- is written when the next page commits. The tab
// then shows the next page's URL for want of a title, and that is not this
// page's name.
TEST_F(WarcPageRecorderTest, APageWithNoTitleIsListedWithoutOne) {
  Land(GURL("https://hop.test/"));
  Land(GURL("https://landing.test/"));

  std::vector<base::DictValue> pages = Pages();
  ASSERT_EQ(1u, pages.size());
  EXPECT_EQ("https://hop.test/", *pages[0].FindString("url"));
  EXPECT_FALSE(pages[0].Find("title"));
}

TEST_F(WarcPageRecorderTest, APageIsListedUnderItsOwnTitle) {
  Land(GURL("https://hop.test/"));
  SetTitle(u"Checking your browser");
  Land(GURL("https://landing.test/"));
  SetTitle(u"Front page");

  std::vector<base::DictValue> pages = Pages();
  ASSERT_EQ(1u, pages.size());
  EXPECT_EQ("https://hop.test/", *pages[0].FindString("url"));
  EXPECT_EQ("Checking your browser", *pages[0].FindString("title"));
}

// What a bot check reloads into, once it is satisfied, is the page the visitor
// came for, and it is listed as well as the check was -- though it takes the
// check's place in the tab's history rather than a place of its own.
TEST_F(WarcPageRecorderTest, APageThatReloadsItselfIsListedAgain) {
  Land(GURL("https://site.test/"));
  SetTitle(u"Checking your browser");
  ReloadFromThePage();
  SetTitle(u"Front page");
  Land(GURL("https://next.test/"));

  std::vector<base::DictValue> pages = Pages();
  ASSERT_EQ(2u, pages.size());
  EXPECT_EQ("https://site.test/", *pages[0].FindString("url"));
  EXPECT_EQ("Checking your browser", *pages[0].FindString("title"));
  EXPECT_EQ("https://site.test/", *pages[1].FindString("url"));
  EXPECT_EQ("Front page", *pages[1].FindString("title"));
}

}  // namespace
