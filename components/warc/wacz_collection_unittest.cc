// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/wacz_collection.h"

#include <string>
#include <string_view>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/bind.h"
#include "components/warc/gzip_member_writer.h"
#include "components/warc/surt.h"
#include "components/warc/warc_record.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/zlib/google/zip.h"
#include "url/gurl.h"

namespace warc {
namespace {

std::vector<uint8_t> ToBytes(std::string_view s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

std::string ToString(const std::vector<uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

// Deflates `data` into one gzip member, as a ".warc.gz" holds each record.
std::vector<uint8_t> AsMember(base::span<const uint8_t> data) {
  std::vector<uint8_t> out;
  GzipMemberWriter writer(
      base::BindLambdaForTesting([&](base::span<const uint8_t> chunk) {
        out.insert(out.end(), chunk.begin(), chunk.end());
        return true;
      }));
  EXPECT_TRUE(writer.Write(data));
  EXPECT_TRUE(writer.Finish());
  return out;
}

// Builds a container the way the tooling does: records written by this code's
// own writer, an index over them keyed by this code's own SURT, and the whole
// lot zipped with the archives stored. A lookup that works here has gone
// through the entire path rather than a mock of it.
class WaczCollectionTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }

  // Adds a response for `url`, captured at `timestamp`.
  void AddResponse(std::string_view url,
                   std::string_view timestamp,
                   std::string_view head,
                   std::string_view body) {
    RecordHeader header;
    header.type = RecordType::kResponse;
    header.target_uri = std::string(url);

    std::vector<uint8_t> block = ToBytes(head);
    const std::vector<uint8_t> body_bytes = ToBytes(body);
    block.insert(block.end(), body_bytes.begin(), body_bytes.end());

    const std::vector<uint8_t> member = AsMember(
        SerializeRecord(header, block, head.size(), DigestAlgorithm::kSha1));

    // The index records where the member sits, which is only knowable while
    // the archive is being assembled.
    index_ += base::StrCat(
        {ToSurt(GURL(std::string(url))), " ", timestamp, " {\"url\": \"", url,
         "\", \"status\": \"200\", \"length\": \"",
         base::NumberToString(member.size()), "\", \"offset\": \"",
         base::NumberToString(archive_.size()),
         "\", \"filename\": \"test.warc.gz\"}\n"});
    pages_ += base::StrCat({"{\"url\": \"", url, "\", \"ts\": \"", timestamp,
                            "\", \"title\": \"Page ", url, "\"}\n"});
    archive_.insert(archive_.end(), member.begin(), member.end());
  }

  std::optional<WaczCollection> Build(bool with_index = true) {
    const base::FilePath content = temp_dir_.GetPath().AppendASCII("content");
    const base::FilePath archive_dir = content.AppendASCII("archive");
    EXPECT_TRUE(base::CreateDirectory(archive_dir));
    EXPECT_TRUE(base::WriteFile(archive_dir.AppendASCII("test.warc.gz"),
                                base::span(archive_)));

    if (with_index) {
      const base::FilePath indexes = content.AppendASCII("indexes");
      EXPECT_TRUE(base::CreateDirectory(indexes));
      EXPECT_TRUE(base::WriteFile(indexes.AppendASCII("index.cdx.gz"),
                                  base::span(AsMember(ToBytes(index_)))));
    }

    const base::FilePath pages = content.AppendASCII("pages");
    EXPECT_TRUE(base::CreateDirectory(pages));
    EXPECT_TRUE(base::WriteFile(pages.AppendASCII("pages.jsonl"),
                                base::StrCat({kPagesHeaderLine, pages_})));

    const base::FilePath path = temp_dir_.GetPath().AppendASCII("c.wacz");
    EXPECT_TRUE(zip::Zip(content, path, /*include_hidden_files=*/false));
    return WaczCollection::Open(
        base::File(path, base::File::FLAG_OPEN | base::File::FLAG_READ));
  }

  static constexpr char kPagesHeaderLine[] =
      "{\"format\": \"json-pages-1.0\", \"id\": \"pages\"}\n";

  base::ScopedTempDir temp_dir_;
  std::vector<uint8_t> archive_;
  std::string index_;
  std::string pages_;
};

TEST_F(WaczCollectionTest, ServesAStoredResponse) {
  AddResponse("https://example.org/page", "20260101120000",
              "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n",
              "<html>archived</html>");

  std::optional<WaczCollection> collection = Build();
  ASSERT_TRUE(collection.has_value());

  std::optional<ArchivedResponse> response =
      collection->Lookup(GURL("https://example.org/page"), "20260101120000");
  ASSERT_TRUE(response.has_value());

  EXPECT_EQ(200, response->headers->response_code());
  EXPECT_EQ("text/html", response->headers->GetNormalizedHeader("Content-Type"));
  EXPECT_EQ("<html>archived</html>", ToString(response->body));
  EXPECT_EQ("20260101120000", response->timestamp);
}

TEST_F(WaczCollectionTest, BodyKeepsTheEncodingItWasStoredIn) {
  // Bodies are captured in wire form, so a gzipped response is still gzipped
  // here and its Content-Encoding still describes it. Handing it over as it
  // stands is what makes replay faithful; decoding it would leave headers
  // describing something the body no longer is.
  const std::vector<uint8_t> gzipped = AsMember(ToBytes("<html>gzipped</html>"));
  AddResponse("https://example.org/z", "20260101120000",
              "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
              "Content-Encoding: gzip\r\n\r\n",
              std::string_view(reinterpret_cast<const char*>(gzipped.data()),
                               gzipped.size()));

  std::optional<WaczCollection> collection = Build();
  ASSERT_TRUE(collection.has_value());
  std::optional<ArchivedResponse> response =
      collection->Lookup(GURL("https://example.org/z"), "20260101120000");
  ASSERT_TRUE(response.has_value());

  EXPECT_EQ("gzip", response->headers->GetNormalizedHeader("Content-Encoding"));
  EXPECT_EQ(gzipped, response->body);
}

TEST_F(WaczCollectionTest, FindsARecordHoweverTheUrlIsWritten) {
  AddResponse("https://example.org/a?b=2&a=1", "20260101120000",
              "HTTP/1.1 200 OK\r\n\r\n", "body");

  std::optional<WaczCollection> collection = Build();
  ASSERT_TRUE(collection.has_value());

  // The same request, spelled differently. Without the transform in the
  // lookup path each of these would miss a record the archive holds.
  for (const char* url : {
           "https://example.org/a?b=2&a=1",
           "https://example.org/a?a=1&b=2",
           "http://www.example.org/a?A=1&B=2",
           "https://example.org:443/a/?b=2&a=1#frag",
       }) {
    SCOPED_TRACE(url);
    std::optional<ArchivedResponse> response =
        collection->Lookup(GURL(url), "20260101120000");
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ("body", ToString(response->body));
  }
}

TEST_F(WaczCollectionTest, ServesTheCaptureNearestTheMomentAsked) {
  AddResponse("https://example.org/p", "20260101000000",
              "HTTP/1.1 200 OK\r\n\r\n", "january");
  AddResponse("https://example.org/p", "20260601000000",
              "HTTP/1.1 200 OK\r\n\r\n", "june");

  std::optional<WaczCollection> collection = Build();
  ASSERT_TRUE(collection.has_value());

  EXPECT_EQ("january", ToString(collection
                                    ->Lookup(GURL("https://example.org/p"),
                                             "20260201000000")
                                    ->body));
  EXPECT_EQ("june", ToString(collection
                                 ->Lookup(GURL("https://example.org/p"),
                                          "20260501000000")
                                 ->body));
}

TEST_F(WaczCollectionTest, WhatIsNotArchivedIsNotServed) {
  AddResponse("https://example.org/have", "20260101120000",
              "HTTP/1.1 200 OK\r\n\r\n", "body");

  std::optional<WaczCollection> collection = Build();
  ASSERT_TRUE(collection.has_value());

  // A miss is a miss. There is nothing here to fall back to, which is the
  // point: a page that reaches for something the archive lacks has to be
  // seen to be missing it, not quietly given the live web instead.
  EXPECT_FALSE(collection
                   ->Lookup(GURL("https://example.org/have-not"),
                            "20260101120000")
                   .has_value());
  EXPECT_FALSE(
      collection->Lookup(GURL("https://other.example/"), "20260101120000")
          .has_value());
  EXPECT_FALSE(collection->Lookup(GURL("about:blank"), "20260101120000")
                   .has_value());
}

TEST_F(WaczCollectionTest, ReadsThePagesTheCaptureVisited) {
  AddResponse("https://example.org/one", "20260101120000",
              "HTTP/1.1 200 OK\r\n\r\n", "1");
  AddResponse("https://example.org/two", "20260101120001",
              "HTTP/1.1 200 OK\r\n\r\n", "2");

  std::optional<WaczCollection> collection = Build();
  ASSERT_TRUE(collection.has_value());

  // Replay has to start somewhere, and this is the only record of which of
  // the archive's URLs were pages rather than what hung off them.
  ASSERT_EQ(2u, collection->pages().size());
  EXPECT_EQ("https://example.org/one", collection->pages()[0].url);
  EXPECT_EQ("20260101120000", collection->pages()[0].timestamp);
  EXPECT_EQ("Page https://example.org/one", collection->pages()[0].title);
  // The line declaring the format is not a page.
  EXPECT_EQ("https://example.org/two", collection->pages()[1].url);
}

TEST_F(WaczCollectionTest, APagesTimeIsAcceptedAsWrittenInThePageList) {
  AddResponse("https://example.org/p", "20260101000000",
              "HTTP/1.1 200 OK\r\n\r\n", "january");
  AddResponse("https://example.org/p", "20260601000000",
              "HTTP/1.1 200 OK\r\n\r\n", "june");

  std::optional<WaczCollection> collection = Build();
  ASSERT_TRUE(collection.has_value());

  // A WACZ spells a time one way in its index and another in its page list,
  // so replaying a page means handing back a time in the wrong form. Passed
  // through unconverted it matches no capture at all and quietly yields the
  // earliest one, which is the sort of miss nobody notices.
  std::optional<ArchivedResponse> response =
      collection->Lookup(GURL("https://example.org/p"), "2026-06-01T00:00:00Z");
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ("june", ToString(response->body));

  EXPECT_EQ("20260601000000", ToIndexTimestamp("2026-06-01T00:00:00Z"));
  EXPECT_EQ("20260601000000", ToIndexTimestamp("20260601000000"));
}

TEST_F(WaczCollectionTest, AnIndexOutOfOrderIsStillSearchable) {
  // A lookup searches the index, so an index that is not sorted does not fail
  // -- it quietly fails to find records the archive holds. These are added in
  // an order that puts the keys the wrong way round.
  AddResponse("https://example.org/old", "20260101120000",
              "HTTP/1.1 200 OK\r\n\r\n", "older");
  AddResponse("https://example.org/new", "20260101120000",
              "HTTP/1.1 200 OK\r\n\r\n", "newer");

  std::optional<WaczCollection> collection = Build();
  ASSERT_TRUE(collection.has_value());

  EXPECT_EQ("older", ToString(collection
                                  ->Lookup(GURL("https://example.org/old"),
                                           "20260101120000")
                                  ->body));
  EXPECT_EQ("newer", ToString(collection
                                  ->Lookup(GURL("https://example.org/new"),
                                           "20260101120000")
                                  ->body));
}

TEST_F(WaczCollectionTest, AContainerWithNoIndexCannotBeServed) {
  AddResponse("https://example.org/p", "20260101120000",
              "HTTP/1.1 200 OK\r\n\r\n", "body");

  // Without an index, finding a record means reading every archive in the
  // container -- which is the work the index exists to avoid, and not work a
  // page load can wait for.
  EXPECT_FALSE(Build(/*with_index=*/false).has_value());
}

}  // namespace
}  // namespace warc
