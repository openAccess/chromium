// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/warc_record_parser.h"

#include <string>
#include <string_view>
#include <vector>

#include "components/warc/warc_record.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace warc {
namespace {

std::vector<uint8_t> ToBytes(std::string_view s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

std::string ToString(base::span<const uint8_t> bytes) {
  return std::string(bytes.begin(), bytes.end());
}

// A response record as the writer produces one, so the tests below read what
// this code actually writes rather than what it imagines it writes.
std::vector<uint8_t> SerializedResponse(std::string_view body) {
  RecordHeader header;
  header.type = RecordType::kResponse;
  header.target_uri = "https://example.org/page";
  header.record_id = "<urn:uuid:11111111-2222-3333-4444-555555555555>";
  header.ip_address = "192.0.2.10";
  header.protocol = "http/1.1";

  const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n";
  std::vector<uint8_t> block = ToBytes(head);
  const std::vector<uint8_t> body_bytes = ToBytes(body);
  block.insert(block.end(), body_bytes.begin(), body_bytes.end());

  return SerializeRecord(header, block, head.size(), DigestAlgorithm::kSha1);
}

TEST(WarcRecordParserTest, ReadsBackWhatTheWriterWrote) {
  const std::vector<uint8_t> serialized = SerializedResponse("<html>hi</html>");

  std::optional<ParsedRecord> record = ParseRecord(serialized);
  ASSERT_TRUE(record.has_value());

  EXPECT_EQ("WARC/1.1", record->version);
  EXPECT_EQ("response", record->Field("WARC-Type"));
  EXPECT_EQ("https://example.org/page", record->Field("WARC-Target-URI"));
  EXPECT_EQ("<urn:uuid:11111111-2222-3333-4444-555555555555>",
            record->Field("WARC-Record-ID"));
  EXPECT_EQ("192.0.2.10", record->Field("WARC-IP-Address"));
  EXPECT_EQ("http/1.1", record->Field("WARC-Protocol"));
  EXPECT_EQ("application/http;msgtype=response", record->Field("Content-Type"));

  // The block comes back exactly, which is the point of the format: a replay
  // has to serve the bytes that arrived, not a re-rendering of them.
  EXPECT_EQ("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<html>hi</html>",
            ToString(record->block));
  EXPECT_EQ(serialized.size(), record->size);
}

TEST(WarcRecordParserTest, DigestsSurviveTheRoundTrip) {
  const std::vector<uint8_t> serialized = SerializedResponse("<html>hi</html>");
  std::optional<ParsedRecord> record = ParseRecord(serialized);
  ASSERT_TRUE(record.has_value());

  // Recomputed from the block that was read back, so this checks the parser
  // returned the same bytes the digest was taken over -- an independent test
  // of the block boundaries, and the check a reader makes to know an archive
  // has not rotted.
  EXPECT_EQ(record->Field("WARC-Block-Digest"),
            ComputeDigest(record->block, DigestAlgorithm::kSha1));
}

TEST(WarcRecordParserTest, FieldLookupIgnoresCase) {
  std::optional<ParsedRecord> record = ParseRecord(SerializedResponse("x"));
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ("response", record->Field("warc-type"));
  EXPECT_EQ("response", record->Field("WARC-TYPE"));
  EXPECT_TRUE(record->Field("WARC-Nonexistent").empty());
}

TEST(WarcRecordParserTest, WalksAConcatenationOfRecords) {
  std::vector<uint8_t> archive;
  for (std::string_view body : {"first", "second", "third"}) {
    const std::vector<uint8_t> record = SerializedResponse(body);
    archive.insert(archive.end(), record.begin(), record.end());
  }

  std::vector<std::string> bodies;
  base::span<const uint8_t> rest(archive);
  while (!rest.empty()) {
    std::optional<ParsedRecord> record = ParseRecord(rest);
    ASSERT_TRUE(record.has_value());
    const std::string block = ToString(record->block);
    bodies.push_back(block.substr(block.find("\r\n\r\n") + 4));
    rest = rest.subspan(record->size);
  }
  EXPECT_EQ(std::vector<std::string>({"first", "second", "third"}), bodies);
}

TEST(WarcRecordParserTest, BlockMayHoldArbitraryBinaryContent) {
  RecordHeader header;
  header.type = RecordType::kResponse;
  header.target_uri = "https://example.org/image";
  // Embedded NULs, a stray CRLF pair, and high bytes -- a gzipped body looks
  // like this, and a parser that scanned for text would cut it in the wrong
  // place.
  const std::vector<uint8_t> block = {0x1f, 0x8b, 0x00, '\r', '\n',
                                      '\r', '\n', 0xff, 0x00, 0x7f};
  const std::vector<uint8_t> serialized =
      SerializeRecord(header, block, block.size(), DigestAlgorithm::kSha1);

  std::optional<ParsedRecord> record = ParseRecord(serialized);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(block, std::vector<uint8_t>(record->block.begin(),
                                        record->block.end()));
  EXPECT_EQ(serialized.size(), record->size);
}

TEST(WarcRecordParserTest, EmptyBlockIsARecord) {
  RecordHeader header;
  header.type = RecordType::kWarcinfo;
  const std::vector<uint8_t> serialized =
      SerializeRecord(header, {}, 0, DigestAlgorithm::kSha1);

  std::optional<ParsedRecord> record = ParseRecord(serialized);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ("warcinfo", record->Field("WARC-Type"));
  EXPECT_TRUE(record->block.empty());
  EXPECT_EQ(serialized.size(), record->size);
}

TEST(WarcRecordParserTest, ShortInputIsIncompleteRatherThanBroken) {
  const std::vector<uint8_t> serialized = SerializedResponse("<html>hi</html>");
  // Every prefix of a record is a record still arriving. Calling one of them
  // malformed would have a reader abandon an archive it is halfway through
  // reading.
  for (size_t keep = 1; keep < serialized.size(); ++keep) {
    SCOPED_TRACE(keep);
    bool incomplete = false;
    EXPECT_FALSE(
        ParseRecord(base::span(serialized).first(keep), &incomplete)
            .has_value());
    EXPECT_TRUE(incomplete);
  }
}

TEST(WarcRecordParserTest, RefusesWhatIsNotARecord) {
  struct {
    const char* name;
    std::string data;
  } const cases[] = {
      {"not a warc record at all", "GET / HTTP/1.1\r\n\r\n"},
      {"no content length", "WARC/1.1\r\nWARC-Type: response\r\n\r\nbody\r\n\r\n"},
      {"unparseable content length",
       "WARC/1.1\r\nContent-Length: soon\r\n\r\n\r\n\r\n"},
      {"field without a colon", "WARC/1.1\r\nnonsense\r\nContent-Length: 0\r\n\r\n\r\n\r\n"},
      {"folded field",
       "WARC/1.1\r\nWARC-Type: response\r\n  folded\r\nContent-Length: 0\r\n\r\n\r\n\r\n"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    bool incomplete = false;
    EXPECT_FALSE(ParseRecord(ToBytes(test.data), &incomplete).has_value());
    EXPECT_FALSE(incomplete);
  }
}

TEST(WarcRecordParserTest, RefusesABlockThatDoesNotEndWhereItPromised) {
  // A Content-Length longer than the block means the separator is not where it
  // should be. Trusting the length would frame every later record wrongly, so
  // the record is refused rather than guessed at.
  const std::string data =
      "WARC/1.1\r\nContent-Length: 4\r\n\r\nbodylonger\r\n\r\n";
  bool incomplete = false;
  EXPECT_FALSE(ParseRecord(ToBytes(data), &incomplete).has_value());
  EXPECT_FALSE(incomplete);
}

}  // namespace
}  // namespace warc
