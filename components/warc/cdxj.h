// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_WARC_CDXJ_H_
#define COMPONENTS_WARC_CDXJ_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"

namespace warc {

// Reading a CDXJ index: what a web archive keeps instead of reading every
// record to find one.
//
// A line is a SURT key, a 14-digit capture time, and a JSON object saying
// where the record is and what it holds:
//
//   org,example)/a 20260910201354 {"url": "...", "offset": "594", ...}
//
// Sorted by that key and time, which is what makes a lookup a search rather
// than a scan, and what the SURT transform exists to arrange.

// One entry. Numbers arrive as JSON strings in the files these come from, and
// are parsed here so a caller does not have to know that.
struct CdxjEntry {
  CdxjEntry();
  CdxjEntry(const CdxjEntry&);
  CdxjEntry& operator=(const CdxjEntry&);
  CdxjEntry(CdxjEntry&&);
  CdxjEntry& operator=(CdxjEntry&&);
  ~CdxjEntry();

  // The SURT key and the capture time, as the line is sorted by.
  std::string key;
  std::string timestamp;

  // The URL as it was requested, which the key cannot be turned back into.
  std::string url;

  // Where the record is: which archive, and the bytes it occupies in it. For
  // a gzipped archive these describe the compressed member, so a reader seeks
  // to `offset` and inflates from there.
  std::string filename;
  uint64_t offset = 0;
  uint64_t length = 0;

  // What the record holds, enough to answer without reading it: the response
  // status, its media type, and the digest of the payload -- which is also
  // how two captures of an unchanged resource are recognised as one.
  std::string status;
  std::string mime;
  std::string digest;
};

// Parses one line. Returns nullopt for a line that is not an entry, including
// the "!meta" line that heads a secondary index.
std::optional<CdxjEntry> ParseCdxjLine(std::string_view line);

// Parses a whole index, skipping lines that are not entries. Blank lines and
// the "!meta" header are expected; anything else malformed is skipped too,
// since one unreadable line is no reason to lose the rest of an archive.
std::vector<CdxjEntry> ParseCdxj(std::string_view text);

// Returns the capture of `key` nearest `timestamp`, or null if the key is not
// in `entries`.
//
// Nearest rather than exact because replay asks for a page as it was at a
// moment, and the archive holds it as it was at the moments it was captured.
// A page and the images on it were captured seconds apart, so demanding an
// exact time would assemble a page out of nothing.
//
// `entries` must be sorted by key, as an index is.
const CdxjEntry* FindNearestCapture(base::span<const CdxjEntry> entries,
                                    std::string_view key,
                                    std::string_view timestamp);

}  // namespace warc

#endif  // COMPONENTS_WARC_CDXJ_H_
