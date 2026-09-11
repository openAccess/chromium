// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/wacz_reader.h"

#include <algorithm>
#include <utility>

#include "base/logging.h"
#include "base/numerics/byte_conversions.h"
#include "base/numerics/checked_math.h"
#include "third_party/zlib/zlib.h"

namespace warc {

namespace {

// ZIP structure, as PKWARE's APPNOTE defines it.
constexpr uint32_t kEndOfCentralDirectorySignature = 0x06054b50;
constexpr uint32_t kZip64LocatorSignature = 0x07064b50;
constexpr uint32_t kZip64EndOfCentralDirectorySignature = 0x06064b50;
constexpr uint32_t kCentralFileHeaderSignature = 0x02014b50;
constexpr uint32_t kLocalFileHeaderSignature = 0x04034b50;

constexpr size_t kEndOfCentralDirectorySize = 22;
constexpr size_t kZip64LocatorSize = 20;
constexpr size_t kZip64EndOfCentralDirectorySize = 56;
constexpr size_t kCentralFileHeaderSize = 46;
constexpr size_t kLocalFileHeaderSize = 30;

// A comment may follow the end record, and its length is only knowable from
// the record itself, so the record is found by searching back for it.
constexpr size_t kMaxCommentSize = 0xFFFF;

constexpr uint16_t kMethodStored = 0;
constexpr uint16_t kZip64ExtraFieldId = 0x0001;

// A 32-bit field set to all ones says the real value is in the Zip64 extra
// field. That is how an archive larger than four gigabytes is described, and a
// WACZ is quite capable of being one.
constexpr uint32_t kNeedsZip64 = 0xFFFFFFFF;
constexpr uint16_t kNeedsZip64Short = 0xFFFF;

uint16_t ReadU16(base::span<const uint8_t> data, size_t offset) {
  return base::U16FromLittleEndian(data.subspan(offset).first<2>());
}

uint32_t ReadU32(base::span<const uint8_t> data, size_t offset) {
  return base::U32FromLittleEndian(data.subspan(offset).first<4>());
}

uint64_t ReadU64(base::span<const uint8_t> data, size_t offset) {
  return base::U64FromLittleEndian(data.subspan(offset).first<8>());
}

// Reads exactly `length` bytes at `offset`, or nothing. A short read means the
// file is not what its own directory says it is.
std::optional<std::vector<uint8_t>> ReadExactly(base::File& file,
                                                uint64_t offset,
                                                uint64_t length) {
  if (length > std::numeric_limits<size_t>::max()) {
    return std::nullopt;
  }
  std::vector<uint8_t> buffer(static_cast<size_t>(length));
  if (length == 0) {
    return buffer;
  }
  std::optional<size_t> read =
      file.Read(static_cast<int64_t>(offset), base::span(buffer));
  if (!read.has_value() || *read != buffer.size()) {
    return std::nullopt;
  }
  return buffer;
}

// The Zip64 extra field carries whichever of the sizes and the offset the
// 32-bit fields could not hold, in that order and only those.
struct Zip64Fields {
  std::optional<uint64_t> uncompressed_size;
  std::optional<uint64_t> compressed_size;
  std::optional<uint64_t> local_header_offset;
};

Zip64Fields ParseZip64Extra(base::span<const uint8_t> extra,
                            bool want_uncompressed,
                            bool want_compressed,
                            bool want_offset) {
  Zip64Fields fields;
  size_t at = 0;
  while (at + 4 <= extra.size()) {
    const uint16_t id = ReadU16(extra, at);
    const uint16_t size = ReadU16(extra, at + 2);
    const size_t body = at + 4;
    if (body + size > extra.size()) {
      break;
    }
    if (id == kZip64ExtraFieldId) {
      size_t cursor = body;
      const auto take = [&](bool wanted) -> std::optional<uint64_t> {
        if (!wanted || cursor + 8 > body + size) {
          return std::nullopt;
        }
        const uint64_t value = ReadU64(extra, cursor);
        cursor += 8;
        return value;
      };
      fields.uncompressed_size = take(want_uncompressed);
      fields.compressed_size = take(want_compressed);
      fields.local_header_offset = take(want_offset);
      break;
    }
    at = body + size;
  }
  return fields;
}

// A deflated ZIP entry carries no header of its own -- the container's
// directory holds what a gzip header would -- so this is raw deflate, and
// asking zlib for gzip framing would reject it.
std::optional<std::vector<uint8_t>> RawInflate(base::span<const uint8_t> data,
                                               uint64_t expected_size) {
  if (expected_size > std::numeric_limits<size_t>::max()) {
    return std::nullopt;
  }
  std::vector<uint8_t> out(static_cast<size_t>(expected_size));
  if (out.empty()) {
    return out;
  }

  z_stream stream = {};
  // A negative window size selects raw deflate.
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
    return std::nullopt;
  }
  stream.next_in = const_cast<Bytef*>(data.data());
  stream.avail_in = static_cast<uInt>(data.size());
  stream.next_out = out.data();
  stream.avail_out = static_cast<uInt>(out.size());

  const int result = inflate(&stream, Z_FINISH);
  const bool complete = result == Z_STREAM_END && stream.avail_out == 0;
  inflateEnd(&stream);
  // The directory said how large this entry is; anything else means the
  // container and its contents disagree.
  return complete ? std::optional(std::move(out)) : std::nullopt;
}

}  // namespace

WaczReader::WaczReader() = default;
WaczReader::WaczReader(WaczReader&&) = default;
WaczReader& WaczReader::operator=(WaczReader&&) = default;
WaczReader::~WaczReader() = default;

std::optional<WaczReader> WaczReader::Open(base::File file) {
  if (!file.IsValid()) {
    return std::nullopt;
  }
  const int64_t length = file.GetLength();
  if (length < static_cast<int64_t>(kEndOfCentralDirectorySize)) {
    return std::nullopt;
  }

  // Search back from the end for the end-of-central-directory record, which
  // is where a ZIP says what it contains. It is last because a ZIP is written
  // front to back, and the directory cannot be written until the entries are.
  const uint64_t tail_size = std::min<uint64_t>(
      static_cast<uint64_t>(length),
      kEndOfCentralDirectorySize + kMaxCommentSize + kZip64LocatorSize);
  const uint64_t tail_at = static_cast<uint64_t>(length) - tail_size;
  std::optional<std::vector<uint8_t>> tail =
      ReadExactly(file, tail_at, tail_size);
  if (!tail) {
    return std::nullopt;
  }

  std::optional<size_t> end_record;
  for (size_t i = tail->size() - kEndOfCentralDirectorySize + 1; i-- > 0;) {
    if (ReadU32(*tail, i) == kEndOfCentralDirectorySignature) {
      end_record = i;
      break;
    }
  }
  if (!end_record) {
    return std::nullopt;
  }

  uint64_t entry_count = ReadU16(*tail, *end_record + 10);
  uint64_t directory_size = ReadU32(*tail, *end_record + 12);
  uint64_t directory_offset = ReadU32(*tail, *end_record + 16);

  // Any of those saturated means the real values are in the Zip64 records,
  // which sit immediately before this one.
  if (entry_count == kNeedsZip64Short || directory_size == kNeedsZip64 ||
      directory_offset == kNeedsZip64) {
    if (*end_record < kZip64LocatorSize) {
      return std::nullopt;
    }
    const size_t locator = *end_record - kZip64LocatorSize;
    if (ReadU32(*tail, locator) != kZip64LocatorSignature) {
      return std::nullopt;
    }
    const uint64_t zip64_at = ReadU64(*tail, locator + 8);
    std::optional<std::vector<uint8_t>> zip64 =
        ReadExactly(file, zip64_at, kZip64EndOfCentralDirectorySize);
    if (!zip64 ||
        ReadU32(*zip64, 0) != kZip64EndOfCentralDirectorySignature) {
      return std::nullopt;
    }
    entry_count = ReadU64(*zip64, 32);
    directory_size = ReadU64(*zip64, 40);
    directory_offset = ReadU64(*zip64, 48);
  }

  std::optional<std::vector<uint8_t>> directory =
      ReadExactly(file, directory_offset, directory_size);
  if (!directory) {
    return std::nullopt;
  }

  WaczReader reader;
  reader.entries_.reserve(
      std::min<uint64_t>(entry_count, directory->size() /
                                          kCentralFileHeaderSize + 1));

  size_t at = 0;
  for (uint64_t i = 0; i < entry_count; ++i) {
    if (at + kCentralFileHeaderSize > directory->size() ||
        ReadU32(*directory, at) != kCentralFileHeaderSignature) {
      return std::nullopt;
    }
    const uint16_t method = ReadU16(*directory, at + 10);
    uint64_t compressed_size = ReadU32(*directory, at + 20);
    uint64_t uncompressed_size = ReadU32(*directory, at + 24);
    const uint16_t name_length = ReadU16(*directory, at + 28);
    const uint16_t extra_length = ReadU16(*directory, at + 30);
    const uint16_t comment_length = ReadU16(*directory, at + 32);
    uint64_t local_header_offset = ReadU32(*directory, at + 42);

    const size_t name_at = at + kCentralFileHeaderSize;
    const size_t extra_at = name_at + name_length;
    const size_t next = extra_at + extra_length + comment_length;
    if (next > directory->size()) {
      return std::nullopt;
    }

    if (uncompressed_size == kNeedsZip64 || compressed_size == kNeedsZip64 ||
        local_header_offset == kNeedsZip64) {
      const Zip64Fields zip64 = ParseZip64Extra(
          base::span(*directory).subspan(extra_at, extra_length),
          uncompressed_size == kNeedsZip64, compressed_size == kNeedsZip64,
          local_header_offset == kNeedsZip64);
      uncompressed_size = zip64.uncompressed_size.value_or(uncompressed_size);
      compressed_size = zip64.compressed_size.value_or(compressed_size);
      local_header_offset =
          zip64.local_header_offset.value_or(local_header_offset);
    }

    Entry entry;
    const base::span<const uint8_t> name_bytes =
        base::span(*directory).subspan(name_at, size_t{name_length});
    entry.name = std::string(name_bytes.begin(), name_bytes.end());
    entry.compressed_size = compressed_size;
    entry.uncompressed_size = uncompressed_size;
    entry.stored = method == kMethodStored;

    // Where the content begins is only knowable from the entry's own local
    // header, because the name and extra field there may be sized differently
    // from the ones in the directory.
    std::optional<std::vector<uint8_t>> local =
        ReadExactly(file, local_header_offset, kLocalFileHeaderSize);
    if (!local || ReadU32(*local, 0) != kLocalFileHeaderSignature) {
      return std::nullopt;
    }
    base::CheckedNumeric<uint64_t> data_offset = local_header_offset;
    data_offset += kLocalFileHeaderSize;
    data_offset += ReadU16(*local, 26);
    data_offset += ReadU16(*local, 28);
    if (!data_offset.AssignIfValid(&entry.data_offset)) {
      return std::nullopt;
    }

    // A ZIP records the directories its entries sit in as entries of their
    // own, holding nothing. Counting one among the archives would have a
    // reader open a file that is not there.
    if (!entry.name.ends_with("/")) {
      reader.by_name_.emplace(entry.name, reader.entries_.size());
      reader.entries_.push_back(std::move(entry));
    }
    at = next;
  }

  reader.file_ = std::move(file);
  return reader;
}

const WaczReader::Entry* WaczReader::Find(std::string_view name) const {
  const auto it = by_name_.find(name);
  return it == by_name_.end() ? nullptr : &entries_[it->second];
}

std::vector<const WaczReader::Entry*> WaczReader::FindAllWithPrefix(
    std::string_view prefix) const {
  std::vector<const Entry*> found;
  for (const Entry& entry : entries_) {
    if (entry.name.starts_with(prefix)) {
      found.push_back(&entry);
    }
  }
  return found;
}

std::optional<std::vector<uint8_t>> WaczReader::ReadEntry(
    const Entry& entry) {
  std::optional<std::vector<uint8_t>> raw =
      ReadExactly(file_, entry.data_offset, entry.compressed_size);
  if (!raw) {
    return std::nullopt;
  }
  if (entry.stored) {
    return raw;
  }
  return RawInflate(*raw, entry.uncompressed_size);
}

std::optional<std::vector<uint8_t>> WaczReader::ReadAt(const Entry& entry,
                                                       uint64_t offset,
                                                       uint64_t length) {
  // An offset into content that is deflated names nothing: the bytes at that
  // position in the file are compressed ones, belonging to no particular part
  // of what they decode to.
  if (!entry.stored) {
    return std::nullopt;
  }
  base::CheckedNumeric<uint64_t> end = offset;
  end += length;
  uint64_t end_value = 0;
  if (!end.AssignIfValid(&end_value) || end_value > entry.compressed_size) {
    return std::nullopt;
  }
  return ReadExactly(file_, entry.data_offset + offset, length);
}

}  // namespace warc
