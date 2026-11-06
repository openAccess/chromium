// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/cdxj.h"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "base/values.h"

namespace warc {

namespace {

// Reads a field that these files write as a JSON string but that means a
// number. Tolerates a real number too, since nothing says it must be a string.
uint64_t NumberField(const base::DictValue& dict, std::string_view name) {
  if (const std::string* text = dict.FindString(name)) {
    uint64_t value = 0;
    return base::StringToUint64(*text, &value) ? value : 0;
  }
  if (std::optional<double> number = dict.FindDouble(name);
      number.has_value() && *number >= 0) {
    return static_cast<uint64_t>(*number);
  }
  return 0;
}

std::string StringField(const base::DictValue& dict, std::string_view name) {
  const std::string* value = dict.FindString(name);
  return value ? *value : std::string();
}

// The query of a key, as the terms a closest capture is judged by. The method
// is not one of them: it decides which captures are in the running at all.
struct KeyQuery {
  std::string_view method;
  std::set<std::string_view> terms;
  std::set<std::string_view> names;
};

constexpr std::string_view kMethodName = "__wb_method";

KeyQuery ParseKeyQuery(std::string_view query) {
  KeyQuery parsed;
  for (std::string_view term : base::SplitStringPiece(
           query, "&", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    const std::string_view name = term.substr(0, term.find('='));
    if (name == kMethodName) {
      parsed.method = term.substr(std::min(term.size(), name.size() + 1));
      continue;
    }
    parsed.terms.insert(term);
    parsed.names.insert(name);
  }
  return parsed;
}

// How well `candidate` stands in for `wanted`. See FindClosestCapture().
int Agreement(const KeyQuery& wanted, const KeyQuery& candidate) {
  int score = 0;
  for (std::string_view term : wanted.terms) {
    const std::string_view name = term.substr(0, term.find('='));
    if (candidate.terms.contains(term)) {
      score += 2;
    } else if (candidate.names.contains(name)) {
      score += 1;
    } else {
      score -= 1;
    }
  }
  for (std::string_view name : candidate.names) {
    if (!wanted.names.contains(name)) {
      score -= 1;
    }
  }
  return score;
}

}  // namespace

base::Time ParseIndexTimestamp(std::string_view timestamp) {
  base::Time::Exploded exploded = {};
  if (timestamp.size() != 14 ||
      !base::StringToInt(timestamp.substr(0, 4), &exploded.year) ||
      !base::StringToInt(timestamp.substr(4, 2), &exploded.month) ||
      !base::StringToInt(timestamp.substr(6, 2), &exploded.day_of_month) ||
      !base::StringToInt(timestamp.substr(8, 2), &exploded.hour) ||
      !base::StringToInt(timestamp.substr(10, 2), &exploded.minute) ||
      !base::StringToInt(timestamp.substr(12, 2), &exploded.second)) {
    return base::Time();
  }
  base::Time parsed;
  if (!base::Time::FromUTCExploded(exploded, &parsed)) {
    return base::Time();
  }
  return parsed;
}

CdxjEntry::CdxjEntry() = default;
CdxjEntry::CdxjEntry(const CdxjEntry&) = default;
CdxjEntry& CdxjEntry::operator=(const CdxjEntry&) = default;
CdxjEntry::CdxjEntry(CdxjEntry&&) = default;
CdxjEntry& CdxjEntry::operator=(CdxjEntry&&) = default;
CdxjEntry::~CdxjEntry() = default;

std::optional<CdxjEntry> ParseCdxjLine(std::string_view line) {
  line = base::TrimWhitespaceASCII(line, base::TRIM_ALL);
  // "!meta" heads a secondary index and describes the file rather than a
  // record in it.
  if (line.empty() || line.starts_with("!")) {
    return std::nullopt;
  }

  const size_t key_end = line.find(' ');
  if (key_end == std::string_view::npos) {
    return std::nullopt;
  }
  const size_t timestamp_end = line.find(' ', key_end + 1);
  if (timestamp_end == std::string_view::npos) {
    return std::nullopt;
  }

  CdxjEntry entry;
  entry.key = std::string(line.substr(0, key_end));
  entry.timestamp =
      std::string(line.substr(key_end + 1, timestamp_end - key_end - 1));
  if (entry.key.empty() || entry.timestamp.empty()) {
    return std::nullopt;
  }

  // Strict JSON: these files come from other tools, and guessing at what a
  // malformed one meant is how a reader ends up seeking to the wrong offset.
  std::optional<base::DictValue> json = base::JSONReader::ReadDict(
      line.substr(timestamp_end + 1), base::JSON_PARSE_RFC);
  if (!json) {
    return std::nullopt;
  }
  const base::DictValue& dict = *json;

  entry.url = StringField(dict, "url");
  entry.filename = StringField(dict, "filename");
  entry.status = StringField(dict, "status");
  entry.mime = StringField(dict, "mime");
  entry.digest = StringField(dict, "digest");
  entry.offset = NumberField(dict, "offset");
  entry.length = NumberField(dict, "length");
  return entry;
}

std::vector<CdxjEntry> ParseCdxj(std::string_view text) {
  std::vector<CdxjEntry> entries;
  for (std::string_view line : base::SplitStringPiece(
           text, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (std::optional<CdxjEntry> entry = ParseCdxjLine(line)) {
      entries.push_back(std::move(*entry));
    }
  }
  return entries;
}

const CdxjEntry* FindNearestCapture(base::span<const CdxjEntry> entries,
                                    std::string_view key,
                                    std::string_view timestamp) {
  return FindNearestCapture(entries, key, timestamp,
                            [](const CdxjEntry&) { return true; });
}

const CdxjEntry* FindNearestCapture(
    base::span<const CdxjEntry> entries,
    std::string_view key,
    std::string_view timestamp,
    base::FunctionRef<bool(const CdxjEntry&)> accept) {
  // The index is sorted by key, so the captures of one URL are a run within
  // it and a search finds where that run begins.
  const auto begin = std::lower_bound(
      entries.begin(), entries.end(), key,
      [](const CdxjEntry& entry, std::string_view k) { return entry.key < k; });

  const base::Time target = ParseIndexTimestamp(timestamp);

  const CdxjEntry* nearest = nullptr;
  base::TimeDelta nearest_distance;
  for (auto it = begin; it != entries.end() && it->key == key; ++it) {
    if (!accept(*it)) {
      continue;
    }
    // Compared as times rather than as the digits they are written in. Read
    // as numbers, a capture a second before midnight looks further from one
    // just after it than a capture a minute later does, and an overnight
    // crawl would hand back the wrong one.
    const base::TimeDelta distance =
        (ParseIndexTimestamp(it->timestamp) - target).magnitude();
    if (!nearest || distance < nearest_distance) {
      nearest = &*it;
      nearest_distance = distance;
    }
  }
  return nearest;
}

const CdxjEntry* FindClosestCapture(
    base::span<const CdxjEntry> entries,
    std::string_view key,
    std::string_view timestamp,
    base::FunctionRef<bool(const CdxjEntry&)> accept) {
  // A key's path ends at its first "?". Its captures are those keyed by the
  // path alone and those keyed by the path and a query, which sort together
  // after it -- though not straight after: "/a-b" sorts between "/a" and
  // "/a?", so the two are found apart.
  const size_t query_start = key.find('?');
  const std::string_view path = key.substr(0, query_start);
  const KeyQuery wanted = ParseKeyQuery(query_start == std::string_view::npos
                                            ? std::string_view()
                                            : key.substr(query_start + 1));
  const std::string with_query = base::StrCat({path, "?"});

  const base::Time target = ParseIndexTimestamp(timestamp);
  const CdxjEntry* closest = nullptr;
  int closest_score = 0;
  base::TimeDelta closest_distance;
  auto consider = [&](const CdxjEntry& entry, std::string_view query) {
    if (!accept(entry)) {
      return;
    }
    const KeyQuery candidate = ParseKeyQuery(query);
    if (candidate.method != wanted.method) {
      return;
    }
    const int score = Agreement(wanted, candidate);
    const base::TimeDelta distance =
        (ParseIndexTimestamp(entry.timestamp) - target).magnitude();
    if (!closest || score > closest_score ||
        (score == closest_score && distance < closest_distance)) {
      closest = &entry;
      closest_score = score;
      closest_distance = distance;
    }
  };

  const auto by_key = [](const CdxjEntry& entry, std::string_view k) {
    return entry.key < k;
  };
  for (auto it = std::lower_bound(entries.begin(), entries.end(), path, by_key);
       it != entries.end() && it->key == path; ++it) {
    consider(*it, std::string_view());
  }
  for (auto it = std::lower_bound(entries.begin(), entries.end(),
                                  std::string_view(with_query), by_key);
       it != entries.end() && it->key.starts_with(with_query); ++it) {
    consider(*it, std::string_view(it->key).substr(with_query.size()));
  }
  return closest;
}

}  // namespace warc
