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
#include "testing/gtest/include/gtest/gtest.h"
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

}  // namespace
