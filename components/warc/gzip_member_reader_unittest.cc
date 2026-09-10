// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/gzip_member_reader.h"

#include <string>
#include <string_view>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/bind.h"
#include "components/warc/gzip_member_writer.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace warc {
namespace {

std::vector<uint8_t> ToBytes(std::string_view s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

std::string ToString(base::span<const uint8_t> bytes) {
  return std::string(bytes.begin(), bytes.end());
}

// One member holding `contents`, as the writer produces them.
std::vector<uint8_t> Member(std::string_view contents) {
  std::vector<uint8_t> out;
  GzipMemberWriter writer(
      base::BindLambdaForTesting([&](base::span<const uint8_t> data) {
        out.insert(out.end(), data.begin(), data.end());
        return true;
      }));
  EXPECT_TRUE(writer.Write(ToBytes(contents)));
  EXPECT_TRUE(writer.Finish());
  return out;
}

TEST(GzipMemberReaderTest, ReadsBackWhatTheWriterWrote) {
  const std::string contents = "WARC/1.1\r\nContent-Length: 0\r\n\r\n\r\n\r\n";
  size_t compressed_size = 0;
  std::optional<std::vector<uint8_t>> out =
      InflateGzipMember(Member(contents), &compressed_size);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(contents, ToString(*out));
}

TEST(GzipMemberReaderTest, PreservesArbitraryBinaryContent) {
  const std::vector<uint8_t> contents = {0x1f, 0x8b, 0x00, 0xff, 0x00, 0x7f};
  std::vector<uint8_t> member;
  {
    GzipMemberWriter writer(
        base::BindLambdaForTesting([&](base::span<const uint8_t> data) {
          member.insert(member.end(), data.begin(), data.end());
          return true;
        }));
    ASSERT_TRUE(writer.Write(contents));
    ASSERT_TRUE(writer.Finish());
  }
  std::optional<std::vector<uint8_t>> out = InflateGzipMember(member, nullptr);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(contents, *out);
}

TEST(GzipMemberReaderTest, StopsAtTheMemberBoundary) {
  // The property the whole layout exists for: a record can be read without
  // reading, or even inflating, the records that follow it.
  std::vector<uint8_t> archive = Member("first");
  const size_t first_size = archive.size();
  const std::vector<uint8_t> second = Member("second");
  archive.insert(archive.end(), second.begin(), second.end());

  size_t compressed_size = 0;
  std::optional<std::vector<uint8_t>> out =
      InflateGzipMember(archive, &compressed_size);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ("first", ToString(*out));
  EXPECT_EQ(first_size, compressed_size);

  // And the size it reported is exactly where the next one starts.
  std::optional<std::vector<uint8_t>> next = InflateGzipMember(
      base::span(archive).subspan(compressed_size), nullptr);
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ("second", ToString(*next));
}

TEST(GzipMemberReaderTest, TruncatedMemberIsRefused) {
  const std::vector<uint8_t> member = Member("a body that was cut short");
  for (size_t keep : {size_t{1}, member.size() / 2, member.size() - 1}) {
    SCOPED_TRACE(keep);
    // Half a member decompresses to something, and handing that back as though
    // it were the record would present a fragment as the whole.
    EXPECT_FALSE(
        InflateGzipMember(base::span(member).first(keep), nullptr).has_value());
  }
}

TEST(GzipMemberReaderTest, RefusesWhatIsNotAMember) {
  EXPECT_FALSE(InflateGzipMember(ToBytes(""), nullptr).has_value());
  EXPECT_FALSE(InflateGzipMember(ToBytes("WARC/1.1\r\n"), nullptr).has_value());
}

}  // namespace
}  // namespace warc
