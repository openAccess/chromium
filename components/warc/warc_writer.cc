// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/warc_writer.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/containers/circular_deque.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/synchronization/lock.h"
#include "base/task/bind_post_task.h"
#include "base/task/thread_pool.h"
#include "base/thread_annotations.h"
#include "components/warc/gzip_member_writer.h"

namespace warc {

namespace {

// Records are separated by two CRLFs, which a spilled record's head stops
// short of.
constexpr std::string_view kRecordSeparator = "\r\n\r\n";

// How much of a spilled body is held in memory at once on the way to the
// archive.
constexpr size_t kBodyChunkSize = 64 * 1024;

}  // namespace

// One queued record: either complete in memory, or a head plus a body that
// stays on disk until it is written straight through to the archive.
struct WarcWriter::QueuedRecord {
  QueuedRecord() = default;
  QueuedRecord(const QueuedRecord&) = delete;
  QueuedRecord& operator=(const QueuedRecord&) = delete;
  QueuedRecord(QueuedRecord&&) = default;
  QueuedRecord& operator=(QueuedRecord&&) = default;
  ~QueuedRecord() = default;

  // The whole record when `body` is absent, otherwise everything before the
  // spilled bytes. The record separator is part of this only in the first case.
  std::vector<uint8_t> head;

  // Set only for a spilled record.
  base::File body;
  uint64_t body_size = 0;
  base::OnceCallback<void(base::File)> on_written;

  bool spilled() const { return body.IsValid(); }
};

// Holds records handed over by the producing sequence until the file sequence
// drains them. Every method is safe to call from any sequence.
class WarcWriter::WriteQueue : public base::RefCountedThreadSafe<WriteQueue> {
 public:
  explicit WriteQueue(size_t max_queued_bytes)
      : max_queued_bytes_(max_queued_bytes) {}

  WriteQueue(const WriteQueue&) = delete;
  WriteQueue& operator=(const WriteQueue&) = delete;

  // Adds `record` unless doing so would exceed the byte budget. Sets
  // `should_post_flush` when the caller needs to post a drain task, which
  // happens only on the transition out of "a flush is already scheduled" so
  // that a burst of records produces one task rather than one task per record.
  bool Add(QueuedRecord record, bool* should_post_flush) {
    base::AutoLock auto_lock(lock_);

    // Records are all-or-nothing: a partially written record would
    // desynchronize every reader for the rest of the file.
    //
    // The budget bounds the backlog, and is deliberately not tested against
    // the incoming record's own size. Doing that would drop a record larger
    // than the whole allowance no matter how idle the disk was, and the
    // records that exceed it are the large resources most worth keeping. So
    // the queue accepts whatever arrives while it is under budget and sheds
    // only once it is not, overshooting by at most one record.
    if (queued_bytes_ >= max_queued_bytes_) {
      ++dropped_records_;
      *should_post_flush = false;
      // Return the spill file, since nothing will ever stream from it.
      if (record.on_written) {
        std::move(record.on_written).Run(std::move(record.body));
      }
      return false;
    }

    // Only the head is accounted: a spilled body never enters memory, so it
    // costs the producing sequence nothing to hold.
    queued_bytes_ += record.head.size();
    records_.push_back(std::move(record));

    *should_post_flush = !flush_pending_;
    flush_pending_ = true;
    return true;
  }

  base::circular_deque<QueuedRecord> TakeAll() {
    base::AutoLock auto_lock(lock_);
    base::circular_deque<QueuedRecord> taken;
    taken.swap(records_);
    queued_bytes_ = 0;
    flush_pending_ = false;
    return taken;
  }

  uint64_t dropped_records() const {
    base::AutoLock auto_lock(lock_);
    return dropped_records_;
  }

  size_t queued_bytes() const {
    base::AutoLock auto_lock(lock_);
    return queued_bytes_;
  }

 private:
  friend class base::RefCountedThreadSafe<WriteQueue>;
  ~WriteQueue() = default;

  mutable base::Lock lock_;
  base::circular_deque<QueuedRecord> records_ GUARDED_BY(lock_);
  size_t queued_bytes_ GUARDED_BY(lock_) = 0;
  uint64_t dropped_records_ GUARDED_BY(lock_) = 0;
  bool flush_pending_ GUARDED_BY(lock_) = false;

  const size_t max_queued_bytes_;
};

// Owns the output file. Lives on, and is destroyed on, the blocking-capable
// file sequence.
class WarcWriter::FileWriter {
 public:
  using Sink = GzipMemberWriter::SinkCallback;

  FileWriter(base::File file, Compression compression)
      : file_(std::move(file)), compression_(compression) {}

  FileWriter(const FileWriter&) = delete;
  FileWriter& operator=(const FileWriter&) = delete;

  ~FileWriter() {
    if (file_.IsValid()) {
      file_.Flush();
    }
  }

  void Flush(scoped_refptr<WriteQueue> queue) {
    base::circular_deque<QueuedRecord> records = queue->TakeAll();
    for (QueuedRecord& record : records) {
      const bool wrote = file_.IsValid() && WriteRecord(record);
      // The body is handed back whether or not it was written, so the producer
      // is never left waiting on a record the archive has already given up on.
      if (record.on_written) {
        std::move(record.on_written).Run(std::move(record.body));
      }
      if (!wrote && !file_.IsValid()) {
        // The archive is closed; drain the rest only to return their bodies.
        continue;
      }
    }
  }

  // Writes one record, compressing it into a member of its own where the
  // archive is gzipped. Returns false once the archive has been closed.
  bool WriteRecord(QueuedRecord& record) {
    std::optional<GzipMemberWriter> member;
    Sink sink =
        base::BindRepeating(&FileWriter::WriteAll, base::Unretained(this));
    if (compression_ == Compression::kGzipPerRecord) {
      member.emplace(sink);
      sink = base::BindRepeating(
          [](GzipMemberWriter* writer, base::span<const uint8_t> data) {
            return writer->Write(data);
          },
          &member.value());
    }

    if (!sink.Run(record.head)) {
      return false;
    }
    if (record.spilled()) {
      if (!StreamSpilledBody(record, sink)) {
        return false;
      }
      // A spilled head stops at the block, so the separator is added here.
      if (!sink.Run(base::as_byte_span(kRecordSeparator))) {
        return false;
      }
    }
    if (member && !member->Finish()) {
      // Half a member is unreadable, and so is everything after it.
      LOG(ERROR) << "Failed compressing WARC record; closing archive.";
      file_.Close();
      return false;
    }
    return true;
  }

  // Copies `record.body_size` bytes out of the spill file and into `sink`,
  // never holding more than one chunk of it.
  bool StreamSpilledBody(QueuedRecord& record, const Sink& sink) {
    std::vector<uint8_t> buffer(kBodyChunkSize);
    uint64_t remaining = record.body_size;
    int64_t offset = 0;
    while (remaining > 0) {
      const size_t wanted =
          static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
      std::optional<size_t> read =
          record.body.Read(offset, base::span(buffer).first(wanted));
      if (!read.has_value() || *read == 0) {
        // The header already promised Content-Length bytes, so a short spill
        // leaves the record -- and everything framed after it -- unreadable.
        LOG(ERROR) << "Spilled WARC body ended early; closing archive.";
        file_.Close();
        return false;
      }
      if (!sink.Run(base::span(buffer).first(*read))) {
        return false;
      }
      offset += static_cast<int64_t>(*read);
      remaining -= *read;
    }
    return true;
  }

  void FlushToDisk(base::OnceClosure callback) {
    if (file_.IsValid()) {
      file_.Flush();
    }
    std::move(callback).Run();
  }

 private:
  // Writes all of `data`. On failure closes the file and returns false, since
  // the archive is then truncated mid-record and nothing further can be framed
  // correctly.
  bool WriteAll(base::span<const uint8_t> data) {
    while (!data.empty()) {
      std::optional<size_t> written = file_.WriteAtCurrentPos(data);
      if (!written.has_value() || *written == 0) {
        // Short of closing the file there is nothing useful to do: the archive
        // is already truncated mid-record, so stop writing rather than emit
        // records that readers would mis-frame.
        LOG(ERROR) << "Failed writing WARC record; closing archive.";
        file_.Close();
        return false;
      }
      data = data.subspan(*written);
    }
    return true;
  }

  base::File file_;
  const Compression compression_;
};

WarcWriter::WarcWriter(base::File file,
                       size_t max_queued_bytes,
                       Compression compression)
    : queue_(base::MakeRefCounted<WriteQueue>(max_queued_bytes)),
      file_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})),
      file_writer_(new FileWriter(std::move(file), compression),
                   base::OnTaskRunnerDeleter(file_task_runner_)) {}

WarcWriter::~WarcWriter() {
  // Drain anything still queued. The FileWriter is deleted on the file sequence
  // after this task runs, so the file stays alive until the write completes.
  file_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&FileWriter::Flush,
                                base::Unretained(file_writer_.get()), queue_));
}

bool WarcWriter::AddRecord(std::vector<uint8_t> record) {
  if (record.empty()) {
    return true;
  }

  QueuedRecord queued_record;
  queued_record.head = std::move(record);
  return Enqueue(std::move(queued_record));
}

bool WarcWriter::AddRecordWithSpilledBody(
    std::vector<uint8_t> head,
    base::File body,
    uint64_t body_size,
    base::OnceCallback<void(base::File)> on_written) {
  QueuedRecord queued_record;
  queued_record.head = std::move(head);
  queued_record.body = std::move(body);
  queued_record.body_size = body_size;
  queued_record.on_written = std::move(on_written);
  return Enqueue(std::move(queued_record));
}

bool WarcWriter::Enqueue(QueuedRecord record) {
  bool should_post_flush = false;
  // A dropped record still carries its body and callback into Add(), which
  // hands them back, so a producer waiting on the spill file is never
  // stranded.
  const bool queued = queue_->Add(std::move(record), &should_post_flush);

  if (should_post_flush) {
    file_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&FileWriter::Flush, base::Unretained(file_writer_.get()),
                       queue_));
  }
  return queued;
}

uint64_t WarcWriter::dropped_records() const {
  return queue_->dropped_records();
}

size_t WarcWriter::queued_bytes() const {
  return queue_->queued_bytes();
}

void WarcWriter::FlushForTesting(base::OnceClosure callback) {
  file_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&FileWriter::Flush,
                                base::Unretained(file_writer_.get()), queue_));
  file_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&FileWriter::FlushToDisk,
                     base::Unretained(file_writer_.get()),
                     base::BindPostTaskToCurrentDefault(std::move(callback))));
}

}  // namespace warc
