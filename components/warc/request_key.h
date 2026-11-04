// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_WARC_REQUEST_KEY_H_
#define COMPONENTS_WARC_REQUEST_KEY_H_

#include <optional>
#include <string>
#include <string_view>

#include "url/gurl.h"

namespace warc {

// Returns the key an index files a request under: its URL's SURT for a GET,
// and for anything else the SURT of its URL with the method -- and for a POST
// or PUT, what it sent -- added to the query.
//
//   POST https://example.org/api {"q": "x"}
//     ->  org,example)/api?__wb_method=post&q=x
//
// A URL alone does not tell one POST from another. A page that sends a query
// in the body of its requests -- a GraphQL API, say -- sends every one of
// them to the same URL, and the archive holds an answer to each. So the index
// keys a request by what it sent as well as where, and a lookup has to put
// the request together the same way or it finds none of them.
//
// This follows cdxj-indexer, which writes the indexes this reads. A form is
// decoded into its fields; JSON, or plain text that parses as JSON, into its
// leaves, named by the key that holds them; anything else is base64 in a
// "__wb_post_data" field. `body` is null when the request's body is not known
// here -- a file being uploaded, say -- and then only the method is added,
// which matches no capture that sent something but leaves a near one to be
// found (see FindClosestCapture()).
//
// Two departures, both where cdxj-indexer reaches for what it has and this
// does not: a multipart body with a boundary is left out rather than split
// into its parts, and a JSON object read here has its keys in sorted order
// rather than as sent, which only changes which of two leaves with one name
// gets the plain name. The key sorts its query, so the order of the leaves is
// otherwise no matter.
std::string RequestKey(const GURL& url,
                       std::string_view method,
                       std::string_view content_type,
                       const std::optional<std::string>& body);

}  // namespace warc

#endif  // COMPONENTS_WARC_REQUEST_KEY_H_
