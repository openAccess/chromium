// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_WARC_WARC_BODY_SPILL_H_
#define COMPONENTS_WARC_WARC_BODY_SPILL_H_

#include <stdint.h>

#include <memory>
#include <vector>

#include "base/files/file.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"

namespace warc {

// Accumulates a record's block in a file rather than in memory.
//
// A WARC record must state its block's length before the block, so a resource
// is held whole until it can be written. Holding a video that way is not
// viable, so its bytes go to a file as they arrive. The writes happen on a
// blocking-capable sequence -- never on the sequence producing the bytes,
// which in the network service is the one running the entire net stack.
//
// Constructed and used on the producing sequence; the file it is given is
// consumed and handed back through Finish().
class WarcBodySpill {
 public:
  // `file` must be open for both reading and writing: the archive writer reads
  // the spilled bytes back out of it. `task_runner` must be the same sequence
  // the archive is written on, which is what makes the appends ordered ahead
  // of the record that streams them -- no completion signal is needed, and
  // none could be relied on, since recording can stop while writes are still
  // queued.
  WarcBodySpill(base::File file,
                scoped_refptr<base::SequencedTaskRunner> task_runner);

  WarcBodySpill(const WarcBodySpill&) = delete;
  WarcBodySpill& operator=(const WarcBodySpill&) = delete;

  ~WarcBodySpill();

  // Queues `chunk` to be appended. Returns without blocking; the write lands
  // on the file sequence in call order.
  void Append(std::vector<uint8_t> chunk);

  // Bytes handed to Append() so far, which is what the record's Content-Length
  // must account for.
  uint64_t size() const { return size_; }

  // Hands over the spilled file. Callable only on the task runner given at
  // construction, and only after every queued append has therefore run. The
  // file is invalid if any write failed: a partially written spill must not be
  // archived, since the record already promises a length.
  base::File TakeFile();

 private:
  class Writer;

  uint64_t size_ = 0;
  scoped_refptr<base::SequencedTaskRunner> task_runner_;
  std::unique_ptr<Writer, base::OnTaskRunnerDeleter> writer_;
};

}  // namespace warc

#endif  // COMPONENTS_WARC_WARC_BODY_SPILL_H_
