// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/request_key.h"

#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/json/json_reader.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "components/warc/surt.h"

namespace warc {

namespace {

// cdxj-indexer cuts what a body adds to the key at this many characters, so a
// key built from a longer body has to be cut at the same place to match.
constexpr size_t kMaxBodyQueryLength = 4096;

// Cuts `query` to `kMaxBodyQueryLength` characters -- characters, as Python
// counts them, so a field in any script is cut where the indexer cut it.
std::string Truncate(std::string query) {
  size_t characters = 0;
  for (size_t i = 0; i < query.size(); ++i) {
    // A byte that continues a UTF-8 sequence is not a character of its own.
    if ((static_cast<unsigned char>(query[i]) & 0xC0) == 0x80) {
      continue;
    }
    if (characters++ == kMaxBodyQueryLength) {
      query.resize(i);
      break;
    }
  }
  return query;
}

// Escapes as Python's quote_plus does, which is how the indexer writes a field
// it made out of JSON: letters, digits and "_.-~" plain, a space as "+", and
// every other byte as an escape.
std::string QuotePlus(std::string_view text) {
  std::string out;
  for (unsigned char c : text) {
    if (base::IsAsciiAlphaNumeric(c) || c == '_' || c == '.' || c == '-' ||
        c == '~') {
      out.push_back(static_cast<char>(c));
    } else if (c == ' ') {
      out.push_back('+');
    } else {
      base::StringAppendF(&out, "%%%02X", c);
    }
  }
  return out;
}

// A JSON leaf written as Python's str() writes it, which is how the indexer
// wrote it into the key.
std::string PythonStr(const base::Value& leaf) {
  switch (leaf.type()) {
    case base::Value::Type::STRING:
      return leaf.GetString();
    case base::Value::Type::BOOLEAN:
      return leaf.GetBool() ? "True" : "False";
    case base::Value::Type::NONE:
      return "None";
    case base::Value::Type::INTEGER:
      return base::NumberToString(leaf.GetInt());
    case base::Value::Type::DOUBLE: {
      // Python reads a whole number as an int whatever its size; this reader
      // keeps only those that fit in 32 bits as one, and the rest -- a time
      // in milliseconds, say -- arrive as doubles. Written as the integers
      // they were sent as, they match.
      const double number = leaf.GetDouble();
      if (std::isfinite(number) && number == std::trunc(number) &&
          std::fabs(number) < 1e18) {
        return base::NumberToString(static_cast<int64_t>(number));
      }
      return base::NumberToString(number);
    }
    default:
      return std::string();
  }
}

// The leaves of a parsed JSON body, each named by the key that holds it, as
// cdxj-indexer's json_parse() lists them. A name taken already gets a suffix,
// ".2_" for its second leaf and so on.
class JsonLeaves {
 public:
  void Add(const base::Value& value, const std::string& name) {
    if (value.is_dict()) {
      for (const auto [key, child] : value.GetDict()) {
        Add(child, key);
      }
    } else if (value.is_list()) {
      for (const base::Value& child : value.GetList()) {
        Add(child, name);
      }
    } else if (!name.empty()) {
      std::string key = name;
      if (names_.contains(key)) {
        // Counted from the name's first leaf, so its first repeat is ".2_".
        int& count = repeats_.try_emplace(name, 1).first->second;
        key = base::StringPrintf("%s.%d_", name.c_str(), ++count);
      }
      names_.insert(key);
      leaves_.emplace_back(std::move(key), PythonStr(value));
    }
  }

  // As Python's urlencode() writes them.
  std::string Encode() const {
    std::vector<std::string> fields;
    for (const auto& [key, value] : leaves_) {
      fields.push_back(base::StrCat({QuotePlus(key), "=", QuotePlus(value)}));
    }
    return base::JoinString(fields, "&");
  }

 private:
  std::vector<std::pair<std::string, std::string>> leaves_;
  std::set<std::string> names_;
  std::map<std::string, int> repeats_;
};

// The leaves of `body`, or nullopt if it is not JSON. A body of one JSON value
// per line is read line by line, as the indexer reads it.
std::optional<std::string> JsonQuery(std::string_view body) {
  JsonLeaves leaves;
  if (std::optional<base::Value> value =
          base::JSONReader::Read(body, base::JSON_PARSE_RFC)) {
    leaves.Add(*value, std::string());
    return leaves.Encode();
  }
  if (body.find('\n') == std::string_view::npos) {
    return std::nullopt;
  }
  for (std::string_view line : base::SplitStringPiece(
           body, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
    std::optional<base::Value> value =
        base::JSONReader::Read(line, base::JSON_PARSE_RFC);
    if (!value) {
      return std::nullopt;
    }
    leaves.Add(*value, std::string());
  }
  return leaves.Encode();
}

std::string Base64Query(std::string_view body) {
  return base::StrCat({"__wb_post_data=", base::Base64Encode(body)});
}

// What the indexer adds to the key for a body, before the key is made.
std::string BodyQuery(std::string_view content_type, std::string_view body) {
  const std::string type = base::ToLowerASCII(content_type);

  if (type.starts_with("application/x-www-form-urlencoded")) {
    if (!base::IsStringUTF8(body)) {
      return Base64Query(body);
    }
    // Decoded once, as the indexer decodes it, then escaped so that the key,
    // which decodes it again, reads the same fields. All but a "#": the
    // indexer puts the decoded form into a URL as it is, where a "#" begins a
    // fragment and the key loses the rest of the form. Kept, so it is lost
    // here too.
    std::string decoded = base::UnescapeBinaryURLComponent(
        body, base::UnescapeRule::REPLACE_PLUS_WITH_SPACE);
    decoded = Truncate(std::move(decoded));
    std::string escaped;
    for (unsigned char c : decoded) {
      if (base::IsAsciiAlphaNumeric(c) || c == '&' || c == '=' || c == '#') {
        escaped.push_back(static_cast<char>(c));
      } else {
        base::StringAppendF(&escaped, "%%%02X", c);
      }
    }
    return escaped;
  }
  if (type.starts_with("multipart/")) {
    // Without a boundary the parts cannot be told apart, and the indexer
    // keys the body as bytes, as here.
    return type.find("boundary=") == std::string::npos
               ? Truncate(Base64Query(body))
               : std::string();
  }
  if (type.starts_with("application/json")) {
    return Truncate(JsonQuery(body).value_or(std::string()));
  }
  if (type.starts_with("text/plain")) {
    std::optional<std::string> json = JsonQuery(body);
    return Truncate(json ? std::move(*json) : Base64Query(body));
  }
  return Truncate(Base64Query(body));
}

}  // namespace

std::string RequestKey(const GURL& url,
                       std::string_view method,
                       std::string_view content_type,
                       const std::optional<std::string>& body) {
  if (method == "GET") {
    return ToSurt(url);
  }

  std::string spec = url.GetWithoutRef().spec();
  base::StrAppend(&spec, {url.has_query() ? "&" : "?",
                          "__wb_method=", base::ToUpperASCII(method)});
  if ((method == "POST" || method == "PUT") && body) {
    const std::string query = BodyQuery(content_type, *body);
    if (!query.empty()) {
      base::StrAppend(&spec, {"&", query});
    }
  }
  return ToSurt(GURL(spec));
}

}  // namespace warc
