// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_WARC_GZIP_MEMBER_READER_H_
#define COMPONENTS_WARC_GZIP_MEMBER_READER_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <vector>

#include "base/containers/span.h"

namespace warc {

// Inflates the single gzip member at the start of `data`.
//
// A ".warc.gz" is a concatenation of members, one per record, and that layout
// exists so a record can be reached without reading the records before it.
// Reading one therefore has to stop where that member stops and say where that
// was: `*compressed_size` receives how many bytes of `data` the member
// occupied, which is what lets a caller step to the next member, or read the
// one record an index pointed at and nothing else.
//
// Returns nullopt if `data` does not begin with a complete, valid member.
// Trailing bytes after the member are ignored, since in an archive they are
// the members that follow.
//
// The member is inflated into memory whole, so this suits records that were
// meant to be held in memory. A body archived by spilling to disk can be
// larger than is wise to inflate this way.
std::optional<std::vector<uint8_t>> InflateGzipMember(
    base::span<const uint8_t> data,
    size_t* compressed_size);

}  // namespace warc

#endif  // COMPONENTS_WARC_GZIP_MEMBER_READER_H_
