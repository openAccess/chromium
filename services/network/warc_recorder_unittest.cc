// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/warc_recorder.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "components/warc/warc_test_util.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/http/http_raw_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace network {
namespace {

scoped_refptr<net::HttpResponseHeaders> MakeResponseHeaders(
    std::string_view raw) {
  return base::MakeRefCounted<net::HttpResponseHeaders>(
      net::HttpUtil::AssembleRawHeaders(raw));
}

// A parsed view of an archive, so tests can assert on a record's fields rather
// than on where a string happens to fall in the file.
struct ParsedRecord {
  std::string Field(std::string_view name) const {
    const std::string prefix = base::StrCat({name, ": "});
    for (std::string_view line : base::SplitStringPiece(
             head, "\r\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
      if (base::StartsWith(line, prefix,
                           base::CompareCase::INSENSITIVE_ASCII)) {
        return std::string(line.substr(prefix.size()));
      }
    }
    return std::string();
  }

  std::string head;
  std::string block;
};

std::vector<ParsedRecord> ParseRecords(std::string_view archive) {
  std::vector<ParsedRecord> records;
  size_t pos = 0;
  while (true) {
    const size_t start = archive.find("WARC/1.1\r\n", pos);
    if (start == std::string_view::npos) {
      break;
    }
    const size_t head_end = archive.find("\r\n\r\n", start);
    if (head_end == std::string_view::npos) {
      break;
    }
    ParsedRecord record;
    record.head = std::string(archive.substr(start, head_end - start));
    size_t length = 0;
    if (!base::StringToSizeT(record.Field("Content-Length"), &length)) {
      break;
    }
    const size_t block_start = head_end + 4;
    record.block = std::string(archive.substr(block_start, length));
    records.push_back(std::move(record));
    // Past the block and the two CRLFs that separate records.
    pos = block_start + length + 4;
  }
  return records;
}

// The record of `type` whose block contains `needle`, or the first of `type`
// when `needle` is empty.
const ParsedRecord* FindRecord(const std::vector<ParsedRecord>& records,
                               std::string_view type,
                               std::string_view needle = "") {
  for (const ParsedRecord& record : records) {
    if (record.Field("WARC-Type") != type) {
      continue;
    }
    if (needle.empty() || record.block.find(needle) != std::string::npos) {
      return &record;
    }
  }
  return nullptr;
}

class WarcRecorderTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }

  base::FilePath ArchivePath() {
    return temp_dir_.GetPath().AppendASCII("out.warc");
  }

  std::unique_ptr<WarcRecorder> MakeRecorder(
      const WarcRecorder::Limits& limits = WarcRecorder::Limits(),
      warc::WarcWriter::Compression compression =
          warc::WarcWriter::Compression::kNone,
      bool redact_credentials = true) {
    base::File file(ArchivePath(),
                    base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_WRITE);
    return std::make_unique<WarcRecorder>(std::move(file), limits, compression,
                                          redact_credentials);
  }

  base::FilePath PathNamed(std::string_view name) {
    return temp_dir_.GetPath().AppendASCII(name);
  }

  base::File OpenAt(const base::FilePath& path) {
    return base::File(path,
                      base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_WRITE);
  }

  std::string ReadAt(const base::FilePath& path) {
    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(path, &contents));
    return contents;
  }

  // Tears the recorder down so everything queued reaches whichever file was
  // current when it was queued.
  void Finish(std::unique_ptr<WarcRecorder> recorder) {
    recorder.reset();
    task_environment_.RunUntilIdle();
  }

  // Tears the recorder down so everything is flushed, then returns the archive.
  std::string FinishAndRead(std::unique_ptr<WarcRecorder> recorder) {
    recorder.reset();
    task_environment_.RunUntilIdle();

    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(ArchivePath(), &contents));
    return contents;
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir temp_dir_;
};

TEST_F(WarcRecorderTest, WritesRequestThenResponseRecord) {
  auto recorder = MakeRecorder();

  {
    auto exchange = recorder->CreateExchangeRecorder("https://example.org");
    exchange->SetTargetUrl(GURL("https://example.org/page"));

    net::HttpRawRequestHeaders request_headers;
    request_headers.set_request_line("GET /page HTTP/1.1\r\n");
    request_headers.Add("Host", "example.org");
    exchange->SetRequestHeaders(request_headers, "GET");

    exchange->SetResponseHeaders(
        MakeResponseHeaders("HTTP/1.1 200 OK\nContent-Type: text/html\n\n"));
    exchange->SetRemoteEndpoint(
        net::IPEndPoint(net::IPAddress(192, 0, 2, 10), 443));
    exchange->SetProtocol("http/1.1");

    const std::string body = "<html>hi</html>";
    exchange->AddBodyBytes(base::as_byte_span(body));
    exchange->Finish();
  }

  const std::string archive = FinishAndRead(std::move(recorder));

  const size_t response_pos = archive.find("WARC-Type: response");
  const size_t request_pos = archive.find("WARC-Type: request");
  ASSERT_NE(response_pos, std::string::npos);
  ASSERT_NE(request_pos, std::string::npos);
  // The request comes first, matching the exchange's chronology and GNU wget's
  // record ordering.
  EXPECT_LT(request_pos, response_pos);

  EXPECT_NE(archive.find("WARC-Target-URI: https://example.org/page"),
            std::string::npos);
  EXPECT_NE(archive.find("WARC-IP-Address: 192.0.2.10"), std::string::npos);
  EXPECT_NE(archive.find("WARC-Protocol: http/1.1"), std::string::npos);
  EXPECT_NE(archive.find("<html>hi</html>"), std::string::npos);
  EXPECT_NE(archive.find("GET /page HTTP/1.1"), std::string::npos);
}

TEST_F(WarcRecorderTest, ResponseRecordIsConcurrentToRequestRecord) {
  auto recorder = MakeRecorder();

  {
    auto exchange = recorder->CreateExchangeRecorder("https://example.org");
    exchange->SetTargetUrl(GURL("https://example.org/"));

    net::HttpRawRequestHeaders request_headers;
    request_headers.set_request_line("GET / HTTP/1.1\r\n");
    exchange->SetRequestHeaders(request_headers, "GET");
    exchange->SetResponseHeaders(MakeResponseHeaders("HTTP/1.1 200 OK\n\n"));
    exchange->Finish();
  }

  const std::string archive = FinishAndRead(std::move(recorder));

  const std::vector<ParsedRecord> records = ParseRecords(archive);
  const ParsedRecord* request = FindRecord(records, "request");
  const ParsedRecord* response = FindRecord(records, "response");
  ASSERT_TRUE(request);
  ASSERT_TRUE(response);

  EXPECT_EQ(request->Field("WARC-Record-ID"),
            response->Field("WARC-Concurrent-To"));
}

TEST_F(WarcRecorderTest, RecordsCarryWarcinfoId) {
  auto recorder = MakeRecorder();
  recorder->WriteWarcinfo("out.warc");

  {
    auto exchange = recorder->CreateExchangeRecorder("https://example.org");
    exchange->SetTargetUrl(GURL("https://example.org/"));

    net::HttpRawRequestHeaders request_headers;
    request_headers.set_request_line("GET / HTTP/1.1\r\n");
    exchange->SetRequestHeaders(request_headers, "GET");
    exchange->SetResponseHeaders(MakeResponseHeaders("HTTP/1.1 200 OK\n\n"));
    exchange->Finish();
  }

  const std::string archive = FinishAndRead(std::move(recorder));
  const std::vector<ParsedRecord> records = ParseRecords(archive);

  // Records are attributed to the warcinfo describing their browsing context,
  // not to the file-level one, which stands as the header for the whole file.
  const ParsedRecord* context_info =
      FindRecord(records, "warcinfo", "browsing-context: https://example.org");
  ASSERT_TRUE(context_info);
  const std::string context_id = context_info->Field("WARC-Record-ID");
  ASSERT_FALSE(context_id.empty());

  const ParsedRecord* request = FindRecord(records, "request");
  const ParsedRecord* response = FindRecord(records, "response");
  ASSERT_TRUE(request);
  ASSERT_TRUE(response);
  EXPECT_EQ(context_id, request->Field("WARC-Warcinfo-ID"));
  EXPECT_EQ(context_id, response->Field("WARC-Warcinfo-ID"));

  // And the file-level record is still there, carrying the archive's name.
  const ParsedRecord* file_info = FindRecord(records, "warcinfo", "software:");
  ASSERT_TRUE(file_info);
  EXPECT_EQ("out.warc", file_info->Field("WARC-Filename"));
}

TEST_F(WarcRecorderTest, PreservesCompressedBodyAndItsContentEncoding) {
  auto recorder = MakeRecorder();

  // Bytes that are not valid UTF-8, standing in for a gzip-compressed body.
  const std::vector<uint8_t> compressed = {0x1f, 0x8b, 0x08, 0x00,
                                           0x00, 0xff, 0x42};

  {
    auto exchange = recorder->CreateExchangeRecorder("https://example.org");
    exchange->SetTargetUrl(GURL("https://example.org/gz"));
    exchange->SetBodyIsWireFormat(true);
    exchange->SetResponseHeaders(
        MakeResponseHeaders("HTTP/1.1 200 OK\nContent-Encoding: gzip\n\n"));
    exchange->AddBodyBytes(compressed);
    exchange->Finish();
  }

  const std::string archive = FinishAndRead(std::move(recorder));

  // The whole point of byte-faithful capture: the stored payload must still be
  // the compressed bytes, matching the Content-Encoding recorded beside it.
  EXPECT_NE(archive.find("Content-Encoding: gzip"), std::string::npos);
  const std::string compressed_as_string(compressed.begin(), compressed.end());
  EXPECT_NE(archive.find(compressed_as_string), std::string::npos);
}

TEST_F(WarcRecorderTest, DecodedBodyHasEncodingHeadersRewritten) {
  auto recorder = MakeRecorder();

  {
    auto exchange = recorder->CreateExchangeRecorder("https://example.org");
    exchange->SetTargetUrl(GURL("https://example.org/gz"));
    // The network stack decoded the body before we saw it.
    exchange->SetBodyIsWireFormat(false);
    exchange->SetResponseHeaders(
        MakeResponseHeaders("HTTP/1.1 200 OK\nContent-Type: text/html\n"
                            "Content-Encoding: gzip\nContent-Length: 7\n\n"));
    exchange->AddBodyBytes(base::as_byte_span(std::string_view("decoded!!")));
    exchange->Finish();
  }

  const std::string archive = FinishAndRead(std::move(recorder));

  // Storing decoded bytes under "Content-Encoding: gzip" would produce a record
  // no reader could interpret, so that header must be gone.
  EXPECT_EQ(archive.find("Content-Encoding"), std::string::npos);
  // Content-Length must describe what is actually stored, not what the server
  // originally said.
  EXPECT_NE(archive.find("Content-Length: 9\r\n"), std::string::npos);
  EXPECT_EQ(archive.find("Content-Length: 7\r\n"), std::string::npos);
  // Unrelated headers survive untouched.
  EXPECT_NE(archive.find("Content-Type: text/html"), std::string::npos);
  EXPECT_NE(archive.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(archive.find("decoded!!"), std::string::npos);
}

TEST_F(WarcRecorderTest, DecodedBodyDropsChunkedTransferEncoding) {
  auto recorder = MakeRecorder();

  {
    auto exchange = recorder->CreateExchangeRecorder("https://example.org");
    exchange->SetTargetUrl(GURL("https://example.org/chunked"));
    exchange->SetBodyIsWireFormat(false);
    exchange->SetResponseHeaders(
        MakeResponseHeaders("HTTP/1.1 200 OK\nTransfer-Encoding: chunked\n\n"));
    exchange->AddBodyBytes(base::as_byte_span(std::string_view("body")));
    exchange->Finish();
  }

  const std::string archive = FinishAndRead(std::move(recorder));

  // net strips chunked framing, so advertising it would misdescribe the stored
  // payload.
  EXPECT_EQ(archive.find("Transfer-Encoding"), std::string::npos);
  EXPECT_NE(archive.find("Content-Length: 4\r\n"), std::string::npos);
}

TEST_F(WarcRecorderTest, MarksOversizedBodyAsLengthTruncated) {
  WarcRecorder::Limits limits;
  limits.max_body_bytes = 8;
  auto recorder = MakeRecorder(limits);

  {
    auto exchange = recorder->CreateExchangeRecorder("https://example.org");
    exchange->SetTargetUrl(GURL("https://example.org/big"));
    exchange->SetResponseHeaders(MakeResponseHeaders("HTTP/1.1 200 OK\n\n"));

    const std::string body = "0123456789abcdef";
    exchange->AddBodyBytes(base::as_byte_span(body));
    EXPECT_TRUE(exchange->body_truncated());
    exchange->Finish();
  }

  const std::string archive = FinishAndRead(std::move(recorder));

  EXPECT_NE(archive.find("WARC-Truncated: length"), std::string::npos);
  // The prefix that fit is kept; the remainder must not appear.
  EXPECT_NE(archive.find("01234567"), std::string::npos);
  EXPECT_EQ(archive.find("89abcdef"), std::string::npos);
}

TEST_F(WarcRecorderTest, ReconstructsRequestLineForHttp2) {
  auto recorder = MakeRecorder();

  {
    auto exchange = recorder->CreateExchangeRecorder("https://example.org");
    exchange->SetTargetUrl(GURL("https://example.org/h2"));

    // HTTP/2 carries pseudo-headers and no request line.
    net::HttpRawRequestHeaders request_headers;
    request_headers.Add(":method", "GET");
    request_headers.Add(":scheme", "https");
    request_headers.Add(":path", "/h2");
    request_headers.Add(":authority", "example.org");
    request_headers.Add("accept", "*/*");
    exchange->SetRequestHeaders(request_headers, "GET");

    exchange->SetResponseHeaders(MakeResponseHeaders("HTTP/1.1 200 OK\n\n"));
    exchange->SetProtocol("h2");
    exchange->Finish();
  }

  const std::string archive = FinishAndRead(std::move(recorder));

  EXPECT_NE(archive.find("GET /h2 HTTP/1.1"), std::string::npos);
  // The authority becomes a Host header so the record is a valid HTTP/1.1
  // message.
  EXPECT_NE(archive.find("Host: example.org"), std::string::npos);
  // Pseudo-headers must not be emitted as ordinary header lines.
  EXPECT_EQ(archive.find(":method"), std::string::npos);
  EXPECT_EQ(archive.find(":authority"), std::string::npos);
  // Readers need to know the header block was reconstructed, not captured.
  EXPECT_NE(archive.find("WARC-Protocol: h2"), std::string::npos);
}

TEST_F(WarcRecorderTest, AbandonedExchangeIsStillRecorded) {
  auto recorder = MakeRecorder();

  {
    auto exchange = recorder->CreateExchangeRecorder("https://example.org");
    exchange->SetTargetUrl(GURL("https://example.org/aborted"));
    exchange->SetResponseHeaders(MakeResponseHeaders("HTTP/1.1 200 OK\n\n"));
    exchange->AddBodyBytes(base::as_byte_span(std::string_view("partial")));
    // Deliberately no Finish() — the load was torn down mid-flight.
  }

  const std::string archive = FinishAndRead(std::move(recorder));

  EXPECT_NE(archive.find("WARC-Target-URI: https://example.org/aborted"),
            std::string::npos);
  EXPECT_NE(archive.find("WARC-Truncated: disconnect"), std::string::npos);
  EXPECT_NE(archive.find("partial"), std::string::npos);
}

TEST_F(WarcRecorderTest, ExchangeWithoutValidUrlIsDropped) {
  auto recorder = MakeRecorder();

  {
    auto exchange = recorder->CreateExchangeRecorder("https://example.org");
    exchange->SetResponseHeaders(MakeResponseHeaders("HTTP/1.1 200 OK\n\n"));
    exchange->Finish();
  }

  EXPECT_EQ("", FinishAndRead(std::move(recorder)));
}

TEST_F(WarcRecorderTest, WarcinfoDescribesTheCapture) {
  auto recorder = MakeRecorder();
  recorder->WriteWarcinfo("out.warc");

  const std::string archive = FinishAndRead(std::move(recorder));

  EXPECT_NE(archive.find("WARC-Type: warcinfo"), std::string::npos);
  EXPECT_NE(archive.find("WARC-Filename: out.warc"), std::string::npos);
  EXPECT_NE(archive.find("Content-Type: application/warc-fields"),
            std::string::npos);
  EXPECT_NE(archive.find("format: WARC File Format 1.1"), std::string::npos);
}

TEST_F(WarcRecorderTest, ExchangeOutlivingRecorderDoesNotCrash) {
  auto recorder = MakeRecorder();
  auto exchange = recorder->CreateExchangeRecorder("https://example.org");
  exchange->SetTargetUrl(GURL("https://example.org/"));
  exchange->SetResponseHeaders(MakeResponseHeaders("HTTP/1.1 200 OK\n\n"));

  // A URLLoader can outlive the recording session; the weak reference must
  // absorb that rather than writing into a freed writer.
  recorder.reset();
  exchange->Finish();
  exchange.reset();
  task_environment_.RunUntilIdle();
}

TEST_F(WarcRecorderTest, GzipCaptureIsOneMemberPerRecord) {
  auto recorder = MakeRecorder(WarcRecorder::Limits(),
                               warc::WarcWriter::Compression::kGzipPerRecord);
  recorder->WriteWarcinfo("out.warc.gz");

  {
    auto exchange = recorder->CreateExchangeRecorder("https://example.org");
    exchange->SetTargetUrl(GURL("https://example.org/page"));

    net::HttpRawRequestHeaders request_headers;
    request_headers.set_request_line("GET /page HTTP/1.1\r\n");
    request_headers.Add("Host", "example.org");
    exchange->SetRequestHeaders(request_headers, "GET");

    exchange->SetResponseHeaders(
        MakeResponseHeaders("HTTP/1.1 200 OK\nContent-Type: text/html\n\n"));
    const std::string body = "<html>hi</html>";
    exchange->AddBodyBytes(base::as_byte_span(body));
    exchange->Finish();
  }

  const std::string archive = FinishAndRead(std::move(recorder));

  // The file's warcinfo, the browsing context's warcinfo, then the request and
  // response — each in a member of its own, so a CDX index built over this
  // archive can point at a record and a reader can decompress just that one.
  const std::optional<std::vector<std::string>> members =
      warc::InflateGzipMembers(archive);
  ASSERT_TRUE(members.has_value());
  ASSERT_EQ(4u, members->size());

  EXPECT_NE((*members)[0].find("WARC-Type: warcinfo"), std::string::npos);
  EXPECT_NE((*members)[0].find("WARC-Filename: out.warc.gz"),
            std::string::npos);

  EXPECT_NE((*members)[1].find("WARC-Type: warcinfo"), std::string::npos);
  EXPECT_NE((*members)[1].find("browsing-context: https://example.org"),
            std::string::npos);

  EXPECT_NE((*members)[2].find("WARC-Type: request"), std::string::npos);
  EXPECT_NE((*members)[2].find("GET /page HTTP/1.1"), std::string::npos);

  EXPECT_NE((*members)[3].find("WARC-Type: response"), std::string::npos);
  EXPECT_NE((*members)[3].find("<html>hi</html>"), std::string::npos);
}

TEST_F(WarcRecorderTest, SpilledBodyIsArchivedWholeWithCorrectDigests) {
  WarcRecorder::Limits limits;
  // Spill almost immediately, so the test exercises the disk path rather than
  // the buffer, without needing a large body.
  limits.max_body_bytes = 1024;
  auto recorder = MakeRecorder(limits);

  const base::FilePath spill_path =
      temp_dir_.GetPath().AppendASCII("spill.bin");
  recorder->SetSpillFile(base::File(spill_path, base::File::FLAG_CREATE_ALWAYS |
                                                    base::File::FLAG_READ |
                                                    base::File::FLAG_WRITE));

  // Distinctive at both ends, so a truncated or misordered spill shows up.
  std::string body(64 * 1024, 'm');
  body.replace(0, 6, "FIRST!");
  body.replace(body.size() - 5, 5, "LAST!");

  {
    auto exchange = recorder->CreateCompletionRecorder("https://example.org");
    exchange->SetTargetUrl(GURL("https://example.org/big.mp4"));
    exchange->SetBodyIsWireFormat(true);
    exchange->SetResponseHeaders(
        MakeResponseHeaders("HTTP/1.1 200 OK\nContent-Type: video/mp4\n\n"));
    // Delivered in pieces, as it arrives off a socket.
    for (size_t offset = 0; offset < body.size(); offset += 4096) {
      exchange->AddBodyBytes(
          base::as_byte_span(std::string_view(body).substr(offset, 4096)));
    }
    exchange->Finish();
  }

  const std::string archive = FinishAndRead(std::move(recorder));

  // The whole body has to be present, in order, once.
  EXPECT_NE(archive.find(body), std::string::npos)
      << "the spilled body did not reach the archive intact";

  // And the record must describe it: a digest taken while the bytes streamed
  // past is worthless if it does not match what a reader recomputes.
  const size_t head_start = archive.find("HTTP/1.1 200 OK");
  ASSERT_NE(head_start, std::string::npos);
  const size_t block_start = archive.rfind("WARC/1.1", head_start);
  ASSERT_NE(block_start, std::string::npos);
  const std::string record_head =
      archive.substr(block_start, head_start - block_start);

  const std::string http_head =
      archive.substr(head_start, archive.find(body) - head_start);
  const std::string block = http_head + body;
  EXPECT_NE(record_head.find(warc::ComputeDigest(base::as_byte_span(block),
                                                 warc::DigestAlgorithm::kSha1)),
            std::string::npos)
      << "WARC-Block-Digest does not match the archived block";
  EXPECT_NE(record_head.find(warc::ComputeDigest(base::as_byte_span(body),
                                                 warc::DigestAlgorithm::kSha1)),
            std::string::npos)
      << "WARC-Payload-Digest does not match the archived payload";
  EXPECT_NE(
      record_head.find("Content-Length: " + base::NumberToString(block.size())),
      std::string::npos);
}

TEST_F(WarcRecorderTest, WithoutASpillFileTheBodyStaysCapped) {
  WarcRecorder::Limits limits;
  limits.max_body_bytes = 4096;
  // Deliberately no SetSpillFile: the browser may have failed to open one.
  auto recorder = MakeRecorder(limits);

  {
    auto exchange = recorder->CreateCompletionRecorder("https://example.org");
    exchange->SetTargetUrl(GURL("https://example.org/big.mp4"));
    exchange->SetBodyIsWireFormat(true);
    exchange->SetResponseHeaders(MakeResponseHeaders("HTTP/1.1 200 OK\n\n"));
    exchange->AddBodyBytes(base::as_byte_span(std::string(64 * 1024, 'z')));
    EXPECT_TRUE(exchange->body_truncated());
    exchange->Finish();
  }

  const std::string archive = FinishAndRead(std::move(recorder));
  // Capped rather than lost, and marked so nobody mistakes it for complete.
  EXPECT_NE(archive.find("WARC-Truncated: length"), std::string::npos);
}

// Records the given exchange, so the grouping tests stay about grouping.
void RecordExchange(WarcRecorder& recorder,
                    const std::string& browsing_context,
                    const GURL& url) {
  auto exchange = recorder.CreateExchangeRecorder(browsing_context);
  exchange->SetTargetUrl(url);
  net::HttpRawRequestHeaders headers;
  headers.set_request_line("GET / HTTP/1.1\r\n");
  exchange->SetRequestHeaders(headers, "GET");
  exchange->SetResponseHeaders(MakeResponseHeaders("HTTP/1.1 200 OK\n\n"));
  exchange->Finish();
}

TEST_F(WarcRecorderTest, EachBrowsingContextGetsItsOwnWarcinfo) {
  auto recorder = MakeRecorder();
  recorder->WriteWarcinfo("out.warc");

  RecordExchange(*recorder, "https://a.example", GURL("https://a.example/1"));
  RecordExchange(*recorder, "https://b.example", GURL("https://b.example/1"));
  // Interleaved, as a browser loading two pages at once produces. The format
  // copes because WARC-Warcinfo-ID overrides the positional association a
  // warcinfo would otherwise imply.
  RecordExchange(*recorder, "https://a.example", GURL("https://a.example/2"));

  const std::vector<ParsedRecord> records =
      ParseRecords(FinishAndRead(std::move(recorder)));

  const ParsedRecord* a_info =
      FindRecord(records, "warcinfo", "browsing-context: https://a.example");
  const ParsedRecord* b_info =
      FindRecord(records, "warcinfo", "browsing-context: https://b.example");
  ASSERT_TRUE(a_info);
  ASSERT_TRUE(b_info);
  EXPECT_NE(a_info->Field("WARC-Record-ID"), b_info->Field("WARC-Record-ID"));
  // Each context's warcinfo says which archive it belongs to. WARC-Filename
  // names the file and so stays on the file-level record; the link from a
  // context back to it goes in the block, where warcinfo fields belong.
  EXPECT_TRUE(a_info->Field("WARC-Filename").empty());
  EXPECT_NE(a_info->block.find("isPartOf: out.warc"), std::string::npos);

  // Every record is attributed to the context that made it.
  int a_records = 0;
  int b_records = 0;
  for (const ParsedRecord& record : records) {
    const std::string type = record.Field("WARC-Type");
    if (type != "request" && type != "response") {
      continue;
    }
    const std::string info = record.Field("WARC-Warcinfo-ID");
    const bool is_a =
        record.Field("WARC-Target-URI").find("a.example") != std::string::npos;
    EXPECT_EQ(is_a ? a_info->Field("WARC-Record-ID")
                   : b_info->Field("WARC-Record-ID"),
              info);
    (is_a ? a_records : b_records)++;
  }
  EXPECT_EQ(4, a_records);
  EXPECT_EQ(2, b_records);
}

TEST_F(WarcRecorderTest, OneWarcinfoPerContextHoweverManyExchanges) {
  auto recorder = MakeRecorder();
  recorder->WriteWarcinfo("out.warc");

  for (int i = 0; i < 5; ++i) {
    RecordExchange(
        *recorder, "https://a.example",
        GURL(base::StrCat({"https://a.example/", base::NumberToString(i)})));
  }

  const std::vector<ParsedRecord> records =
      ParseRecords(FinishAndRead(std::move(recorder)));

  // A context is described once, not once per exchange.
  int context_infos = 0;
  for (const ParsedRecord& record : records) {
    if (record.Field("WARC-Type") == "warcinfo" &&
        record.block.find("browsing-context:") != std::string::npos) {
      ++context_infos;
    }
  }
  EXPECT_EQ(1, context_infos);
}

TEST_F(WarcRecorderTest, TrafficNoPageIsResponsibleForIsSeparated) {
  auto recorder = MakeRecorder();
  recorder->WriteWarcinfo("out.warc");

  RecordExchange(*recorder, "https://a.example", GURL("https://a.example/1"));
  // The browser's own background traffic has no top-level origin. Keeping it
  // apart is the difference between an archive of a page and an archive of a
  // page plus whatever Chrome happened to be doing.
  RecordExchange(*recorder, "", GURL("https://update.googleapis.com/service"));

  const std::vector<ParsedRecord> records =
      ParseRecords(FinishAndRead(std::move(recorder)));

  const ParsedRecord* page_info =
      FindRecord(records, "warcinfo", "browsing-context: https://a.example");
  const ParsedRecord* browser_info =
      FindRecord(records, "warcinfo", "no page is responsible for");
  ASSERT_TRUE(page_info);
  ASSERT_TRUE(browser_info);
  // The unattributed one names no context, since there is none to name.
  EXPECT_EQ(browser_info->block.find("browsing-context:"), std::string::npos);

  const ParsedRecord* update = nullptr;
  for (const ParsedRecord& record : records) {
    if (record.Field("WARC-Target-URI").find("update.googleapis") !=
        std::string::npos) {
      update = &record;
      break;
    }
  }
  ASSERT_TRUE(update);
  EXPECT_EQ(browser_info->Field("WARC-Record-ID"),
            update->Field("WARC-Warcinfo-ID"));
}

// Records an exchange carrying the credentials a real session would.
void RecordCredentialedExchange(WarcRecorder& recorder) {
  auto exchange = recorder.CreateExchangeRecorder("https://example.org");
  exchange->SetTargetUrl(GURL("https://example.org/"));

  net::HttpRawRequestHeaders headers;
  headers.set_request_line("GET / HTTP/1.1\r\n");
  headers.Add("Host", "example.org");
  headers.Add("Cookie", "session=super-secret-value; other=abc");
  headers.Add("Authorization", "Bearer super-secret-token");
  headers.Add("Accept", "text/html");
  exchange->SetRequestHeaders(headers, "GET");

  exchange->SetResponseHeaders(MakeResponseHeaders(
      "HTTP/1.1 200 OK\n"
      "Content-Type: text/html\n"
      "Set-Cookie: session=another-secret-value; Path=/\n\n"));
  exchange->Finish();
}

TEST_F(WarcRecorderTest, CredentialsAreRedactedByDefault) {
  auto recorder = MakeRecorder();
  recorder->WriteWarcinfo("out.warc");
  RecordCredentialedExchange(*recorder);

  const std::string archive = FinishAndRead(std::move(recorder));

  // The values are gone, wherever they appeared.
  EXPECT_EQ(archive.find("super-secret-value"), std::string::npos);
  EXPECT_EQ(archive.find("super-secret-token"), std::string::npos);
  EXPECT_EQ(archive.find("another-secret-value"), std::string::npos);

  // But the fields remain, so a reader can tell a redacted request from one
  // that carried no cookie at all.
  EXPECT_NE(archive.find("Cookie: [redacted]"), std::string::npos);
  EXPECT_NE(archive.find("Authorization: [redacted]"), std::string::npos);
  EXPECT_NE(archive.find("Set-Cookie: [redacted]"), std::string::npos);

  // Everything else is untouched.
  EXPECT_NE(archive.find("Host: example.org"), std::string::npos);
  EXPECT_NE(archive.find("Accept: text/html"), std::string::npos);
  EXPECT_NE(archive.find("Content-Type: text/html"), std::string::npos);
  EXPECT_NE(archive.find("GET / HTTP/1.1"), std::string::npos);
  EXPECT_NE(archive.find("HTTP/1.1 200 OK"), std::string::npos);
}

TEST_F(WarcRecorderTest, RedactionIsDeclaredInTheWarcinfo) {
  auto recorder = MakeRecorder();
  recorder->WriteWarcinfo("out.warc");
  RecordCredentialedExchange(*recorder);

  const std::vector<ParsedRecord> records =
      ParseRecords(FinishAndRead(std::move(recorder)));

  // The block digests cover the redacted text, not what crossed the wire, so
  // an archive that withholds something has to say so.
  for (const char* needle :
       {"software:", "browsing-context: https://example.org"}) {
    const ParsedRecord* info = FindRecord(records, "warcinfo", needle);
    ASSERT_TRUE(info) << needle;
    EXPECT_NE(info->block.find("redacted: cookie, set-cookie, authorization"),
              std::string::npos)
        << "warcinfo does not disclose the redaction: " << info->block;
  }
}

TEST_F(WarcRecorderTest, CredentialsAreKeptWhenExplicitlyRequested) {
  auto recorder =
      MakeRecorder(WarcRecorder::Limits(), warc::WarcWriter::Compression::kNone,
                   /*redact_credentials=*/false);
  recorder->WriteWarcinfo("out.warc");
  RecordCredentialedExchange(*recorder);

  const std::string archive = FinishAndRead(std::move(recorder));

  // Reproducing an exchange exactly is a legitimate need; it just cannot be
  // the default.
  EXPECT_NE(archive.find("super-secret-value"), std::string::npos);
  EXPECT_NE(archive.find("super-secret-token"), std::string::npos);
  EXPECT_NE(archive.find("another-secret-value"), std::string::npos);
  EXPECT_EQ(archive.find("[redacted]"), std::string::npos);
  // And nothing claims a redaction that did not happen.
  EXPECT_EQ(archive.find("redacted:"), std::string::npos);
}

TEST_F(WarcRecorderTest, RedactionLeavesContentLengthAndDigestsConsistent) {
  auto recorder = MakeRecorder();
  RecordCredentialedExchange(*recorder);

  const std::vector<ParsedRecord> records =
      ParseRecords(FinishAndRead(std::move(recorder)));

  // ParseRecords walks the file by Content-Length, so it only yields records
  // whose declared length matches what was written -- shortening a header
  // without restating the length would desynchronise every record after it.
  const ParsedRecord* request = FindRecord(records, "request");
  const ParsedRecord* response = FindRecord(records, "response");
  ASSERT_TRUE(request);
  ASSERT_TRUE(response);
  EXPECT_EQ(request->block.size(),
            static_cast<size_t>(std::stoi(request->Field("Content-Length"))));
  EXPECT_EQ(warc::ComputeDigest(base::as_byte_span(response->block),
                                warc::DigestAlgorithm::kSha1),
            response->Field("WARC-Block-Digest"));
}

// The warcinfo record describing `browsing_context` within `records`.
const ParsedRecord* FindContextWarcinfo(
    const std::vector<ParsedRecord>& records,
    std::string_view browsing_context) {
  return FindRecord(records, "warcinfo",
                    base::StrCat({"browsing-context: ", browsing_context}));
}

TEST_F(WarcRecorderTest, RotationSendsLaterRecordsToTheNewFile) {
  auto recorder = MakeRecorder();
  recorder->WriteWarcinfo("out.warc");
  const base::FilePath second = PathNamed("second.warc");

  RecordExchange(*recorder, "https://a.example", GURL("https://a.example/1"));
  recorder->Rotate(OpenAt(second), "second.warc", base::DoNothing());
  RecordExchange(*recorder, "https://b.example", GURL("https://b.example/1"));
  Finish(std::move(recorder));

  const std::string old_file = ReadAt(ArchivePath());
  const std::string new_file = ReadAt(second);

  EXPECT_NE(old_file.find("https://a.example/1"), std::string::npos);
  EXPECT_EQ(old_file.find("https://b.example/1"), std::string::npos);
  EXPECT_NE(new_file.find("https://b.example/1"), std::string::npos);
  EXPECT_EQ(new_file.find("https://a.example/1"), std::string::npos);
}

TEST_F(WarcRecorderTest, EachFileGetsItsOwnFileLevelWarcinfo) {
  auto recorder = MakeRecorder();
  recorder->WriteWarcinfo("out.warc");
  const base::FilePath second = PathNamed("second.warc");

  RecordExchange(*recorder, "https://a.example", GURL("https://a.example/1"));
  recorder->Rotate(OpenAt(second), "second.warc", base::DoNothing());
  RecordExchange(*recorder, "https://a.example", GURL("https://a.example/2"));
  Finish(std::move(recorder));

  // Each segment names itself, so a reader handed one file alone can still say
  // what it was called when it was written.
  const std::vector<ParsedRecord> old_records =
      ParseRecords(ReadAt(ArchivePath()));
  const std::vector<ParsedRecord> new_records = ParseRecords(ReadAt(second));

  const ParsedRecord* old_info = FindRecord(old_records, "warcinfo");
  const ParsedRecord* new_info = FindRecord(new_records, "warcinfo");
  ASSERT_TRUE(old_info);
  ASSERT_TRUE(new_info);
  EXPECT_EQ("out.warc", old_info->Field("WARC-Filename"));
  EXPECT_EQ("second.warc", new_info->Field("WARC-Filename"));
}

TEST_F(WarcRecorderTest, ContextWarcinfoIsReMintedAfterRotation) {
  auto recorder = MakeRecorder();
  recorder->WriteWarcinfo("out.warc");
  const base::FilePath second = PathNamed("second.warc");

  // One context spanning the rotation, which is the case that goes wrong if the
  // minted ids are carried across: the records in the new file would cite a
  // warcinfo that stayed behind in the old one.
  RecordExchange(*recorder, "https://a.example", GURL("https://a.example/1"));
  recorder->Rotate(OpenAt(second), "second.warc", base::DoNothing());
  RecordExchange(*recorder, "https://a.example", GURL("https://a.example/2"));
  Finish(std::move(recorder));

  const std::vector<ParsedRecord> old_records =
      ParseRecords(ReadAt(ArchivePath()));
  const std::vector<ParsedRecord> new_records = ParseRecords(ReadAt(second));

  const ParsedRecord* old_context =
      FindContextWarcinfo(old_records, "https://a.example");
  const ParsedRecord* new_context =
      FindContextWarcinfo(new_records, "https://a.example");
  ASSERT_TRUE(old_context);
  ASSERT_TRUE(new_context) << "the new file has no warcinfo for the context "
                              "that kept recording into it";
  EXPECT_NE(old_context->Field("WARC-Record-ID"),
            new_context->Field("WARC-Record-ID"));

  // And the context's warcinfo points at the file it now lives in.
  EXPECT_NE(new_context->block.find("isPartOf: second.warc"),
            std::string::npos);

  // Every record cites a warcinfo present in its own file.
  for (const auto& [records, context] :
       {std::make_pair(old_records, old_context),
        std::make_pair(new_records, new_context)}) {
    for (const ParsedRecord& record : records) {
      const std::string type = record.Field("WARC-Type");
      if (type != "request" && type != "response") {
        continue;
      }
      EXPECT_EQ(context->Field("WARC-Record-ID"),
                record.Field("WARC-Warcinfo-ID"));
    }
  }
}

TEST_F(WarcRecorderTest, ContextThatGoesQuietCostsTheNewFileNoWarcinfo) {
  auto recorder = MakeRecorder();
  recorder->WriteWarcinfo("out.warc");
  const base::FilePath second = PathNamed("second.warc");

  RecordExchange(*recorder, "https://a.example", GURL("https://a.example/1"));
  recorder->Rotate(OpenAt(second), "second.warc", base::DoNothing());
  RecordExchange(*recorder, "https://b.example", GURL("https://b.example/1"));
  Finish(std::move(recorder));

  // A warcinfo is written when a context first archives something, not when the
  // file opens, so a context that stops after the rotation leaves no record
  // describing nothing.
  const std::vector<ParsedRecord> new_records = ParseRecords(ReadAt(second));
  EXPECT_FALSE(FindContextWarcinfo(new_records, "https://a.example"));
  EXPECT_TRUE(FindContextWarcinfo(new_records, "https://b.example"));
}

TEST_F(WarcRecorderTest, RotationDoesNotSplitAnExchange) {
  auto recorder = MakeRecorder();
  recorder->WriteWarcinfo("out.warc");
  const base::FilePath second = PathNamed("second.warc");

  // An exchange already under way when the rotation arrives.
  auto exchange = recorder->CreateExchangeRecorder("https://a.example");
  exchange->SetTargetUrl(GURL("https://a.example/1"));
  net::HttpRawRequestHeaders headers;
  headers.set_request_line("GET / HTTP/1.1\r\n");
  exchange->SetRequestHeaders(headers, "GET");
  exchange->SetResponseHeaders(MakeResponseHeaders("HTTP/1.1 200 OK\n\n"));

  recorder->Rotate(OpenAt(second), "second.warc", base::DoNothing());

  // The pair is handed over as one act, so it lands wholly in the file that is
  // current when the exchange completes rather than half in each.
  exchange->Finish();
  exchange.reset();
  Finish(std::move(recorder));

  const std::vector<ParsedRecord> old_records =
      ParseRecords(ReadAt(ArchivePath()));
  const std::vector<ParsedRecord> new_records = ParseRecords(ReadAt(second));

  EXPECT_FALSE(FindRecord(old_records, "request"));
  EXPECT_FALSE(FindRecord(old_records, "response"));
  const ParsedRecord* request = FindRecord(new_records, "request");
  const ParsedRecord* response = FindRecord(new_records, "response");
  ASSERT_TRUE(request);
  ASSERT_TRUE(response);
  EXPECT_EQ(request->Field("WARC-Record-ID"),
            response->Field("WARC-Concurrent-To"));
}

TEST_F(WarcRecorderTest, RotatingToNoFileStopsRecording) {
  auto recorder = MakeRecorder();
  recorder->WriteWarcinfo("out.warc");

  RecordExchange(*recorder, "https://a.example", GURL("https://a.example/1"));

  // Stopping still reports the file it closed, or a caller waiting to package
  // the last segment of a session would wait forever.
  base::test::TestFuture<void> stopped;
  recorder->Rotate(base::File(), std::string(), stopped.GetCallback());
  ASSERT_TRUE(stopped.Wait());

  RecordExchange(*recorder, "https://b.example", GURL("https://b.example/1"));
  Finish(std::move(recorder));

  const std::string old_file = ReadAt(ArchivePath());
  EXPECT_NE(old_file.find("https://a.example/1"), std::string::npos);
  // Nowhere to write it, and nothing appended to the file just closed.
  EXPECT_EQ(old_file.find("https://b.example/1"), std::string::npos);
}

TEST_F(WarcRecorderTest, RotationReportsTheOutgoingFileComplete) {
  auto recorder = MakeRecorder();
  recorder->WriteWarcinfo("out.warc");
  const base::FilePath second = PathNamed("second.warc");

  RecordExchange(*recorder, "https://a.example", GURL("https://a.example/1"));

  base::test::TestFuture<void> rotated;
  recorder->Rotate(OpenAt(second), "second.warc", rotated.GetCallback());
  ASSERT_TRUE(rotated.Wait());

  // The reply is what tells a caller the segment may be indexed or packaged, so
  // everything recorded before the rotation has to be on disk by the time it
  // arrives -- with the recorder still alive and recording elsewhere, not torn
  // down. Reading the file here is the whole assertion.
  const std::string old_file = ReadAt(ArchivePath());
  EXPECT_NE(old_file.find("WARC-Filename: out.warc"), std::string::npos);
  EXPECT_NE(old_file.find("https://a.example/1"), std::string::npos);

  Finish(std::move(recorder));
}

}  // namespace
}  // namespace network
