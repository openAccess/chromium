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
                   std::string_view body) {
    RecordHeader header;
    header.type = RecordType::kResponse;
    header.target_uri = std::string(url);
    std::vector<uint8_t> block = ToBytes(head);
    const std::vector<uint8_t> body_bytes = ToBytes(body);
    block.insert(block.end(), body_bytes.begin(), body_bytes.end());
    const std::vector<uint8_t> member = AsMember(
        SerializeRecord(header, block, head.size(), DigestAlgorithm::kSha1));

    index_ += base::StrCat(
        {ToSurt(GURL(std::string(url))), " ", kTimestamp, " {\"url\": \"", url,
         "\", \"length\": \"", base::NumberToString(member.size()),
         "\", \"offset\": \"", base::NumberToString(archive_.size()),
         "\", \"filename\": \"t.warc.gz\"}\n"});
    archive_.insert(archive_.end(), member.begin(), member.end());
  }

  mojo::Remote<network::mojom::URLLoaderFactory> BuildFactory() {
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

    open_archive_ = WaczArchive::Open(
        base::File(path, base::File::FLAG_OPEN | base::File::FLAG_READ),
        kTimestamp);
    return mojo::Remote<network::mojom::URLLoaderFactory>(
        open_archive_->CreateFactory());
  }

  // Issues `url` through `factory`. The loader is kept alive for as long as
  // the caller needs the answer; dropping it would cancel the request.
  void Start(mojo::Remote<network::mojom::URLLoaderFactory>& factory,
             std::string_view url,
             network::TestURLLoaderClient* client,
             std::string_view method = "GET") {
    network::ResourceRequest request;
    request.url = GURL(std::string(url));
    request.method = std::string(method);

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
                        network::TestURLLoaderClient* client) {
    Start(factory, url, client);
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
      std::string_view method = "GET") {
    Start(factory, url, client, method);
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

TEST_F(WaczUrlLoaderFactoryTest, AnUnreadableArchiveServesNothing) {
  const base::FilePath path = temp_dir_.GetPath().AppendASCII("not.wacz");
  ASSERT_TRUE(base::WriteFile(path, "this is not a container"));
  scoped_refptr<WaczArchive> archive = WaczArchive::Open(
      base::File(path, base::File::FLAG_OPEN | base::File::FLAG_READ),
      kTimestamp);
  mojo::Remote<network::mojom::URLLoaderFactory> factory(
      archive->CreateFactory());

  // Whether a container can be read is only known once it has been, which is
  // not something the caller can wait for -- so it shows up as every request
  // failing rather than as a factory that was never made.
  network::TestURLLoaderClient client;
  FetchExpectingFailure(factory, "https://example.org/p", &client);
  EXPECT_EQ(net::ERR_FILE_NOT_FOUND, client.completion_status().error_code);
}

}  // namespace
}  // namespace warc
