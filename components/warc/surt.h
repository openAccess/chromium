// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_WARC_SURT_H_
#define COMPONENTS_WARC_SURT_H_

#include <string>

#include "url/gurl.h"

namespace warc {

// Returns the SURT (Sort-friendly URI Reordering Transform) key for `url`:
//
//   https://www.Example.org:443/A/b/?b=2&a=1#frag  ->  org,example)/a/b?a=1&b=2
//
// This is how a web archive index is keyed, and the reason is sorting. With
// the host reversed, every page of a site sorts together and every subdomain
// sorts beneath it, so a lookup is a search in a sorted file rather than a
// scan. Nothing else in the transform matters as much as being the same
// transform everyone else uses: an index is written by one tool and read by
// another, and a key that differs by a character finds nothing at all.
//
// The shape is fixed by convention rather than by a standard, so this follows
// the `surt` Python library that cdxj-indexer and pywb use, which is what
// writes the indexes in practice. The scheme is dropped, so http and https
// share a key; userinfo and fragment go; a "www." or "www2." prefix goes; the
// host is lowercased, split on dots and reversed; a non-default port follows
// it; the path is lowercased with a trailing slash removed; and query
// parameters are lowercased and sorted, which makes two orderings of the same
// request one key.
//
// Returns an empty string for a URL with no host to reverse.
//
// Escapes are decoded rather than kept, and kept decoded: an escape is not a
// second way of writing a character, and a key that treated it as one would
// file a resource under a name that no lookup asks for. Only what cannot be
// written plainly is escaped again.
//
// One known departure from that library, on a URL a browser does not produce:
// a path climbing above its own root ("/../x") is resolved away here, because
// GURL resolves it before this code sees it, where the reference keeps it
// literally. A browser resolves a URL before it asks for it, so replay does
// not meet this; an index built by another crawler could hold one.
std::string ToSurt(const GURL& url);

}  // namespace warc

#endif  // COMPONENTS_WARC_SURT_H_
