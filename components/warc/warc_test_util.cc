// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/warc_test_util.h"

#include <stddef.h>

#include <utility>

#include "third_party/zlib/zlib.h"

namespace warc {

std::optional<std::vector<std::string>> InflateGzipMembers(
    std::string_view data) {
  std::vector<std::string> members;

  while (!data.empty()) {
    z_stream stream = {};
    // 16 + MAX_WBITS asks for the gzip wrapper rather than zlib or raw deflate.
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
      return std::nullopt;
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());

    std::string member;
    char buffer[4096];
    int result = Z_OK;
    while (result != Z_STREAM_END) {
      stream.next_out = reinterpret_cast<Bytef*>(buffer);
      stream.avail_out = sizeof(buffer);
      result = inflate(&stream, Z_NO_FLUSH);
      // Anything else — including the Z_BUF_ERROR that a truncated member ends
      // on — means this is not a well-formed member.
      if (result != Z_OK && result != Z_STREAM_END) {
        inflateEnd(&stream);
        return std::nullopt;
      }
      member.append(buffer, sizeof(buffer) - stream.avail_out);
    }

    // Whatever this member did not consume begins the next one.
    data.remove_prefix(data.size() - stream.avail_in);
    inflateEnd(&stream);
    members.push_back(std::move(member));
  }

  return members;
}

}  // namespace warc
