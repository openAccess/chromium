// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_WARC_GZIP_MEMBER_WRITER_H_
#define COMPONENTS_WARC_GZIP_MEMBER_WRITER_H_

#include <stdint.h>

#include <vector>

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "third_party/zlib/zlib.h"

namespace warc {

// Deflates one gzip member incrementally.
//
// A ".warc.gz" holds one gzip member per record. Compressing a record from a
// single buffer is fine while the record is in memory, but a record whose body
// is streamed off disk is never in memory whole -- so its member has to be
// deflated in pieces. Everything written between construction and Finish()
// becomes exactly one member.
//
// This class must be used on a single sequence, and deflating is CPU-bound, so
// that should not be a sequence where blocking matters.
class GzipMemberWriter {
 public:
  // Receives compressed bytes as they become available. Returning false aborts
  // the member; every later call fails without invoking the sink again.
  using SinkCallback = base::RepeatingCallback<bool(base::span<const uint8_t>)>;

  explicit GzipMemberWriter(SinkCallback sink);

  GzipMemberWriter(const GzipMemberWriter&) = delete;
  GzipMemberWriter& operator=(const GzipMemberWriter&) = delete;

  ~GzipMemberWriter();

  // Adds `data` to the member. Returns false once anything has failed.
  bool Write(base::span<const uint8_t> data);

  // Ends the member, flushing what deflate still holds. Must be called exactly
  // once; without it the member is incomplete and no reader can parse it.
  bool Finish();

 private:
  bool Deflate(base::span<const uint8_t> data, int flush);

  SinkCallback sink_;
  z_stream stream_;
  std::vector<uint8_t> buffer_;
  bool stream_valid_ = false;
  bool ok_ = true;
  bool finished_ = false;
};

}  // namespace warc

#endif  // COMPONENTS_WARC_GZIP_MEMBER_WRITER_H_
