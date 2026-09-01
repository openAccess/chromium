// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/warc_writer.h"

#include <optional>
#include <string>
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
#include "third_party/zlib/google/compression_utils.h"

namespace warc {

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
  bool Add(std::vector<uint8_t> record, bool* should_post_flush) {
    base::AutoLock auto_lock(lock_);

    // Records are all-or-nothing: a partially written record would
    // desynchronize every reader for the rest of the file.
    if (queued_bytes_ + record.size() > max_queued_bytes_) {
      ++dropped_records_;
      *should_post_flush = false;
      return false;
    }

    queued_bytes_ += record.size();
    records_.push_back(std::move(record));

    *should_post_flush = !flush_pending_;
    flush_pending_ = true;
    return true;
  }

  base::circular_deque<std::vector<uint8_t>> TakeAll() {
    base::AutoLock auto_lock(lock_);
    base::circular_deque<std::vector<uint8_t>> taken;
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
  base::circular_deque<std::vector<uint8_t>> records_ GUARDED_BY(lock_);
  size_t queued_bytes_ GUARDED_BY(lock_) = 0;
  uint64_t dropped_records_ GUARDED_BY(lock_) = 0;
  bool flush_pending_ GUARDED_BY(lock_) = false;

  const size_t max_queued_bytes_;
};

// Owns the output file. Lives on, and is destroyed on, the blocking-capable
// file sequence.
class WarcWriter::FileWriter {
 public:
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
    base::circular_deque<std::vector<uint8_t>> records = queue->TakeAll();
    if (!file_.IsValid()) {
      return;
    }

    for (const std::vector<uint8_t>& record : records) {
      if (compression_ == Compression::kNone) {
        if (!WriteAll(record)) {
          return;
        }
        continue;
      }

      // One gzip member per record, so a reader can decompress any single
      // record without reading the ones before it.
      std::string member;
      if (!compression::GzipCompress(record, &member)) {
        // Emitting the record uncompressed instead would leave raw bytes in
        // the middle of a member stream, which no gzip reader could get past.
        LOG(ERROR) << "Failed compressing WARC record; closing archive.";
        file_.Close();
        return;
      }
      if (!WriteAll(base::as_byte_span(member))) {
        return;
      }
    }
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

  bool should_post_flush = false;
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
