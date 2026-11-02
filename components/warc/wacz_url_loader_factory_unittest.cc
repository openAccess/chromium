// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/wacz_url_loader_factory.h"

#include <string>
#include <string_view>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/run_loop.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "components/warc/gzip_member_writer.h"
#include "components/warc/surt.h"
#include "components/warc/warc_record.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/test/test_url_loader_client.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/zlib/google/zip.h"
#include "url/gurl.h"

namespace warc {
namespace {

std::vector<uint8_t> ToBytes(std::string_view s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

std::vector<uint8_t> AsMember(base::span<const uint8_t> data) {
  std::vector<uint8_t> out;
  GzipMemberWriter writer(
      base::BindLambdaForTesting([&](base::span<const uint8_t> chunk) {
        out.insert(out.end(), chunk.begin(), chunk.end());
        return true;
      }));
  EXPECT_TRUE(writer.Write(data));
  EXPECT_TRUE(writer.Finish());
  return out;
}

class WaczUrlLoaderFactoryTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }

  void AddResponse(std::string_view url,
                   std::string_view head,
                   std::string_view body,
                   std::string_view timestamp = kTimestamp) {
    RecordHeader header;
    header.type = RecordType::kResponse;
    header.target_uri = std::string(url);
    std::vector<uint8_t> block = ToBytes(head);
    const std::vector<uint8_t> body_bytes = ToBytes(body);
    block.insert(block.end(), body_bytes.begin(), body_bytes.end());
    const std::vector<uint8_t> member = AsMember(
        SerializeRecord(header, block, head.size(), DigestAlgorithm::kSha1));

    index_ += base::StrCat(
        {ToSurt(GURL(std::string(url))), " ", timestamp, " {\"url\": \"", url,
         "\", \"status\": \"", head.substr(9, 3), "\", \"length\": \"",
         base::NumberToString(member.size()), "\", \"offset\": \"",
         base::NumberToString(archive_.size()),
         "\", \"filename\": \"t.warc.gz\"}\n"});
    archive_.insert(archive_.end(), member.begin(), member.end());
  }

  // Serves what was added, at `timestamp`. The archive stays open in
  // `open_archive_`, so a test can make it a second factory.
  mojo::Remote<network::mojom::URLLoaderFactory> BuildFactory(
      const std::string& timestamp = kTimestamp) {
    const base::FilePath content = temp_dir_.GetPath().AppendASCII("content");
    const base::FilePath archive_dir = content.AppendASCII("archive");
    const base::FilePath indexes = content.AppendASCII("indexes");
    EXPECT_TRUE(base::CreateDirectory(archive_dir));
    EXPECT_TRUE(base::CreateDirectory(indexes));
    EXPECT_TRUE(base::WriteFile(archive_dir.AppendASCII("t.warc.gz"),
                                base::span(archive_)));
    EXPECT_TRUE(base::WriteFile(indexes.AppendASCII("index.cdx.gz"),
                                base::span(AsMember(ToBytes(index_)))));

    const base::FilePath path = temp_dir_.GetPath().AppendASCII("c.wacz");
    EXPECT_TRUE(zip::Zip(content, path, /*include_hidden_files=*/false));

    open_archive_ = WaczArchive::Open(path);
    return mojo::Remote<network::mojom::URLLoaderFactory>(
        open_archive_->CreateFactory(timestamp));
  }

  // Issues `url` through `factory`. The loader is kept alive for as long as
  // the caller needs the answer; dropping it would cancel the request.
  void Start(mojo::Remote<network::mojom::URLLoaderFactory>& factory,
             std::string_view url,
             network::TestURLLoaderClient* client,
             std::string_view method = "GET",
             std::string_view range = "") {
    network::ResourceRequest request;
    request.url = GURL(std::string(url));
    request.method = std::string(method);
    if (!range.empty()) {
      request.headers.SetHeader(net::HttpRequestHeaders::kRange, range);
    }

    factory->CreateLoaderAndStart(
        loader_.InitWithNewPipeAndPassReceiver(), /*request_id=*/1,
        /*options=*/0, request, client->CreateRemote(),
        net::MutableNetworkTrafficAnnotationTag());
  }

  // Issues `url` and returns the body, draining the pipe as it is written.
  //
  // A body larger than the pipe is written in pieces, and each piece waits for
  // room -- so a test that waited for completion before reading anything would
  // wait for a write that is waiting for the test.
  std::string FetchBody(mojo::Remote<network::mojom::URLLoaderFactory>& factory,
                        std::string_view url,
                        network::TestURLLoaderClient* client,
                        std::string_view range = "") {
    Start(factory, url, client, "GET", range);
    client->RunUntilResponseReceived();
    if (!client->response_body().is_valid()) {
      client->RunUntilComplete();
      return std::string();
    }

    class Drainer : public mojo::DataPipeDrainer::Client {
     public:
      explicit Drainer(base::OnceClosure done) : done_(std::move(done)) {}
      void OnDataAvailable(base::span<const uint8_t> data) override {
        body.append(data.begin(), data.end());
      }
      void OnDataComplete() override { std::move(done_).Run(); }
      std::string body;

     private:
      base::OnceClosure done_;
    };

    base::RunLoop loop;
    Drainer drainer(loop.QuitClosure());
    mojo::DataPipeDrainer pipe_drainer(&drainer,
                                       client->response_body_release());
    loop.Run();
    client->RunUntilComplete();
    return drainer.body;
  }

  // Issues `url` and waits only for the outcome, for cases with no body.
  void FetchExpectingFailure(
      mojo::Remote<network::mojom::URLLoaderFactory>& factory,
      std::string_view url,
      network::TestURLLoaderClient* client,
      std::string_view method = "GET",
      std::string_view range = "") {
    Start(factory, url, client, method, range);
    client->RunUntilComplete();
  }

  mojo::PendingRemote<network::mojom::URLLoader> loader_;
  scoped_refptr<WaczArchive> open_archive_;

  static constexpr char kTimestamp[] = "20260101120000";

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir temp_dir_;
  std::vector<uint8_t> archive_;
  std::string index_;
};

TEST_F(WaczUrlLoaderFactoryTest, ServesAnArchivedPage) {
  AddResponse("https://example.org/page",
              "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
              "X-From-Capture: yes\r\n\r\n",
              "<html>archived</html>");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::TestURLLoaderClient client;
  const std::string body =
      FetchBody(factory, "https://example.org/page", &client);

  EXPECT_EQ(net::OK, client.completion_status().error_code);
  ASSERT_TRUE(client.response_head());
  EXPECT_EQ(200, client.response_head()->headers->response_code());
  EXPECT_EQ("text/html", client.response_head()->mime_type);
  EXPECT_EQ("utf-8", client.response_head()->charset);
  // The headers are the ones the site sent, not a summary of them, so a
  // replayed page sees what it saw when it was captured.
  EXPECT_EQ("yes", client.response_head()->headers->GetNormalizedHeader(
                       "X-From-Capture"));

  EXPECT_EQ("<html>archived</html>", body);
}

TEST_F(WaczUrlLoaderFactoryTest, ServesABodyLargerThanThePipe) {
  // Written in pieces as the renderer takes them, so a response bigger than
  // the pipe is not truncated at its capacity.
  const std::string big(300 * 1024, 'x');
  AddResponse("https://example.org/big",
              "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n", big);
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::TestURLLoaderClient client;
  const std::string body =
      FetchBody(factory, "https://example.org/big", &client);

  EXPECT_EQ(net::OK, client.completion_status().error_code);
  EXPECT_EQ(big.size(), body.size());
  EXPECT_EQ(big, body);
}

TEST_F(WaczUrlLoaderFactoryTest, AMissFailsRatherThanGoingToTheNetwork) {
  AddResponse("https://example.org/have", "HTTP/1.1 200 OK\r\n\r\n", "body");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  // The property the whole factory exists to guarantee. Anything else here --
  // an empty 200, a fall-through to the network -- would show an incomplete
  // archive as a complete page.
  network::TestURLLoaderClient client;
  FetchExpectingFailure(factory, "https://example.org/have-not", &client);
  EXPECT_EQ(net::ERR_FILE_NOT_FOUND, client.completion_status().error_code);
  EXPECT_FALSE(client.response_head());
}

TEST_F(WaczUrlLoaderFactoryTest, ServesTheSamePageHoweverTheUrlIsWritten) {
  AddResponse("https://example.org/a?b=2&a=1", "HTTP/1.1 200 OK\r\n\r\n",
              "body");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::TestURLLoaderClient client;
  EXPECT_EQ("body", FetchBody(factory, "http://www.example.org/a/?a=1&b=2#frag",
                              &client));
  EXPECT_EQ(net::OK, client.completion_status().error_code);
}

TEST_F(WaczUrlLoaderFactoryTest, OnlyWhatACaptureCouldHaveStored) {
  AddResponse("https://example.org/p", "HTTP/1.1 200 OK\r\n\r\n", "body");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  // A capture stores replies to requests the browser made. There is no stored
  // reply to a request nobody made, and inventing one would be a guess.
  network::TestURLLoaderClient client;
  FetchExpectingFailure(factory, "https://example.org/p", &client, "POST");
  EXPECT_EQ(net::ERR_FILE_NOT_FOUND, client.completion_status().error_code);
}

TEST_F(WaczUrlLoaderFactoryTest, AnArchivedRedirectIsServedAsARedirect) {
  AddResponse("https://example.org/old",
              "HTTP/1.1 301 Moved Permanently\r\n"
              "Location: https://example.org/new\r\n\r\n",
              "");
  AddResponse("https://example.org/new", "HTTP/1.1 200 OK\r\n\r\n", "arrived");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  // Handed back as the redirect it was, for whoever asked to follow -- and
  // wherever it points is served out of the same archive, so nothing here has
  // to chase it.
  network::TestURLLoaderClient client;
  FetchBody(factory, "https://example.org/old", &client);
  EXPECT_EQ(net::OK, client.completion_status().error_code);
  ASSERT_TRUE(client.response_head());
  EXPECT_EQ(301, client.response_head()->headers->response_code());
  EXPECT_EQ("https://example.org/new",
            client.response_head()->headers->GetNormalizedHeader("Location"));
}

TEST_F(WaczUrlLoaderFactoryTest, ANavigationMissLandsOnAPageSayingSo) {
  AddResponse("https://example.org/have", "HTTP/1.1 200 OK\r\n\r\n", "body");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::TestURLLoaderClient client;
  network::ResourceRequest request;
  request.url = GURL("https://example.org/gone");
  request.method = "GET";
  request.destination = network::mojom::RequestDestination::kDocument;
  factory->CreateLoaderAndStart(loader_.InitWithNewPipeAndPassReceiver(),
                                /*request_id=*/1,
                                /*options=*/0, request, client.CreateRemote(),
                                net::MutableNetworkTrafficAnnotationTag());

  client.RunUntilResponseReceived();
  ASSERT_TRUE(client.response_head());
  // A document has nowhere to show a failure, so it gets a page that says
  // which URL the archive lacks rather than an empty error.
  EXPECT_EQ(404, client.response_head()->headers->response_code());
  EXPECT_EQ("text/html", client.response_head()->mime_type);

  std::string body;
  base::RunLoop loop;
  class Drain : public mojo::DataPipeDrainer::Client {
   public:
    Drain(std::string* out, base::OnceClosure done)
        : out_(out), done_(std::move(done)) {}
    void OnDataAvailable(base::span<const uint8_t> data) override {
      out_->append(data.begin(), data.end());
    }
    void OnDataComplete() override { std::move(done_).Run(); }

   private:
    raw_ptr<std::string> out_;
    base::OnceClosure done_;
  } drain(&body, loop.QuitClosure());
  mojo::DataPipeDrainer drainer(&drain, client.response_body_release());
  loop.Run();
  client.RunUntilComplete();

  EXPECT_EQ(net::OK, client.completion_status().error_code);
  EXPECT_NE(std::string::npos, body.find("https://example.org/gone"));
  EXPECT_NE(std::string::npos, body.find("Not in this archive"));
}

TEST_F(WaczUrlLoaderFactoryTest, HeadersThatWouldOutliveTheVisitAreDropped) {
  AddResponse("https://example.org/p",
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/html\r\n"
              "Strict-Transport-Security: max-age=31536000\r\n"
              "Clear-Site-Data: \"storage\"\r\n"
              "Report-To: {\"endpoints\":[{\"url\":\"https://collector\"}]}\r\n"
              "Content-Security-Policy-Report-Only: default-src 'none'; "
              "report-uri https://collector\r\n"
              "Content-Security-Policy: default-src 'self'\r\n\r\n",
              "<html>page</html>");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::TestURLLoaderClient client;
  FetchBody(factory, "https://example.org/p", &client);
  ASSERT_TRUE(client.response_head());
  const net::HttpResponseHeaders& headers = *client.response_head()->headers;

  // Each of these reaches past the page: one rewrites later navigations to
  // the host, one would clear the storage the replay is using, and the
  // reporting ones would have a visit to an archive make requests to a
  // collector that has nothing to do with it.
  EXPECT_FALSE(headers.HasHeader("Strict-Transport-Security"));
  EXPECT_FALSE(headers.HasHeader("Clear-Site-Data"));
  EXPECT_FALSE(headers.HasHeader("Report-To"));
  EXPECT_FALSE(headers.HasHeader("Content-Security-Policy-Report-Only"));

  // The enforcing policy stays: the page had it when it was captured, and
  // replaying the page means replaying what constrained it.
  EXPECT_EQ("default-src 'self'",
            headers.GetNormalizedHeader("Content-Security-Policy"));
  EXPECT_EQ("text/html", client.response_head()->mime_type);
}

// The moment belongs to the factory, not to the archive: two tabs replaying
// one archive weeks apart each see their own capture of the same URL, out of
// the one container opened once.
TEST_F(WaczUrlLoaderFactoryTest, EachFactoryServesItsOwnMoment) {
  constexpr char kHead[] = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n";
  AddResponse("https://example.org/news", kHead, "<html>march</html>",
              "20260301120000");
  AddResponse("https://example.org/news", kHead, "<html>april</html>",
              "20260401120000");

  mojo::Remote<network::mojom::URLLoaderFactory> march =
      BuildFactory("20260301120000");
  // Written as a page list writes it, as a tab opened from one will be.
  mojo::Remote<network::mojom::URLLoaderFactory> april(
      open_archive_->CreateFactory("2026-04-01T12:00:00Z"));

  network::TestURLLoaderClient march_client;
  EXPECT_EQ("<html>march</html>",
            FetchBody(march, "https://example.org/news", &march_client));
  loader_.reset();

  network::TestURLLoaderClient april_client;
  EXPECT_EQ("<html>april</html>",
            FetchBody(april, "https://example.org/news", &april_client));
  loader_.reset();

  // And asking the second did not move the first.
  network::TestURLLoaderClient again_client;
  EXPECT_EQ("<html>march</html>",
            FetchBody(march, "https://example.org/news", &again_client));
}

// A media player asks for the bytes it wants, and appends what it is given to
// its buffer. Handed the whole file instead, it appends the wrong bytes.
TEST_F(WaczUrlLoaderFactoryTest, ARangeIsAnsweredWithThoseBytes) {
  AddResponse("https://example.org/v.mp4",
              "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\n"
              "Content-Length: 10\r\nAccept-Ranges: bytes\r\n\r\n",
              "0123456789");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::TestURLLoaderClient client;
  EXPECT_EQ("2345", FetchBody(factory, "https://example.org/v.mp4", &client,
                              "bytes=2-5"));
  EXPECT_EQ(net::OK, client.completion_status().error_code);
  const net::HttpResponseHeaders& headers = *client.response_head()->headers;
  EXPECT_EQ(206, headers.response_code());
  EXPECT_EQ("bytes 2-5/10", headers.GetNormalizedHeader("Content-Range"));
  EXPECT_EQ("4", headers.GetNormalizedHeader("Content-Length"));
  EXPECT_EQ(4, client.response_head()->content_length);
  EXPECT_EQ("video/mp4", client.response_head()->mime_type);
  loader_.reset();

  // To the end, and the last few bytes, as a player reading an index asks.
  network::TestURLLoaderClient to_end;
  EXPECT_EQ("789", FetchBody(factory, "https://example.org/v.mp4", &to_end,
                             "bytes=7-"));
  EXPECT_EQ(
      "bytes 7-9/10",
      to_end.response_head()->headers->GetNormalizedHeader("Content-Range"));
  loader_.reset();

  network::TestURLLoaderClient suffix;
  EXPECT_EQ("6789", FetchBody(factory, "https://example.org/v.mp4", &suffix,
                              "bytes=-4"));
  EXPECT_EQ(
      "bytes 6-9/10",
      suffix.response_head()->headers->GetNormalizedHeader("Content-Range"));
}

TEST_F(WaczUrlLoaderFactoryTest, ARangeTheResourceDoesNotHaveIsRefused) {
  AddResponse("https://example.org/v.mp4", "HTTP/1.1 200 OK\r\n\r\n",
              "0123456789");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::TestURLLoaderClient client;
  EXPECT_EQ("", FetchBody(factory, "https://example.org/v.mp4", &client,
                          "bytes=20-"));
  const net::HttpResponseHeaders& headers = *client.response_head()->headers;
  EXPECT_EQ(416, headers.response_code());
  EXPECT_EQ("bytes */10", headers.GetNormalizedHeader("Content-Range"));
}

// The archive holds the partial response a player was given live and, fetched
// after it, the whole file. The whole file answers whatever is asked of it
// now; the part only what was asked of it then.
TEST_F(WaczUrlLoaderFactoryTest, TheWholeIsServedRatherThanANearerPart) {
  AddResponse("https://example.org/v.mp4",
              "HTTP/1.1 206 Partial Content\r\n"
              "Content-Range: bytes 0-1/10\r\n\r\n",
              "01", "20260101120000");
  AddResponse("https://example.org/v.mp4", "HTTP/1.1 200 OK\r\n\r\n",
              "0123456789", "20260101120005");
  mojo::Remote<network::mojom::URLLoaderFactory> factory =
      BuildFactory("20260101120000");

  network::TestURLLoaderClient whole;
  EXPECT_EQ("0123456789",
            FetchBody(factory, "https://example.org/v.mp4", &whole));
  EXPECT_EQ(200, whole.response_head()->headers->response_code());
  loader_.reset();

  network::TestURLLoaderClient part;
  EXPECT_EQ("456", FetchBody(factory, "https://example.org/v.mp4", &part,
                             "bytes=4-6"));
  EXPECT_EQ(206, part.response_head()->headers->response_code());
}

// Where only a part was captured, it answers for the bytes it holds, and the
// rest is missing -- not a slice of something else passed off as them.
TEST_F(WaczUrlLoaderFactoryTest, APartAnswersOnlyForTheBytesItHolds) {
  AddResponse("https://example.org/v.mp4",
              "HTTP/1.1 206 Partial Content\r\n"
              "Content-Range: bytes 2-5/10\r\n\r\n",
              "2345");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::TestURLLoaderClient inside;
  EXPECT_EQ("34", FetchBody(factory, "https://example.org/v.mp4", &inside,
                            "bytes=3-4"));
  EXPECT_EQ(
      "bytes 3-4/10",
      inside.response_head()->headers->GetNormalizedHeader("Content-Range"));
  loader_.reset();

  network::TestURLLoaderClient outside;
  FetchExpectingFailure(factory, "https://example.org/v.mp4", &outside, "GET",
                        "bytes=4-8");
  EXPECT_EQ(net::ERR_FILE_NOT_FOUND, outside.completion_status().error_code);
}

// Several ranges at once may be answered with the whole resource, and are.
TEST_F(WaczUrlLoaderFactoryTest, SeveralRangesAreAnsweredWithTheWhole) {
  AddResponse("https://example.org/v.mp4", "HTTP/1.1 200 OK\r\n\r\n",
              "0123456789");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::TestURLLoaderClient client;
  EXPECT_EQ("0123456789", FetchBody(factory, "https://example.org/v.mp4",
                                    &client, "bytes=0-1,5-6"));
  EXPECT_EQ(200, client.response_head()->headers->response_code());
}

// A redirect is what the server said whatever range was asked for.
TEST_F(WaczUrlLoaderFactoryTest, ARangedRequestForARedirectGetsTheRedirect) {
  AddResponse("https://example.org/old",
              "HTTP/1.1 302 Found\r\nLocation: https://example.org/new\r\n\r\n",
              "");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::TestURLLoaderClient client;
  FetchBody(factory, "https://example.org/old", &client, "bytes=0-1");
  ASSERT_TRUE(client.response_head());
  EXPECT_EQ(302, client.response_head()->headers->response_code());
}

TEST_F(WaczUrlLoaderFactoryTest, AnUnreadableArchiveServesNothing) {
  const base::FilePath path = temp_dir_.GetPath().AppendASCII("not.wacz");
  ASSERT_TRUE(base::WriteFile(path, "this is not a container"));
  scoped_refptr<WaczArchive> archive = WaczArchive::Open(path);
  mojo::Remote<network::mojom::URLLoaderFactory> factory(
      archive->CreateFactory(kTimestamp));

  // Whether a container can be read is only known once it has been, which is
  // not something the caller can wait for -- so it shows up as every request
  // failing rather than as a factory that was never made.
  network::TestURLLoaderClient client;
  FetchExpectingFailure(factory, "https://example.org/p", &client);
  EXPECT_EQ(net::ERR_FILE_NOT_FOUND, client.completion_status().error_code);
}

}  // namespace
}  // namespace warc
