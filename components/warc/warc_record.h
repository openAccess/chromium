// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_WARC_WARC_RECORD_H_
#define COMPONENTS_WARC_WARC_RECORD_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/time/time.h"

namespace warc {

// Serialization of WARC records as defined by ISO 28500:2017 (WARC 1.1).
//
// A WARC file is a concatenation of records. Each record is:
//
//   WARC/1.1<CRLF>
//   <named field><CRLF>            (one or more)
//   <CRLF>                         (blank line ends the header)
//   <block>                        (exactly Content-Length bytes)
//   <CRLF><CRLF>                   (record separator)
//
// For web capture the block of a `response` record is the raw HTTP response
// exactly as it arrived on the wire: status line, header block, and body still
// in its transfer encoding (e.g. still gzip-compressed). Recording decoded
// bodies while retaining the original Content-Encoding header produces records
// that no WARC reader can correctly interpret, so callers must supply
// untransformed bytes.

// Record types from ISO 28500 section 6.
enum class RecordType {
  kWarcinfo,
  kRequest,
  kResponse,
  kResource,
  kMetadata,
  kRevisit,
};

// Algorithm used for WARC-Block-Digest and WARC-Payload-Digest.
//
// SHA-1 is the conventional choice: CDX indexes and the deduplication paths in
// common WARC tooling assume `sha1:` labels. Chromium discourages SHA-1 in new
// code (see crypto/hash.h), so kSha256 is offered for callers who prefer to
// avoid it and accept reduced interoperability. kNone omits both digest fields,
// which the specification permits.
enum class DigestAlgorithm {
  kSha1,
  kSha256,
  kNone,
};

// Returns a new record identifier of the form "<urn:uuid:...>", including the
// angle brackets required for a WARC-Record-ID field value.
std::string GenerateRecordId();

// Formats `time` as a WARC-Date. WARC 1.1 permits sub-second precision; this
// emits microseconds, e.g. "2026-09-01T12:34:56.123456Z".
std::string FormatWarcDate(base::Time time);

// The named fields of a single record. Fields left empty are omitted, except
// where noted.
struct RecordHeader {
  RecordHeader();
  RecordHeader(const RecordHeader&);
  RecordHeader& operator=(const RecordHeader&);
  ~RecordHeader();

  RecordType type = RecordType::kResponse;

  // WARC-Record-ID. A fresh identifier is generated when this is empty.
  std::string record_id;

  // WARC-Date. Defaults to the current time when left at the default value.
  base::Time date;

  // WARC-Target-URI. Required for every type except warcinfo and metadata.
  std::string target_uri;

  // WARC-Concurrent-To. Links a response record to the request it answers; the
  // value must include angle brackets.
  std::string concurrent_to;

  // WARC-Warcinfo-ID. The warcinfo record describing the capture this record
  // belongs to, which is how a reader attributes records in a concatenated
  // archive to the run that produced them.
  std::string warcinfo_id;

  // WARC-IP-Address. The address the request was actually sent to, which is
  // what makes a capture reproducible across DNS changes.
  std::string ip_address;

  // Content-Type of the block. When empty a value appropriate to `type` is
  // used: "application/http;msgtype=response" for responses, the request
  // equivalent for requests, and "application/warc-fields" for warcinfo.
  std::string content_type;

  // WARC-Filename, valid only on warcinfo records.
  std::string filename;

  // WARC-Refers-To, valid on revisit and metadata records.
  std::string refers_to;

  // WARC-Profile, required on revisit records.
  std::string profile;

  // WARC-Protocol. A non-ISO field, but conventional in web archiving tools,
  // recording the protocol actually used ("http/1.1", "h2", "h3"). It matters
  // here because HTTP/2 and HTTP/3 never carry a literal header block, so the
  // header lines in such a record are reconstructed rather than captured, and a
  // reader has no other way to tell.
  std::string protocol;

  // WARC-Truncated. Set when the block does not hold the complete payload, with
  // the reason the specification defines: "length" when a size limit was hit,
  // "time" for a timeout, "disconnect" when the connection dropped, or
  // "unspecified". Recording a partial body without this field would present
  // truncated content as though it were complete.
  std::string truncated;
};

// Serializes one complete record, including the trailing record separator.
//
// `block` is the record block and is written verbatim; Content-Length is
// derived from its length.
//
// `payload_offset` is the index within `block` at which the HTTP entity body
// begins, used to compute WARC-Payload-Digest. Pass block.size() for records
// with no distinct payload (such as warcinfo), which omits the field. The
// block digest always covers the whole block.
std::vector<uint8_t> SerializeRecord(const RecordHeader& header,
                                     base::span<const uint8_t> block,
                                     size_t payload_offset,
                                     DigestAlgorithm algorithm);

// Builds the block for a warcinfo record: "application/warc-fields" content
// describing the capture, given as ordered name/value pairs.
std::vector<uint8_t> BuildWarcinfoBlock(
    const std::vector<std::pair<std::string, std::string>>& fields);

// Returns the labeled digest of `data`, e.g. "sha1:AAMB5AZUUZ7ZCOVJPGA...".
// The digest is unpadded base32 as WARC tooling expects. Returns an empty
// string for kNone.
std::string ComputeDigest(base::span<const uint8_t> data,
                          DigestAlgorithm algorithm);

// Returns the WARC-Type field value for `type`.
std::string_view RecordTypeToString(RecordType type);

}  // namespace warc

#endif  // COMPONENTS_WARC_WARC_RECORD_H_
