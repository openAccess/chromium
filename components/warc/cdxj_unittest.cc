// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/cdxj.h"

#include <string>
#include <string_view>
#include <vector>

#include "base/strings/strcat.h"
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

const CdxjEntry* Closest(const std::vector<CdxjEntry>& entries,
                         std::string_view key,
                         std::string_view timestamp = "20260101000000") {
  return FindClosestCapture(entries, key, timestamp,
                            [](const CdxjEntry&) { return true; });
}

// A config fetched with a new cache-buster every load. The buster matches no
// capture, so what is left to choose by is the rest of the query.
TEST(CdxjTest, TheClosestCaptureAgreesWithTheRestOfTheQuery) {
  const std::vector<CdxjEntry> entries = Captures({
      {"org,example)/config.js?cb=111&env=prod", "20260101000000"},
      {"org,example)/config.js?cb=222&env=staging", "20260101000000"},
  });
  const CdxjEntry* found =
      Closest(entries, "org,example)/config.js?cb=999&env=prod");
  ASSERT_TRUE(found);
  EXPECT_EQ("org,example)/config.js?cb=111&env=prod", found->key);
}

TEST(CdxjTest, AmongEquallyCloseCapturesTheNearestInTimeAnswers) {
  const std::vector<CdxjEntry> entries = Captures({
      {"org,example)/feed?t=1", "20260101000000"},
      {"org,example)/feed?t=2", "20260601000000"},
  });
  const CdxjEntry* found =
      Closest(entries, "org,example)/feed?t=3", "20260501000000");
  ASSERT_TRUE(found);
  EXPECT_EQ("20260601000000", found->timestamp);
}

// What a POST was told is no answer to a GET, however alike the two look.
TEST(CdxjTest, TheClosestCaptureWasMadeByTheSameMethod) {
  const std::vector<CdxjEntry> entries = Captures({
      {"org,example)/api?__wb_method=post&q=a", "20260101000000"},
  });
  EXPECT_FALSE(Closest(entries, "org,example)/api?q=a"));
  EXPECT_FALSE(Closest(entries, "org,example)/api?__wb_method=put&q=a"));

  const CdxjEntry* found =
      Closest(entries, "org,example)/api?__wb_method=post&q=b");
  ASSERT_TRUE(found);
  EXPECT_EQ("org,example)/api?__wb_method=post&q=a", found->key);
}

// "/a-b" sorts between "/a" and "/a?...", and "/ab" after them all; neither
// is "/a".
TEST(CdxjTest, TheClosestCaptureIsOfTheSamePath) {
  const std::vector<CdxjEntry> entries = Captures({
      {"org,example)/a", "20260101000000"},
      {"org,example)/a-b?x=2", "20260101000000"},
      {"org,example)/a?x=1", "20260101000000"},
      {"org,example)/ab", "20260101000000"},
  });

  const CdxjEntry* found = Closest(entries, "org,example)/a?x=2");
  ASSERT_TRUE(found);
  EXPECT_EQ("org,example)/a?x=1", found->key);

  // Asked with a query, a capture made without one still counts.
  found = Closest(entries, "org,example)/ab?x=2");
  ASSERT_TRUE(found);
  EXPECT_EQ("org,example)/ab", found->key);

  EXPECT_FALSE(Closest(entries, "org,example)/b?x=1"));
  EXPECT_FALSE(Closest(entries, "org,example)/a-?x=2"));
}

const CdxjEntry* Remade(const std::vector<CdxjEntry>& entries,
                        std::string_view key,
                        std::string_view timestamp = "20260101120000") {
  return FindCaptureIgnoringVisitValues(entries, key, timestamp,
                                        [](const CdxjEntry&) { return true; });
}

constexpr char kId[] = "50b0c9e2-bcca-4206-a3b7-068d73120f22";
constexpr char kOtherId[] = "9f8e7d6c-5b4a-4321-8fed-cba987654321";

// The consent frame a news site opened before it had an id for the visitor,
// opened again once it has one: the id in its own parameter, and strung among
// tildes in another the capture did not have.
TEST(CdxjTest, AnIdMadeSinceTheCaptureIsSetAside) {
  const std::vector<CdxjEntry> entries = Captures({
      {"com,privacy-mgmt,cdn)/us_pm/index.html?message_id=1504881&uuid=null&"
       "version=v1",
       "20260101115950"},
  });

  const CdxjEntry* found =
      Remade(entries, base::StrCat({"com,privacy-mgmt,cdn)/us_pm/index.html?"
                                    "concatenateduuid=~",
                                    kId, "~~&message_id=1504881&uuid=", kId,
                                    "&version=v1"}));
  ASSERT_TRUE(found);
  EXPECT_EQ("20260101115950", found->timestamp);
}

// The other way about: the capture had an id the visit has not made yet.
TEST(CdxjTest, AnIdTheVisitHasNotMadeYetIsSetAside) {
  const std::vector<CdxjEntry> entries = Captures({
      {base::StrCat({"org,example)/frame?uuid=", kId, "&w=300"}),
       "20260101115950"},
  });
  EXPECT_TRUE(Remade(entries, "org,example)/frame?uuid=null&w=300"));
  EXPECT_TRUE(Remade(entries, "org,example)/frame?uuid=&w=300"));
  EXPECT_TRUE(Remade(entries, "org,example)/frame?w=300"));
}

// An id may be what chose the page. A site that fetches the parts of its
// pages by their UUIDs asks for another part with another UUID.
TEST(CdxjTest, OneIdIsNotTakenForAnother) {
  const std::vector<CdxjEntry> entries = Captures({
      {base::StrCat({"com,apnews)/fragment?_fragmentid=", kId}),
       "20260101115950"},
      {"org,example)/frame?session=0123456789abcdef0123", "20260101115950"},
  });
  EXPECT_FALSE(
      Remade(entries, base::StrCat({"com,apnews)/fragment?_fragmentid=",
                                    kOtherId})));
  EXPECT_FALSE(
      Remade(entries, "org,example)/frame?session=fedcba9876543210fedc"));
}

// A cache-buster stamped with the time, where the capture had its own -- or
// had none, as a library adds one to a request it would not have cached.
TEST(CdxjTest, AReadingOfTheClockIsSetAside) {
  // 2026-01-01T11:59:50Z in milliseconds, as captured, and 15 seconds later
  // as replayed: the replay's clock starts at its moment and runs on.
  const std::vector<CdxjEntry> entries = Captures({
      {"org,example)/frame?cb=1767268790123&w=300", "20260101115950"},
      {"org,example)/page", "20260101115950"},
  });
  EXPECT_TRUE(Remade(entries, "org,example)/frame?cb=1767268805000&w=300"));
  // In seconds rather than milliseconds, it is still the clock.
  EXPECT_TRUE(Remade(entries, "org,example)/frame?cb=1767268805&w=300"));
  EXPECT_TRUE(Remade(entries, "org,example)/page?_=1767268805000"));

  // Thirteen digits that are no time near the visit are some other number,
  // and one may have chosen the page.
  EXPECT_FALSE(Remade(entries, "org,example)/frame?cb=1467268805000&w=300"));
  // Nor can a reading be told for one without a moment to read it against.
  EXPECT_FALSE(
      Remade(entries, "org,example)/frame?cb=1767268805000&w=300", ""));
}

TEST(CdxjTest, ARandomNumberIsSetAside) {
  const std::vector<CdxjEntry> entries = Captures({
      {"org,example)/ad.html?rnd=0.8473625101&slot=top", "20260101115950"},
  });
  EXPECT_TRUE(Remade(entries, "org,example)/ad.html?rnd=0.12345678&slot=top"));
  EXPECT_FALSE(
      Remade(entries, "org,example)/ad.html?rnd=0.12345678&slot=side"));
}

// Whatever is not a value made for the visit has to agree, as it would for
// an exact match: setting one value aside sets aside no other.
TEST(CdxjTest, WhatTheVisitDidNotMakeMustAgree) {
  const std::vector<CdxjEntry> entries = Captures({
      {"org,example)/article?id=1&uuid=null", "20260101115950"},
      {"org,example)/search?q=null", "20260101115950"},
  });
  EXPECT_FALSE(
      Remade(entries, base::StrCat({"org,example)/article?id=2&uuid=", kId})));
  // A value where the capture had none is no id unless it looks like one.
  EXPECT_FALSE(Remade(entries, "org,example)/search?q=news"));
  // A number counted out is chosen however long it is, and so is a word that
  // happens to be spelled in hex.
  EXPECT_FALSE(Remade(entries, "org,example)/search?q=1234567890123456"));
  EXPECT_FALSE(Remade(entries, "org,example)/search?q=deadbeefdeadbeef"));
  // An id among words is part of a name, not an id.
  EXPECT_FALSE(
      Remade(entries, base::StrCat({"org,example)/search?q=story-", kId})));
}

// A parameter given twice is a list the page chose, and is compared as one.
TEST(CdxjTest, AListIsNotSetAside) {
  const std::vector<CdxjEntry> entries = Captures({
      {"org,example)/frame?a=null&a=x", "20260101115950"},
  });
  EXPECT_FALSE(
      Remade(entries, base::StrCat({"org,example)/frame?a=", kId, "&a=x"})));
  EXPECT_TRUE(Remade(entries, "org,example)/frame?a=null&a=x"));
}

TEST(CdxjTest, ARemadeRequestIsAnsweredByItsOwnMethod) {
  const std::vector<CdxjEntry> entries = Captures({
      {"org,example)/frame?__wb_method=post&uuid=null", "20260101115950"},
  });
  EXPECT_FALSE(Remade(entries, base::StrCat({"org,example)/frame?uuid=", kId})));
  EXPECT_TRUE(Remade(entries, base::StrCat({"org,example)/frame?__wb_method="
                                            "post&uuid=",
                                            kId})));
}

TEST(CdxjTest, AmongRemadeRequestsTheNearestInTimeAnswers) {
  const std::vector<CdxjEntry> entries = Captures({
      {"org,example)/frame?uuid=null", "20260101000000"},
      {base::StrCat({"org,example)/frame?uuid=", kOtherId}), "20260101115950"},
      {"org,example)/frame?uuid=undefined", "20260601000000"},
  });
  const CdxjEntry* found =
      Remade(entries, base::StrCat({"org,example)/frame?uuid=", kId}),
             "20260501000000");
  ASSERT_TRUE(found);
  EXPECT_EQ("20260601000000", found->timestamp);
}

}  // namespace
}  // namespace warc
