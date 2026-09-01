// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/warc_record.h"

#include <string>
#include <string_view>
#include <vector>

#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace warc {
namespace {

std::vector<uint8_t> ToBytes(std::string_view s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

std::string ToString(const std::vector<uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

// Returns the header portion of a serialized record: every field line
// including its terminating CRLF, but not the blank line that ends the header.
std::string HeadOf(const std::vector<uint8_t>& record) {
  const std::string text = ToString(record);
  const size_t end = text.find("\r\n\r\n");
  CHECK_NE(end, std::string::npos);
  return text.substr(0, end + 2);
}

TEST(WarcRecordTest, StructureFollowsSpecLayout) {
  RecordHeader header;
  header.type = RecordType::kResponse;
  header.target_uri = "https://example.org/";
  header.record_id = "<urn:uuid:11111111-2222-3333-4444-555555555555>";
  header.date = base::Time::UnixEpoch();

  const std::vector<uint8_t> block = ToBytes("HTTP/1.1 200 OK\r\n\r\nhi");
  const std::vector<uint8_t> record =
      SerializeRecord(header, block, block.size(), DigestAlgorithm::kNone);
  const std::string text = ToString(record);

  EXPECT_TRUE(text.starts_with("WARC/1.1\r\n"));
  // The block must appear verbatim, followed by the two-CRLF separator.
  EXPECT_TRUE(text.ends_with("HTTP/1.1 200 OK\r\n\r\nhi\r\n\r\n"));
  EXPECT_NE(text.find("WARC-Type: response\r\n"), std::string::npos);
  EXPECT_NE(text.find("WARC-Target-URI: https://example.org/\r\n"),
            std::string::npos);
  EXPECT_NE(text.find("Content-Type: application/http;msgtype=response\r\n"),
            std::string::npos);
}

TEST(WarcRecordTest, ContentLengthMatchesBlockExactly) {
  RecordHeader header;
  header.target_uri = "https://example.org/";

  // Include bytes that are not valid UTF-8 and an embedded NUL, as a real
  // gzip-compressed body would contain.
  const std::vector<uint8_t> block = {0x1f, 0x8b, 0x08, 0x00, 0x00,
                                      0xff, 0xfe, 0x42, 0x00};
  const std::vector<uint8_t> record =
      SerializeRecord(header, block, block.size(), DigestAlgorithm::kNone);

  EXPECT_NE(HeadOf(record).find("Content-Length: 9\r\n"), std::string::npos);

  // The payload bytes must survive serialization untouched; this is the whole
  // point of byte-faithful capture.
  const std::string text = ToString(record);
  const size_t body_start = text.find("\r\n\r\n") + 4;
  const std::vector<uint8_t> round_tripped(
      record.begin() + body_start, record.begin() + body_start + block.size());
  EXPECT_EQ(block, round_tripped);
}

TEST(WarcRecordTest, EmptyPayloadDigestMatchesWellKnownValue) {
  // The SHA-1 of the empty string, unpadded base32, is a value that appears
  // throughout CDX indexes for empty payloads. Matching it confirms both the
  // hash and the base32 alphabet/padding are what WARC tooling expects.
  EXPECT_EQ("sha1:3I42H3S6NNFQ2MSVX7XZKYAYSCX5QBYJ",
            ComputeDigest({}, DigestAlgorithm::kSha1));
}

TEST(WarcRecordTest, BlockAndPayloadDigestsCoverDifferentRanges) {
  RecordHeader header;
  header.target_uri = "https://example.org/";

  const std::string_view http = "HTTP/1.1 200 OK\r\n\r\n";
  const std::string_view body = "payload";
  const std::vector<uint8_t> block =
      ToBytes(std::string(http) + std::string(body));
  const std::vector<uint8_t> record =
      SerializeRecord(header, block, http.size(), DigestAlgorithm::kSha1);
  const std::string head = HeadOf(record);

  // The payload digest must cover only the entity body, not the HTTP headers.
  const std::string expected_payload =
      ComputeDigest(ToBytes(body), DigestAlgorithm::kSha1);
  const std::string expected_block =
      ComputeDigest(block, DigestAlgorithm::kSha1);

  EXPECT_NE(head.find("WARC-Payload-Digest: " + expected_payload),
            std::string::npos);
  EXPECT_NE(head.find("WARC-Block-Digest: " + expected_block),
            std::string::npos);
  EXPECT_NE(expected_payload, expected_block);
}

TEST(WarcRecordTest, PayloadDigestOmittedWhenNoDistinctPayload) {
  RecordHeader header;
  header.type = RecordType::kWarcinfo;

  const std::vector<uint8_t> block = ToBytes("software: test\r\n");
  const std::vector<uint8_t> record =
      SerializeRecord(header, block, block.size(), DigestAlgorithm::kSha1);
  const std::string head = HeadOf(record);

  EXPECT_NE(head.find("WARC-Block-Digest: "), std::string::npos);
  EXPECT_EQ(head.find("WARC-Payload-Digest: "), std::string::npos);
}

TEST(WarcRecordTest, DigestsOmittedEntirelyForNone) {
  RecordHeader header;
  header.target_uri = "https://example.org/";

  const std::vector<uint8_t> block = ToBytes("body");
  const std::string head =
      HeadOf(SerializeRecord(header, block, 0, DigestAlgorithm::kNone));

  EXPECT_EQ(head.find("WARC-Block-Digest"), std::string::npos);
  EXPECT_EQ(head.find("WARC-Payload-Digest"), std::string::npos);
}

TEST(WarcRecordTest, DateUsesMicrosecondPrecisionUtc) {
  const base::Time time =
      base::Time::UnixEpoch() + base::Microseconds(1'000'000'123'456);
  const std::string formatted = FormatWarcDate(time);

  // 1000000 seconds after the epoch is 1970-01-12T13:46:40Z.
  EXPECT_EQ("1970-01-12T13:46:40.123456Z", formatted);
}

TEST(WarcRecordTest, GeneratedRecordIdsAreBracketedUuidUrns) {
  const std::string id = GenerateRecordId();

  EXPECT_TRUE(id.starts_with("<urn:uuid:"));
  EXPECT_TRUE(id.ends_with(">"));
  EXPECT_NE(id, GenerateRecordId());
}

TEST(WarcRecordTest, EmptyFieldsAreOmitted) {
  RecordHeader header;
  header.target_uri = "https://example.org/";
  // ip_address, concurrent_to, filename etc. deliberately left empty.

  const std::string head =
      HeadOf(SerializeRecord(header, ToBytes("x"), 1, DigestAlgorithm::kNone));

  EXPECT_EQ(head.find("WARC-IP-Address"), std::string::npos);
  EXPECT_EQ(head.find("WARC-Concurrent-To"), std::string::npos);
  EXPECT_EQ(head.find("WARC-Filename"), std::string::npos);
}

TEST(WarcRecordTest, WarcinfoBlockIsWarcFields) {
  const std::vector<uint8_t> block = BuildWarcinfoBlock({
      {"software", "Chromium"},
      {"format", "WARC File Format 1.1"},
      {"skipped", ""},
  });

  EXPECT_EQ("software: Chromium\r\nformat: WARC File Format 1.1\r\n",
            ToString(block));
}

}  // namespace
}  // namespace warc
