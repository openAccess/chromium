// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/warc_body_spill.h"

#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/logging.h"

namespace warc {

// Owns the spill file. Lives on, and is destroyed on, the file sequence.
class WarcBodySpill::Writer {
 public:
  explicit Writer(base::File file) : file_(std::move(file)) {}

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  ~Writer() = default;

  void Append(std::vector<uint8_t> chunk) {
    if (!ok_ || chunk.empty()) {
      return;
    }
    base::span<const uint8_t> remaining(chunk);
    while (!remaining.empty()) {
      std::optional<size_t> written = file_.WriteAtCurrentPos(remaining);
      if (!written.has_value() || *written == 0) {
        LOG(ERROR) << "Failed spilling WARC body to disk.";
        ok_ = false;
        return;
      }
      remaining = remaining.subspan(*written);
    }
  }

  base::File TakeFile() {
    if (!ok_) {
      return base::File();
    }
    return std::move(file_);
  }

 private:
  base::File file_;
  bool ok_ = true;
};

WarcBodySpill::WarcBodySpill(
    base::File file,
    scoped_refptr<base::SequencedTaskRunner> task_runner)
    : task_runner_(std::move(task_runner)),
      writer_(new Writer(std::move(file)),
              base::OnTaskRunnerDeleter(task_runner_)) {}

WarcBodySpill::~WarcBodySpill() = default;

void WarcBodySpill::Append(std::vector<uint8_t> chunk) {
  size_ += chunk.size();
  task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&Writer::Append, base::Unretained(writer_.get()),
                     std::move(chunk)));
}

base::File WarcBodySpill::TakeFile() {
  CHECK(task_runner_->RunsTasksInCurrentSequence());
  return writer_->TakeFile();
}

}  // namespace warc
