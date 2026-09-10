// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/warc_record_parser.h"

#include <algorithm>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"

namespace warc {

namespace {

constexpr std::string_view kVersionPrefix = "WARC/";
constexpr std::string_view kCrLf = "\r\n";
constexpr std::string_view kRecordSeparator = "\r\n\r\n";

std::string_view AsStringView(base::span<const uint8_t> data) {
  return std::string_view(reinterpret_cast<const char*>(data.data()),
                          data.size());
}

// Marks the parse as needing more bytes rather than as broken, and fails.
std::optional<ParsedRecord> NeedMore(bool* incomplete) {
  if (incomplete) {
    *incomplete = true;
  }
  return std::nullopt;
}

}  // namespace

ParsedRecord::ParsedRecord() = default;
ParsedRecord::ParsedRecord(const ParsedRecord&) = default;
ParsedRecord& ParsedRecord::operator=(const ParsedRecord&) = default;
ParsedRecord::ParsedRecord(ParsedRecord&&) = default;
ParsedRecord& ParsedRecord::operator=(ParsedRecord&&) = default;
ParsedRecord::~ParsedRecord() = default;

std::string_view ParsedRecord::Field(std::string_view name) const {
  for (const auto& [field_name, value] : fields) {
    if (base::EqualsCaseInsensitiveASCII(field_name, name)) {
      return value;
    }
  }
  return std::string_view();
}

std::optional<ParsedRecord> ParseRecord(base::span<const uint8_t> data,
                                        bool* incomplete) {
  if (incomplete) {
    *incomplete = false;
  }

  const std::string_view text = AsStringView(data);
  if (text.size() < kVersionPrefix.size()) {
    return NeedMore(incomplete);
  }
  if (!text.starts_with(kVersionPrefix)) {
    return std::nullopt;
  }

  // The header ends at the first blank line. Finding it first means the whole
  // header is known to be present before any of it is interpreted.
  const size_t header_end = text.find(kRecordSeparator);
  if (header_end == std::string_view::npos) {
    return NeedMore(incomplete);
  }

  ParsedRecord record;
  const std::string_view header = text.substr(0, header_end);
  size_t line_start = 0;
  bool first_line = true;
  while (line_start < header.size()) {
    size_t line_end = header.find(kCrLf, line_start);
    if (line_end == std::string_view::npos) {
      line_end = header.size();
    }
    const std::string_view line =
        header.substr(line_start, line_end - line_start);
    line_start = line_end + kCrLf.size();

    if (first_line) {
      record.version = std::string(line);
      first_line = false;
      continue;
    }
    // A field continued onto the next line is legal in the grammar the format
    // borrows, and is not handled here: no writer in web archiving produces
    // one, and guessing at where a value resumes would be worse than saying
    // plainly that this record was not understood.
    if (line.empty() || base::IsAsciiWhitespace(line.front())) {
      return std::nullopt;
    }
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      return std::nullopt;
    }
    std::string_view name = line.substr(0, colon);
    std::string_view value = line.substr(colon + 1);
    // One optional space after the colon is the convention; anything else is
    // whitespace the value never meant to carry.
    value = base::TrimWhitespaceASCII(value, base::TRIM_ALL);
    if (name.empty()) {
      return std::nullopt;
    }
    record.fields.emplace_back(std::string(name), std::string(value));
  }

  uint64_t content_length = 0;
  const std::string_view length_field = record.Field("Content-Length");
  if (length_field.empty() ||
      !base::StringToUint64(length_field, &content_length)) {
    // Without it the block has no end, and so neither does the record: every
    // record after this one would be found in the wrong place.
    return std::nullopt;
  }

  const size_t block_start = header_end + kRecordSeparator.size();
  // Checked against the buffer before being added to it, so that a length
  // wildly larger than the file cannot wrap around into a small number.
  if (content_length > data.size() - block_start) {
    return NeedMore(incomplete);
  }
  const size_t block_end = block_start + content_length;
  if (data.size() - block_end < kRecordSeparator.size()) {
    return NeedMore(incomplete);
  }
  if (text.substr(block_end, kRecordSeparator.size()) != kRecordSeparator) {
    // The block did not end where its length promised, so the bytes after it
    // are not the next record and nothing further can be framed.
    return std::nullopt;
  }

  record.block = data.subspan(block_start, content_length);
  record.size = block_end + kRecordSeparator.size();
  return record;
}

}  // namespace warc
