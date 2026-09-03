// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/gzip_member_writer.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/bind.h"
#include "components/warc/warc_test_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace warc {
namespace {

class GzipMemberWriterTest : public testing::Test {
 protected:
  // Collects compressed output, as the file sequence would write it out.
  GzipMemberWriter::SinkCallback Sink() {
    return base::BindRepeating(
        [](std::string* out, base::span<const uint8_t> data) {
          out->append(reinterpret_cast<const char*>(data.data()), data.size());
          return true;
        },
        &output_);
  }

  std::string output_;
};

TEST_F(GzipMemberWriterTest, ChunkedWritesProduceOneMember) {
  {
    GzipMemberWriter writer(Sink());
    // Split across calls, as a record streamed off disk arrives.
    for (std::string_view piece :
         {"WARC/1.1\r\n", "Content-Length: 5\r\n\r\n", "hello", "\r\n\r\n"}) {
      ASSERT_TRUE(writer.Write(base::as_byte_span(piece)));
    }
    ASSERT_TRUE(writer.Finish());
  }

  // However the writes were split, the result must be a single member holding
  // exactly their concatenation.
  const std::optional<std::vector<std::string>> members =
      InflateGzipMembers(output_);
  ASSERT_TRUE(members.has_value());
  ASSERT_EQ(1u, members->size());
  EXPECT_EQ("WARC/1.1\r\nContent-Length: 5\r\n\r\nhello\r\n\r\n",
            (*members)[0]);
}

TEST_F(GzipMemberWriterTest, SurvivesOutputLargerThanOneChunk) {
  // Incompressible enough that deflate has to fill the output buffer more than
  // once, which is the loop the streaming path depends on.
  std::string big;
  for (int i = 0; i < 120000; ++i) {
    big += base::NumberToString(i * 2654435761u % 1000000);
  }
  ASSERT_GT(big.size(), 256u * 1024);

  {
    GzipMemberWriter writer(Sink());
    ASSERT_TRUE(writer.Write(base::as_byte_span(big)));
    ASSERT_TRUE(writer.Finish());
  }

  const std::optional<std::vector<std::string>> members =
      InflateGzipMembers(output_);
  ASSERT_TRUE(members.has_value());
  ASSERT_EQ(1u, members->size());
  EXPECT_EQ(big, (*members)[0]);
}

TEST_F(GzipMemberWriterTest, EmptyMemberIsStillValid) {
  {
    GzipMemberWriter writer(Sink());
    ASSERT_TRUE(writer.Finish());
  }

  const std::optional<std::vector<std::string>> members =
      InflateGzipMembers(output_);
  ASSERT_TRUE(members.has_value());
  ASSERT_EQ(1u, members->size());
  EXPECT_EQ("", (*members)[0]);
}

TEST_F(GzipMemberWriterTest, BinaryContentRoundTrips) {
  std::string binary;
  for (int i = 0; i < 256; ++i) {
    binary.push_back(static_cast<char>(i));
  }

  {
    GzipMemberWriter writer(Sink());
    ASSERT_TRUE(writer.Write(base::as_byte_span(binary)));
    ASSERT_TRUE(writer.Finish());
  }

  const std::optional<std::vector<std::string>> members =
      InflateGzipMembers(output_);
  ASSERT_TRUE(members.has_value());
  EXPECT_EQ(binary, (*members)[0]);
}

TEST_F(GzipMemberWriterTest, SinkFailureStopsTheMember) {
  int calls = 0;
  GzipMemberWriter writer(
      base::BindLambdaForTesting([&calls](base::span<const uint8_t>) {
        ++calls;
        return false;
      }));

  // Enough data that deflate produces output, so the sink is actually reached.
  const std::string data(512 * 1024, 'z');
  EXPECT_FALSE(writer.Write(base::as_byte_span(data)));
  EXPECT_FALSE(writer.Finish());
  // Once the sink refuses, nothing further is offered to it.
  EXPECT_EQ(1, calls);
}

}  // namespace
}  // namespace warc
