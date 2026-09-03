// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/warc_writer.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "components/warc/warc_test_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace warc {
namespace {

std::vector<uint8_t> ToBytes(std::string_view s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

class WarcWriterTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }

  base::FilePath ArchivePath() {
    return temp_dir_.GetPath().AppendASCII("test.warc");
  }

  base::File OpenArchive() {
    return base::File(ArchivePath(),
                      base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_WRITE);
  }

  // Blocks until everything queued so far has reached disk, then returns the
  // file's contents.
  std::string ReadArchiveAfterFlush(WarcWriter& writer) {
    base::test::TestFuture<void> flushed;
    writer.FlushForTesting(flushed.GetCallback());
    EXPECT_TRUE(flushed.Wait());

    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(ArchivePath(), &contents));
    return contents;
  }

  // A spill holding `contents`, as a completion would have filled it.
  std::unique_ptr<WarcBodySpill> MakeSpill(WarcWriter& writer,
                                           std::string_view contents) {
    const base::FilePath path = temp_dir_.GetPath().AppendASCII("spill.bin");
    base::File file(path, base::File::FLAG_CREATE_ALWAYS |
                              base::File::FLAG_READ | base::File::FLAG_WRITE);
    EXPECT_TRUE(file.IsValid());
    std::unique_ptr<WarcBodySpill> spill =
        writer.CreateBodySpill(std::move(file));
    spill->Append(ToBytes(contents));
    return spill;
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir temp_dir_;
};

TEST_F(WarcWriterTest, WritesRecordsVerbatimInOrder) {
  WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/1024,
                    WarcWriter::Compression::kNone);

  EXPECT_TRUE(writer.AddRecord(ToBytes("first")));
  EXPECT_TRUE(writer.AddRecord(ToBytes("second")));
  EXPECT_TRUE(writer.AddRecord(ToBytes("third")));

  EXPECT_EQ("firstsecondthird", ReadArchiveAfterFlush(writer));
}

TEST_F(WarcWriterTest, PreservesArbitraryBinaryContent) {
  WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/1024,
                    WarcWriter::Compression::kNone);

  // Embedded NULs and high bytes, as a gzip-compressed body would contain.
  const std::vector<uint8_t> record = {0x1f, 0x8b, 0x00, 0xff, 0x00, 0x7f};
  EXPECT_TRUE(writer.AddRecord(record));

  const std::string contents = ReadArchiveAfterFlush(writer);
  EXPECT_EQ(record, ToBytes(contents));
}

TEST_F(WarcWriterTest, DropsWholeRecordsWhenOverBudget) {
  // Budget fits the first record but not a second.
  WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/8,
                    WarcWriter::Compression::kNone);

  EXPECT_TRUE(writer.AddRecord(ToBytes("12345678")));
  EXPECT_FALSE(writer.AddRecord(ToBytes("would not fit")));

  EXPECT_EQ(1u, writer.dropped_records());

  // The dropped record must not appear even partially; a truncated record would
  // desynchronize every reader for the rest of the file.
  EXPECT_EQ("12345678", ReadArchiveAfterFlush(writer));
}

TEST_F(WarcWriterTest, RecordLargerThanTheWholeBudgetIsStillWritten) {
  WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/8,
                    WarcWriter::Compression::kNone);

  // The budget bounds a backlog on a slow disk. Enforcing it against a single
  // record would drop exactly the largest resources however idle the queue is
  // -- a completed video is bigger than any sensible backlog allowance -- and
  // drop them silently, which is the one thing an archive must never do.
  const std::string big(64, 'x');
  EXPECT_TRUE(writer.AddRecord(ToBytes(big)));
  EXPECT_EQ(0u, writer.dropped_records());
  EXPECT_EQ(big, ReadArchiveAfterFlush(writer));
}

TEST_F(WarcWriterTest, BudgetRecoversAfterDrain) {
  WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/8,
                    WarcWriter::Compression::kNone);

  EXPECT_TRUE(writer.AddRecord(ToBytes("12345678")));
  EXPECT_EQ(8u, writer.queued_bytes());

  // Once the queue drains, the budget is available again.
  EXPECT_EQ("12345678", ReadArchiveAfterFlush(writer));
  EXPECT_EQ(0u, writer.queued_bytes());

  EXPECT_TRUE(writer.AddRecord(ToBytes("abcdefgh")));
  EXPECT_EQ("12345678abcdefgh", ReadArchiveAfterFlush(writer));
  EXPECT_EQ(0u, writer.dropped_records());
}

TEST_F(WarcWriterTest, EmptyRecordIsANoOp) {
  WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/1024,
                    WarcWriter::Compression::kNone);

  EXPECT_TRUE(writer.AddRecord({}));

  EXPECT_EQ("", ReadArchiveAfterFlush(writer));
  EXPECT_EQ(0u, writer.dropped_records());
}

TEST_F(WarcWriterTest, DestructionFlushesQueuedRecords) {
  {
    WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/1024,
                      WarcWriter::Compression::kNone);
    EXPECT_TRUE(writer.AddRecord(ToBytes("queued-at-destruction")));
    // Deliberately no flush before going out of scope.
  }
  task_environment_.RunUntilIdle();

  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(ArchivePath(), &contents));
  EXPECT_EQ("queued-at-destruction", contents);
}

TEST_F(WarcWriterTest, InvalidFileDoesNotCrash) {
  // An unopened file stands in for the browser failing to open the path.
  WarcWriter writer(base::File(), /*max_queued_bytes=*/1024,
                    WarcWriter::Compression::kNone);

  EXPECT_TRUE(writer.AddRecord(ToBytes("dropped on the floor")));
  task_environment_.RunUntilIdle();
}

TEST_F(WarcWriterTest, ManyRecordsCoalesceIntoFewFlushes) {
  WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/1024 * 1024,
                    WarcWriter::Compression::kNone);

  std::string expected;
  for (int i = 0; i < 200; ++i) {
    const std::string record = "record" + base::NumberToString(i);
    EXPECT_TRUE(writer.AddRecord(ToBytes(record)));
    expected += record;
  }

  EXPECT_EQ(expected, ReadArchiveAfterFlush(writer));
  EXPECT_EQ(0u, writer.dropped_records());
}

TEST_F(WarcWriterTest, GzipWritesOneMemberPerRecord) {
  WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/1024,
                    WarcWriter::Compression::kGzipPerRecord);

  EXPECT_TRUE(writer.AddRecord(ToBytes("first")));
  EXPECT_TRUE(writer.AddRecord(ToBytes("second")));
  EXPECT_TRUE(writer.AddRecord(ToBytes("third")));

  const std::string contents = ReadArchiveAfterFlush(writer);
  ASSERT_GE(contents.size(), 2u);
  EXPECT_EQ('\x1f', contents[0]);
  EXPECT_EQ('\x8b', contents[1]);

  // Member boundaries must line up with record boundaries, so that a reader
  // holding a record's offset can decompress that record and nothing else.
  const std::optional<std::vector<std::string>> members =
      InflateGzipMembers(contents);
  ASSERT_TRUE(members.has_value());
  EXPECT_EQ(std::vector<std::string>({"first", "second", "third"}), *members);
}

TEST_F(WarcWriterTest, GzipPreservesArbitraryBinaryContent) {
  WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/1024,
                    WarcWriter::Compression::kGzipPerRecord);

  // Embedded NULs and high bytes, as a gzip-compressed body would contain —
  // a record that is itself already compressed still has to round-trip.
  const std::vector<uint8_t> record = {0x1f, 0x8b, 0x00, 0xff, 0x00, 0x7f};
  EXPECT_TRUE(writer.AddRecord(record));

  const std::optional<std::vector<std::string>> members =
      InflateGzipMembers(ReadArchiveAfterFlush(writer));
  ASSERT_TRUE(members.has_value());
  ASSERT_EQ(1u, members->size());
  EXPECT_EQ(record, ToBytes((*members)[0]));
}

TEST_F(WarcWriterTest, GzipWritesNothingWhenNoRecordsQueued) {
  WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/1024,
                    WarcWriter::Compression::kGzipPerRecord);

  // An empty record must not produce an empty member; a reader would see a
  // record boundary where no record exists.
  EXPECT_TRUE(writer.AddRecord({}));

  EXPECT_EQ("", ReadArchiveAfterFlush(writer));
}

TEST_F(WarcWriterTest, GzipBudgetCountsUncompressedBytes) {
  WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/64,
                    WarcWriter::Compression::kGzipPerRecord);

  // Deflated this is a few dozen bytes, but the queue must hold it undeflated:
  // the budget bounds memory on the producing sequence, not bytes on disk.
  EXPECT_TRUE(writer.AddRecord(ToBytes(std::string(4096, 'a'))));
  EXPECT_EQ(4096u, writer.queued_bytes());

  // Which leaves the backlog over budget, so the next record sheds.
  EXPECT_FALSE(writer.AddRecord(ToBytes("next")));
  EXPECT_EQ(1u, writer.dropped_records());
}

TEST_F(WarcWriterTest, GzipDestructionFlushesQueuedRecords) {
  {
    WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/1024,
                      WarcWriter::Compression::kGzipPerRecord);
    EXPECT_TRUE(writer.AddRecord(ToBytes("queued-at-destruction")));
    // Deliberately no flush before going out of scope: an archive abandoned
    // mid-capture still has to end on a complete member.
  }
  task_environment_.RunUntilIdle();

  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(ArchivePath(), &contents));

  const std::optional<std::vector<std::string>> members =
      InflateGzipMembers(contents);
  ASSERT_TRUE(members.has_value());
  EXPECT_EQ(std::vector<std::string>({"queued-at-destruction"}), *members);
}

TEST_F(WarcWriterTest, SpilledBodyIsStreamedIntoTheArchive) {
  WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/1024,
                    WarcWriter::Compression::kNone);

  const std::string body(300 * 1024, 'q');
  base::test::TestFuture<base::File> returned;
  EXPECT_TRUE(
      writer.AddRecordWithSpilledBody(ToBytes("HEAD:"), MakeSpill(writer, body),
                                      body.size(), returned.GetCallback()));

  // Head, then the spilled bytes, then the separator the head stops short of.
  EXPECT_EQ("HEAD:" + body + "\r\n\r\n", ReadArchiveAfterFlush(writer));
  EXPECT_TRUE(returned.Get().IsValid());
}

TEST_F(WarcWriterTest, SpilledBodyIsGzippedIntoOneMember) {
  WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/1024,
                    WarcWriter::Compression::kGzipPerRecord);

  // Bigger than the streaming chunk, so the body really is deflated in pieces.
  const std::string body(300 * 1024, 'w');
  base::test::TestFuture<base::File> returned;
  EXPECT_TRUE(
      writer.AddRecordWithSpilledBody(ToBytes("HEAD:"), MakeSpill(writer, body),
                                      body.size(), returned.GetCallback()));

  // A record split across many deflate calls still has to land in exactly one
  // member, or its byte offset means nothing to a reader.
  const std::optional<std::vector<std::string>> members =
      InflateGzipMembers(ReadArchiveAfterFlush(writer));
  ASSERT_TRUE(members.has_value());
  ASSERT_EQ(1u, members->size());
  EXPECT_EQ("HEAD:" + body + "\r\n\r\n", (*members)[0]);
}

TEST_F(WarcWriterTest, SpilledBodyDoesNotCountAgainstTheQueueBudget) {
  // A budget far smaller than the body: the point of spilling is that the
  // body never occupies the producing sequence's memory at all, so it cannot
  // be what pushes the queue over.
  WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/64,
                    WarcWriter::Compression::kNone);

  const std::string body(512 * 1024, 'e');
  base::test::TestFuture<base::File> returned;
  EXPECT_TRUE(
      writer.AddRecordWithSpilledBody(ToBytes("HEAD:"), MakeSpill(writer, body),
                                      body.size(), returned.GetCallback()));
  EXPECT_EQ(5u, writer.queued_bytes());
  EXPECT_EQ(0u, writer.dropped_records());

  EXPECT_EQ("HEAD:" + body + "\r\n\r\n", ReadArchiveAfterFlush(writer));
}

TEST_F(WarcWriterTest, SpillFileComesBackWhenTheRecordIsDropped) {
  WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/4,
                    WarcWriter::Compression::kNone);

  // Fill the budget so the spilled record is shed.
  EXPECT_TRUE(writer.AddRecord(ToBytes("12345678")));

  base::test::TestFuture<base::File> returned;
  EXPECT_FALSE(writer.AddRecordWithSpilledBody(ToBytes("HEAD:"),
                                               MakeSpill(writer, "dropped"), 7,
                                               returned.GetCallback()));

  // The producer waits on this file before reusing it; never returning it
  // would stall every later completion.
  EXPECT_TRUE(returned.Get().IsValid());
  EXPECT_EQ(1u, writer.dropped_records());
}

TEST_F(WarcWriterTest, SpilledRecordIsWrittenWholeAmongOthers) {
  WarcWriter writer(OpenArchive(), /*max_queued_bytes=*/1024,
                    WarcWriter::Compression::kNone);

  const std::string body(100 * 1024, 'p');
  base::test::TestFuture<base::File> returned;
  EXPECT_TRUE(writer.AddRecord(ToBytes("before")));
  EXPECT_TRUE(
      writer.AddRecordWithSpilledBody(ToBytes("HEAD:"), MakeSpill(writer, body),
                                      body.size(), returned.GetCallback()));
  EXPECT_TRUE(writer.AddRecord(ToBytes("after")));

  EXPECT_EQ("before" + std::string("HEAD:") + body + "\r\n\r\n" + "after",
            ReadArchiveAfterFlush(writer));
}

}  // namespace
}  // namespace warc
