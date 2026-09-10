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
  std::unique_ptr<WarcBodySpill> spill;
  // Pulled out of `spill` on the file sequence, once every append has run.
  base::File spill_file;
  uint64_t body_size = 0;

  // A spilled record is not writable until the appends filling it have run.
  // Records are written in order, so an unready one holds up those behind it
  // rather than being skipped.
  bool ready = true;
  uint64_t id = 0;
  base::OnceCallback<void(base::File)> on_written;

  // Set only for a rotation: the file that replaces the archive when this entry
  // is drained, or an invalid file to stop writing altogether. A rotation
  // carries no head and no body, and so costs the queue's budget nothing.
  std::optional<base::File> rotate_to;

  bool spilled() const { return spill != nullptr; }
  bool is_rotation() const { return rotate_to.has_value(); }
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
  //
  // A dropped record is handed back rather than released here: it may own a
  // spill, which can only be reclaimed on the file sequence.
  std::optional<QueuedRecord> Add(QueuedRecord record,
                                  bool* should_post_flush) {
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
      return record;
    }

    // Only the head is accounted: a spilled body never enters memory, so it
    // costs the producing sequence nothing to hold.
    queued_bytes_ += record.head.size();
    records_.push_back(std::move(record));

    *should_post_flush = !flush_pending_;
    flush_pending_ = true;
    return std::nullopt;
  }

  // Adds a rotation, which takes its place in line among the records so that
  // the file it installs receives exactly those queued after it.
  //
  // A rotation is never shed for being over budget, as a record would be:
  // every entry behind it is destined for the file it installs, so dropping
  // one would divert all of them into the archive they do not belong to. It
  // costs no budget either, having no head to hold in memory.
  void AddRotation(QueuedRecord rotation, bool* should_post_flush) {
    base::AutoLock auto_lock(lock_);
    records_.push_back(std::move(rotation));
    *should_post_flush = !flush_pending_;
    flush_pending_ = true;
  }

  // Pops the oldest record if it is ready to write. Returns nothing when the
  // queue is empty or its head is a spilled record whose bytes have not all
  // been written yet; writing past it would reorder the archive.
  std::optional<QueuedRecord> TakeFrontIfReady() {
    base::AutoLock auto_lock(lock_);
    if (records_.empty() || !records_.front().ready) {
      flush_pending_ = false;
      return std::nullopt;
    }
    QueuedRecord record = std::move(records_.front());
    records_.pop_front();
    queued_bytes_ -= record.head.size();
    return record;
  }

  void MarkReady(uint64_t id) {
    base::AutoLock auto_lock(lock_);
    for (QueuedRecord& record : records_) {
      if (record.id == id) {
        record.ready = true;
        return;
      }
    }
  }

  uint64_t NextId() {
    base::AutoLock auto_lock(lock_);
    return ++last_id_;
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
  uint64_t last_id_ GUARDED_BY(lock_) = 0;

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
    while (std::optional<QueuedRecord> taken = queue->TakeFrontIfReady()) {
      QueuedRecord& record = *taken;
      if (record.is_rotation()) {
        Rotate(std::move(*record.rotate_to));
        continue;
      }
      const bool wrote = file_.IsValid() && WriteRecord(record);
      // The file comes back whether or not the record was written, so the
      // producer is never left waiting on one the archive has given up on.
      if (record.on_written) {
        std::move(record.on_written)
            .Run(
                record.spill_file.IsValid()
                    ? std::move(record.spill_file)
                    : (record.spill ? record.spill->TakeFile() : base::File()));
      }
      // A closed archive still drains the queue, so every spill comes back.
      std::ignore = wrote;
    }
  }

  // Installs `file` as the archive and closes the one it replaces.
  //
  // The outgoing file is flushed rather than merely closed: a rotated-off
  // segment is finished, and whatever reads it next -- an index, a container,
  // another process entirely -- is entitled to find it complete on disk the
  // moment the boundary passes.
  //
  // An invalid `file` stops recording, and every record drained afterwards is
  // discarded by the IsValid() check in Flush(). A valid one revives an archive
  // that a write failure had closed, since that failure describes the file it
  // happened in, not its successor.
  void Rotate(base::File file) {
    if (file_.IsValid()) {
      file_.Flush();
    }
    file_ = std::move(file);
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
    // Every append queued before this record ran on this same sequence, so the
    // file is complete by now.
    if (!record.spill_file.IsValid()) {
      record.spill_file = record.spill->TakeFile();
    }
    if (!record.spill_file.IsValid()) {
      LOG(ERROR) << "WARC body spill failed; closing archive.";
      file_.Close();
      return false;
    }
    std::vector<uint8_t> buffer(kBodyChunkSize);
    uint64_t remaining = record.body_size;
    int64_t offset = 0;
    while (remaining > 0) {
      const size_t wanted =
          static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
      std::optional<size_t> read =
          record.spill_file.Read(offset, base::span(buffer).first(wanted));
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

void WarcWriter::Rotate(base::File file) {
  QueuedRecord rotation;
  rotation.rotate_to = std::move(file);

  bool should_post_flush = false;
  queue_->AddRotation(std::move(rotation), &should_post_flush);
  if (should_post_flush) {
    file_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&FileWriter::Flush, base::Unretained(file_writer_.get()),
                       queue_));
  }
}

std::unique_ptr<WarcBodySpill> WarcWriter::CreateBodySpill(base::File file) {
  return std::make_unique<WarcBodySpill>(std::move(file), file_task_runner_);
}

bool WarcWriter::AddRecordWithSpilledBody(
    std::vector<uint8_t> head,
    std::unique_ptr<WarcBodySpill> spill,
    uint64_t body_size,
    base::OnceCallback<void(base::File)> on_written) {
  QueuedRecord queued_record;
  queued_record.head = std::move(head);
  queued_record.spill = std::move(spill);
  queued_record.body_size = body_size;
  queued_record.on_written = std::move(on_written);
  // The bytes are still on their way to the spill file. This task is posted
  // behind every one of those writes, so by the time it runs the record really
  // is complete -- and until then it holds its place in the archive's order.
  queued_record.ready = false;
  queued_record.id = queue_->NextId();
  const uint64_t id = queued_record.id;

  if (!Enqueue(std::move(queued_record))) {
    return false;
  }
  file_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](scoped_refptr<WriteQueue> queue, FileWriter* writer, uint64_t id) {
            queue->MarkReady(id);
            writer->Flush(queue);
          },
          queue_, base::Unretained(file_writer_.get()), id));
  return true;
}

bool WarcWriter::Enqueue(QueuedRecord record) {
  bool should_post_flush = false;
  std::optional<QueuedRecord> dropped =
      queue_->Add(std::move(record), &should_post_flush);

  if (dropped) {
    // Reclaiming the spill means touching the file sequence, and a producer
    // waiting on that file must not be stranded just because the record was
    // shed.
    file_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](QueuedRecord shed) {
              if (shed.on_written) {
                std::move(shed.on_written)
                    .Run(shed.spill ? shed.spill->TakeFile() : base::File());
              }
            },
            std::move(*dropped)));
    return false;
  }

  if (should_post_flush) {
    file_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&FileWriter::Flush, base::Unretained(file_writer_.get()),
                       queue_));
  }
  return true;
}

uint64_t WarcWriter::dropped_records() const {
  return queue_->dropped_records();
}

size_t WarcWriter::queued_bytes() const {
  return queue_->queued_bytes();
}

void WarcWriter::Flush(base::OnceClosure callback) {
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
