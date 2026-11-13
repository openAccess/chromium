// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_WARC_CDXJ_H_
#define COMPONENTS_WARC_CDXJ_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/function_ref.h"
#include "base/time/time.h"

namespace warc {

// Reading a CDXJ index: what a web archive keeps instead of reading every
// record to find one.
//
// A line is a SURT key, a 14-digit capture time, and a JSON object saying
// where the record is and what it holds:
//
//   org,example)/a 20260910201354 {"url": "...", "offset": "594", ...}
//
// Sorted by that key and time, which is what makes a lookup a search rather
// than a scan, and what the SURT transform exists to arrange.

// One entry. Numbers arrive as JSON strings in the files these come from, and
// are parsed here so a caller does not have to know that.
struct CdxjEntry {
  CdxjEntry();
  CdxjEntry(const CdxjEntry&);
  CdxjEntry& operator=(const CdxjEntry&);
  CdxjEntry(CdxjEntry&&);
  CdxjEntry& operator=(CdxjEntry&&);
  ~CdxjEntry();

  // The SURT key and the capture time, as the line is sorted by.
  std::string key;
  std::string timestamp;

  // The URL as it was requested, which the key cannot be turned back into.
  std::string url;

  // Where the record is: which archive, and the bytes it occupies in it. For
  // a gzipped archive these describe the compressed member, so a reader seeks
  // to `offset` and inflates from there.
  std::string filename;
  uint64_t offset = 0;
  uint64_t length = 0;

  // What the record holds, enough to answer without reading it: the response
  // status, its media type, and the digest of the payload -- which is also
  // how two captures of an unchanged resource are recognised as one.
  std::string status;
  std::string mime;
  std::string digest;
};

// "20260910201354" -- an index's capture time -- as a time. Returns the null
// time for anything that is not 14 digits of a real date, which sorts such an
// entry furthest from any sensible request rather than nearest to it.
base::Time ParseIndexTimestamp(std::string_view timestamp);

// Parses one line. Returns nullopt for a line that is not an entry, including
// the "!meta" line that heads a secondary index.
std::optional<CdxjEntry> ParseCdxjLine(std::string_view line);

// Parses a whole index, skipping lines that are not entries. Blank lines and
// the "!meta" header are expected; anything else malformed is skipped too,
// since one unreadable line is no reason to lose the rest of an archive.
std::vector<CdxjEntry> ParseCdxj(std::string_view text);

// Returns the capture of `key` nearest `timestamp`, or null if the key is not
// in `entries`.
//
// Nearest rather than exact because replay asks for a page as it was at a
// moment, and the archive holds it as it was at the moments it was captured.
// A page and the images on it were captured seconds apart, so demanding an
// exact time would assemble a page out of nothing.
//
// `entries` must be sorted by key, as an index is.
const CdxjEntry* FindNearestCapture(base::span<const CdxjEntry> entries,
                                    std::string_view key,
                                    std::string_view timestamp);

// As above, among only the captures `accept` takes.
const CdxjEntry* FindNearestCapture(
    base::span<const CdxjEntry> entries,
    std::string_view key,
    std::string_view timestamp,
    base::FunctionRef<bool(const CdxjEntry&)> accept);

// Returns the capture that best stands in for `key` when there is none of it:
// a capture of the same URL path by the same method, whose query agrees with
// `key`'s most, and the nearest `timestamp` of those that agree as well. Null
// if the path was never captured by that method.
//
// A page asks again for much of what it asked for when it was captured, but
// not in quite the same words. A cache-buster, a time, an id made for the
// visit -- each load puts a new one in the query, so the request replay makes
// was never made before and an exact lookup finds nothing. The resource is
// there, filed under the query the capture used.
//
// Agreeing counts each parameter. One with the same value scores most, one
// with another value a little, since the two requests at least meant the
// same thing by it, and one on only one side counts against. Nothing is
// treated as a cache-buster by name: the parameters that differ between
// every capture weigh the same in all of them, and the ones that decide which
// resource was meant decide it.
//
// The method is part of the path for this. A GET is never answered with what
// a POST was told.
const CdxjEntry* FindClosestCapture(
    base::span<const CdxjEntry> entries,
    std::string_view key,
    std::string_view timestamp,
    base::FunctionRef<bool(const CdxjEntry&)> accept);

// Returns the capture of `key`'s URL path, by the same method, whose query is
// `key`'s own once the values each load makes anew are set aside -- the
// nearest `timestamp` of those. Null if there is none.
//
// For a request only a capture of its own URL may answer, a page or a frame,
// that a page still words anew each time: the consent frame a news site opens
// with an id it made for the visit, where the capture opened it before there
// was an id to give. FindClosestCapture() would answer that with whatever lay
// nearest, which for a page risks landing on the wrong one. This answers only
// with a capture that differs in values that cannot have chosen the page.
//
// A value counts as made for the visit when it is
//
//   - a reading of the clock: 10 or 13 digits, as seconds or milliseconds,
//     within a day of the moment asked for -- or, for a capture's value, of
//     when it was captured;
//   - a random fraction: "0." and six digits or more; or
//   - an id: a UUID, or 16 hex digits or more with both letters and digits,
//     alone or among punctuation, as a page strings its ids together.
//
// One is set aside where the other query has nothing in its place -- no such
// parameter, or "", "null", "undefined", "nan", "false" or "0" -- and a clock
// or a random number also where the other has another of its kind.
//
// Two ids are never taken for each other. Unlike a clock or a die, an id may
// be what chose the page: a site that fetches the parts of its pages by their
// UUIDs is asking for another part when it asks with another one.
const CdxjEntry* FindCaptureIgnoringVisitValues(
    base::span<const CdxjEntry> entries,
    std::string_view key,
    std::string_view timestamp,
    base::FunctionRef<bool(const CdxjEntry&)> accept);

// Calls `visit` with each capture of `key`'s URL path -- the part of a key
// before its query -- by any method and with any query or none, in index
// order. `entries` must be sorted by key.
//
// What an archive holds of a URL when it does not hold the URL itself: the
// same resource asked for in other words, or by another method.
void ForEachCaptureOfPath(base::span<const CdxjEntry> entries,
                          std::string_view key,
                          base::FunctionRef<void(const CdxjEntry&)> visit);

// The captures from `key`'s host -- the part of a SURT key up to its ")",
// which is the host and port -- or none if the key names no host. `entries`
// must be sorted by key, which puts a host's captures together.
base::span<const CdxjEntry> CapturesOfHost(base::span<const CdxjEntry> entries,
                                           std::string_view key);

// The method a key files its capture under, upper-cased: the one it names in
// its query, or "GET" for a key that names none, as a GET's never does.
std::string KeyMethod(std::string_view key);

}  // namespace warc

#endif  // COMPONENTS_WARC_CDXJ_H_
