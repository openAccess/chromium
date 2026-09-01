// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/warc_recorder.h"

#include <algorithm>
#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "net/http/http_util.h"

namespace network {

namespace {

// Pseudo-header names used by HTTP/2 and HTTP/3. They carry the information an
// HTTP/1.x request line holds, and must not be emitted as ordinary header
// lines.
// ":scheme" is also a pseudo-header, but it has no HTTP/1.1 equivalent and is
// dropped by the generic filter below rather than being named here.
constexpr char kMethodPseudoHeader[] = ":method";
constexpr char kPathPseudoHeader[] = ":path";
constexpr char kAuthorityPseudoHeader[] = ":authority";

// HTTP message framing is CRLF regardless of platform.
constexpr std::string_view kCrlf = "\r\n";

bool IsPseudoHeader(std::string_view name) {
  return !name.empty() && name.front() == ':';
}

bool StartsWithFieldName(std::string_view line, std::string_view name) {
  return line.size() > name.size() &&
         base::EqualsCaseInsensitiveASCII(line.substr(0, name.size()), name) &&
         line[name.size()] == ':';
}

// Rewrites an HTTP/1.1 header block so it describes a payload that the network
// stack already decoded: the transfer encoding is gone, so the headers claiming
// it must go too, and the length must match what is actually stored.
std::string RewriteHeadersForDecodedBody(std::string_view head,
                                         size_t body_size) {
  std::string out;
  out.reserve(head.size());

  bool wrote_length = false;
  for (std::string_view line : base::SplitStringPiece(
           head, "\r\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    // The payload is no longer encoded or chunked, so these would misdescribe
    // it. net has already removed chunked framing by this point.
    if (StartsWithFieldName(line, "content-encoding") ||
        StartsWithFieldName(line, "transfer-encoding")) {
      continue;
    }
    if (StartsWithFieldName(line, "content-length")) {
      base::StrAppend(
          &out, {"Content-Length: ", base::NumberToString(body_size), kCrlf});
      wrote_length = true;
      continue;
    }
    base::StrAppend(&out, {line, kCrlf});
  }

  if (!wrote_length) {
    base::StrAppend(
        &out, {"Content-Length: ", base::NumberToString(body_size), kCrlf});
  }

  // Restore the blank line that terminates the header block.
  out.append(kCrlf);
  return out;
}

std::string FindHeader(const net::HttpRawRequestHeaders& headers,
                       std::string_view name) {
  for (const auto& [key, value] : headers.headers()) {
    if (base::EqualsCaseInsensitiveASCII(key, name)) {
      return value;
    }
  }
  return std::string();
}

}  // namespace

WarcExchangeRecorder::WarcExchangeRecorder(
    base::WeakPtr<warc::WarcWriter> writer,
    size_t max_body_bytes,
    const std::string& warcinfo_id)
    : writer_(std::move(writer)),
      max_body_bytes_(max_body_bytes),
      warcinfo_id_(warcinfo_id),
      date_(base::Time::Now()) {}

WarcExchangeRecorder::~WarcExchangeRecorder() {
  // A load that was cancelled or torn down mid-flight still produced bytes that
  // belong in the archive; record what was seen rather than discarding it.
  if (!finished_ && (has_request_headers_ || has_response_headers_)) {
    if (truncated_reason_.empty() && !body_.empty()) {
      truncated_reason_ = "disconnect";
    }
    EmitRecords();
  }
}

void WarcExchangeRecorder::SetTargetUrl(const GURL& url) {
  target_url_ = url;
}

void WarcExchangeRecorder::SetRequestHeaders(
    const net::HttpRawRequestHeaders& headers,
    const std::string& method) {
  std::string block;

  if (!headers.request_line().empty()) {
    // HTTP/1.x: the request line is captured verbatim and already ends in CRLF.
    block = headers.request_line();
  } else {
    // HTTP/2 and HTTP/3 have no request line, so reconstruct one from the
    // pseudo-headers. The origin never saw these bytes in this form, which is
    // what WARC-Protocol on the emitted record records.
    std::string request_method = FindHeader(headers, kMethodPseudoHeader);
    if (request_method.empty()) {
      request_method = method;
    }
    std::string path = FindHeader(headers, kPathPseudoHeader);
    if (path.empty()) {
      path = target_url_.PathForRequest();
    }
    block = base::StrCat({request_method, " ", path, " HTTP/1.1\r\n"});
  }

  // The authority pseudo-header is the HTTP/1.x Host header; carry it over so
  // the reconstructed request is a well-formed HTTP/1.1 message.
  const std::string authority = FindHeader(headers, kAuthorityPseudoHeader);
  if (!authority.empty() && FindHeader(headers, "host").empty()) {
    base::StrAppend(&block, {"Host: ", authority, "\r\n"});
  }

  for (const auto& [name, value] : headers.headers()) {
    if (IsPseudoHeader(name)) {
      continue;
    }
    base::StrAppend(&block, {name, ": ", value, "\r\n"});
  }
  block.append("\r\n");

  request_block_ = std::move(block);
  has_request_headers_ = true;
}

void WarcExchangeRecorder::SetResponseHeaders(
    scoped_refptr<const net::HttpResponseHeaders> headers) {
  if (!headers) {
    return;
  }

  // raw_headers() stores lines NUL-separated; this restores CRLF framing and
  // appends the blank line that ends the header block. For HTTP/2 and HTTP/3
  // the result is a reconstruction, since no such block was ever transmitted.
  response_head_ =
      net::HttpUtil::ConvertHeadersBackToHTTPResponse(headers->raw_headers());
  has_response_headers_ = true;
}

void WarcExchangeRecorder::SetRemoteEndpoint(const net::IPEndPoint& endpoint) {
  if (endpoint.address().IsValid()) {
    ip_address_ = endpoint.ToStringWithoutPort();
  }
}

void WarcExchangeRecorder::SetProtocol(const std::string& protocol) {
  protocol_ = protocol;
}

void WarcExchangeRecorder::SetBodyIsWireFormat(bool is_wire_format) {
  body_is_wire_format_ = is_wire_format;
}

void WarcExchangeRecorder::AddBodyBytes(base::span<const uint8_t> bytes) {
  if (finished_ || bytes.empty()) {
    return;
  }

  if (body_.size() >= max_body_bytes_) {
    truncated_reason_ = "length";
    return;
  }

  const size_t room = max_body_bytes_ - body_.size();
  if (bytes.size() > room) {
    // Keep the prefix that fits and mark the record, rather than dropping the
    // body wholesale: a partial payload with an honest WARC-Truncated is more
    // useful than nothing, and silently storing it would be worse than both.
    bytes = bytes.first(room);
    truncated_reason_ = "length";
  }

  body_.insert(body_.end(), bytes.begin(), bytes.end());
}

void WarcExchangeRecorder::SetTruncated(const std::string& reason) {
  // A length truncation is already the strongest statement about the payload;
  // don't overwrite it with a weaker one.
  if (truncated_reason_ != "length") {
    truncated_reason_ = reason;
  }
}

void WarcExchangeRecorder::Finish() {
  if (finished_) {
    return;
  }
  EmitRecords();
}

void WarcExchangeRecorder::EmitRecords() {
  finished_ = true;

  if (!writer_ || !target_url_.is_valid()) {
    return;
  }

  const std::string target_uri = target_url_.spec();

  // The request record is written first and the response record points back at
  // it with WARC-Concurrent-To. This follows the exchange's chronology and
  // matches what GNU wget produces, which is the ordering most existing tooling
  // has been exercised against.
  std::string request_id;

  if (has_request_headers_) {
    warc::RecordHeader header;
    header.type = warc::RecordType::kRequest;
    header.target_uri = target_uri;
    header.date = date_;
    header.ip_address = ip_address_;
    header.protocol = protocol_;
    header.warcinfo_id = warcinfo_id_;
    header.record_id = warc::GenerateRecordId();
    request_id = header.record_id;

    const std::vector<uint8_t> block(request_block_.begin(),
                                     request_block_.end());
    // Request bodies are not captured yet, so the block is headers only and has
    // no distinct payload to digest.
    writer_->AddRecord(warc::SerializeRecord(header, block, block.size(),
                                             warc::DigestAlgorithm::kSha1));
  }

  if (has_response_headers_) {
    warc::RecordHeader header;
    header.type = warc::RecordType::kResponse;
    header.target_uri = target_uri;
    header.date = date_;
    header.ip_address = ip_address_;
    header.protocol = protocol_;
    header.truncated = truncated_reason_;
    header.warcinfo_id = warcinfo_id_;
    header.concurrent_to = request_id;

    // When the body reaching us was already decoded, the recorded headers must
    // be corrected to describe what is actually stored.
    const std::string head =
        body_is_wire_format_
            ? response_head_
            : RewriteHeadersForDecodedBody(response_head_, body_.size());

    std::vector<uint8_t> block(head.begin(), head.end());
    const size_t payload_offset = block.size();
    block.insert(block.end(), body_.begin(), body_.end());

    writer_->AddRecord(warc::SerializeRecord(header, block, payload_offset,
                                             warc::DigestAlgorithm::kSha1));
  }
}

WarcRecorder::WarcRecorder(base::File file,
                           const Limits& limits,
                           warc::WarcWriter::Compression compression)
    : writer_(std::make_unique<warc::WarcWriter>(std::move(file),
                                                 limits.max_queued_bytes,
                                                 compression)),
      limits_(limits),
      writer_weak_factory_(writer_.get()) {}

WarcRecorder::~WarcRecorder() = default;

std::unique_ptr<WarcExchangeRecorder> WarcRecorder::CreateExchangeRecorder() {
  return std::make_unique<WarcExchangeRecorder>(
      writer_weak_factory_.GetWeakPtr(), limits_.max_body_bytes, warcinfo_id_);
}

void WarcRecorder::WriteWarcinfo(const std::string& filename) {
  warc::RecordHeader header;
  header.type = warc::RecordType::kWarcinfo;
  header.date = base::Time::Now();
  header.filename = filename;
  header.record_id = warc::GenerateRecordId();

  // Remembered so every subsequent record can point back at this one.
  warcinfo_id_ = header.record_id;

  const std::vector<uint8_t> block = warc::BuildWarcinfoBlock({
      {"software", "Chromium"},
      {"format", "WARC File Format 1.1"},
      {"conformsTo",
       "https://iipc.github.io/warc-specifications/specifications/warc-format/"
       "warc-1.1/"},
  });

  writer_->AddRecord(warc::SerializeRecord(header, block, block.size(),
                                           warc::DigestAlgorithm::kSha1));
}

}  // namespace network
