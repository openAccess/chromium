// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/warc_test_util.h"

#include <stddef.h>
#include <stdint.h>

#include <utility>

#include "base/containers/span.h"
#include "components/warc/gzip_member_reader.h"

namespace warc {

std::optional<std::vector<std::string>> InflateGzipMembers(
    std::string_view data) {
  std::vector<std::string> members;

  base::span<const uint8_t> rest = base::as_byte_span(data);
  while (!rest.empty()) {
    size_t compressed_size = 0;
    std::optional<std::vector<uint8_t>> member =
        InflateGzipMember(rest, &compressed_size);
    if (!member || compressed_size == 0) {
      return std::nullopt;
    }
    members.emplace_back(member->begin(), member->end());
    // Whatever this member did not consume begins the next one.
    rest = rest.subspan(compressed_size);
  }

  return members;
}

}  // namespace warc
