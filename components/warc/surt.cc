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

namespace warc {

namespace {

// Decodes the escapes that mean nothing: RFC 3986 calls ALPHA, DIGIT, "-",
// ".", "_" and "~" unreserved, so "%41" and "A" are the same character and an
// index must not hold two keys for them. Everything else is left as it stands,
// since decoding it could change where the path divides.
std::string DecodeUnreserved(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const int high = base::HexDigitToInt(text[i + 1]);
      const int low = base::HexDigitToInt(text[i + 2]);
      if (base::IsHexDigit(text[i + 1]) && base::IsHexDigit(text[i + 2])) {
        const char decoded = static_cast<char>(high * 16 + low);
        if (base::IsAsciiAlphaNumeric(decoded) || decoded == '-' ||
            decoded == '.' || decoded == '_' || decoded == '~') {
          out.push_back(decoded);
          i += 2;
          continue;
        }
      }
    }
    out.push_back(text[i]);
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
std::string SortQuery(std::string_view query) {
  std::vector<std::string> terms = base::SplitString(
      query, "&", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
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

  std::string path = base::ToLowerASCII(DecodeUnreserved(url.path()));
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
  base::StrAppend(&key, {")", path});

  if (url.has_query()) {
    const std::string query =
        SortQuery(base::ToLowerASCII(DecodeUnreserved(url.query())));
    if (!query.empty()) {
      base::StrAppend(&key, {"?", query});
    }
  }

  // The fragment never reaches a server, so it cannot distinguish two
  // responses and has no place in a key that names one.
  return key;
}

}  // namespace warc
