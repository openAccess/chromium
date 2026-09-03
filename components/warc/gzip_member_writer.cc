// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/gzip_member_writer.h"

#include <utility>

#include "base/check.h"

namespace warc {

namespace {

// Compressed output is drained in chunks this size; deflate is called again
// while it keeps filling them.
constexpr size_t kOutputChunkSize = 64 * 1024;

// 16 + MAX_WBITS selects the gzip wrapper rather than zlib or raw deflate, so
// what comes out is a self-contained member.
constexpr int kGzipWindowBits = 16 + MAX_WBITS;

constexpr int kDefaultMemLevel = 8;

}  // namespace

GzipMemberWriter::GzipMemberWriter(SinkCallback sink)
    : sink_(std::move(sink)), stream_(), buffer_(kOutputChunkSize) {
  if (deflateInit2(&stream_, Z_DEFAULT_COMPRESSION, Z_DEFLATED, kGzipWindowBits,
                   kDefaultMemLevel, Z_DEFAULT_STRATEGY) != Z_OK) {
    ok_ = false;
    return;
  }
  stream_valid_ = true;
}

GzipMemberWriter::~GzipMemberWriter() {
  if (stream_valid_) {
    deflateEnd(&stream_);
  }
}

bool GzipMemberWriter::Write(base::span<const uint8_t> data) {
  CHECK(!finished_);
  if (!ok_) {
    return false;
  }
  if (data.empty()) {
    return true;
  }
  return Deflate(data, Z_NO_FLUSH);
}

bool GzipMemberWriter::Finish() {
  CHECK(!finished_);
  finished_ = true;
  if (!ok_) {
    return false;
  }
  return Deflate({}, Z_FINISH);
}

bool GzipMemberWriter::Deflate(base::span<const uint8_t> data, int flush) {
  // zlib does not take a const pointer here even though it only reads.
  stream_.next_in =
      const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data.data()));
  stream_.avail_in = static_cast<uInt>(data.size());

  do {
    stream_.next_out = reinterpret_cast<Bytef*>(buffer_.data());
    stream_.avail_out = static_cast<uInt>(buffer_.size());

    const int result = deflate(&stream_, flush);
    if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) {
      ok_ = false;
      return false;
    }

    const size_t produced = buffer_.size() - stream_.avail_out;
    if (produced > 0 && !sink_.Run(base::span(buffer_).first(produced))) {
      ok_ = false;
      return false;
    }

    if (result == Z_STREAM_END) {
      return true;
    }
    // Keep going while deflate is still filling the output buffer, or still
    // holds input it has not consumed.
  } while (stream_.avail_out == 0 || stream_.avail_in > 0);

  return true;
}

}  // namespace warc
