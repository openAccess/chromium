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
#include "components/warc/warc_body_spill.h"

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

  // Closes the current archive and continues into `file`, which must already be
  // open for writing -- or be invalid, which stops recording and drops every
  // record queued after this point.
  //
  // The boundary is exact: every record queued before this call goes to the old
  // file and every record queued after it to the new one. That is why a
  // rotation travels through the same queue as the records rather than being
  // posted alongside it -- flush tasks are coalesced, so a flush posted before
  // the rotation would otherwise drain records queued after it into the file
  // they do not belong to. An index that names a file and an offset is only
  // meaningful if that boundary holds.
  //
  // Unlike a record, a rotation is never dropped for being over budget:
  // discarding one would send every later record to the wrong file.
  //
  // A rotation also reopens an archive that a write failure had closed, since
  // the failure applies to the file it happened in and not to its successor.
  void Rotate(base::File file);

  // Returns a spill for a block too large to hold in memory, writing into
  // `file` on this writer's own file sequence. That shared sequence is what
  // orders the spilled bytes ahead of the record that streams them, so no
  // completion signal is needed between the two -- and none could be relied
  // on, since recording can stop while writes are still queued.
  std::unique_ptr<WarcBodySpill> CreateBodySpill(base::File file);

  // Queues a record whose block is partly on disk: `head` is the WARC header
  // and everything preceding the spilled bytes, and the first `body_size`
  // bytes of `spill` complete the block. The record separator is appended
  // here, so `head` must not already carry one.
  //
  // Only the head counts against the queue's byte budget, since the body never
  // enters memory. The spill's file is handed to `on_written` once the record
  // has been written -- or dropped -- so the caller can reuse it; reusing it
  // any earlier would corrupt a record still being streamed out of it.
  bool AddRecordWithSpilledBody(
      std::vector<uint8_t> head,
      std::unique_ptr<WarcBodySpill> spill,
      uint64_t body_size,
      base::OnceCallback<void(base::File)> on_written);

  // Number of records dropped so far because the queue was over budget.
  uint64_t dropped_records() const;

  // Number of bytes currently queued and not yet written.
  size_t queued_bytes() const;

  // Runs `callback` on the caller's sequence once every record queued before
  // this call has reached the file and the file has been flushed to disk.
  //
  // Queued behind a Rotate(), this is what makes "the file just closed is
  // complete" observable: the rotation drains first, so by the time `callback`
  // runs the outgoing file has been written, flushed and closed, and whatever
  // indexes or packages that segment may begin reading it.
  void Flush(base::OnceClosure callback);

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
