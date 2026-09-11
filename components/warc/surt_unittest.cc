// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/surt.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace warc {
namespace {

// Every expectation here was taken from the `surt` Python library that
// cdxj-indexer and pywb use, rather than from reading the convention and
// guessing at it. An index written by those tools has to be readable by this
// one, so where they disagree they are right by definition.
struct Case {
  const char* url;
  const char* key;
};

TEST(SurtTest, MatchesTheReferenceImplementation) {
  const Case cases[] = {
      // The shape.
      {"http://example.org/", "org,example)/"},
      {"http://example.org", "org,example)/"},
      {"http://sub.domain.example.co.uk/x", "uk,co,example,domain,sub)/x"},

      // http and https share a key: an archive holds the exchange, and which
      // scheme carried it does not make it a different page.
      {"https://example.org/", "org,example)/"},
      {"ftp://example.org/f", "org,example)/f"},

      // Ports the scheme already implies are not worth keying on.
      {"http://example.org:80/p", "org,example)/p"},
      {"https://example.org:443/p", "org,example)/p"},
      {"http://example.org:8080/p", "org,example:8080)/p"},
      {"https://example.org:8443/x", "org,example:8443)/x"},

      // "www" and a site are the same site; "wwwfoo" is not.
      {"http://www.example.org/path", "org,example)/path"},
      {"http://www2.example.org/x", "org,example)/x"},
      {"http://wwwfoo.example.org/x", "org,example,wwwfoo)/x"},
      {"http://www.www.example.org/x", "org,example,www)/x"},

      // Case, trailing dots, userinfo and fragments name nothing.
      {"http://WWW.Example.ORG/Path", "org,example)/path"},
      {"http://example.org/UPPER/Case.HTML", "org,example)/upper/case.html"},
      {"http://EXAMPLE.org./x", "org,example)/x"},
      {"http://user:pw@example.org/p", "org,example)/p"},
      {"http://example.org/p#frag", "org,example)/p"},

      // A directory with and without its slash is one place, but the root
      // keeps the only path it has.
      {"http://example.org/a/b/", "org,example)/a/b"},
      {"http://example.org/a/b//", "org,example)/a/b"},
      {"http://example.org//", "org,example)/"},

      // Queries: sorted whole and lowercased, so that two orderings of one
      // request are one key. An empty query is no query.
      {"http://example.org/a/b?b=2&a=1", "org,example)/a/b?a=1&b=2"},
      {"http://example.org/p?Q=Value&B=2", "org,example)/p?b=2&q=value"},
      {"http://example.org/x?b=2&b=1&a=3", "org,example)/x?a=3&b=1&b=2"},
      {"http://example.org/p?a=1&a=2", "org,example)/p?a=1&a=2"},
      {"http://example.org/p?", "org,example)/p"},
      {"http://example.org/p?a=", "org,example)/p?a="},
      {"http://example.org?q=1", "org,example)/?q=1"},

      // Escapes that stand for an ordinary character are not a second way of
      // writing it.
      {"http://example.org/%41", "org,example)/a"},
      {"http://example.org/%7Euser/", "org,example)/~user"},

      // Anything else keeps its escaping, and non-ASCII arrives already
      // escaped as UTF-8.
      {"http://example.org/a b", "org,example)/a%20b"},
      {"http://example.org/a+b", "org,example)/a+b"},
      {"http://example.org/ümlaut", "org,example)/%c3%bcmlaut"},
      {"http://example.org/p;params", "org,example)/p;params"},

      // An address is a host like any other, but an IPv6 literal has no
      // hierarchy to reverse.
      {"http://127.0.0.1:8771/index.html", "1,0,0,127:8771)/index.html"},
      {"http://[2001:db8::1]/p", "2001:db8::1)/p"},
      {"http://[2001:db8::1]:8080/p", "2001:db8::1:8080)/p"},

      // Paths that name their way back out.
      {"http://example.org/./x", "org,example)/x"},
      {"http://example.org/a//b/../c", "org,example)/a/c"},
  };

  for (const Case& test : cases) {
    SCOPED_TRACE(test.url);
    EXPECT_EQ(test.key, ToSurt(GURL(test.url)));
  }
}

TEST(SurtTest, KnownDeparturesFromTheReference) {
  // Recorded rather than fixed. Each of these is a URL a browser resolves and
  // escapes before it ever reaches the network, so replay does not meet them;
  // an index built by another crawler could hold one, and then these are the
  // keys it would be filed under and this is where to start.
  const struct {
    const char* url;
    const char* ours;
    const char* reference;
  } cases[] = {
      // The reference reads an escaped slash as a path separator.
      {"http://example.org/a%2Fb", "org,example)/a%2fb", "org,example)/a/b"},
      // The reference keeps a path that climbs above the root.
      {"http://example.org/../x", "org,example)/x", "org,example)/../x"},
      // The reference unescapes until nothing changes, so "%2561" is "a".
      {"http://example.org/%2561", "org,example)/%2561", "org,example)/a"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.url);
    EXPECT_EQ(test.ours, ToSurt(GURL(test.url)));
    EXPECT_STRNE(test.ours, test.reference);
  }
}

TEST(SurtTest, UrlsWithNoHostHaveNoKey) {
  // Nothing an archive holds is addressed this way, and a key without a host
  // would sort among the sites as though it were one.
  for (const char* url : {"", "not a url", "about:blank", "data:text/html,hi",
                          "file:///tmp/x", "javascript:void(0)"}) {
    SCOPED_TRACE(url);
    EXPECT_EQ("", ToSurt(GURL(url)));
  }
}

TEST(SurtTest, SchemeAndPortAgreeOnOneKey) {
  // The property the whole transform exists for: requests that name the same
  // resource have to arrive at the same key, or replay looks for a page the
  // index filed under another name.
  const GURL expected("https://www.example.org/A/?b=2&a=1");
  for (const char* url : {
           "https://www.example.org/A/?b=2&a=1",
           "https://www.example.org:443/a?a=1&b=2",
           "http://www.Example.org/a/?A=1&B=2",
           "https://example.org/a?b=2&a=1#top",
       }) {
    SCOPED_TRACE(url);
    EXPECT_EQ(ToSurt(expected), ToSurt(GURL(url)));
  }
}

}  // namespace
}  // namespace warc
