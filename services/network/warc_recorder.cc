// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/warc_recorder.h"

#include <algorithm>
#include <utility>

#include "base/check_op.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "net/http/http_util.h"

namespace network {

namespace {

// Published location of the format this archive claims to follow, repeated in
// every warcinfo record so each is self-describing.
constexpr char kConformsTo[] =
    "https://iipc.github.io/warc-specifications/specifications/warc-format/"
    "warc-1.1/";

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

// Header fields whose values are credentials. An archive that hands its reader
// the session it was captured with is a liability rather than a record, and
// none of these are needed to understand what was served.
constexpr std::string_view kCredentialHeaders[] = {
    "cookie", "set-cookie", "authorization", "proxy-authorization"};

constexpr std::string_view kRedactionMarker = "[redacted]";

// Replaces the value of every credential-bearing field in an HTTP/1.1 header
// block. The field name is kept, in the casing it was sent with, so the record
// still shows that one was present and a reader can tell a redacted request
// from a request that carried no cookie at all. The first line -- the request
// or status line -- is never a header and is left alone.
std::string RedactCredentialHeaders(std::string_view head) {
  std::string out;
  out.reserve(head.size());

  bool first_line = true;
  for (std::string_view line : base::SplitStringPiece(
           head, kCrlf, base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (first_line) {
      first_line = false;
      base::StrAppend(&out, {line, kCrlf});
      continue;
    }

    std::string_view name;
    for (std::string_view candidate : kCredentialHeaders) {
      if (StartsWithFieldName(line, candidate)) {
        name = line.substr(0, candidate.size());
        break;
      }
    }
    if (name.empty()) {
      base::StrAppend(&out, {line, kCrlf});
    } else {
      base::StrAppend(&out, {name, ": ", kRedactionMarker, kCrlf});
    }
  }

  // Restore the blank line that terminates the header block.
  out.append(kCrlf);
  return out;
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

// Whether `head` claims an encoding that a decoded payload would contradict.
// A response that declared none needs no correction even though the net stack
// nominally did the decoding, and storing its header block verbatim is
// strictly more faithful than reserializing it.
bool DeclaresTransferEncoding(std::string_view head) {
  for (std::string_view line : base::SplitStringPiece(
           head, "\r\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (StartsWithFieldName(line, "content-encoding") ||
        StartsWithFieldName(line, "transfer-encoding")) {
      return true;
    }
  }
  return false;
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
    base::RepeatingCallback<std::string()> warcinfo_id,
    bool redact_credentials)
    : writer_(std::move(writer)),
      max_body_bytes_(max_body_bytes),
      warcinfo_id_(std::move(warcinfo_id)),
      redact_credentials_(redact_credentials),
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

  request_block_ =
      redact_credentials_ ? RedactCredentialHeaders(block) : std::move(block);
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
  if (redact_credentials_) {
    response_head_ = RedactCredentialHeaders(response_head_);
  }
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

void WarcExchangeRecorder::EnableBodySpilling(
    SpillFactory spill_factory,
    base::RepeatingCallback<void(base::File)> return_file) {
  spill_factory_ = std::move(spill_factory);
  return_spill_file_ = std::move(return_file);
}

bool WarcExchangeRecorder::BeginSpill() {
  // A decoded body has its header block rewritten at emit time, which would
  // not match a block digest taken as the bytes streamed past.
  if (!body_is_wire_format_ || !spill_factory_) {
    return false;
  }
  std::unique_ptr<warc::WarcBodySpill> spill = spill_factory_.Run();
  if (!spill) {
    return false;
  }

  block_hasher_.emplace(crypto::hash::HashKind::kSha1);
  payload_hasher_.emplace(crypto::hash::HashKind::kSha1);
  // The block is the HTTP header block followed by the body, so the block
  // digest starts from the head.
  block_hasher_->Update(base::as_byte_span(response_head_));
  block_hasher_->Update(body_);
  payload_hasher_->Update(body_);

  spill->Append(std::move(body_));
  body_.clear();
  spill_ = std::move(spill);
  return true;
}

void WarcExchangeRecorder::AddBodyBytes(base::span<const uint8_t> bytes) {
  if (finished_ || bytes.empty()) {
    return;
  }

  if (spill_) {
    block_hasher_->Update(bytes);
    payload_hasher_->Update(bytes);
    spill_->Append(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    return;
  }

  // At the ceiling the body moves to disk rather than being cut short, which
  // removes the limit instead of raising it: once the bytes are spilled their
  // size costs no memory. Truncation below is what happens when there is no
  // spill file to move them to.
  if (body_.size() + bytes.size() > max_body_bytes_ && BeginSpill()) {
    AddBodyBytes(bytes);
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
  // Resolved once: both records belong to the same context, and asking twice
  // would be asking the recorder to mint the same warcinfo twice.
  const std::string warcinfo_id = warcinfo_id_ ? warcinfo_id_.Run() : "";

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
    header.warcinfo_id = warcinfo_id;
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
    header.warcinfo_id = warcinfo_id;
    header.concurrent_to = request_id;

    if (spill_) {
      EmitSpilledResponseRecord(header);
      return;
    }

    // When the body reaching us was already decoded, the recorded headers must
    // be corrected to describe what is actually stored -- but only if they
    // claim an encoding in the first place.
    const bool needs_rewrite =
        !body_is_wire_format_ && DeclaresTransferEncoding(response_head_);
    const std::string head = needs_rewrite ? RewriteHeadersForDecodedBody(
                                                 response_head_, body_.size())
                                           : response_head_;

    std::vector<uint8_t> block(head.begin(), head.end());
    const size_t payload_offset = block.size();
    block.insert(block.end(), body_.begin(), body_.end());

    writer_->AddRecord(warc::SerializeRecord(header, block, payload_offset,
                                             warc::DigestAlgorithm::kSha1));
  }
}

void WarcExchangeRecorder::EmitSpilledResponseRecord(
    const warc::RecordHeader& header) {
  // No more bytes can arrive, so the digests are final even though the block
  // itself is on disk and never was in memory whole.
  std::vector<uint8_t> block_digest(crypto::hash::kSha1Size);
  std::vector<uint8_t> payload_digest(crypto::hash::kSha1Size);
  block_hasher_->Finish(block_digest);
  payload_hasher_->Finish(payload_digest);

  const uint64_t body_size = spill_->size();
  std::vector<uint8_t> head = warc::SerializeRecordHeader(
      header, response_head_.size() + body_size,
      warc::LabelDigest(block_digest, warc::DigestAlgorithm::kSha1),
      warc::LabelDigest(payload_digest, warc::DigestAlgorithm::kSha1));
  // Only the body was spilled, so the HTTP header block still precedes it.
  head.insert(head.end(), response_head_.begin(), response_head_.end());

  // No waiting is needed: the spill wrote on the archive's own file sequence,
  // so every append is already ordered ahead of this record.
  if (writer_) {
    writer_->AddRecordWithSpilledBody(std::move(head), std::move(spill_),
                                      body_size, return_spill_file_);
  }
}

WarcRecorder::WarcRecorder(base::File file,
                           const Limits& limits,
                           warc::WarcWriter::Compression compression,
                           bool redact_credentials)
    : writer_(std::make_unique<warc::WarcWriter>(std::move(file),
                                                 limits.max_queued_bytes,
                                                 compression)),
      limits_(limits),
      redact_credentials_(redact_credentials),
      writer_weak_factory_(writer_.get()) {}

WarcRecorder::~WarcRecorder() {
  // An archive that is quietly missing records is worse than one that failed
  // outright, because nothing about the file looks wrong.
  const uint64_t dropped = writer_->dropped_records();
  if (dropped > 0) {
    LOG(ERROR) << "WARC recording: " << dropped
               << " record(s) dropped because the write queue could not keep "
                  "up; the archive is incomplete.";
  }
}

std::unique_ptr<WarcExchangeRecorder> WarcRecorder::CreateExchangeRecorder(
    const std::string& browsing_context) {
  return std::make_unique<WarcExchangeRecorder>(
      writer_weak_factory_.GetWeakPtr(), limits_.max_body_bytes,
      WarcinfoResolver(browsing_context), redact_credentials_);
}

std::unique_ptr<WarcExchangeRecorder> WarcRecorder::CreateCompletionRecorder(
    const std::string& browsing_context) {
  auto recorder = std::make_unique<WarcExchangeRecorder>(
      writer_weak_factory_.GetWeakPtr(), limits_.max_body_bytes,
      WarcinfoResolver(browsing_context), redact_credentials_);
  recorder->EnableBodySpilling(
      // A WeakPtr cannot bind to a method that returns a value, and an
      // exchange recorder may outlive the session recorder.
      base::BindRepeating(
          [](base::WeakPtr<WarcRecorder> self)
              -> std::unique_ptr<warc::WarcBodySpill> {
            return self ? self->TakeBodySpill() : nullptr;
          },
          weak_factory_.GetWeakPtr()),
      base::BindRepeating(&WarcRecorder::ReturnSpillFile,
                          weak_factory_.GetWeakPtr()));
  return recorder;
}

void WarcRecorder::SetSpillFile(base::File file) {
  spill_file_ = std::move(file);
}

std::unique_ptr<warc::WarcBodySpill> WarcRecorder::TakeBodySpill() {
  if (!spill_file_.IsValid()) {
    return nullptr;
  }
  // Start from an empty file: what a previous completion left behind is not
  // part of this one's block.
  if (!spill_file_.SetLength(0)) {
    return nullptr;
  }
  spill_file_.Seek(base::File::FROM_BEGIN, 0);
  if (!writer_) {
    return nullptr;
  }
  return writer_->CreateBodySpill(std::move(spill_file_));
}

void WarcRecorder::ReturnSpillFile(base::File file) {
  spill_file_ = std::move(file);
}

void WarcRecorder::WriteWarcinfo(const std::string& filename) {
  filename_ = filename;

  // The file-level record, carrying WARC-Filename. Exchanges point at their own
  // context's warcinfo rather than this one, which stands as the header for the
  // file as a whole.
  warc::RecordHeader header;
  header.type = warc::RecordType::kWarcinfo;
  header.date = base::Time::Now();
  header.filename = filename;
  header.record_id = warc::GenerateRecordId();

  std::vector<std::pair<std::string, std::string>> fields = {
      {"software", "Chromium"},
      {"format", "WARC File Format 1.1"},
      {"conformsTo", kConformsTo},
  };
  if (redact_credentials_) {
    fields.emplace_back("redacted",
                        "cookie, set-cookie, authorization, "
                        "proxy-authorization");
  }
  const std::vector<uint8_t> block = warc::BuildWarcinfoBlock(fields);

  writer_->AddRecord(warc::SerializeRecord(header, block, block.size(),
                                           warc::DigestAlgorithm::kSha1));
}

void WarcRecorder::Rotate(base::File file, const std::string& filename) {
  const bool recording_continues = file.IsValid();

  // Handed over before anything below it, so every record that follows lands in
  // the new file. The rotation and the records travel the same queue, which
  // preserves the order they were given in.
  writer_->Rotate(std::move(file));

  // These name warcinfo records that stay behind in the file just closed. Kept,
  // they would leave every record in the new file citing a record it does not
  // contain. WARC 1.1 permits that -- WARC-Warcinfo-ID may cross files -- but a
  // segment that cannot describe itself defeats the point of writing a rotated
  // set at all. Cleared, each context mints a fresh warcinfo into the new file
  // the next time it archives anything, and a context that goes quiet after the
  // rotation costs the new file nothing.
  context_warcinfo_ids_.clear();

  if (!recording_continues) {
    filename_.clear();
    return;
  }
  WriteWarcinfo(filename);
}

base::RepeatingCallback<std::string()> WarcRecorder::WarcinfoResolver(
    const std::string& browsing_context) {
  // An exchange can outlive the session recorder, in which case there is
  // nothing left to attribute it to.
  return base::BindRepeating(
      [](base::WeakPtr<WarcRecorder> self,
         const std::string& context) -> std::string {
        return self ? self->WarcinfoIdForContext(context) : std::string();
      },
      weak_factory_.GetWeakPtr(), browsing_context);
}

const std::string& WarcRecorder::WarcinfoIdForContext(
    const std::string& browsing_context) {
  auto existing = context_warcinfo_ids_.find(browsing_context);
  if (existing != context_warcinfo_ids_.end()) {
    return existing->second;
  }

  warc::RecordHeader header;
  header.type = warc::RecordType::kWarcinfo;
  header.date = base::Time::Now();
  header.record_id = warc::GenerateRecordId();

  // The format defines no field for a browsing context, so the context is
  // described in the warcinfo block -- free-form "application/warc-fields" --
  // and records are bound to it by WARC-Warcinfo-ID. Nothing here is outside
  // the standard: no new named field is invented, and a reader that does not
  // care simply follows the id it already understands.
  std::vector<std::pair<std::string, std::string>> fields = {
      {"software", "Chromium"},
      {"format", "WARC File Format 1.1"},
      {"conformsTo", kConformsTo},
  };
  if (!filename_.empty()) {
    fields.emplace_back("isPartOf", filename_);
  }
  if (redact_credentials_) {
    fields.emplace_back("redacted",
                        "cookie, set-cookie, authorization, "
                        "proxy-authorization");
  }
  if (browsing_context.empty()) {
    fields.emplace_back(
        "description",
        "Requests no page is responsible for, such as the browser's own "
        "background traffic");
  } else {
    fields.emplace_back("browsing-context", browsing_context);
    fields.emplace_back("description",
                        base::StrCat({"Requests made by the browsing context ",
                                      browsing_context}));
  }

  const std::vector<uint8_t> block = warc::BuildWarcinfoBlock(fields);
  writer_->AddRecord(warc::SerializeRecord(header, block, block.size(),
                                           warc::DigestAlgorithm::kSha1));

  return context_warcinfo_ids_.emplace(browsing_context, header.record_id)
      .first->second;
}

}  // namespace network
