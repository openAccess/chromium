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

class WarcRecorderTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }

  base::FilePath ArchivePath() {
    return temp_dir_.GetPath().AppendASCII("out.warc");
  }

  std::unique_ptr<WarcRecorder> MakeRecorder(
      const WarcRecorder::Limits& limits = WarcRecorder::Limits(),
      warc::WarcWriter::Compression compression =
          warc::WarcWriter::Compression::kNone) {
    base::File file(ArchivePath(),
                    base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_WRITE);
    return std::make_unique<WarcRecorder>(std::move(file), limits, compression);
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
    auto exchange = recorder->CreateExchangeRecorder();
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
    auto exchange = recorder->CreateExchangeRecorder();
    exchange->SetTargetUrl(GURL("https://example.org/"));

    net::HttpRawRequestHeaders request_headers;
    request_headers.set_request_line("GET / HTTP/1.1\r\n");
    exchange->SetRequestHeaders(request_headers, "GET");
    exchange->SetResponseHeaders(MakeResponseHeaders("HTTP/1.1 200 OK\n\n"));
    exchange->Finish();
  }

  const std::string archive = FinishAndRead(std::move(recorder));

  // The request is written first, so the first record id in the file is its
  // own; the response must point back at it.
  constexpr std::string_view kRecordIdField = "WARC-Record-ID: ";
  const size_t id_pos = archive.find(kRecordIdField);
  ASSERT_NE(id_pos, std::string::npos);
  const size_t id_start = id_pos + kRecordIdField.size();
  const size_t id_end = archive.find("\r\n", id_start);
  ASSERT_NE(id_end, std::string::npos);
  const std::string request_id = archive.substr(id_start, id_end - id_start);

  EXPECT_NE(archive.find("WARC-Concurrent-To: " + request_id),
            std::string::npos);
}

TEST_F(WarcRecorderTest, RecordsCarryWarcinfoId) {
  auto recorder = MakeRecorder();
  recorder->WriteWarcinfo("out.warc");

  {
    auto exchange = recorder->CreateExchangeRecorder();
    exchange->SetTargetUrl(GURL("https://example.org/"));

    net::HttpRawRequestHeaders request_headers;
    request_headers.set_request_line("GET / HTTP/1.1\r\n");
    exchange->SetRequestHeaders(request_headers, "GET");
    exchange->SetResponseHeaders(MakeResponseHeaders("HTTP/1.1 200 OK\n\n"));
    exchange->Finish();
  }

  const std::string archive = FinishAndRead(std::move(recorder));

  // The warcinfo record is written first, so the first record id is its own.
  constexpr std::string_view kRecordIdField = "WARC-Record-ID: ";
  const size_t id_pos = archive.find(kRecordIdField);
  ASSERT_NE(id_pos, std::string::npos);
  const size_t id_start = id_pos + kRecordIdField.size();
  const size_t id_end = archive.find("\r\n", id_start);
  ASSERT_NE(id_end, std::string::npos);
  const std::string warcinfo_id = archive.substr(id_start, id_end - id_start);

  // Both the request and the response must be attributed to that capture.
  const std::string field = "WARC-Warcinfo-ID: " + warcinfo_id;
  const size_t first = archive.find(field);
  ASSERT_NE(first, std::string::npos);
  EXPECT_NE(archive.find(field, first + 1), std::string::npos);
}

TEST_F(WarcRecorderTest, PreservesCompressedBodyAndItsContentEncoding) {
  auto recorder = MakeRecorder();

  // Bytes that are not valid UTF-8, standing in for a gzip-compressed body.
  const std::vector<uint8_t> compressed = {0x1f, 0x8b, 0x08, 0x00,
                                           0x00, 0xff, 0x42};

  {
    auto exchange = recorder->CreateExchangeRecorder();
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
    auto exchange = recorder->CreateExchangeRecorder();
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
    auto exchange = recorder->CreateExchangeRecorder();
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
    auto exchange = recorder->CreateExchangeRecorder();
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
    auto exchange = recorder->CreateExchangeRecorder();
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
    auto exchange = recorder->CreateExchangeRecorder();
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
    auto exchange = recorder->CreateExchangeRecorder();
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
  auto exchange = recorder->CreateExchangeRecorder();
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
    auto exchange = recorder->CreateExchangeRecorder();
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

  // warcinfo, request, response — each in a member of its own, so a CDX index
  // built over this archive can point at a record and a reader can decompress
  // just that record.
  const std::optional<std::vector<std::string>> members =
      warc::InflateGzipMembers(archive);
  ASSERT_TRUE(members.has_value());
  ASSERT_EQ(3u, members->size());

  EXPECT_NE((*members)[0].find("WARC-Type: warcinfo"), std::string::npos);
  EXPECT_NE((*members)[0].find("WARC-Filename: out.warc.gz"),
            std::string::npos);

  EXPECT_NE((*members)[1].find("WARC-Type: request"), std::string::npos);
  EXPECT_NE((*members)[1].find("GET /page HTTP/1.1"), std::string::npos);

  EXPECT_NE((*members)[2].find("WARC-Type: response"), std::string::npos);
  EXPECT_NE((*members)[2].find("<html>hi</html>"), std::string::npos);
}

}  // namespace
}  // namespace network
