// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/cdxj.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
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

// The query of a key as its parameters, each with the values it was given,
// sorted. A name given alone has the empty value. The method is left out, as
// in KeyQuery.
using KeyParameters =
    std::map<std::string_view, std::vector<std::string_view>, std::less<>>;

KeyParameters ParseKeyParameters(std::string_view query) {
  KeyParameters parameters;
  for (std::string_view term : base::SplitStringPiece(
           query, "&", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    const size_t equals = term.find('=');
    const std::string_view name = term.substr(0, equals);
    if (name == kMethodName) {
      continue;
    }
    parameters[name].push_back(equals == std::string_view::npos
                                   ? std::string_view()
                                   : term.substr(equals + 1));
  }
  for (auto& [name, values] : parameters) {
    std::sort(values.begin(), values.end());
  }
  return parameters;
}

// What a value in a query is, as far as telling whether a load made it for
// itself goes. See FindCaptureIgnoringVisitValues().
enum class ValueKind {
  // Chosen by whoever wrote the URL, as far as can be told.
  kChosen,
  // Nothing: no value yet.
  kEmpty,
  kClock,
  kRandom,
  kId,
};

// How a page writes a value it does not have yet. Keys are lower-cased.
constexpr std::string_view kEmptyValues[] = {"",    "null", "undefined",
                                             "nan", "false", "0"};

// How far a reading of the clock may lie from the moment it was taken at.
// Replay starts a page's clock at its capture and lets it run, so a visit's
// readings drift from its moment as long as the visit lasts.
constexpr base::TimeDelta kClockReach = base::Days(1);

bool IsClockReading(std::string_view value, base::Time moment) {
  if (moment.is_null() || (value.size() != 10 && value.size() != 13)) {
    return false;
  }
  int64_t number = 0;
  if (!base::StringToInt64(value, &number) || number < 0) {
    return false;
  }
  const base::Time reading =
      value.size() == 13 ? base::Time::FromMillisecondsSinceUnixEpoch(number)
                         : base::Time::FromSecondsSinceUnixEpoch(number);
  return (reading - moment).magnitude() <= kClockReach;
}

bool IsRandomFraction(std::string_view value) {
  return value.size() >= 8 && value.starts_with("0.") &&
         std::ranges::all_of(value.substr(2),
                             [](char c) { return base::IsAsciiDigit(c); });
}

bool IsUuid(std::string_view text) {
  if (text.size() != 36) {
    return false;
  }
  for (size_t i = 0; i < text.size(); ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? text[i] != '-' : !base::IsHexDigit(text[i])) {
      return false;
    }
  }
  return true;
}

// Letters and digits both: sixteen hex digits that are all digits are a
// number -- an id a site counts out, chosen like any other -- and sixteen
// that are all letters are a word.
bool IsHexId(std::string_view text) {
  return text.size() >= 16 &&
         std::ranges::all_of(text, [](char c) { return base::IsHexDigit(c); }) &&
         std::ranges::any_of(text, [](char c) { return base::IsAsciiDigit(c); }) &&
         std::ranges::any_of(text, [](char c) { return base::IsAsciiAlpha(c); });
}

// An id alone, or ids among punctuation: "~<uuid>~~", as one consent library
// joins the ids it holds into one parameter.
bool IsMadeId(std::string_view value) {
  const auto is_part = [](char c) {
    return base::IsAsciiAlphaNumeric(c) || c == '-';
  };
  bool any = false;
  size_t start = 0;
  while (start < value.size()) {
    if (!is_part(value[start])) {
      ++start;
      continue;
    }
    size_t end = start;
    while (end < value.size() && is_part(value[end])) {
      ++end;
    }
    const std::string_view piece = value.substr(start, end - start);
    if (!IsUuid(piece) && !IsHexId(piece)) {
      return false;
    }
    any = true;
    start = end;
  }
  return any;
}

ValueKind KindOf(std::string_view value, base::Time moment) {
  if (std::ranges::find(kEmptyValues, value) != std::end(kEmptyValues)) {
    return ValueKind::kEmpty;
  }
  if (IsClockReading(value, moment)) {
    return ValueKind::kClock;
  }
  if (IsRandomFraction(value)) {
    return ValueKind::kRandom;
  }
  if (IsMadeId(value)) {
    return ValueKind::kId;
  }
  return ValueKind::kChosen;
}

// Whether the values one parameter was given in two queries -- none, where
// it was not given -- differ only as values a load makes for itself do, each
// judged by the moment its query was made at.
bool DifferOnlyAsVisitValues(base::span<const std::string_view> wanted,
                             base::Time wanted_moment,
                             base::span<const std::string_view> captured,
                             base::Time captured_moment) {
  if (std::ranges::equal(wanted, captured)) {
    return true;
  }
  // A parameter given several times is a list, and a list is chosen.
  if (wanted.size() > 1 || captured.size() > 1) {
    return false;
  }
  const ValueKind a =
      wanted.empty() ? ValueKind::kEmpty : KindOf(wanted[0], wanted_moment);
  const ValueKind b = captured.empty() ? ValueKind::kEmpty
                                       : KindOf(captured[0], captured_moment);
  if (a == ValueKind::kChosen || b == ValueKind::kChosen) {
    return false;
  }
  // Something made for the visit where the other had nothing yet.
  if (a == ValueKind::kEmpty || b == ValueKind::kEmpty) {
    return a != b;
  }
  // Another reading of the clock, another roll of the die -- but not another
  // id, which may name another thing.
  return a == b && a != ValueKind::kId;
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
  const size_t query_start = key.find('?');
  const std::string_view path = key.substr(0, query_start);
  const KeyQuery wanted = ParseKeyQuery(query_start == std::string_view::npos
                                            ? std::string_view()
                                            : key.substr(query_start + 1));

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

  ForEachCaptureOfPath(entries, key, [&](const CdxjEntry& entry) {
    consider(entry, entry.key.size() > path.size()
                        ? std::string_view(entry.key).substr(path.size() + 1)
                        : std::string_view());
  });
  return closest;
}

const CdxjEntry* FindCaptureIgnoringVisitValues(
    base::span<const CdxjEntry> entries,
    std::string_view key,
    std::string_view timestamp,
    base::FunctionRef<bool(const CdxjEntry&)> accept) {
  const size_t query_start = key.find('?');
  const std::string_view path = key.substr(0, query_start);
  const std::string method = KeyMethod(key);
  const KeyParameters wanted =
      ParseKeyParameters(query_start == std::string_view::npos
                             ? std::string_view()
                             : key.substr(query_start + 1));
  const base::Time target = ParseIndexTimestamp(timestamp);
  const auto values_of = [](const KeyParameters& parameters,
                            std::string_view name) {
    const auto it = parameters.find(name);
    return it == parameters.end() ? base::span<const std::string_view>()
                                  : base::span(it->second);
  };

  const CdxjEntry* found = nullptr;
  base::TimeDelta found_distance;
  ForEachCaptureOfPath(entries, key, [&](const CdxjEntry& entry) {
    if (!accept(entry) || KeyMethod(entry.key) != method) {
      return;
    }
    const KeyParameters captured = ParseKeyParameters(
        entry.key.size() > path.size()
            ? std::string_view(entry.key).substr(path.size() + 1)
            : std::string_view());
    const base::Time captured_at = ParseIndexTimestamp(entry.timestamp);
    for (const auto& [name, values] : wanted) {
      if (!DifferOnlyAsVisitValues(values, target, values_of(captured, name),
                                   captured_at)) {
        return;
      }
    }
    for (const auto& [name, values] : captured) {
      if (!wanted.contains(name) &&
          !DifferOnlyAsVisitValues({}, target, values, captured_at)) {
        return;
      }
    }
    const base::TimeDelta distance = (captured_at - target).magnitude();
    if (!found || distance < found_distance) {
      found = &entry;
      found_distance = distance;
    }
  });
  return found;
}

void ForEachCaptureOfPath(base::span<const CdxjEntry> entries,
                          std::string_view key,
                          base::FunctionRef<void(const CdxjEntry&)> visit) {
  // A key's path ends at its first "?". Its captures are those keyed by the
  // path alone and those keyed by the path and a query, which sort together
  // after it -- though not straight after: "/a-b" sorts between "/a" and
  // "/a?", so the two are found apart.
  const std::string_view path = key.substr(0, key.find('?'));
  const std::string with_query = base::StrCat({path, "?"});
  const auto by_key = [](const CdxjEntry& entry, std::string_view k) {
    return entry.key < k;
  };
  for (auto it = std::lower_bound(entries.begin(), entries.end(), path, by_key);
       it != entries.end() && it->key == path; ++it) {
    visit(*it);
  }
  for (auto it = std::lower_bound(entries.begin(), entries.end(),
                                  std::string_view(with_query), by_key);
       it != entries.end() && it->key.starts_with(with_query); ++it) {
    visit(*it);
  }
}

base::span<const CdxjEntry> CapturesOfHost(base::span<const CdxjEntry> entries,
                                           std::string_view key) {
  const size_t host_end = key.find(')');
  if (host_end == std::string_view::npos) {
    return {};
  }
  // Up to and including the ")", so that "com,example)" is not taken for the
  // start of "com,example,www)" or "com,examples)".
  const std::string_view host = key.substr(0, host_end + 1);
  const size_t begin = static_cast<size_t>(std::distance(
      entries.begin(),
      std::lower_bound(entries.begin(), entries.end(), host,
                       [](const CdxjEntry& entry, std::string_view k) {
                         return entry.key < k;
                       })));
  size_t end = begin;
  while (end < entries.size() && entries[end].key.starts_with(host)) {
    ++end;
  }
  return entries.subspan(begin, end - begin);
}

std::string KeyMethod(std::string_view key) {
  const size_t query_start = key.find('?');
  const std::string_view method =
      query_start == std::string_view::npos
          ? std::string_view()
          : ParseKeyQuery(key.substr(query_start + 1)).method;
  return method.empty() ? std::string("GET") : base::ToUpperASCII(method);
}

}  // namespace warc
