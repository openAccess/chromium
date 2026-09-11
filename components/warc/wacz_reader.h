// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_WARC_WACZ_READER_H_
#define COMPONENTS_WARC_WACZ_READER_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file.h"

namespace warc {

// Reads a WACZ: the ZIP a web archive is delivered in, holding the WARC files,
// the index over them, and the list of pages that were captured.
//
// This reads the container's directory rather than unpacking it, because a
// WACZ is meant to be used where it lies. The archives inside are stored
// uncompressed precisely so that a record can be read from one by seeking --
// unpacking a gigabyte of WARC to serve one page would defeat the arrangement
// the format was built around. So each entry is located once, and afterwards
// a record is a read at an offset.
//
// Chromium's zip::ZipReader extracts whole entries and cannot say where one
// begins, which is the one thing needed here, so the directory is parsed
// directly.
class WaczReader {
 public:
  // Where an entry's bytes are and how they are kept.
  struct Entry {
    std::string name;

    // Where the content begins in the file, past the entry's local header.
    // For a stored entry this is where its bytes are: a record at offset N of
    // an archive is at `data_offset + N` of the WACZ.
    uint64_t data_offset = 0;

    uint64_t compressed_size = 0;
    uint64_t uncompressed_size = 0;

    // Stored rather than deflated. The format requires it of the archives and
    // the gzipped index, since seeking into a deflated entry is not possible.
    bool stored = false;
  };

  WaczReader(const WaczReader&) = delete;
  WaczReader& operator=(const WaczReader&) = delete;

  WaczReader(WaczReader&&);
  WaczReader& operator=(WaczReader&&);

  ~WaczReader();

  // Reads the container's directory. Returns nullopt if `file` is not a ZIP
  // this can make sense of.
  static std::optional<WaczReader> Open(base::File file);

  // The entry named, or null. Names are the paths the container holds, such
  // as "datapackage.json" or "archive/data.warc.gz".
  const Entry* Find(std::string_view name) const;

  // Every entry whose name begins with `prefix`, in the order the directory
  // holds them -- "archive/" for the WARC files, "indexes/" for the index.
  std::vector<const Entry*> FindAllWithPrefix(std::string_view prefix) const;

  // The whole of an entry, inflating it if it was deflated. For the small
  // parts -- the data package, the page list, the index -- not for an archive.
  std::optional<std::vector<uint8_t>> ReadEntry(const Entry& entry);

  // `length` bytes at `offset` within a stored entry, which is how a record is
  // read out of an archive without touching the rest of it. Fails for an entry
  // that is deflated, where an offset into the content means nothing.
  std::optional<std::vector<uint8_t>> ReadAt(const Entry& entry,
                                             uint64_t offset,
                                             uint64_t length);

 private:
  WaczReader();

  base::File file_;
  std::vector<Entry> entries_;
  std::map<std::string, size_t, std::less<>> by_name_;
};

}  // namespace warc

#endif  // COMPONENTS_WARC_WACZ_READER_H_
