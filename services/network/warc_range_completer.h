// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_NETWORK_WARC_RANGE_COMPLETER_H_
#define SERVICES_NETWORK_WARC_RANGE_COMPLETER_H_

#include <stddef.h>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/component_export.h"
#include "base/containers/circular_deque.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "net/base/isolation_info.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_request_headers.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace net {
class URLRequest;
class URLRequestContext;
}  // namespace net

namespace network {

class WarcRecorder;

// Fetches, in full, resources that a page only ever asked for ranges of.
//
// A media player reads a file's header, seeks to the index at its end, and
// abandons the rest; the bytes in between never cross the wire. No amount of
// stitching the stored partials can produce the complete resource, because
// those bytes do not exist anywhere -- so an archive of a page with video
// holds a few per cent of it. The only truthful way to get the whole resource
// is to fetch it, which this does as its own HTTP exchange, recorded like any
// other. Nothing in the resulting archive is synthesised.
//
// The partial responses stay in the archive as well. They are what the page
// actually received, they cost little beside the complete copy, and dropping
// them would discard evidence of what was served.
//
// Everything here runs on the network sequence.
class COMPONENT_EXPORT(NETWORK_SERVICE) WarcRangeCompleter {
 public:
  // Time without a single body byte after which a completion is abandoned and
  // its record marked "WARC-Truncated: time". Completion is deliberately not
  // bounded by size, so this is the only thing that ends a fetch which never
  // does -- a live stream, or a server that simply stops sending.
  static constexpr base::TimeDelta kStallTimeout = base::Seconds(30);

  // Completions run behind the page's own loads and compete with them for
  // bandwidth, so only a few are allowed to run at once; the rest wait.
  static constexpr size_t kMaxConcurrentFetches = 2;

  // `url_request_context` must outlive this object, which is why a
  // NetworkContext owns one of these and not the NetworkService: an in-flight
  // net::URLRequest may not outlive the context that created it.
  explicit WarcRangeCompleter(net::URLRequestContext* url_request_context);

  WarcRangeCompleter(const WarcRangeCompleter&) = delete;
  WarcRangeCompleter& operator=(const WarcRangeCompleter&) = delete;

  ~WarcRangeCompleter();

  // Schedules a complete fetch of whatever `partial` asked for a range of,
  // unless this session already has one. `recorder` mints the record; it is
  // passed per call rather than held because recording can stop while a fetch
  // is still in flight.
  void CompleteIfNeeded(const net::URLRequest& partial, WarcRecorder* recorder);

  size_t started_for_testing() const { return started_; }
  size_t finished_for_testing() const { return finished_; }
  size_t pending_for_testing() const { return pending_.size(); }

 private:
  class Fetch;

  // Everything needed to reissue the load as the page would have made it. The
  // originating URLRequest is gone by the time a queued completion starts, so
  // its context is copied rather than referenced.
  struct Queued {
    Queued();
    Queued(const Queued&);
    Queued& operator=(const Queued&);
    ~Queued();

    GURL url;
    net::HttpRequestHeaders headers;
    net::IsolationInfo isolation_info;
    net::SiteForCookies site_for_cookies;
    std::optional<url::Origin> initiator;

    // The completion belongs to the same browsing context as the ranged
    // requests that prompted it, so it is grouped with them in the archive.
    std::string browsing_context;
  };

  void MaybeStartNext();
  void OnFetchDone(Fetch* fetch);

  const raw_ptr<net::URLRequestContext> url_request_context_;

  // The session's recorder, refreshed on each call. Null while recording is
  // not active, which a queued completion has to tolerate: recording can stop
  // between a 206 arriving and its completion starting.
  raw_ptr<WarcRecorder> recorder_ = nullptr;

  // URLs already fetched, in flight, or queued. A page requests dozens of
  // ranges of the same file; it must only be fetched once.
  std::set<GURL> seen_;

  base::circular_deque<Queued> pending_;
  std::vector<std::unique_ptr<Fetch>> active_;

  size_t started_ = 0;
  size_t finished_ = 0;

  base::WeakPtrFactory<WarcRangeCompleter> weak_factory_{this};
};

}  // namespace network

#endif  // SERVICES_NETWORK_WARC_RANGE_COMPLETER_H_
