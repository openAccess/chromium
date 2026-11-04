// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_WARC_WACZ_COLLECTION_H_
#define COMPONENTS_WARC_WACZ_COLLECTION_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file.h"
#include "base/memory/scoped_refptr.h"
#include "components/warc/cdxj.h"
#include "components/warc/wacz_reader.h"
#include "net/http/http_response_headers.h"
#include "url/gurl.h"

namespace warc {

// An archived response, as it was stored.
struct ArchivedResponse {
  ArchivedResponse();
  ArchivedResponse(const ArchivedResponse&) = delete;
  ArchivedResponse& operator=(const ArchivedResponse&) = delete;
  ArchivedResponse(ArchivedResponse&&);
  ArchivedResponse& operator=(ArchivedResponse&&);
  ~ArchivedResponse();

  scoped_refptr<net::HttpResponseHeaders> headers;

  // The entity body, decoded. The archive stores what arrived on the wire --
  // a gzipped response is stored gzipped, which is what the digests cover --
  // but replay does not pass through the network service, so nothing between
  // here and the page would decode it. The headers are corrected to match, so
  // what a caller gets is what the page saw when it was captured.
  std::vector<uint8_t> body;

  // When this was captured, as the index records it.
  std::string timestamp;
};

// What a page asked for: as much of the request as decides which capture
// answers it.
struct ReplayRequest {
  ReplayRequest();
  ReplayRequest(const ReplayRequest&);
  ReplayRequest& operator=(const ReplayRequest&);
  ReplayRequest(ReplayRequest&&);
  ReplayRequest& operator=(ReplayRequest&&);
  ~ReplayRequest();

  GURL url;
  std::string method = "GET";

  // What a POST or PUT sent, and what it said it was. Null when the body is
  // not known here (see RequestKey()).
  std::string content_type;
  std::optional<std::string> body;

  // Whether a capture of the same resource asked for in other words may
  // answer when there is none of it (see FindClosestCapture()).
  //
  // Not for a page: a visit to a page the archive does not hold should land
  // on one saying so, not on whichever page lay nearest. What a page fetches
  // as it loads is another matter. Asked for the moment it was captured, it
  // would have had the answer stored; it misses only because its request
  // carries something new each time.
  bool accept_closest = false;
};

// The 14 digits an index keys a capture time by, from either that or the
// RFC 3339 a page list writes ("2026-09-10T20:13:54Z"). The two halves of a
// WACZ spell a time differently, so anything replaying a page from the page
// list has to pass its time through this or it will look for a capture at no
// time at all and be given the earliest one instead.
std::string ToIndexTimestamp(std::string_view time);

// One page a capture visited, from the list a WACZ carries.
struct PageEntry {
  std::string url;
  std::string timestamp;
  std::string title;
};

// The pages a WACZ says it captured, or an empty list if it names none or
// cannot be read.
//
// Separate from opening a collection because it is a smaller question than
// being ready to answer for one: this reads the container's directory and one
// small entry, where opening reads and sorts the whole index. It is also the
// question that comes first, since until a browser knows which of an
// archive's URLs were pages there is nothing for it to land on.
std::vector<PageEntry> ReadPageList(base::File file);

// A WACZ opened for reading: the index held in memory, the archives read
// where they lie.
//
// This is what a replay serves from. A request arrives, and the collection
// answers it out of the archive or says it has nothing -- which is the whole
// of replay, less the plumbing that carries the answer to a renderer.
class WaczCollection {
 public:
  WaczCollection(const WaczCollection&) = delete;
  WaczCollection& operator=(const WaczCollection&) = delete;

  WaczCollection(WaczCollection&&);
  WaczCollection& operator=(WaczCollection&&);

  ~WaczCollection();

  // Opens `file` and reads its index and page list. Returns nullopt if it is
  // not a container this can read, or holds no index.
  static std::optional<WaczCollection> Open(base::File file);

  // The response stored for `url` at the capture nearest `timestamp`, or
  // nullopt if the archive does not hold it.
  //
  // Nothing here falls back to the network, because there is nothing to fall
  // back to: a collection knows only what it holds. A caller that wants the
  // live web has to go and ask for it, deliberately and somewhere else.
  //
  // `timestamp` may be written either way a WACZ writes one; it is passed
  // through ToIndexTimestamp() so that a page's own time can be handed
  // straight back.
  //
  // A capture of the whole resource is preferred to a nearer capture of part
  // of it -- a 206 -- which is served only where there is nothing else.
  std::optional<ArchivedResponse> Lookup(const GURL& url,
                                         std::string_view timestamp);

  // As above, for a request by any method. A request other than a GET is
  // found by what it sent as well as its URL (see RequestKey()). A HEAD the
  // capture never made is answered by a GET of the same URL, whose headers
  // are what the HEAD would have been told; leaving out the body is for the
  // caller.
  std::optional<ArchivedResponse> Lookup(const ReplayRequest& request,
                                         std::string_view timestamp);

  // The pages the capture visited. Replay has to start somewhere, and this is
  // the only record of which URLs were pages rather than the images and
  // scripts that hung off them.
  const std::vector<PageEntry>& pages() const { return pages_; }

  // The moment a visit to `url` should start at: the time of the first page
  // the list names at `url` whose capture the site answered with a success.
  // Failing that, the first page at `url`; failing that, empty, which a lookup
  // takes as the earliest capture.
  //
  // Not simply the first page at `url`, because a capture lists every page
  // the browser landed on, and some of those were the site turning a visitor
  // away for a moment -- a bot check that answers 401 and reloads the same
  // URL once it is satisfied. Live, that is a step on the way in. Replayed,
  // the reload asks again at the same moment and gets the same 401, so a
  // visit begun there never gets in.
  std::string LandingMoment(const GURL& url) const;

  // Every entry in the index, sorted as the index is.
  const std::vector<CdxjEntry>& index() const { return index_; }

 private:
  WaczCollection();

  // The response `entry` points at, read out of its archive.
  std::optional<ArchivedResponse> Read(const CdxjEntry& entry);

  std::optional<WaczReader> reader_;
  std::vector<CdxjEntry> index_;
  std::vector<PageEntry> pages_;

  // The archives by the name the index calls them, so an entry's `filename`
  // finds the container entry holding it.
  std::map<std::string, const WaczReader::Entry*, std::less<>> archives_;
};

}  // namespace warc

#endif  // COMPONENTS_WARC_WACZ_COLLECTION_H_
