// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/cdxj.h"

#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace warc {
namespace {

// A line exactly as py-wacz writes one, fields and all, so this reads what
// the tooling produces rather than what would be convenient.
constexpr char kRealLine[] =
    R"(1,0,0,127:8771)/a.html 20260910201354 {"url": "http://127.0.0.1:8771/a.html", )"
    R"("mime": "text/html", "status": "200", "digest": "sha1:3BW5C6O7ZLE7YA5DBDLG26IXODJ3HOLC", )"
    R"("length": "594", "offset": "20669", "filename": "chrdl-20260910201354-00000.warc.gz", )"
    R"("recordDigest": "sha256:43ba75cd", "referrer": "http://127.0.0.1:8771/index.html"})";

TEST(CdxjTest, ReadsALineAsWrittenByTheTooling) {
  std::optional<CdxjEntry> entry = ParseCdxjLine(kRealLine);
  ASSERT_TRUE(entry.has_value());

  EXPECT_EQ("1,0,0,127:8771)/a.html", entry->key);
  EXPECT_EQ("20260910201354", entry->timestamp);
  EXPECT_EQ("http://127.0.0.1:8771/a.html", entry->url);
  EXPECT_EQ("chrdl-20260910201354-00000.warc.gz", entry->filename);
  EXPECT_EQ("text/html", entry->mime);
  EXPECT_EQ("200", entry->status);
  EXPECT_EQ("sha1:3BW5C6O7ZLE7YA5DBDLG26IXODJ3HOLC", entry->digest);

  // Written as JSON strings, wanted as numbers: this is where a record is,
  // and seeking to "20669" would not work.
  EXPECT_EQ(20669u, entry->offset);
  EXPECT_EQ(594u, entry->length);
}

TEST(CdxjTest, AcceptsNumbersWrittenAsNumbers) {
  // Nothing requires them to be strings, and an index that writes them plainly
  // is still an index.
  std::optional<CdxjEntry> entry = ParseCdxjLine(
      R"(org,example)/ 20260101000000 {"offset": 12, "length": 34})");
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(12u, entry->offset);
  EXPECT_EQ(34u, entry->length);
}

TEST(CdxjTest, SkipsWhatIsNotAnEntry) {
  // The header of a secondary index describes the file, not a record in it.
  EXPECT_FALSE(
      ParseCdxjLine(
          R"(!meta 0 {"format": "cdxj-gzip-1.0", "filename": "index.cdx.gz"})")
          .has_value());
  EXPECT_FALSE(ParseCdxjLine("").has_value());
  EXPECT_FALSE(ParseCdxjLine("org,example)/ 20260101000000").has_value());
  EXPECT_FALSE(ParseCdxjLine("org,example)/ 20260101000000 not-json")
                   .has_value());
  EXPECT_FALSE(ParseCdxjLine("org,example)/ 20260101000000 [1,2]").has_value());
}

TEST(CdxjTest, OneBadLineDoesNotLoseTheRest) {
  const std::string index =
      R"(!meta 0 {"format": "cdxj-gzip-1.0"})" "\n"
      R"(org,example)/a 20260101000000 {"url": "http://example.org/a"})" "\n"
      "garbage that is not an entry at all\n"
      "\n"
      R"(org,example)/b 20260101000001 {"url": "http://example.org/b"})" "\n";

  const std::vector<CdxjEntry> entries = ParseCdxj(index);
  // An archive is not worth abandoning over a line of it.
  ASSERT_EQ(2u, entries.size());
  EXPECT_EQ("http://example.org/a", entries[0].url);
  EXPECT_EQ("http://example.org/b", entries[1].url);
}

std::vector<CdxjEntry> Captures(
    const std::vector<std::pair<std::string, std::string>>& key_and_time) {
  std::vector<CdxjEntry> entries;
  for (const auto& [key, timestamp] : key_and_time) {
    CdxjEntry entry;
    entry.key = key;
    entry.timestamp = timestamp;
    entry.url = key + "@" + timestamp;
    entries.push_back(entry);
  }
  return entries;
}

TEST(CdxjTest, FindsTheCaptureNearestTheMomentAsked) {
  const std::vector<CdxjEntry> entries = Captures({
      {"org,example)/a", "20260101000000"},
      {"org,example)/b", "20260101000000"},
      {"org,example)/b", "20260301120000"},
      {"org,example)/b", "20260601000000"},
      {"org,example)/c", "20260101000000"},
  });

  const CdxjEntry* found =
      FindNearestCapture(entries, "org,example)/b", "20260228000000");
  ASSERT_TRUE(found);
  EXPECT_EQ("20260301120000", found->timestamp);

  // Before everything and after everything both land on an end.
  EXPECT_EQ("20260101000000",
            FindNearestCapture(entries, "org,example)/b", "20250101000000")
                ->timestamp);
  EXPECT_EQ("20260601000000",
            FindNearestCapture(entries, "org,example)/b", "20270101000000")
                ->timestamp);
}

TEST(CdxjTest, NearestIsMeasuredInTimeNotInDigits) {
  // A second before midnight and a minute after it. Read as numbers the first
  // looks 64,000 away and the second only 100, so an overnight crawl would
  // hand back the later capture; read as times the first is nearer, which it
  // is.
  const std::vector<CdxjEntry> entries = Captures({
      {"org,example)/p", "20260831235959"},
      {"org,example)/p", "20260901000100"},
  });

  const CdxjEntry* found =
      FindNearestCapture(entries, "org,example)/p", "20260901000000");
  ASSERT_TRUE(found);
  EXPECT_EQ("20260831235959", found->timestamp);
}

TEST(CdxjTest, AKeyThatIsNotThereFindsNothing) {
  const std::vector<CdxjEntry> entries =
      Captures({{"org,example)/a", "20260101000000"}});
  EXPECT_FALSE(FindNearestCapture(entries, "org,example)/missing",
                                  "20260101000000"));
  EXPECT_FALSE(FindNearestCapture({}, "org,example)/a", "20260101000000"));
}

}  // namespace
}  // namespace warc
