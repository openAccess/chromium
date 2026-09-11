// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/wacz_collection.h"

#include <algorithm>
#include <utility>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "components/warc/gzip_member_reader.h"
#include "components/warc/surt.h"
#include "components/warc/warc_record_parser.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"

namespace warc {

namespace {

constexpr char kArchivePrefix[] = "archive/";
constexpr char kPagesEntry[] = "pages/pages.jsonl";

std::string_view AsStringView(base::span<const uint8_t> data) {
  return std::string_view(reinterpret_cast<const char*>(data.data()),
                          data.size());
}

// The index, whichever way the container keeps it. A ".gz" entry is gzipped
// inside a stored entry, which is what makes the index seekable in a large
// archive even though nothing here seeks into it yet.
std::optional<std::vector<CdxjEntry>> ReadIndex(WaczReader& reader) {
  for (std::string_view name :
       {"indexes/index.cdx.gz", "indexes/index.cdx", "indexes/index.cdxj"}) {
    const WaczReader::Entry* entry = reader.Find(name);
    if (!entry) {
      continue;
    }
    std::optional<std::vector<uint8_t>> raw = reader.ReadEntry(*entry);
    if (!raw) {
      return std::nullopt;
    }
    if (name.ends_with(".gz")) {
      // Written as gzip members so a reader can decompress one block of the
      // index without the rest; read whole here, which is right for an index
      // of thousands and wrong for one of millions.
      std::vector<CdxjEntry> entries;
      base::span<const uint8_t> rest(*raw);
      while (!rest.empty()) {
        size_t consumed = 0;
        std::optional<std::vector<uint8_t>> block =
            InflateGzipMember(rest, &consumed);
        if (!block || consumed == 0) {
          return std::nullopt;
        }
        std::vector<CdxjEntry> block_entries =
            ParseCdxj(AsStringView(*block));
        entries.insert(entries.end(),
                       std::make_move_iterator(block_entries.begin()),
                       std::make_move_iterator(block_entries.end()));
        rest = rest.subspan(consumed);
      }
      return entries;
    }
    return ParseCdxj(AsStringView(*raw));
  }
  return std::nullopt;
}

std::vector<PageEntry> ReadPages(WaczReader& reader) {
  std::vector<PageEntry> pages;
  const WaczReader::Entry* entry = reader.Find(kPagesEntry);
  if (!entry) {
    return pages;
  }
  std::optional<std::vector<uint8_t>> raw = reader.ReadEntry(*entry);
  if (!raw) {
    return pages;
  }
  for (std::string_view line : base::SplitStringPiece(
           AsStringView(*raw), "\n", base::TRIM_WHITESPACE,
           base::SPLIT_WANT_NONEMPTY)) {
    std::optional<base::DictValue> object =
        base::JSONReader::ReadDict(line, base::JSON_PARSE_RFC);
    if (!object) {
      continue;
    }
    const std::string* url = object->FindString("url");
    // The first line declares the format rather than naming a page, and a
    // page without a URL is not one anything can be replayed from.
    if (!url || url->empty()) {
      continue;
    }
    PageEntry page;
    page.url = *url;
    if (const std::string* ts = object->FindString("ts")) {
      page.timestamp = *ts;
    }
    if (const std::string* title = object->FindString("title")) {
      page.title = *title;
    }
    pages.push_back(std::move(page));
  }
  return pages;
}

// Splits the stored HTTP message into its header block and its body. A
// response record holds the message exactly as it arrived, which is why an
// archive can be replayed at all -- and why this is a parse of the wire
// format rather than a reconstruction from parts.
std::optional<ArchivedResponse> ParseStoredResponse(
    base::span<const uint8_t> block) {
  const std::string_view text = AsStringView(block);
  const size_t header_end = text.find("\r\n\r\n");
  if (header_end == std::string_view::npos) {
    return std::nullopt;
  }

  ArchivedResponse response;
  response.headers = base::MakeRefCounted<net::HttpResponseHeaders>(
      net::HttpUtil::AssembleRawHeaders(text.substr(0, header_end + 4)));
  if (response.headers->response_code() == 0) {
    return std::nullopt;
  }
  const base::span<const uint8_t> body = block.subspan(header_end + 4);
  response.body.assign(body.begin(), body.end());
  return response;
}

}  // namespace

std::string ToIndexTimestamp(std::string_view time) {
  // Both spellings are the same digits in the same order, so keeping the
  // digits is the whole conversion: "2026-09-10T20:13:54Z" and
  // "20260910201354" arrive at each other.
  std::string digits;
  digits.reserve(14);
  for (char c : time) {
    if (base::IsAsciiDigit(c)) {
      digits.push_back(c);
      if (digits.size() == 14) {
        break;
      }
    }
  }
  return digits;
}

ArchivedResponse::ArchivedResponse() = default;
ArchivedResponse::ArchivedResponse(ArchivedResponse&&) = default;
ArchivedResponse& ArchivedResponse::operator=(ArchivedResponse&&) = default;
ArchivedResponse::~ArchivedResponse() = default;

WaczCollection::WaczCollection() = default;
WaczCollection::WaczCollection(WaczCollection&&) = default;
WaczCollection& WaczCollection::operator=(WaczCollection&&) = default;
WaczCollection::~WaczCollection() = default;

std::optional<WaczCollection> WaczCollection::Open(base::File file) {
  std::optional<WaczReader> reader = WaczReader::Open(std::move(file));
  if (!reader) {
    return std::nullopt;
  }

  std::optional<std::vector<CdxjEntry>> index = ReadIndex(*reader);
  // Without an index there is no way to find a record short of reading every
  // archive in the container, which is the situation the index exists to
  // avoid. A container without one is one this cannot serve.
  if (!index) {
    return std::nullopt;
  }

  WaczCollection collection;
  collection.pages_ = ReadPages(*reader);
  collection.index_ = std::move(*index);

  // A lookup searches the index, so the index has to be sorted. It is supposed
  // to arrive that way and usually does, but an index assembled from blocks or
  // appended to by a crawler need not be, and an unsorted one does not fail --
  // it silently fails to find records the archive holds, which is the worst
  // way for an archive to be wrong. Sorting it once at open costs nothing
  // beside reading it.
  std::stable_sort(
      collection.index_.begin(), collection.index_.end(),
      [](const CdxjEntry& a, const CdxjEntry& b) { return a.key < b.key; });
  collection.reader_ = std::move(reader);

  // The index names an archive by its own name; the container holds it under
  // "archive/".
  for (const WaczReader::Entry* entry :
       collection.reader_->FindAllWithPrefix(kArchivePrefix)) {
    collection.archives_.emplace(
        entry->name.substr(std::string_view(kArchivePrefix).size()), entry);
  }
  return collection;
}

std::optional<ArchivedResponse> WaczCollection::Lookup(
    const GURL& url,
    std::string_view timestamp) {
  const std::string key = ToSurt(url);
  if (key.empty()) {
    return std::nullopt;
  }
  const CdxjEntry* entry =
      FindNearestCapture(index_, key, ToIndexTimestamp(timestamp));
  if (!entry) {
    return std::nullopt;
  }

  const auto archive = archives_.find(entry->filename);
  if (archive == archives_.end()) {
    // The index points into an archive the container does not hold, which can
    // happen where a container was assembled from a larger capture.
    LOG(ERROR) << "WACZ index names a missing archive: " << entry->filename;
    return std::nullopt;
  }

  std::optional<std::vector<uint8_t>> stored =
      reader_->ReadAt(*archive->second, entry->offset, entry->length);
  if (!stored) {
    return std::nullopt;
  }

  // A gzipped archive stores each record as a member of its own, so the bytes
  // the index pointed at are that member and nothing else.
  std::vector<uint8_t> record_bytes;
  if (archive->second->name.ends_with(".gz")) {
    std::optional<std::vector<uint8_t>> inflated =
        InflateGzipMember(*stored, nullptr);
    if (!inflated) {
      return std::nullopt;
    }
    record_bytes = std::move(*inflated);
  } else {
    record_bytes = std::move(*stored);
  }

  std::optional<ParsedRecord> record = ParseRecord(record_bytes);
  if (!record || record->Field("WARC-Type") != "response") {
    return std::nullopt;
  }

  std::optional<ArchivedResponse> response = ParseStoredResponse(record->block);
  if (response) {
    response->timestamp = entry->timestamp;
  }
  return response;
}

}  // namespace warc
