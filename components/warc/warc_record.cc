// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/warc_record.h"

#include <cinttypes>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/uuid.h"
#include "components/base32/base32.h"
#include "crypto/hash.h"

namespace warc {

namespace {

// Every structural delimiter in a WARC file is CRLF, independent of platform.
constexpr std::string_view kCrlf = "\r\n";

// Records are separated by two CRLFs following the block.
constexpr std::string_view kRecordSeparator = "\r\n\r\n";

constexpr std::string_view kVersion = "WARC/1.1";

void AppendField(std::string& out,
                 std::string_view name,
                 std::string_view value) {
  if (value.empty()) {
    return;
  }
  base::StrAppend(&out, {name, ": ", value, kCrlf});
}

std::string_view DefaultContentType(RecordType type) {
  switch (type) {
    case RecordType::kRequest:
      return "application/http;msgtype=request";
    case RecordType::kResponse:
      return "application/http;msgtype=response";
    case RecordType::kWarcinfo:
      return "application/warc-fields";
    case RecordType::kMetadata:
      return "application/warc-fields";
    case RecordType::kResource:
    case RecordType::kRevisit:
      return "";
  }
  NOTREACHED();
}

}  // namespace

RecordHeader::RecordHeader() = default;
RecordHeader::RecordHeader(const RecordHeader&) = default;
RecordHeader& RecordHeader::operator=(const RecordHeader&) = default;
RecordHeader::~RecordHeader() = default;

std::string_view RecordTypeToString(RecordType type) {
  switch (type) {
    case RecordType::kWarcinfo:
      return "warcinfo";
    case RecordType::kRequest:
      return "request";
    case RecordType::kResponse:
      return "response";
    case RecordType::kResource:
      return "resource";
    case RecordType::kMetadata:
      return "metadata";
    case RecordType::kRevisit:
      return "revisit";
  }
  NOTREACHED();
}

std::string GenerateRecordId() {
  return base::StrCat(
      {"<urn:uuid:", base::Uuid::GenerateRandomV4().AsLowercaseString(), ">"});
}

std::string FormatWarcDate(base::Time time) {
  base::Time::Exploded exploded;
  time.UTCExplode(&exploded);

  // Time::Exploded only carries millisecond resolution, so recover the
  // sub-millisecond part directly from the epoch offset. WARC 1.1 allows this
  // finer precision and it keeps records distinguishable when many resources
  // are captured within the same millisecond.
  const int64_t since_epoch_us =
      (time - base::Time::UnixEpoch()).InMicroseconds();
  int64_t microseconds = since_epoch_us % base::Time::kMicrosecondsPerSecond;
  if (microseconds < 0) {
    microseconds += base::Time::kMicrosecondsPerSecond;
  }

  return base::StringPrintf("%04d-%02d-%02dT%02d:%02d:%02d.%06" PRId64 "Z",
                            exploded.year, exploded.month,
                            exploded.day_of_month, exploded.hour,
                            exploded.minute, exploded.second, microseconds);
}

std::string ComputeDigest(base::span<const uint8_t> data,
                          DigestAlgorithm algorithm) {
  std::string_view label;
  crypto::hash::HashKind kind;
  switch (algorithm) {
    case DigestAlgorithm::kNone:
      return std::string();
    case DigestAlgorithm::kSha1:
      label = "sha1:";
      kind = crypto::hash::HashKind::kSha1;
      break;
    case DigestAlgorithm::kSha256:
      label = "sha256:";
      kind = crypto::hash::HashKind::kSha256;
      break;
  }

  std::vector<uint8_t> digest(crypto::hash::DigestSizeForHashKind(kind));
  crypto::hash::Hash(kind, data, digest);

  // WARC digests are conventionally unpadded base32.
  return base::StrCat(
      {label,
       base32::Base32Encode(digest, base32::Base32EncodePolicy::OMIT_PADDING)});
}

std::vector<uint8_t> BuildWarcinfoBlock(
    const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string block;
  for (const auto& [name, value] : fields) {
    AppendField(block, name, value);
  }
  return std::vector<uint8_t>(block.begin(), block.end());
}

std::vector<uint8_t> SerializeRecord(const RecordHeader& header,
                                     base::span<const uint8_t> block,
                                     size_t payload_offset,
                                     DigestAlgorithm algorithm) {
  CHECK_LE(payload_offset, block.size());

  std::string head = base::StrCat({kVersion, kCrlf});

  AppendField(head, "WARC-Type", RecordTypeToString(header.type));

  const std::string record_id =
      header.record_id.empty() ? GenerateRecordId() : header.record_id;
  AppendField(head, "WARC-Record-ID", record_id);

  const base::Time date =
      header.date.is_null() ? base::Time::Now() : header.date;
  AppendField(head, "WARC-Date", FormatWarcDate(date));

  AppendField(head, "WARC-Target-URI", header.target_uri);
  AppendField(head, "WARC-Concurrent-To", header.concurrent_to);
  AppendField(head, "WARC-Warcinfo-ID", header.warcinfo_id);
  AppendField(head, "WARC-IP-Address", header.ip_address);
  AppendField(head, "WARC-Filename", header.filename);
  AppendField(head, "WARC-Refers-To", header.refers_to);
  AppendField(head, "WARC-Profile", header.profile);
  AppendField(head, "WARC-Protocol", header.protocol);
  AppendField(head, "WARC-Truncated", header.truncated);

  if (algorithm != DigestAlgorithm::kNone) {
    AppendField(head, "WARC-Block-Digest", ComputeDigest(block, algorithm));
    // A payload digest is only meaningful when the block has an entity body
    // distinct from its headers.
    if (payload_offset < block.size()) {
      AppendField(head, "WARC-Payload-Digest",
                  ComputeDigest(block.subspan(payload_offset), algorithm));
    }
  }

  const std::string_view content_type = header.content_type.empty()
                                            ? DefaultContentType(header.type)
                                            : header.content_type;
  AppendField(head, "Content-Type", content_type);

  // Content-Length is mandatory and must match the block exactly; readers use
  // it to find the next record rather than scanning for a delimiter.
  AppendField(head, "Content-Length", base::NumberToString(block.size()));

  // Blank line terminates the header.
  head.append(kCrlf);

  std::vector<uint8_t> out;
  out.reserve(head.size() + block.size() + kRecordSeparator.size());
  out.insert(out.end(), head.begin(), head.end());
  out.insert(out.end(), block.begin(), block.end());
  out.insert(out.end(), kRecordSeparator.begin(), kRecordSeparator.end());
  return out;
}

}  // namespace warc
