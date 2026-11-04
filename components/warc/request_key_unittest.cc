// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/request_key.h"

#include <optional>
#include <string>

#include "components/warc/surt.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace warc {
namespace {

std::string Key(const char* method,
                const char* url,
                const char* content_type,
                std::optional<std::string> body) {
  return RequestKey(GURL(url), method, content_type, body);
}

// Every expected key here is what cdxj-indexer 1.4.6 wrote for the same
// request (cdxj_indexer.postquery.append_method_query, then surt.surt()). A
// key that differs by a character finds nothing, so nothing less than the
// indexer's own output will do as the reference.

TEST(RequestKeyTest, AGetIsKeyedByItsUrlAlone) {
  EXPECT_EQ(ToSurt(GURL("https://example.org/a?b=2&a=1")),
            Key("GET", "https://example.org/a?b=2&a=1", "", std::nullopt));
}

TEST(RequestKeyTest, AnotherMethodIsNamedInTheKey) {
  EXPECT_EQ("org,example)/script.js?__wb_method=head",
            Key("HEAD", "https://example.org/script.js", "", std::nullopt));
}

TEST(RequestKeyTest, JsonIsKeyedByItsLeaves) {
  EXPECT_EQ(
      "org,example)/graphql?__wb_method=post&f=1.5&id=42&none=none&ok=true&"
      "query=q1&ts=1793401045886",
      Key("POST", "https://example.org/graphql", "application/json",
          R"({"query":"q1","variables":{"id":42,"ok":true,"none":null,)"
          R"("f":1.5,"ts":1793401045886}})"));
}

// A name used twice is told apart by a suffix, and the leaves join whatever
// query the URL already had.
TEST(RequestKeyTest, ARepeatedJsonNameIsNumbered) {
  EXPECT_EQ("org,example)/api?__wb_method=post&a=1&a.2_=2&c=3&c.2_=4&x=1",
            Key("POST", "https://example.org/api?x=1", "application/json",
                R"({"a":1,"b":{"a":2},"c":[3,4]})"));
}

TEST(RequestKeyTest, JsonOnePerLineIsKeyedByEveryLine) {
  EXPECT_EQ("org,example)/ndjson?__wb_method=post&e=1&e.2_=2",
            Key("POST", "https://example.org/ndjson", "application/json",
                "{\"e\":1}\n{\"e\":2}"));
}

TEST(RequestKeyTest, ASpaceInJsonIsWrittenAsAPlus) {
  EXPECT_EQ("org,example)/put?__wb_method=put&k=v+w",
            Key("PUT", "https://example.org/put", "application/json",
                R"({"k":"v w"})"));
}

// sendBeacon() sends a string as plain text, and what pages send that way is
// mostly JSON. It is keyed as JSON when it parses, and as bytes when not.
TEST(RequestKeyTest, PlainTextIsKeyedAsJsonWhenItIsJson) {
  EXPECT_EQ("org,example)/beacon?__wb_method=post&event=view&n=2",
            Key("POST", "https://example.org/beacon",
                "text/plain;charset=UTF-8", R"({"event":"view","n":2})"));
  EXPECT_EQ(
      "org,example)/"
      "beacon?__wb_method=post&__wb_post_data=bm90igpzb24gyxqgywxs",
      Key("POST", "https://example.org/beacon", "text/plain;charset=UTF-8",
          "not json at all"));
}

// Including the indexer's own accidents: a "%26" inside a field decodes to an
// "&" that splits it, and a "%23" to a "#" that ends the key there.
TEST(RequestKeyTest, AFormIsKeyedByItsFields) {
  EXPECT_EQ("org,example)/form?__wb_method=post&b&name=j%c3%b6rg%20m&q=&tags=a",
            Key("POST", "https://example.org/form",
                "application/x-www-form-urlencoded",
                "name=J%C3%B6rg+M&tags=a%26b&q=%23x"));
}

TEST(RequestKeyTest, AnythingElseIsKeyedByItsBytes) {
  EXPECT_EQ("org,example)/bin?__wb_method=post&__wb_post_data=aah/ymluyxj5",
            Key("POST", "https://example.org/bin", "application/octet-stream",
                std::string("\x00\x01\xff"
                            "binary",
                            9)));
}

// A beacon with nothing in it, of which a capture holds many.
TEST(RequestKeyTest, AnEmptyBodyIsKeyedAsEmpty) {
  EXPECT_EQ("org,example)/ping?__wb_method=post&__wb_post_data=",
            Key("POST", "https://example.org/ping", "", ""));
}

// With no boundary there are no parts to find, so the body is keyed as bytes.
TEST(RequestKeyTest, MultipartWithNoBoundaryIsKeyedByItsBytes) {
  EXPECT_EQ("org,example)/sync?__wb_method=post&__wb_post_data=",
            Key("POST", "https://example.org/sync", "multipart/form-data", ""));
}

TEST(RequestKeyTest, ABodyNotKnownIsLeftOut) {
  EXPECT_EQ(
      "org,example)/upload?__wb_method=post",
      Key("POST", "https://example.org/upload", "image/png", std::nullopt));
}

}  // namespace
}  // namespace warc
