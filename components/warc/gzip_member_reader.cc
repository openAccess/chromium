// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/gzip_member_reader.h"

#include <utility>

#include "third_party/zlib/zlib.h"

namespace warc {

namespace {

// How much is inflated at a time. Records are usually far smaller than this;
// the buffer only bounds how often the output vector grows.
constexpr size_t kChunkSize = 64 * 1024;

}  // namespace

std::optional<std::vector<uint8_t>> InflateGzipMember(
    base::span<const uint8_t> data,
    size_t* compressed_size) {
  if (data.empty()) {
    return std::nullopt;
  }

  z_stream stream = {};
  // 16 + MAX_WBITS selects gzip framing rather than raw deflate or zlib.
  if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
    return std::nullopt;
  }

  stream.next_in = const_cast<Bytef*>(data.data());
  stream.avail_in = static_cast<uInt>(data.size());

  std::vector<uint8_t> out;
  std::vector<uint8_t> chunk(kChunkSize);
  int result = Z_OK;
  do {
    stream.next_out = chunk.data();
    stream.avail_out = static_cast<uInt>(chunk.size());
    result = inflate(&stream, Z_NO_FLUSH);
    if (result != Z_OK && result != Z_STREAM_END) {
      inflateEnd(&stream);
      return std::nullopt;
    }
    const size_t produced = chunk.size() - stream.avail_out;
    out.insert(out.end(), chunk.begin(), chunk.begin() + produced);
    // Ran out of input before the member ended: the member is truncated, and
    // an incomplete record is not one a reader may hand on.
    if (result != Z_STREAM_END && stream.avail_in == 0 && produced == 0) {
      inflateEnd(&stream);
      return std::nullopt;
    }
  } while (result != Z_STREAM_END);

  if (compressed_size) {
    *compressed_size = stream.total_in;
  }
  inflateEnd(&stream);
  return out;
}

}  // namespace warc
