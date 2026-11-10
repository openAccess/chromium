// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/side_panel/archive/archive_agent_queries.h"

#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/json/json_reader.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/wacz_replay/replay_log.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/warc/wacz_url_loader_factory.h"
#include "net/base/net_errors.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace {

class ArchiveAgentQueriesTest : public ChromeRenderViewHostTestHarness {
 protected:
  // Records a request to `url`: served with `status`, missing, or failed.
  void Record(const std::string& url,
              int status,
              bool missing = false,
              int net_error = net::OK,
              const std::string& answered_by = std::string()) {
    warc::ReplayOutcome outcome;
    outcome.url = GURL(url);
    outcome.method = "GET";
    outcome.destination = missing ? "image" : "script";
    outcome.missing = missing;
    outcome.net_error = net_error;
    outcome.status_code = status;
    outcome.mime_type = status ? "text/javascript" : "";
    outcome.body_bytes = status ? 100 : 0;
    if (!answered_by.empty()) {
      outcome.captured_url = GURL(answered_by);
    }
    outcome.time = base::Time::Now();
    ReplayLog::Record(web_contents(), base::FilePath("/archives/a.wacz"),
                      base::Time::UnixEpoch() + base::Days(20000), outcome);
  }

  base::DictValue Ask(const std::string& tool,
                      base::DictValue params = base::DictValue()) {
    std::optional<base::DictValue> answer = base::JSONReader::ReadDict(
        AnswerArchiveQuery(web_contents(), tool, params),
        base::JSON_PARSE_RFC);
    EXPECT_TRUE(answer);
    return answer ? std::move(*answer) : base::DictValue();
  }

  void RecordAFewRequests() {
    Record("https://example.org/a.js", 200);
    Record("https://example.org/missing.png", 0, /*missing=*/true);
    Record("https://cdn.example.net/b.js?cb=2", 200, false, net::OK,
           "https://cdn.example.net/b.js?cb=1");
    Record("https://example.org/loop", 0, false, net::ERR_TOO_MANY_REDIRECTS);
    Record("https://cdn.example.net/gone.js", 404);
  }
};

TEST_F(ArchiveAgentQueriesTest, ATabNeitherArchivingNorReplayingSaysSo) {
  base::DictValue status = Ask("tab_status");
  EXPECT_EQ("not archiving", *status.FindString("mode"));
  EXPECT_EQ(0, status.FindInt("listable"));
}

TEST_F(ArchiveAgentQueriesTest, AReplaySaysWhatItHasAskedFor) {
  RecordAFewRequests();
  base::DictValue status = Ask("tab_status");
  EXPECT_EQ("replaying", *status.FindString("mode"));
  EXPECT_EQ("/archives/a.wacz", *status.FindString("archive"));
  EXPECT_EQ(5, status.FindInt("requests"));
  EXPECT_EQ(1, status.FindInt("missing"));
  EXPECT_EQ(1, status.FindInt("failed"));
  // Missing, failed, and the 404.
  EXPECT_EQ(3, status.FindInt("errors"));
}

TEST_F(ArchiveAgentQueriesTest, RequestsAreListedByWhatBecameOfThem) {
  RecordAFewRequests();
  auto urls = [&](const std::string& which) {
    base::DictValue params;
    params.Set("which", which);
    base::DictValue answer = Ask("list_requests", std::move(params));
    std::string urls;
    for (const base::Value& request : *answer.FindList("requests")) {
      urls += *request.GetDict().FindString("url") + " ";
    }
    return urls;
  };
  EXPECT_EQ("https://example.org/missing.png ", urls("missing"));
  EXPECT_EQ("https://example.org/loop ", urls("failed"));
  EXPECT_EQ("https://cdn.example.net/b.js?cb=2 ", urls("closest"));
  EXPECT_EQ(
      "https://example.org/missing.png https://example.org/loop "
      "https://cdn.example.net/gone.js ",
      urls("errors"));

  base::DictValue missing = Ask("list_requests", [] {
    base::DictValue params;
    params.Set("which", "missing");
    return params;
  }());
  const base::DictValue& row = (*missing.FindList("requests"))[0].GetDict();
  EXPECT_EQ(2, row.FindInt("n"));
  EXPECT_EQ(true, row.FindBool("missing"));
  EXPECT_EQ("image", *row.FindString("for"));
  EXPECT_FALSE(row.Find("bytes"));

  base::DictValue closest = Ask("list_requests", [] {
    base::DictValue params;
    params.Set("which", "closest");
    return params;
  }());
  EXPECT_EQ("https://cdn.example.net/b.js?cb=1",
            *(*closest.FindList("requests"))[0].GetDict().FindString(
                "answered_by"));
}

TEST_F(ArchiveAgentQueriesTest, AListIsFilteredLimitedAndOrdered) {
  RecordAFewRequests();
  base::DictValue params;
  params.Set("contains", "CDN.example");
  params.Set("limit", 1);
  params.Set("newest_first", true);
  base::DictValue answer = Ask("list_requests", std::move(params));
  EXPECT_EQ(2, answer.FindInt("matching"));
  EXPECT_EQ(1, answer.FindInt("returned"));
  EXPECT_EQ("https://cdn.example.net/gone.js",
            *(*answer.FindList("requests"))[0].GetDict().FindString("url"));
}

TEST_F(ArchiveAgentQueriesTest, RequestsAreCountedByHost) {
  RecordAFewRequests();
  base::DictValue summary = Ask("summarize_requests");
  EXPECT_EQ(5, summary.FindInt("requests"));
  EXPECT_EQ(2, summary.FindInt("hosts"));
  const base::DictValue& busiest =
      (*summary.FindList("by_host"))[0].GetDict();
  EXPECT_EQ("example.org", *busiest.FindString("host"));
  EXPECT_EQ(3, busiest.FindInt("requests"));
  EXPECT_EQ(1, busiest.FindInt("missing"));
  EXPECT_EQ(1, busiest.FindInt("failed"));
  EXPECT_EQ(1, summary.FindDict("by_status")->FindInt("missing"));
  EXPECT_EQ(2, summary.FindDict("by_status")->FindInt("200"));
}

TEST_F(ArchiveAgentQueriesTest, AnUnknownToolOrFilterIsAnError) {
  EXPECT_TRUE(Ask("delete_everything").FindString("error"));
  base::DictValue params;
  params.Set("which", "everything");
  EXPECT_TRUE(Ask("list_requests", std::move(params)).FindString("error"));
}

}  // namespace
