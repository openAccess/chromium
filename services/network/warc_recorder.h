// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_NETWORK_WARC_RECORDER_H_
#define SERVICES_NETWORK_WARC_RECORDER_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "base/component_export.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "components/warc/warc_record.h"
#include "components/warc/warc_writer.h"
#include "net/base/ip_endpoint.h"
#include "net/http/http_raw_request_headers.h"
#include "net/http/http_response_headers.h"
#include "url/gurl.h"

namespace network {

// Records HTTP exchanges into a WARC file (ISO 28500).
//
// One WarcRecorder exists per recording session — created when the browser
// hands the network service an open file, and living until the service shuts
// down. Each recorded URLLoader owns a WarcExchangeRecorder obtained from it.
//
// Everything here runs on the network sequence. Only in-memory work happens
// there; the underlying warc::WarcWriter moves the actual file I/O to a
// blocking-capable sequence.

// Accumulates a single request/response exchange and emits the resulting pair
// of WARC records when it completes.
//
// A WARC record must state its block's exact length before the block itself, so
// the response body has to be complete before anything can be written. Bodies
// are therefore buffered, and `max_body_bytes` caps that buffer: past the cap
// the body is truncated and the record is marked "WARC-Truncated: length", as
// the specification requires. Without that marking a truncated response would
// be indistinguishable from a complete one.
class COMPONENT_EXPORT(NETWORK_SERVICE) WarcExchangeRecorder {
 public:
  // `warcinfo_id` identifies the warcinfo record describing this capture, and
  // is stamped on every record so a reader can attribute them to this run.
  WarcExchangeRecorder(base::WeakPtr<warc::WarcWriter> writer,
                       size_t max_body_bytes,
                       const std::string& warcinfo_id);

  WarcExchangeRecorder(const WarcExchangeRecorder&) = delete;
  WarcExchangeRecorder& operator=(const WarcExchangeRecorder&) = delete;

  // Emits the records if Finish() was never called, so that a load abandoned
  // part-way is still archived rather than silently dropped.
  ~WarcExchangeRecorder();

  // The URL this exchange targets. Must be set before Finish().
  void SetTargetUrl(const GURL& url);

  // The request exactly as it went to the wire, from
  // URLRequest::SetRequestHeadersCallback. `method` is needed because HTTP/2
  // and HTTP/3 carry no request line and it must be reconstructed.
  void SetRequestHeaders(const net::HttpRawRequestHeaders& headers,
                         const std::string& method);

  // The response headers, from URLRequest::SetResponseHeadersCallback. That
  // callback reports what the server actually sent, unlike
  // URLRequest::response_headers(), which on a revalidation returns headers
  // merged with the cached entry.
  void SetResponseHeaders(
      scoped_refptr<const net::HttpResponseHeaders> headers);

  // The endpoint the request actually reached, recorded as WARC-IP-Address so
  // the capture stays interpretable after DNS changes.
  void SetRemoteEndpoint(const net::IPEndPoint& endpoint);

  // The negotiated protocol ("http/1.1", "h2", "h3"), recorded because HTTP/2
  // and HTTP/3 header blocks are reconstructed rather than captured.
  void SetProtocol(const std::string& protocol);

  // Whether AddBodyBytes() will receive the payload in its wire form, still
  // carrying whatever transfer encoding the server applied.
  //
  // When false the body has already been decoded by the network stack, and the
  // recorded header block is rewritten at emit time to drop Content-Encoding
  // and restate Content-Length. That loses byte fidelity, but the alternative —
  // storing a decoded payload beneath a header claiming it is gzipped — yields
  // a record no reader can correctly interpret.
  void SetBodyIsWireFormat(bool is_wire_format);

  // Response body bytes, in order.
  void AddBodyBytes(base::span<const uint8_t> bytes);

  // Marks the body as ending early for a reason other than the size cap.
  // `reason` must be a value the specification defines: "time", "disconnect",
  // or "unspecified".
  void SetTruncated(const std::string& reason);

  // Writes the request and response records. Safe to call once; later calls do
  // nothing.
  void Finish();

  bool body_truncated() const { return !truncated_reason_.empty(); }

 private:
  void EmitRecords();

  base::WeakPtr<warc::WarcWriter> writer_;
  const size_t max_body_bytes_;
  const std::string warcinfo_id_;

  GURL target_url_;
  base::Time date_;
  std::string protocol_;
  std::string ip_address_;
  std::string truncated_reason_;

  // The reconstructed request block: request line plus header lines.
  std::string request_block_;
  bool has_request_headers_ = false;

  // The response header block, in HTTP/1.1 wire form.
  std::string response_head_;
  bool has_response_headers_ = false;
  bool body_is_wire_format_ = false;

  std::vector<uint8_t> body_;

  bool finished_ = false;
};

class COMPONENT_EXPORT(NETWORK_SERVICE) WarcRecorder {
 public:
  struct COMPONENT_EXPORT(NETWORK_SERVICE) Limits {
    // Ceiling on records queued but not yet written. Bodies are far larger than
    // the log entries this pattern is borrowed from, so a slow disk must be
    // allowed to shed load rather than grow memory without bound.
    size_t max_queued_bytes = 64u * 1024 * 1024;

    // Ceiling on a single buffered response body.
    size_t max_body_bytes = 32u * 1024 * 1024;
  };

  // `file` must already be open for writing: the network service is sandboxed
  // and cannot open arbitrary paths, so the browser process opens it and passes
  // the handle across. `compression` selects the on-disk framing; see
  // warc::WarcWriter::Compression.
  WarcRecorder(base::File file,
               const Limits& limits,
               warc::WarcWriter::Compression compression);

  WarcRecorder(const WarcRecorder&) = delete;
  WarcRecorder& operator=(const WarcRecorder&) = delete;

  ~WarcRecorder();

  // Returns a recorder for one exchange.
  std::unique_ptr<WarcExchangeRecorder> CreateExchangeRecorder();

  // Writes the warcinfo record that describes this capture. Called once, before
  // any exchange records.
  void WriteWarcinfo(const std::string& filename);

  warc::WarcWriter& writer_for_testing() { return *writer_; }

 private:
  std::unique_ptr<warc::WarcWriter> writer_;
  const Limits limits_;

  // Identifier of this capture's warcinfo record, empty until it is written.
  std::string warcinfo_id_;

  base::WeakPtrFactory<warc::WarcWriter> writer_weak_factory_;
};

}  // namespace network

#endif  // SERVICES_NETWORK_WARC_RECORDER_H_
