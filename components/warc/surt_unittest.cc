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

TEST(SurtTest, EscapesAreDecodedRatherThanKept) {
  // An escape is not a second way of writing a character, so a key that kept
  // one would file a resource under a name no lookup asks for. Leaving "%7C"
  // and "%2C" escaped is what made a replay of Wikipedia miss every
  // stylesheet: the index had the pipes written out and the lookup asked for
  // them escaped.
  const Case cases[] = {
      {"https://e.org/p?m=a%7Cb", "org,e)/p?m=a|b"},
      {"https://e.org/p?m=a%2Cb", "org,e)/p?m=a,b"},
      {"https://e.org/p?m=a%3Ab", "org,e)/p?m=a:b"},
      {"https://e.org/a%7Cb", "org,e)/a|b"},
      {"https://e.org/a%3Fb", "org,e)/a?b"},
      {"https://e.org/p?a=%2Fb", "org,e)/p?a=/b"},
      // An escaped "&" divides the query where the decoded URL divides it.
      {"https://e.org/p?z=%26a", "org,e)/p?a&z="},
      // Escaped again by a crawler, and still the same resource.
      {"https://e.org/%2561", "org,e)/a"},
      // What cannot be written plainly stays escaped.
      {"https://e.org/a%09b", "org,e)/a%09b"},
      {"https://e.org/a%23b", "org,e)/a%23b"},
      {"https://e.org/p?a=100%25", "org,e)/p?a=100%25"},
      {"https://e.org/p?x=%C3%BC", "org,e)/p?x=%c3%bc"},
      {"https://e.org/p?q=a+b", "org,e)/p?q=a+b"},
  };
  for (const Case& test : cases) {
    SCOPED_TRACE(test.url);
    EXPECT_EQ(test.key, ToSurt(GURL(test.url)));
  }
}

TEST(SurtTest, TheOneRemainingDepartureFromTheReference) {
  // A path that climbs above its own root. GURL resolves it away before this
  // code ever sees the URL, where the reference keeps it literally. Nothing a
  // browser requests looks like this, since a browser resolves a URL before
  // it asks for it.
  EXPECT_EQ("org,example)/x", ToSurt(GURL("http://example.org/../x")));
  // The reference would say "org,example)/../x".
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
