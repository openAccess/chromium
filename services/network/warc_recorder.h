// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_NETWORK_WARC_RECORDER_H_
#define SERVICES_NETWORK_WARC_RECORDER_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/component_export.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "components/warc/warc_body_spill.h"
#include "components/warc/warc_record.h"
#include "components/warc/warc_writer.h"
#include "crypto/hash.h"
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
  // `warcinfo_id` yields the record id of the warcinfo describing this
  // exchange's browsing context, which is stamped on every record so a reader
  // can attribute it. It is resolved when the records are emitted, not now, so
  // that an exchange which archives nothing does not leave behind a warcinfo
  // describing nothing.
  WarcExchangeRecorder(base::WeakPtr<warc::WarcWriter> writer,
                       size_t max_body_bytes,
                       base::RepeatingCallback<std::string()> warcinfo_id,
                       bool redact_credentials);

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
  // This must reflect what the network stack actually did for this response,
  // not what was asked of it: net declines to skip decoding in cases the
  // caller cannot predict, such as a response carrying `use-as-dictionary`.
  //
  // When false the body has already been decoded by the network stack, and a
  // recorded header block that claims an encoding is rewritten at emit time to
  // drop Content-Encoding and restate Content-Length. That loses byte
  // fidelity, but the alternative — storing a decoded payload beneath a header
  // claiming it is gzipped — yields a record no reader can correctly
  // interpret.
  void SetBodyIsWireFormat(bool is_wire_format);

  // Response body bytes, in order.
  void AddBodyBytes(base::span<const uint8_t> bytes);

  // Supplies a file to divert the body into once it outgrows the in-memory
  // ceiling, rather than truncating it there. A record must state its length
  // before its block, so without this a whole resource has to be buffered, and
  // a video cannot be.
  //
  // `spill_factory` returns nothing when no file is free, in which case the
  // body stays in memory under the usual cap. Only meaningful for a body in
  // wire form: a decoded body has its header block rewritten at emit time,
  // which would invalidate a digest taken while the bytes streamed past.
  using SpillFactory =
      base::RepeatingCallback<std::unique_ptr<warc::WarcBodySpill>()>;
  void EnableBodySpilling(
      SpillFactory spill_factory,
      base::RepeatingCallback<void(base::File)> return_file);

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

  // Moves whatever is buffered to disk and starts hashing as bytes go past.
  // Returns false when no spill file was available.
  bool BeginSpill();

  // Queues the response record whose body is already on disk.
  void EmitSpilledResponseRecord(const warc::RecordHeader& header);

  base::WeakPtr<warc::WarcWriter> writer_;
  const size_t max_body_bytes_;
  const base::RepeatingCallback<std::string()> warcinfo_id_;

  // Whether cookies and authorization headers keep their values. Off by
  // default: recording is reachable from chrome://flags, and a stray archive
  // should not be a working credential.
  const bool redact_credentials_;

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

  // Set once the body has been diverted to disk. From then on `body_` stays
  // empty and the digests come from these, since the bytes are never all in
  // memory at once.
  std::unique_ptr<warc::WarcBodySpill> spill_;
  std::optional<crypto::hash::Hasher> block_hasher_;
  std::optional<crypto::hash::Hasher> payload_hasher_;

  SpillFactory spill_factory_;
  base::RepeatingCallback<void(base::File)> return_spill_file_;

  bool finished_ = false;
};

class COMPONENT_EXPORT(NETWORK_SERVICE) WarcRecorder {
 public:
  struct COMPONENT_EXPORT(NETWORK_SERVICE) Limits {
    // Ceiling on records queued but not yet written. Bodies are far larger than
    // the log entries this pattern is borrowed from, so a slow disk must be
    // allowed to shed load rather than grow memory without bound.
    size_t max_queued_bytes = 64u * 1024 * 1024;

    // How much of a response body is held in memory. A record must state its
    // length before its block, so a body has to be complete before it can be
    // written; this bounds what that costs.
    //
    // It is the only bound on a body, and it is deliberately not a size limit
    // on the resource. Past it the body goes to disk where a spill file is
    // available, which removes the limit rather than raising it -- bytes on
    // disk cost no memory -- and is truncated with "WARC-Truncated: length"
    // only where one is not.
    size_t max_body_bytes = 32u * 1024 * 1024;
  };

  // `file` must already be open for writing: the network service is sandboxed
  // and cannot open arbitrary paths, so the browser process opens it and passes
  // the handle across. `compression` selects the on-disk framing; see
  // warc::WarcWriter::Compression.
  // `redact_credentials` strips the values of cookie and authorization fields
  // from every record, which is the default; the archive says so in its
  // warcinfo, since the block digests then describe the redacted text rather
  // than what crossed the wire.
  WarcRecorder(base::File file,
               const Limits& limits,
               warc::WarcWriter::Compression compression,
               bool redact_credentials);

  WarcRecorder(const WarcRecorder&) = delete;
  WarcRecorder& operator=(const WarcRecorder&) = delete;

  ~WarcRecorder();

  // Returns a recorder for one exchange. `browsing_context` is the serialized
  // top-level origin the request belongs to, or empty for a request no page is
  // responsible for -- the browser's own background traffic.
  //
  // Each distinct context gets a warcinfo record of its own, which is how the
  // format groups records inside one file. WARC 1.1 makes WARC-Warcinfo-ID
  // override the positional association a warcinfo otherwise implies ("the
  // WARC-Warcinfo-ID field value overrides any association with a previously
  // occurring warcinfo record"), so contexts may interleave freely -- which
  // they do, since a browser loads several pages at once.
  std::unique_ptr<WarcExchangeRecorder> CreateExchangeRecorder(
      const std::string& browsing_context);

  // Returns a recorder for a whole-resource fetch made to complete a resource
  // the page only requested ranges of. Such a body goes to disk rather than
  // memory once it grows past a threshold, so a completed video costs the
  // archive no more memory than a page subresource. It belongs to the same
  // browsing context as the ranged requests that prompted it.
  std::unique_ptr<WarcExchangeRecorder> CreateCompletionRecorder(
      const std::string& browsing_context);

  // Hands the network service a file to spill large bodies into. The service
  // is sandboxed and cannot open one itself, so the browser opens it alongside
  // the archive, exactly as it does the archive.
  void SetSpillFile(base::File file);

  // Writes the warcinfo record that heads a file, naming it in WARC-Filename.
  // Called before any exchange records reach that file: once when the session
  // starts, and again for each file a rotation opens.
  void WriteWarcinfo(const std::string& filename);

  // Closes the current archive and continues into `file`, which the browser
  // opens for the same reason it opens the first one -- the network service is
  // sandboxed. `filename` names the new file in its own warcinfo, so every
  // segment says what it is called wherever it later ends up.
  //
  // An invalid `file` stops recording instead: later records are discarded, and
  // no warcinfo is written since nothing would receive it.
  //
  // A rotation never splits an exchange. A request and its response are handed
  // to the writer together as one act, and a rotation takes its place in the
  // same queue, so a concurrent pair cannot end up straddling two files.
  //
  // `on_previous_file_complete` runs once the file being closed has been
  // written, flushed and closed -- not merely handed over. Until then the
  // segment is still being drained onto disk, so anything that reads it, such
  // as an index or a container built from it, has to wait for this.
  void Rotate(base::File file,
              const std::string& filename,
              base::OnceClosure on_previous_file_complete);

  // Receives the file a rotation should continue into, and the name to record
  // in its warcinfo. An invalid file declines, which stops recording.
  using NextFileCallback =
      base::OnceCallback<void(base::File file, const std::string& filename)>;

  // Rotates on its own once the file being written passes `max_file_bytes`,
  // asking `request_next_file` for each new file. Zero disables it, which is
  // the default: a session then records into the single file it was given.
  //
  // The threshold is soft, and cannot be otherwise. The size is read between
  // exchanges, the count it reads lags what is queued, and obtaining a file
  // means a round trip to the browser -- so a file passes the threshold and
  // keeps growing until the answer comes back. Records are never split or held
  // to make a file land on a boundary, because a WARC's value is that it is
  // complete, not that it is a particular size.
  void SetRotationPolicy(
      uint64_t max_file_bytes,
      base::RepeatingCallback<void(NextFileCallback)> request_next_file);

  warc::WarcWriter& writer_for_testing() { return *writer_; }

 private:
  std::unique_ptr<warc::WarcWriter> writer_;
  const Limits limits_;
  const bool redact_credentials_;

  // Name recorded in the file-level warcinfo record, repeated as `isPartOf` in
  // each context's own warcinfo.
  std::string filename_;

  // Warcinfo record id per browsing context, minted on first sight of one
  // within the current file. Rotation clears them, since the records they name
  // stay behind in the file being closed.
  std::map<std::string, std::string> context_warcinfo_ids_;

  // Asks for a new file if the current one has grown past the threshold.
  // Called where exchanges begin, which is the natural point at which no record
  // is part-written and the network sequence is already here.
  void MaybeRotate();

  // Continues into the file the request came back with, or stops if it
  // declined.
  void OnNextFile(base::File file, const std::string& filename);

  uint64_t max_file_bytes_ = 0;
  base::RepeatingCallback<void(NextFileCallback)> request_next_file_;

  // Set while a request for the next file is outstanding. Without it the
  // threshold would be crossed on every exchange until the answer arrived, and
  // each one would ask again.
  bool awaiting_next_file_ = false;

  // Resolves, on demand, the warcinfo id describing `browsing_context`.
  base::RepeatingCallback<std::string()> WarcinfoResolver(
      const std::string& browsing_context);

  // Returns the warcinfo id describing `browsing_context`, writing that
  // warcinfo record the first time the context actually archives something.
  const std::string& WarcinfoIdForContext(const std::string& browsing_context);

  // The single spill file, absent while a completion is using it. Completions
  // run one at a time, so one file suffices; a completion that finds it gone
  // keeps its body in memory under the usual cap.
  base::File spill_file_;

  std::unique_ptr<warc::WarcBodySpill> TakeBodySpill();
  void ReturnSpillFile(base::File file);

  base::WeakPtrFactory<warc::WarcWriter> writer_weak_factory_;
  base::WeakPtrFactory<WarcRecorder> weak_factory_{this};
};

}  // namespace network

#endif  // SERVICES_NETWORK_WARC_RECORDER_H_
