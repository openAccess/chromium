// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/surt.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"

namespace warc {

namespace {

// Decodes every escape, and keeps going until nothing changes.
//
// An escape is not a second way of writing a character -- "%41" is "A" -- so
// a key that kept them would file one resource under several names, and a
// lookup would miss what the archive holds. Repeatedly, because "%2561" is
// "%41" is "a", and a crawler that escaped an already-escaped URL should not
// thereby hide it.
std::string DecodeEscapes(std::string_view text) {
  std::string out(text);
  // Bounded: each pass strictly shortens the string, and four is far past any
  // depth a real URL is escaped to.
  for (int pass = 0; pass < 4; ++pass) {
    std::string next;
    next.reserve(out.size());
    bool changed = false;
    for (size_t i = 0; i < out.size(); ++i) {
      if (out[i] == '%' && i + 2 < out.size() && base::IsHexDigit(out[i + 1]) &&
          base::IsHexDigit(out[i + 2])) {
        next.push_back(static_cast<char>(base::HexDigitToInt(out[i + 1]) * 16 +
                                         base::HexDigitToInt(out[i + 2])));
        i += 2;
        changed = true;
        continue;
      }
      next.push_back(out[i]);
    }
    out = std::move(next);
    if (!changed) {
      break;
    }
  }
  return out;
}

// Escapes what cannot be written plainly: anything below a printable
// character or above ASCII, and the two characters that would otherwise be
// read as something other than themselves -- a "%" starting an escape, a "#"
// starting a fragment.
//
// Everything else stays literal, including the "|" and "," that a site's own
// URLs are full of. Leaving those escaped is what made a replay of Wikipedia
// miss every stylesheet: the index had them written out and the lookup asked
// for them escaped.
std::string EscapeForKey(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (unsigned char c : text) {
    if (c <= 0x20 || c >= 0x7F || c == '%' || c == '#') {
      base::StringAppendF(&out, "%%%02x", c);
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

// "www.example.org" and "www2.example.org" are the same site as
// "example.org" often enough that archives key them together.
std::string_view StripWwwPrefix(std::string_view host) {
  if (!base::StartsWith(host, "www", base::CompareCase::SENSITIVE)) {
    return host;
  }
  size_t i = 3;
  while (i < host.size() && base::IsAsciiDigit(host[i])) {
    ++i;
  }
  if (i < host.size() && host[i] == '.') {
    return host.substr(i + 1);
  }
  return host;
}

// "example.org" becomes "org,example", so a site's pages sort together and its
// subdomains sort beneath it. An IPv6 literal is left alone: its colons are
// not a hierarchy to reverse.
std::string ReverseHost(std::string_view host) {
  if (host.starts_with("[") && host.ends_with("]")) {
    return std::string(host.substr(1, host.size() - 2));
  }
  std::vector<std::string_view> labels = base::SplitStringPiece(
      host, ".", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  std::reverse(labels.begin(), labels.end());
  return base::JoinString(labels, ",");
}

// Sorted whole, not by name: two requests differing only in the order of their
// parameters are one request, and sorting "b=1&b=2" by the whole term keeps
// repeated names in a settled order too.
//
// Split after decoding rather than before, so that a parameter whose value
// held an escaped "&" divides where the decoded URL divides.
std::string NormalizeQuery(std::string_view query) {
  const std::string decoded = base::ToLowerASCII(DecodeEscapes(query));
  std::vector<std::string> terms = base::SplitString(
      decoded, "&", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  for (std::string& term : terms) {
    term = EscapeForKey(term);
  }
  std::sort(terms.begin(), terms.end());
  return base::JoinString(terms, "&");
}

}  // namespace

std::string ToSurt(const GURL& url) {
  if (!url.is_valid() || !url.has_host()) {
    return std::string();
  }

  std::string_view host = url.host();
  // A trailing dot names the same host, absolutely rather than relatively.
  if (host.ends_with(".")) {
    host.remove_suffix(1);
  }
  std::string key = ReverseHost(StripWwwPrefix(host));
  if (key.empty()) {
    return std::string();
  }

  // Only a port that the scheme does not imply, so that the default and the
  // absence of one are the same key.
  if (url.has_port() && url.EffectiveIntPort() != url::PORT_UNSPECIFIED &&
      url.IntPort() != url::PORT_UNSPECIFIED &&
      url.EffectiveIntPort() != url::DefaultPortForScheme(url.scheme())) {
    base::StrAppend(&key, {":", url.port()});
  }

  std::string path = base::ToLowerASCII(DecodeEscapes(url.path()));
  // An empty path segment names nothing, so "/a//b" and "/a/b" are one path.
  // Left in, they would be two keys for one page -- and a server that treats
  // them alike, which most do, would have the archive holding both.
  std::string collapsed;
  collapsed.reserve(path.size());
  for (char c : path) {
    if (c == '/' && !collapsed.empty() && collapsed.back() == '/') {
      continue;
    }
    collapsed.push_back(c);
  }
  path = std::move(collapsed);

  // A directory and the same directory named without its slash are one place.
  // The root is the exception: strip it and there is no path left to name.
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  if (path.empty()) {
    path = "/";
  }
  base::StrAppend(&key, {")", EscapeForKey(path)});

  if (url.has_query()) {
    const std::string query = NormalizeQuery(url.query());
    if (!query.empty()) {
      base::StrAppend(&key, {"?", query});
    }
  }

  // The fragment never reaches a server, so it cannot distinguish two
  // responses and has no place in a key that names one.
  return key;
}

}  // namespace warc
