// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/side_panel/archive/archive_agent_queries.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <map>
#include <string_view>
#include <vector>

#include "base/functional/bind.h"
#include "base/i18n/time_formatting.h"
#include "base/json/json_writer.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/time/time.h"
#include "chrome/browser/wacz_replay/replay_log.h"
#include "chrome/browser/wacz_replay/replay_partition.h"
#include "chrome/browser/warc_tab_capture.h"
#include "components/warc/cdxj.h"
#include "components/warc/surt.h"
#include "components/warc/wacz_collection.h"
#include "components/warc/wacz_url_loader_factory.h"
#include "content/public/browser/web_contents.h"
#include "net/base/net_errors.h"
#include "url/gurl.h"

namespace {

// The most requests list_requests returns at once, and the number it returns
// when not told.
constexpr int kMaxListed = 200;
constexpr int kDefaultListed = 50;

// The most hosts summarize_requests names.
constexpr size_t kMaxHosts = 40;

// One request, as either a capture or a replay tells of it.
struct Row {
  size_t number = 0;
  std::string method;
  GURL url;
  int status = 0;
  std::string mime_type;
  std::string destination;
  uint64_t bytes = 0;
  bool truncated = false;
  bool missing = false;
  std::string error;
  GURL answered_by;
  base::Time captured;
  base::Time time;
  warc::MissExplanation miss;

  bool failed() const { return !error.empty(); }
  bool is_error() const {
    return missing || failed() || status == 0 || status >= 400;
  }
};

// The tab's requests, oldest first, as many as are kept.
std::vector<Row> RowsOf(content::WebContents* contents) {
  std::vector<Row> rows;
  if (!contents) {
    return rows;
  }
  if (ReplayLog* log = ReplayLog::FromWebContents(contents)) {
    size_t number = log->request_count() - log->requests().size();
    for (const warc::ReplayOutcome& request : log->requests()) {
      Row& row = rows.emplace_back();
      row.number = ++number;
      row.method = request.method;
      row.url = request.url;
      row.status = request.status_code;
      row.mime_type = request.mime_type;
      row.destination = request.destination;
      row.bytes = request.body_bytes;
      row.missing = request.missing;
      if (request.net_error != net::OK) {
        row.error = net::ErrorToShortString(request.net_error);
      }
      row.answered_by = request.captured_url;
      row.captured = request.captured;
      row.time = request.time;
      row.miss = request.miss;
    }
    return rows;
  }
  if (WarcTabCapture* capture = WarcTabCapture::FromWebContents(contents)) {
    size_t number = capture->exchange_count() - capture->exchanges().size();
    for (const WarcTabCapture::Exchange& exchange : capture->exchanges()) {
      Row& row = rows.emplace_back();
      row.number = ++number;
      row.method = exchange.method;
      row.url = exchange.url;
      row.status = exchange.status_code;
      row.mime_type = exchange.mime_type;
      row.bytes = exchange.body_bytes;
      row.truncated = exchange.truncated;
      row.time = exchange.time;
    }
  }
  return rows;
}

// What a model is told a miss means; see the system prompt.
std::string_view MissReasonName(warc::MissReason reason) {
  switch (reason) {
    case warc::MissReason::kUnknown:
      return "unknown";
    case warc::MissReason::kHostNotCaptured:
      return "host_never_captured";
    case warc::MissReason::kPathNotCaptured:
      return "url_never_captured";
    case warc::MissReason::kOtherQuery:
      return "other_query_only";
    case warc::MissReason::kOtherMethod:
      return "other_method_only";
    case warc::MissReason::kRangeNotHeld:
      return "range_not_held";
    case warc::MissReason::kUnreadable:
      return "unreadable";
  }
  return "unknown";
}

// An index's 14-digit time as ISO 8601, or as it was written if it is not one.
std::string IndexTimeAsIso8601(const std::string& timestamp) {
  const base::Time time = warc::ParseIndexTimestamp(timestamp);
  return time.is_null() ? timestamp : base::TimeFormatAsIso8601(time);
}

base::DictValue ToDict(const Row& row) {
  base::DictValue dict;
  dict.Set("n", base::checked_cast<int>(row.number));
  dict.Set("method", row.method);
  dict.Set("url", row.url.spec());
  if (row.status) {
    dict.Set("status", row.status);
  }
  if (!row.mime_type.empty()) {
    dict.Set("type", row.mime_type);
  }
  if (!row.destination.empty()) {
    dict.Set("for", row.destination);
  }
  if (!row.missing && !row.failed()) {
    dict.Set("bytes", static_cast<double>(row.bytes));
  }
  if (row.truncated) {
    dict.Set("truncated", true);
  }
  if (row.missing) {
    dict.Set("missing", true);
    dict.Set("why_missing", MissReasonName(row.miss.reason));
    if (!row.miss.url.empty()) {
      base::DictValue nearest;
      nearest.Set("url", row.miss.url);
      nearest.Set("method", row.miss.method);
      nearest.Set("captured_at", IndexTimeAsIso8601(row.miss.timestamp));
      dict.Set("nearest_capture", std::move(nearest));
    }
  }
  if (row.failed()) {
    dict.Set("error", row.error);
  }
  if (!row.answered_by.is_empty()) {
    dict.Set("answered_by", row.answered_by.spec());
  }
  if (!row.captured.is_null()) {
    dict.Set("captured_at", base::TimeFormatAsIso8601(row.captured));
  }
  dict.Set("at", base::TimeFormatAsIso8601(row.time));
  return dict;
}

std::string ToJson(const base::DictValue& dict) {
  return base::WriteJson(dict).value_or("{}");
}

std::string Error(const std::string& message) {
  base::DictValue error;
  error.Set("error", message);
  return ToJson(error);
}

std::string TabStatus(content::WebContents* contents) {
  base::DictValue status;
  if (!contents) {
    return Error("The panel has no tab.");
  }
  base::DictValue page;
  page.Set("url", contents->GetLastCommittedURL().spec());
  page.Set("title", base::UTF16ToUTF8(contents->GetTitle()));
  status.Set("page", std::move(page));

  const std::vector<Row> rows = RowsOf(contents);
  if (ReplayLog::FromWebContents(contents) ||
      wacz_replay::IsReplayTab(contents)) {
    status.Set("mode", "replaying");
    if (ReplayLog* log = ReplayLog::FromWebContents(contents)) {
      status.Set("archive", log->archive().AsUTF8Unsafe());
      if (!log->moment().is_null()) {
        status.Set("replaying_as_of", base::TimeFormatAsIso8601(log->moment()));
      }
      status.Set("requests", base::checked_cast<int>(log->request_count()));
      status.Set("missing", base::checked_cast<int>(log->missing_count()));
      status.Set("bytes", static_cast<double>(log->body_bytes()));
    }
  } else if (WarcTabCapture* capture =
                 WarcTabCapture::FromWebContents(contents)) {
    status.Set("mode", capture->started() ? "archiving" : "starting");
    status.Set("archive_directory", capture->directory().AsUTF8Unsafe());
    status.Set("requests", base::checked_cast<int>(capture->exchange_count()));
    status.Set("bytes", static_cast<double>(capture->body_bytes()));
  } else {
    status.Set("mode", "not archiving");
    status.Set("note",
               "The tab is neither being archived nor replaying an archive, so "
               "there are no requests to look at.");
  }
  status.Set("failed", base::checked_cast<int>(std::ranges::count_if(
                           rows, [](const Row& row) { return row.failed(); })));
  status.Set("errors",
             base::checked_cast<int>(std::ranges::count_if(
                 rows, [](const Row& row) { return row.is_error(); })));
  status.Set("listable", base::checked_cast<int>(rows.size()));
  return ToJson(status);
}

std::string ListRequests(content::WebContents* contents,
                         const base::DictValue& params) {
  const std::string* which_param = params.FindString("which");
  const std::string which = which_param ? *which_param : "all";
  if (which != "all" && which != "missing" && which != "failed" &&
      which != "errors" && which != "closest") {
    return Error("Unknown 'which': " + which);
  }
  const std::string* contains = params.FindString("contains");
  const std::string needle =
      contains ? base::ToLowerASCII(*contains) : std::string();
  const int limit = std::clamp(params.FindInt("limit").value_or(kDefaultListed),
                               1, kMaxListed);
  const bool newest_first = params.FindBool("newest_first").value_or(false);

  std::vector<Row> rows = RowsOf(contents);
  std::erase_if(rows, [&](const Row& row) {
    if (which == "missing" && !row.missing) {
      return true;
    }
    if (which == "failed" && !row.failed()) {
      return true;
    }
    if (which == "errors" && !row.is_error()) {
      return true;
    }
    if (which == "closest" && row.answered_by.is_empty()) {
      return true;
    }
    return !needle.empty() &&
           base::ToLowerASCII(row.url.spec()).find(needle) == std::string::npos;
  });
  if (newest_first) {
    std::ranges::reverse(rows);
  }

  base::ListValue listed;
  for (const Row& row : rows) {
    if (static_cast<int>(listed.size()) == limit) {
      break;
    }
    listed.Append(ToDict(row));
  }
  base::DictValue result;
  result.Set("matching", base::checked_cast<int>(rows.size()));
  result.Set("returned", base::checked_cast<int>(listed.size()));
  result.Set("requests", std::move(listed));
  return ToJson(result);
}

std::string SummarizeRequests(content::WebContents* contents) {
  struct Counts {
    int requests = 0;
    int missing = 0;
    int failed = 0;
    int errors = 0;
    double bytes = 0;
  };
  std::map<std::string, Counts> by_host;
  std::map<std::string, int> by_type;
  std::map<std::string, int> by_status;
  std::map<std::string_view, int> missing_by_reason;
  const std::vector<Row> rows = RowsOf(contents);
  for (const Row& row : rows) {
    Counts& host = by_host[row.url.GetHost()];
    ++host.requests;
    host.missing += row.missing;
    host.failed += row.failed();
    host.errors += row.is_error();
    host.bytes += row.bytes;

    ++by_type[!row.mime_type.empty() ? row.mime_type
              : !row.destination.empty()
                  ? "(none; for " + row.destination + ")"
                  : "(none)"];
    ++by_status[row.missing    ? "missing"
                 : row.failed() ? "failed: " + row.error
                 : row.status   ? base::NumberToString(row.status)
                                : "no response"];
    if (row.missing) {
      ++missing_by_reason[MissReasonName(row.miss.reason)];
    }
  }

  std::vector<std::pair<std::string, Counts>> hosts(by_host.begin(),
                                                    by_host.end());
  std::ranges::stable_sort(hosts, [](const auto& a, const auto& b) {
    return a.second.requests > b.second.requests;
  });
  base::ListValue host_list;
  for (const auto& [host, counts] : hosts) {
    if (host_list.size() == kMaxHosts) {
      break;
    }
    base::DictValue entry;
    entry.Set("host", host);
    entry.Set("requests", counts.requests);
    entry.Set("missing", counts.missing);
    entry.Set("failed", counts.failed);
    entry.Set("errors", counts.errors);
    entry.Set("bytes", counts.bytes);
    host_list.Append(std::move(entry));
  }
  base::DictValue types;
  for (const auto& [type, count] : by_type) {
    types.Set(type, count);
  }
  base::DictValue statuses;
  for (const auto& [status, count] : by_status) {
    statuses.Set(status, count);
  }

  base::DictValue reasons;
  for (const auto& [reason, count] : missing_by_reason) {
    reasons.Set(reason, count);
  }

  base::DictValue summary;
  summary.Set("requests", base::checked_cast<int>(rows.size()));
  summary.Set("hosts", base::checked_cast<int>(by_host.size()));
  summary.Set("by_host", std::move(host_list));
  summary.Set("by_type", std::move(types));
  summary.Set("by_status", std::move(statuses));
  if (!reasons.empty()) {
    summary.Set("missing_by_reason", std::move(reasons));
  }
  return ToJson(summary);
}

base::DictValue ToDict(const warc::CdxjEntry& entry) {
  base::DictValue dict;
  dict.Set("url", entry.url);
  dict.Set("method", warc::KeyMethod(entry.key));
  int status = 0;
  if (base::StringToInt(entry.status, &status)) {
    dict.Set("status", status);
  }
  if (!entry.mime.empty()) {
    dict.Set("type", entry.mime);
  }
  dict.Set("captured_at", IndexTimeAsIso8601(entry.timestamp));
  return dict;
}

// Runs on the archive's sequence, where it may be read.
void SearchOpenedArchive(base::DictValue params,
                         base::OnceCallback<void(std::string)> answer,
                         const warc::WaczArchiveSource& source) {
  const warc::WaczCollection* collection = source.collection();
  std::move(answer).Run(
      collection ? SearchArchiveIndex(collection->index(), params)
                 : Error("The archive could not be read, so it cannot be "
                         "searched."));
}

}  // namespace

std::string AnswerArchiveQuery(content::WebContents* contents,
                               const std::string& tool,
                               const base::DictValue& params) {
  if (tool == "tab_status") {
    return TabStatus(contents);
  }
  if (tool == "list_requests") {
    return ListRequests(contents, params);
  }
  if (tool == "summarize_requests") {
    return SummarizeRequests(contents);
  }
  return Error("No such tool: " + tool);
}

std::string SearchArchiveIndex(base::span<const warc::CdxjEntry> index,
                               const base::DictValue& params) {
  const std::string* url_param = params.FindString("url");
  const std::string* contains = params.FindString("contains");
  if (!url_param && !contains) {
    return Error("Give 'url', 'contains' or both.");
  }
  const std::string needle =
      contains ? base::ToLowerASCII(*contains) : std::string();
  const int limit = std::clamp(params.FindInt("limit").value_or(kDefaultListed),
                               1, kMaxListed);

  base::DictValue result;
  result.Set("index_entries", base::checked_cast<int>(index.size()));
  std::vector<const warc::CdxjEntry*> found;
  const auto take = [&](const warc::CdxjEntry& entry) {
    if (needle.empty() ||
        base::ToLowerASCII(entry.url).find(needle) != std::string::npos) {
      found.push_back(&entry);
    }
  };
  GURL url;
  if (url_param) {
    url = GURL(*url_param);
    if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
      return Error("'url' is not an http(s) URL: " + *url_param);
    }
    const std::string key = warc::ToSurt(url);
    result.Set("url", url.spec());
    result.Set("host_captures", base::checked_cast<int>(
                                    warc::CapturesOfHost(index, key).size()));
    warc::ForEachCaptureOfPath(index, key, take);
  } else {
    for (const warc::CdxjEntry& entry : index) {
      take(entry);
    }
  }

  base::ListValue captures;
  for (const warc::CdxjEntry* entry : found) {
    if (static_cast<int>(captures.size()) == limit) {
      break;
    }
    base::DictValue capture = ToDict(*entry);
    if (url.is_valid()) {
      capture.Set("this_url", entry->url == url.spec());
    }
    captures.Append(std::move(capture));
  }
  result.Set("matching", base::checked_cast<int>(found.size()));
  result.Set("returned", base::checked_cast<int>(captures.size()));
  result.Set("captures", std::move(captures));
  return ToJson(result);
}

void SearchArchive(content::WebContents* contents,
                   const base::DictValue& params,
                   base::OnceCallback<void(std::string)> answer) {
  ReplayLog* log = contents ? ReplayLog::FromWebContents(contents) : nullptr;
  if (!log) {
    std::move(answer).Run(
        Error("The tab is not replaying an archive, so there is no index to "
              "search. An archive being written has none until it is "
              "packaged as a WACZ."));
    return;
  }
  log->opened_archive()->source().PostTaskWithThisObject(base::BindOnce(
      &SearchOpenedArchive, params.Clone(),
      base::BindPostTaskToCurrentDefault(std::move(answer))));
}
