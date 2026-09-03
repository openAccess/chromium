// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_WARC_WARC_WRITER_H_
#define COMPONENTS_WARC_WARC_WRITER_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <vector>

#include "base/files/file.h"
#include "base/memory/ref_counted.h"
#include "base/task/sequenced_task_runner.h"

namespace warc {

// Writes serialized WARC records to a file.
//
// Records are handed in from the sequence that produced them — in the network
// service that is the sequence running the entire net stack, where blocking on
// disk would stall every socket, DNS callback and cache transaction in the
// browser. So records are only copied into an in-memory queue there, and a
// separate blocking-capable sequence owns the file and drains the queue.
//
// The queue has a hard byte budget. Response bodies are unbounded in size and a
// slow disk must not translate into unbounded memory growth, so once the budget
// is exceeded whole records are dropped and counted rather than partially
// written — a truncated record would corrupt every record after it, since
// readers rely on Content-Length to find the next record boundary.
//
// This class may be constructed on any sequence. AddRecord() is safe to call
// from any sequence.
class WarcWriter {
 public:
  // Whether records are gzip-compressed on their way to disk.
  //
  // kGzipPerRecord compresses each record into a gzip member of its own and
  // concatenates the members. That layout is what a conventional ".warc.gz"
  // is: gzip permits members to be concatenated, and one member per record is
  // what lets a reader seek to a record's byte offset and decompress that
  // record alone. Compressing the archive as a single gzip stream would still
  // be valid gzip, but every read would have to start from the first record.
  //
  // Compression runs on the file sequence rather than the producing one, so a
  // large body costs the net stack no CPU. The queue's byte budget therefore
  // counts uncompressed bytes, which is what actually bounds memory here.
  enum class Compression {
    kNone,
    kGzipPerRecord,
  };

  // `file` must already be open for writing; the network service is sandboxed
  // and cannot open arbitrary paths itself, so the browser process opens the
  // file and passes the handle over mojo.
  WarcWriter(base::File file, size_t max_queued_bytes, Compression compression);

  WarcWriter(const WarcWriter&) = delete;
  WarcWriter& operator=(const WarcWriter&) = delete;

  ~WarcWriter();

  // Queues one fully serialized record. Returns false if the record was
  // dropped because the queue was over budget.
  bool AddRecord(std::vector<uint8_t> record);

  // Queues a record whose block is too large to hold in memory: `head` is the
  // WARC header and everything preceding the spilled bytes, and the first
  // `body_size` bytes of `body` complete the block. The record separator is
  // appended here, so `head` must not already carry one.
  //
  // Only the head counts against the queue's byte budget, since the body never
  // enters memory: it is streamed from `body` straight into the archive on the
  // file sequence. `body` is consumed, and returned through `on_written` once
  // the record has been written, so the caller can reuse or release it --
  // reusing it any earlier would corrupt a record still being streamed.
  bool AddRecordWithSpilledBody(
      std::vector<uint8_t> head,
      base::File body,
      uint64_t body_size,
      base::OnceCallback<void(base::File)> on_written);

  // Number of records dropped so far because the queue was over budget.
  uint64_t dropped_records() const;

  // Number of bytes currently queued and not yet written.
  size_t queued_bytes() const;

  // Runs `callback` on the caller's sequence once every record queued before
  // this call has reached the file. For tests and for flushing at shutdown.
  void FlushForTesting(base::OnceClosure callback);

 private:
  struct QueuedRecord;
  class WriteQueue;

  bool Enqueue(QueuedRecord record);

  class FileWriter;

  scoped_refptr<WriteQueue> queue_;
  scoped_refptr<base::SequencedTaskRunner> file_task_runner_;

  // Destroyed on `file_task_runner_` so that the file outlives any pending
  // write tasks.
  std::unique_ptr<FileWriter, base::OnTaskRunnerDeleter> file_writer_;
};

}  // namespace warc

#endif  // COMPONENTS_WARC_WARC_WRITER_H_
