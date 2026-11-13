// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/side_panel/archive/archive_agent_queries.h"

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/json/json_reader.h"
#include "base/memory/scoped_refptr.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/wacz_replay/replay_log.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/warc/cdxj.h"
#include "components/warc/wacz_collection.h"
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
              const std::string& answered_by = std::string(),
              warc::MissExplanation miss = warc::MissExplanation()) {
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
    outcome.miss = std::move(miss);
    outcome.time = base::Time::Now();
    if (!archive_) {
      // Never there, so never read: these tests ask only what the tab asked.
      archive_ = warc::WaczArchive::Open(base::FilePath("/archives/a.wacz"));
    }
    ReplayLog::Record(web_contents(), archive_,
                      base::Time::UnixEpoch() + base::Days(20000), outcome);
  }

  void RecordMissing(const std::string& url,
                     warc::MissReason reason,
                     const std::string& nearest_url = std::string()) {
    warc::MissExplanation miss;
    miss.reason = reason;
    if (!nearest_url.empty()) {
      miss.url = nearest_url;
      miss.method = "GET";
      miss.timestamp = "20261109211701";
    }
    Record(url, 0, /*missing=*/true, net::OK, std::string(), std::move(miss));
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
    RecordMissing("https://example.org/missing.png",
                  warc::MissReason::kPathNotCaptured);
    Record("https://cdn.example.net/b.js?cb=2", 200, false, net::OK,
           "https://cdn.example.net/b.js?cb=1");
    Record("https://example.org/loop", 0, false, net::ERR_TOO_MANY_REDIRECTS);
    Record("https://cdn.example.net/gone.js", 404);
  }

  scoped_refptr<warc::WaczArchive> archive_;
};

warc::CdxjEntry Entry(const std::string& key,
                      const std::string& url,
                      const std::string& status = "200",
                      const std::string& mime = "application/json") {
  warc::CdxjEntry entry;
  entry.key = key;
  entry.timestamp = "20261109211701";
  entry.url = url;
  entry.status = status;
  entry.mime = mime;
  return entry;
}

// An index as an archive's is: sorted by key.
std::vector<warc::CdxjEntry> AnIndex() {
  return {
      Entry("net,example)/x", "https://example.net/x"),
      Entry("org,example)/api", "https://example.org/api"),
      Entry("org,example)/api-docs", "https://example.org/api-docs", "200",
            "text/html"),
      Entry("org,example)/api?__wb_method=post&q=1", "https://example.org/api"),
      Entry("org,example)/api?v=2", "https://example.org/api?v=2", "404"),
      Entry("org,example)/other", "https://example.org/other"),
  };
}

base::DictValue Search(base::DictValue params) {
  std::optional<base::DictValue> answer = base::JSONReader::ReadDict(
      SearchArchiveIndex(AnIndex(), params), base::JSON_PARSE_RFC);
  EXPECT_TRUE(answer);
  return answer ? std::move(*answer) : base::DictValue();
}

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

// A missing request says why, and which capture came nearest where one did.
TEST_F(ArchiveAgentQueriesTest, AMissingRequestSaysWhy) {
  RecordMissing("https://ads.example.com/pixel.gif",
                warc::MissReason::kHostNotCaptured);
  RecordMissing("https://example.org/frame?uuid=null",
                warc::MissReason::kOtherQuery,
                "https://example.org/frame?uuid=abc");
  base::DictValue params;
  params.Set("which", "missing");
  base::DictValue answer = Ask("list_requests", std::move(params));
  const base::ListValue& rows = *answer.FindList("requests");
  ASSERT_EQ(2u, rows.size());

  EXPECT_EQ("host_never_captured", *rows[0].GetDict().FindString("why_missing"));
  EXPECT_FALSE(rows[0].GetDict().Find("nearest_capture"));

  EXPECT_EQ("other_query_only", *rows[1].GetDict().FindString("why_missing"));
  const base::DictValue* nearest = rows[1].GetDict().FindDict("nearest_capture");
  ASSERT_TRUE(nearest);
  EXPECT_EQ("https://example.org/frame?uuid=abc", *nearest->FindString("url"));
  EXPECT_EQ("GET", *nearest->FindString("method"));
  EXPECT_EQ("2026-11-09T21:17:01.000Z", *nearest->FindString("captured_at"));
}

TEST_F(ArchiveAgentQueriesTest, MissesAreCountedByWhy) {
  RecordAFewRequests();
  RecordMissing("https://ads.example.com/a.gif",
                warc::MissReason::kHostNotCaptured);
  RecordMissing("https://ads.example.com/b.gif",
                warc::MissReason::kHostNotCaptured);
  const base::DictValue summary = Ask("summarize_requests");
  const base::DictValue* reasons = summary.FindDict("missing_by_reason");
  ASSERT_TRUE(reasons);
  EXPECT_EQ(2, reasons->FindInt("host_never_captured"));
  EXPECT_EQ(1, reasons->FindInt("url_never_captured"));
  EXPECT_EQ(2u, reasons->size());
}

// Everything at a URL's path, by any method and with any query: what the
// archive holds of a resource when it does not hold the request for it.
TEST(ArchiveAgentSearchTest, FindsWhatTheArchiveHoldsAtAUrl) {
  base::DictValue params;
  params.Set("url", "https://example.org/api");
  base::DictValue answer = Search(std::move(params));
  EXPECT_EQ(6, answer.FindInt("index_entries"));
  // Everything from example.org, and nothing from example.net.
  EXPECT_EQ(5, answer.FindInt("host_captures"));
  // Not /api-docs, which only begins the same.
  EXPECT_EQ(3, answer.FindInt("matching"));
  const base::ListValue& captures = *answer.FindList("captures");
  ASSERT_EQ(3u, captures.size());

  EXPECT_EQ("GET", *captures[0].GetDict().FindString("method"));
  EXPECT_EQ(true, captures[0].GetDict().FindBool("this_url"));
  EXPECT_EQ("2026-11-09T21:17:01.000Z",
            *captures[0].GetDict().FindString("captured_at"));

  EXPECT_EQ("POST", *captures[1].GetDict().FindString("method"));

  EXPECT_EQ("https://example.org/api?v=2",
            *captures[2].GetDict().FindString("url"));
  EXPECT_EQ(false, captures[2].GetDict().FindBool("this_url"));
  EXPECT_EQ(404, captures[2].GetDict().FindInt("status"));
}

TEST(ArchiveAgentSearchTest, FindsWhatTheArchiveHoldsByText) {
  base::DictValue params;
  params.Set("contains", "Example.ORG/API");
  params.Set("limit", 2);
  base::DictValue answer = Search(std::move(params));
  EXPECT_EQ(4, answer.FindInt("matching"));
  EXPECT_EQ(2, answer.FindInt("returned"));
  EXPECT_FALSE(answer.Find("host_captures"));
}

TEST(ArchiveAgentSearchTest, NeedsSomethingToSearchFor) {
  EXPECT_TRUE(Search(base::DictValue()).FindString("error"));
  base::DictValue params;
  params.Set("url", "file:///etc/passwd");
  EXPECT_TRUE(Search(std::move(params)).FindString("error"));
}

TEST_F(ArchiveAgentQueriesTest, ATabNotReplayingHasNoArchiveToSearch) {
  base::test::TestFuture<std::string> answer;
  base::DictValue params;
  params.Set("contains", "a");
  SearchArchive(web_contents(), params, answer.GetCallback());
  std::optional<base::DictValue> error =
      base::JSONReader::ReadDict(answer.Get(), base::JSON_PARSE_RFC);
  ASSERT_TRUE(error);
  EXPECT_TRUE(error->FindString("error"));
}

// The search goes to the archive's own sequence and back. An archive that
// could not be read says so rather than answering nothing.
TEST_F(ArchiveAgentQueriesTest, AnUnreadableArchiveSaysSoWhenSearched) {
  RecordAFewRequests();
  base::test::TestFuture<std::string> answer;
  base::DictValue params;
  params.Set("contains", "a");
  SearchArchive(web_contents(), params, answer.GetCallback());
  std::optional<base::DictValue> error =
      base::JSONReader::ReadDict(answer.Get(), base::JSON_PARSE_RFC);
  ASSERT_TRUE(error);
  ASSERT_TRUE(error->FindString("error"));
  EXPECT_NE(std::string::npos,
            error->FindString("error")->find("could not be read"));
}

}  // namespace
