// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_WARC_WARC_RECORD_PARSER_H_
#define COMPONENTS_WARC_WARC_RECORD_PARSER_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/memory/raw_span.h"

namespace warc {

// Reading the format warc_record.h writes. See that header for the layout.
//
// Records are returned as the named fields the file actually holds rather than
// as the struct that produced them: an archive may come from any tool, may
// carry fields this code has never heard of, and a reader that dropped them
// would quietly lose what it could not name. What a caller wants from a record
// -- a target URI, a payload digest -- it asks for by field name.

// One record, as read.
struct ParsedRecord {
  ParsedRecord();
  ParsedRecord(const ParsedRecord&);
  ParsedRecord& operator=(const ParsedRecord&);
  ParsedRecord(ParsedRecord&&);
  ParsedRecord& operator=(ParsedRecord&&);
  ~ParsedRecord();

  // The value of `name`, compared without regard to case because the
  // specification fixes the spelling of field names but not their casing, and
  // tools differ. Empty when the field is absent, which is also what an empty
  // field looks like; the distinction has never mattered to a reader here.
  std::string_view Field(std::string_view name) const;

  // "WARC/1.1", or whatever version the record declared. Kept rather than
  // checked, so that a 1.0 archive can be read and a reader that cares can
  // decide for itself.
  std::string version;

  // Every named field, in the order the file holds them.
  std::vector<std::pair<std::string, std::string>> fields;

  // The block, pointing into the buffer that was parsed rather than copied out
  // of it -- a response body can be large, and replaying one should not mean
  // duplicating it. The buffer must outlive the record.
  base::raw_span<const uint8_t> block;

  // What the whole record occupies, header and block and the separator that
  // ends it. A caller walking an archive advances by this.
  size_t size = 0;
};

// Parses the record at the start of `data`.
//
// Returns nullopt when `data` does not begin with a complete, well-formed
// record. `*incomplete`, when given, distinguishes a record that more bytes
// would finish from one that no amount of reading will fix -- which is the
// difference between waiting for the rest of a file and refusing it.
std::optional<ParsedRecord> ParseRecord(base::span<const uint8_t> data,
                                        bool* incomplete = nullptr);

}  // namespace warc

#endif  // COMPONENTS_WARC_WARC_RECORD_PARSER_H_
