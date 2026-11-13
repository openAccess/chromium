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
#include "components/warc/cdxj.h"
#include "components/warc/gzip_member_writer.h"
#include "components/warc/request_key.h"
#include "components/warc/surt.h"
#include "components/warc/warc_record.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/url_request/url_request.h"
#include "services/network/public/cpp/http_request_headers_update_params.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/resource_request_body.h"
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
    AddResponseUnder(ToSurt(GURL(std::string(url))), url, head, body,
                     timestamp);
  }

  // As AddResponse(), filed in the index under `key` -- which is how a
  // request other than a GET is filed, by what it sent as well as its URL.
  void AddResponseUnder(std::string_view key,
                        std::string_view url,
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
        {key, " ", timestamp, " {\"url\": \"", url, "\", \"status\": \"",
         head.substr(9, 3), "\", \"length\": \"",
         base::NumberToString(member.size()), "\", \"offset\": \"",
         base::NumberToString(archive_.size()),
         "\", \"filename\": \"t.warc.gz\"}\n"});
    archive_.insert(archive_.end(), member.begin(), member.end());
  }

  // Serves what was added, at `timestamp`. The archive stays open in
  // `open_archive_`, so a test can make it a second factory.
  mojo::Remote<network::mojom::URLLoaderFactory> BuildFactory(
      const std::string& timestamp = kTimestamp,
      ReplayReporter reporter = ReplayReporter()) {
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
        open_archive_->CreateFactory(timestamp, std::move(reporter)));
  }

  // A reporter that keeps what it is told in `outcomes_`.
  ReplayReporter KeepOutcomes() {
    return base::BindRepeating(
        [](std::vector<ReplayOutcome>* outcomes, const ReplayOutcome& outcome) {
          outcomes->push_back(outcome);
        },
        base::Unretained(&outcomes_));
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
    Start(factory, request, client);
  }

  void Start(mojo::Remote<network::mojom::URLLoaderFactory>& factory,
             const network::ResourceRequest& request,
             network::TestURLLoaderClient* client) {
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
    return ReadBody(client);
  }

  std::string FetchBody(mojo::Remote<network::mojom::URLLoaderFactory>& factory,
                        const network::ResourceRequest& request,
                        network::TestURLLoaderClient* client) {
    Start(factory, request, client);
    return ReadBody(client);
  }

  // Follows the redirect `client` was handed, as a client that wants to go
  // where it points does.
  void Follow(network::TestURLLoaderClient* client) {
    mojo::Remote<network::mojom::URLLoader> loader(std::move(loader_));
    loader->FollowRedirect(network::HttpRequestHeadersUpdateParams(),
                           std::nullopt);
    loader_ = loader.Unbind();
    client->ClearHasReceivedRedirect();
  }

  std::string ReadBody(network::TestURLLoaderClient* client) {
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
  std::vector<ReplayOutcome> outcomes_;

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

// A GraphQL API takes every query at one URL, and is told them apart by what
// each one sent.
TEST_F(WaczUrlLoaderFactoryTest, APostIsAnsweredByWhatItSent) {
  const GURL api("https://example.org/graphql");
  AddResponseUnder(
      RequestKey(api, "POST", "application/json", R"({"query":"a"})"),
      api.spec(), "HTTP/1.1 200 OK\r\n\r\n", "answer to a");
  AddResponseUnder(
      RequestKey(api, "POST", "application/json", R"({"query":"b"})"),
      api.spec(), "HTTP/1.1 200 OK\r\n\r\n", "answer to b");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::ResourceRequest request;
  request.url = api;
  request.method = "POST";
  request.headers.SetHeader(net::HttpRequestHeaders::kContentType,
                            "application/json");
  request.request_body = network::ResourceRequestBody::CreateFromCopyOfBytes(
      base::as_byte_span(std::string_view(R"({"query":"b"})")));

  network::TestURLLoaderClient client;
  EXPECT_EQ("answer to b", FetchBody(factory, request, &client));
  EXPECT_EQ(net::OK, client.completion_status().error_code);
}

// Pages ask for a HEAD to see whether something is there, and a capture
// seldom holds one. The GET's headers are what the HEAD would have been told.
TEST_F(WaczUrlLoaderFactoryTest, AHeadIsAnsweredWithTheHeadersOfAGet) {
  AddResponse("https://example.org/ads.js",
              "HTTP/1.1 200 OK\r\nContent-Type: text/javascript\r\n\r\n",
              "var ads = 1;");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::ResourceRequest request;
  request.url = GURL("https://example.org/ads.js");
  request.method = "HEAD";
  network::TestURLLoaderClient client;
  EXPECT_EQ("", FetchBody(factory, request, &client));
  EXPECT_EQ(net::OK, client.completion_status().error_code);
  ASSERT_TRUE(client.response_head());
  EXPECT_EQ(200, client.response_head()->headers->response_code());
  EXPECT_EQ("text/javascript", client.response_head()->mime_type);
}

// A script fetching its data with a new cache-buster each time gets the data
// the capture fetched with the old one.
TEST_F(WaczUrlLoaderFactoryTest, ASubresourceIsAnsweredByTheClosestCapture) {
  AddResponse("https://example.org/data.json?cb=111&v=2",
              "HTTP/1.1 200 OK\r\n\r\n", "data");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::TestURLLoaderClient client;
  EXPECT_EQ(
      "data",
      FetchBody(factory, "https://example.org/data.json?cb=999&v=2", &client));
  EXPECT_EQ(net::OK, client.completion_status().error_code);
}

// A visit to a page the archive lacks says so, rather than landing on
// whichever page lies nearest. So does a frame, in the way a frame can.
TEST_F(WaczUrlLoaderFactoryTest, APageIsNotAnsweredByAnother) {
  AddResponse("https://example.org/article?id=1",
              "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n",
              "<html>one</html>");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::ResourceRequest request;
  request.url = GURL("https://example.org/article?id=2");
  request.method = "GET";
  request.destination = network::mojom::RequestDestination::kDocument;
  {
    network::TestURLLoaderClient client;
    const std::string body = FetchBody(factory, request, &client);
    ASSERT_TRUE(client.response_head());
    EXPECT_EQ(404, client.response_head()->headers->response_code());
    EXPECT_NE(std::string::npos, body.find("Not in this archive"));
  }

  request.destination = network::mojom::RequestDestination::kIframe;
  {
    network::TestURLLoaderClient client;
    Start(factory, request, &client);
    client.RunUntilComplete();
    EXPECT_EQ(net::ERR_FILE_NOT_FOUND, client.completion_status().error_code);
  }
}

// A frame a page opens with an id it made for the visit -- where the capture
// opened it before there was one -- is the frame the capture holds, and is
// served from it. So is a page, and each says which capture answered.
TEST_F(WaczUrlLoaderFactoryTest, AFrameWithAnIdMadeForTheVisitIsServed) {
  AddResponse("https://example.org/consent?site=7&uuid=null",
              "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n",
              "<html>consent</html>");
  mojo::Remote<network::mojom::URLLoaderFactory> factory =
      BuildFactory(kTimestamp, KeepOutcomes());

  network::ResourceRequest request;
  request.url = GURL(
      "https://example.org/consent?site=7&"
      "uuid=50b0c9e2-bcca-4206-a3b7-068d73120f22");
  request.method = "GET";
  for (const auto destination : {network::mojom::RequestDestination::kIframe,
                                 network::mojom::RequestDestination::kDocument}) {
    request.destination = destination;
    network::TestURLLoaderClient client;
    EXPECT_EQ("<html>consent</html>", FetchBody(factory, request, &client));
    EXPECT_EQ(net::OK, client.completion_status().error_code);
  }

  ASSERT_EQ(2u, outcomes_.size());
  for (const ReplayOutcome& outcome : outcomes_) {
    EXPECT_FALSE(outcome.missing);
    EXPECT_EQ(request.url, outcome.url);
    EXPECT_EQ(GURL("https://example.org/consent?site=7&uuid=null"),
              outcome.captured_url);
  }
}

// Handed over as the redirect it was, as the server handed it over, so that
// the client can check it and follow it: where a frame or a script ends up is
// its URL and its origin. Served as a response that merely said 301, it was
// taken for the end of the request, and a frame given one failed to load.
TEST_F(WaczUrlLoaderFactoryTest,
       AnArchivedRedirectIsFollowedThroughTheArchive) {
  AddResponse("https://example.org/old",
              "HTTP/1.1 301 Moved Permanently\r\n"
              "Location: /new\r\n\r\n",
              "moved");
  AddResponse("https://example.org/new", "HTTP/1.1 200 OK\r\n\r\n", "arrived");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::TestURLLoaderClient client;
  Start(factory, "https://example.org/old", &client);
  client.RunUntilRedirectReceived();
  EXPECT_EQ(301, client.redirect_info().status_code);
  EXPECT_EQ(GURL("https://example.org/new"), client.redirect_info().new_url);
  EXPECT_EQ(301, client.response_head()->headers->response_code());

  Follow(&client);
  EXPECT_EQ("arrived", ReadBody(&client));
  EXPECT_EQ(net::OK, client.completion_status().error_code);
  EXPECT_EQ(200, client.response_head()->headers->response_code());
}

// A form posted and answered with "see other" is sent on as a GET, and it is
// the GET the archive holds an answer to.
TEST_F(WaczUrlLoaderFactoryTest, ARedirectedPostIsFollowedAsAGet) {
  const GURL login("https://example.org/login");
  AddResponseUnder(
      RequestKey(login, "POST", "application/x-www-form-urlencoded", "u=a"),
      login.spec(),
      "HTTP/1.1 303 See Other\r\nLocation: https://example.org/home\r\n\r\n",
      "");
  AddResponse("https://example.org/home", "HTTP/1.1 200 OK\r\n\r\n", "home");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::ResourceRequest request;
  request.url = login;
  request.method = "POST";
  request.headers.SetHeader(net::HttpRequestHeaders::kContentType,
                            "application/x-www-form-urlencoded");
  request.request_body = network::ResourceRequestBody::CreateFromCopyOfBytes(
      base::as_byte_span(std::string_view("u=a")));

  network::TestURLLoaderClient client;
  Start(factory, request, &client);
  client.RunUntilRedirectReceived();
  EXPECT_EQ("GET", client.redirect_info().new_method);

  Follow(&client);
  EXPECT_EQ("home", ReadBody(&client));
}

// Captures can send a request round in a circle no server did -- two
// redirects captured at different moments, or found as each other's nearest.
TEST_F(WaczUrlLoaderFactoryTest, ARedirectLoopEndsWhereTheNetworkWouldEndIt) {
  AddResponse("https://example.org/a",
              "HTTP/1.1 302 Found\r\nLocation: /b\r\n\r\n", "");
  AddResponse("https://example.org/b",
              "HTTP/1.1 302 Found\r\nLocation: /a\r\n\r\n", "");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::TestURLLoaderClient client;
  Start(factory, "https://example.org/a", &client);
  for (int i = 0; i < net::URLRequest::kMaxRedirects; ++i) {
    client.RunUntilRedirectReceived();
    Follow(&client);
  }
  client.RunUntilComplete();
  EXPECT_FALSE(client.has_received_redirect());
  EXPECT_EQ(net::ERR_TOO_MANY_REDIRECTS, client.completion_status().error_code);
}

TEST_F(WaczUrlLoaderFactoryTest, APageRedirectedOutOfTheArchiveSaysWhereTo) {
  AddResponse(
      "https://example.org/old",
      "HTTP/1.1 302 Found\r\nLocation: https://example.org/gone\r\n\r\n", "");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::ResourceRequest request;
  request.url = GURL("https://example.org/old");
  request.destination = network::mojom::RequestDestination::kDocument;
  network::TestURLLoaderClient client;
  Start(factory, request, &client);
  client.RunUntilRedirectReceived();

  Follow(&client);
  const std::string body = ReadBody(&client);
  EXPECT_NE(std::string::npos, body.find("https://example.org/gone"));
  EXPECT_EQ(404, client.response_head()->headers->response_code());
}

// Nothing an archive holds came from anywhere but the web.
TEST_F(WaczUrlLoaderFactoryTest, ARedirectOffTheWebIsNotFollowed) {
  AddResponse("https://example.org/old",
              "HTTP/1.1 302 Found\r\nLocation: data:text/html,hi\r\n\r\n", "");
  mojo::Remote<network::mojom::URLLoaderFactory> factory = BuildFactory();

  network::TestURLLoaderClient client;
  Start(factory, "https://example.org/old", &client);
  client.RunUntilRedirectReceived();

  Follow(&client);
  client.RunUntilComplete();
  EXPECT_EQ(net::ERR_UNSAFE_REDIRECT, client.completion_status().error_code);
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
  Start(factory, "https://example.org/old", &client, "GET", "bytes=0-1");
  client.RunUntilRedirectReceived();
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

// What each request was answered with is told as it is answered: the stored
// status, type and size, and when the capture that answered was taken.
TEST_F(WaczUrlLoaderFactoryTest, TellsWhatEachRequestWasAnsweredWith) {
  AddResponse("https://example.org/",
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: Text/HTML; charset=utf-8\r\n\r\n",
              "<html>page</html>", "20260101115959");
  mojo::Remote<network::mojom::URLLoaderFactory> factory =
      BuildFactory(kTimestamp, KeepOutcomes());

  network::ResourceRequest request;
  request.url = GURL("https://example.org/");
  request.method = "GET";
  request.destination = network::mojom::RequestDestination::kDocument;
  network::TestURLLoaderClient client;
  EXPECT_EQ("<html>page</html>", FetchBody(factory, request, &client));

  ASSERT_EQ(1u, outcomes_.size());
  const ReplayOutcome& outcome = outcomes_[0];
  EXPECT_EQ(GURL("https://example.org/"), outcome.url);
  EXPECT_EQ("GET", outcome.method);
  EXPECT_EQ("document", outcome.destination);
  EXPECT_FALSE(outcome.missing);
  EXPECT_EQ(net::OK, outcome.net_error);
  EXPECT_EQ(200, outcome.status_code);
  EXPECT_EQ("text/html", outcome.mime_type);
  EXPECT_EQ(17u, outcome.body_bytes);
  EXPECT_EQ(ParseIndexTimestamp("20260101115959"), outcome.captured);
  EXPECT_TRUE(outcome.captured_url.is_empty());
  EXPECT_FALSE(outcome.time.is_null());
}

// A request the archive cannot answer is told as missing, whether the page is
// shown a page saying so or the request simply fails.
TEST_F(WaczUrlLoaderFactoryTest, TellsOfARequestTheArchiveCannotAnswer) {
  AddResponse("https://example.org/", "HTTP/1.1 200 OK\r\n\r\n", "page");
  mojo::Remote<network::mojom::URLLoaderFactory> factory =
      BuildFactory(kTimestamp, KeepOutcomes());

  network::ResourceRequest request;
  request.url = GURL("https://example.org/gone");
  request.method = "GET";
  request.destination = network::mojom::RequestDestination::kDocument;
  {
    network::TestURLLoaderClient client;
    FetchBody(factory, request, &client);
  }
  request.url = GURL("https://example.org/gone.js");
  request.destination = network::mojom::RequestDestination::kScript;
  {
    network::TestURLLoaderClient client;
    Start(factory, request, &client);
    client.RunUntilComplete();
  }

  ASSERT_EQ(2u, outcomes_.size());
  EXPECT_EQ(GURL("https://example.org/gone"), outcomes_[0].url);
  EXPECT_EQ("document", outcomes_[0].destination);
  EXPECT_TRUE(outcomes_[0].missing);
  EXPECT_EQ(0, outcomes_[0].status_code);
  EXPECT_EQ(GURL("https://example.org/gone.js"), outcomes_[1].url);
  EXPECT_EQ("script", outcomes_[1].destination);
  EXPECT_TRUE(outcomes_[1].missing);
}

// Answered by a capture of other words, the request says which: the closest
// capture of a subresource is not what was asked for, and may not be what the
// page wanted.
TEST_F(WaczUrlLoaderFactoryTest, TellsWhichCaptureAnsweredInOtherWords) {
  AddResponse("https://example.org/data.json?cb=111&v=2",
              "HTTP/1.1 200 OK\r\n\r\n", "data");
  mojo::Remote<network::mojom::URLLoaderFactory> factory =
      BuildFactory(kTimestamp, KeepOutcomes());

  network::TestURLLoaderClient client;
  EXPECT_EQ(
      "data",
      FetchBody(factory, "https://example.org/data.json?cb=999&v=2", &client));

  ASSERT_EQ(1u, outcomes_.size());
  EXPECT_EQ(GURL("https://example.org/data.json?cb=999&v=2"), outcomes_[0].url);
  EXPECT_EQ(GURL("https://example.org/data.json?cb=111&v=2"),
            outcomes_[0].captured_url);
  EXPECT_FALSE(outcomes_[0].missing);
}

// A redirect is an answer of its own, and the request it sends the page on to
// is another, as each would be on the network.
TEST_F(WaczUrlLoaderFactoryTest, TellsOfARedirectAndWhereItLed) {
  AddResponse("https://example.org/old",
              "HTTP/1.1 301 Moved Permanently\r\n"
              "Location: /new\r\n\r\n",
              "");
  AddResponse("https://example.org/new", "HTTP/1.1 200 OK\r\n\r\n", "arrived");
  mojo::Remote<network::mojom::URLLoaderFactory> factory =
      BuildFactory(kTimestamp, KeepOutcomes());

  network::TestURLLoaderClient client;
  Start(factory, "https://example.org/old", &client);
  client.RunUntilRedirectReceived();
  Follow(&client);
  EXPECT_EQ("arrived", ReadBody(&client));

  ASSERT_EQ(2u, outcomes_.size());
  EXPECT_EQ(GURL("https://example.org/old"), outcomes_[0].url);
  EXPECT_EQ(301, outcomes_[0].status_code);
  EXPECT_EQ(GURL("https://example.org/new"), outcomes_[1].url);
  EXPECT_EQ(200, outcomes_[1].status_code);
}

// A redirect loop the replay ends is told as the failure it is, after the
// redirects that made it.
TEST_F(WaczUrlLoaderFactoryTest, TellsOfARedirectLoopItEnded) {
  AddResponse("https://example.org/loop",
              "HTTP/1.1 302 Found\r\nLocation: /loop\r\n\r\n", "");
  mojo::Remote<network::mojom::URLLoaderFactory> factory =
      BuildFactory(kTimestamp, KeepOutcomes());

  network::TestURLLoaderClient client;
  Start(factory, "https://example.org/loop", &client);
  for (int i = 0; i < net::URLRequest::kMaxRedirects; ++i) {
    client.RunUntilRedirectReceived();
    Follow(&client);
  }
  client.RunUntilComplete();
  ASSERT_EQ(net::ERR_TOO_MANY_REDIRECTS, client.completion_status().error_code);

  ASSERT_FALSE(outcomes_.empty());
  EXPECT_EQ(net::ERR_TOO_MANY_REDIRECTS, outcomes_.back().net_error);
  EXPECT_EQ(302, outcomes_[outcomes_.size() - 2].status_code);
}

// The miss reported for `request`, which the archive is expected to miss.
// Kept apart from the tests so that each says only what it asks and what it
// expects to be told.
class WaczUrlLoaderFactoryMissTest : public WaczUrlLoaderFactoryTest {
 protected:
  MissExplanation MissFor(const network::ResourceRequest& request,
                          std::string_view range = "") {
    mojo::Remote<network::mojom::URLLoaderFactory> factory =
        BuildFactory(kTimestamp, KeepOutcomes());
    network::ResourceRequest ranged = request;
    if (!range.empty()) {
      ranged.headers.SetHeader(net::HttpRequestHeaders::kRange, range);
    }
    network::TestURLLoaderClient client;
    Start(factory, ranged, &client);
    client.RunUntilComplete();
    EXPECT_EQ(1u, outcomes_.size());
    if (outcomes_.empty()) {
      return MissExplanation();
    }
    EXPECT_TRUE(outcomes_[0].missing);
    return outcomes_[0].miss;
  }

  static network::ResourceRequest Get(
      std::string_view url,
      network::mojom::RequestDestination destination =
          network::mojom::RequestDestination::kScript) {
    network::ResourceRequest request;
    request.url = GURL(std::string(url));
    request.method = "GET";
    request.destination = destination;
    return request;
  }
};

// A host the capture never reached is the replay asking for something new:
// nothing about matching could have found it.
TEST_F(WaczUrlLoaderFactoryMissTest, TellsOfAHostNeverCaptured) {
  // Keyed "net,example,ads)" and "net,examples)": each begins with the
  // "net,example" of the host asked for, and neither is it.
  AddResponse("https://ads.example.net/a.js", "HTTP/1.1 200 OK\r\n\r\n",
              "x");
  AddResponse("https://examples.net/a.js", "HTTP/1.1 200 OK\r\n\r\n", "x");

  const MissExplanation miss = MissFor(Get("https://example.net/a.js"));
  EXPECT_EQ(MissReason::kHostNotCaptured, miss.reason);
  EXPECT_TRUE(miss.url.empty());
}

TEST_F(WaczUrlLoaderFactoryMissTest, TellsOfAPathNeverCaptured) {
  AddResponse("https://example.org/", "HTTP/1.1 200 OK\r\n\r\n", "page");
  // Shares a prefix with the path asked for, and is not it.
  AddResponse("https://example.org/app.js.map", "HTTP/1.1 200 OK\r\n\r\n",
              "x");

  const MissExplanation miss = MissFor(Get("https://example.org/app.js"));
  EXPECT_EQ(MissReason::kPathNotCaptured, miss.reason);
  EXPECT_TRUE(miss.url.empty());
}

// A page asked for with another query than it was captured with is not
// answered with the capture -- and says which capture that was, since that is
// the page the visitor probably meant. Another id is another query: it may
// name another thing.
TEST_F(WaczUrlLoaderFactoryMissTest, TellsOfAPageCapturedWithAnotherQuery) {
  AddResponse(
      "https://example.org/frame?uuid=9f8e7d6c-5b4a-4321-8fed-cba987654321&"
      "w=300",
      "HTTP/1.1 200 OK\r\n\r\n", "frame", "20260101115950");

  const MissExplanation miss = MissFor(
      Get("https://example.org/frame?"
          "uuid=50b0c9e2-bcca-4206-a3b7-068d73120f22&w=300",
          network::mojom::RequestDestination::kIframe));
  EXPECT_EQ(MissReason::kOtherQuery, miss.reason);
  EXPECT_EQ(
      "https://example.org/frame?uuid=9f8e7d6c-5b4a-4321-8fed-cba987654321&"
      "w=300",
      miss.url);
  EXPECT_EQ("GET", miss.method);
  EXPECT_EQ("20260101115950", miss.timestamp);
}

// A frame that a capture differing only in its visit's id would answer,
// were the capture readable, is missing for that and not for its query.
TEST_F(WaczUrlLoaderFactoryMissTest, TellsOfARemadeFrameThatCannotBeRead) {
  AddResponse("https://example.org/", "HTTP/1.1 200 OK\r\n\r\n", "page");
  index_ += base::StrCat(
      {ToSurt(GURL("https://example.org/frame?uuid=null")), " ", kTimestamp,
       " {\"url\": \"https://example.org/frame?uuid=null\", \"status\": "
       "\"200\", \"length\": \"10\", \"offset\": \"0\", "
       "\"filename\": \"elsewhere.warc.gz\"}\n"});

  const MissExplanation miss = MissFor(
      Get("https://example.org/frame?uuid=50b0c9e2-bcca-4206-a3b7-068d73120f22",
          network::mojom::RequestDestination::kIframe));
  EXPECT_EQ(MissReason::kUnreadable, miss.reason);
  EXPECT_EQ("https://example.org/frame?uuid=null", miss.url);
}

TEST_F(WaczUrlLoaderFactoryMissTest, TellsOfAPathCapturedByAnotherMethod) {
  AddResponse("https://example.org/api", "HTTP/1.1 200 OK\r\n\r\n", "{}");

  network::ResourceRequest request = Get("https://example.org/api");
  request.method = "POST";
  request.destination = network::mojom::RequestDestination::kEmpty;
  const MissExplanation miss = MissFor(request);
  EXPECT_EQ(MissReason::kOtherMethod, miss.reason);
  EXPECT_EQ("https://example.org/api", miss.url);
  EXPECT_EQ("GET", miss.method);
}

// And the other way about: the method is read out of the key a capture is
// filed under, since a URL does not carry it.
TEST_F(WaczUrlLoaderFactoryMissTest, TellsOfAPathCapturedOnlyByAPost) {
  const GURL api("https://example.org/graphql");
  AddResponseUnder(RequestKey(api, "POST", "application/json", "{\"q\":1}"),
                   api.spec(), "HTTP/1.1 200 OK\r\n\r\n", "{}");

  const MissExplanation miss = MissFor(Get(api.spec()));
  EXPECT_EQ(MissReason::kOtherMethod, miss.reason);
  EXPECT_EQ(api.spec(), miss.url);
  EXPECT_EQ("POST", miss.method);
}

TEST_F(WaczUrlLoaderFactoryMissTest, TellsOfARangeThePartDoesNotHold) {
  AddResponse("https://example.org/v.mp4",
              "HTTP/1.1 206 Partial Content\r\n"
              "Content-Range: bytes 2-5/10\r\n\r\n",
              "2345");

  const MissExplanation miss =
      MissFor(Get("https://example.org/v.mp4",
                  network::mojom::RequestDestination::kVideo),
              "bytes=4-8");
  EXPECT_EQ(MissReason::kRangeNotHeld, miss.reason);
  EXPECT_EQ("https://example.org/v.mp4", miss.url);
  EXPECT_EQ(kTimestamp, miss.timestamp);
}

// An index can name a record its container does not hold, where a container
// was put together from part of a larger capture. The archive has the
// resource in name only, which is a different fault from not having it.
TEST_F(WaczUrlLoaderFactoryMissTest, TellsOfACaptureThatCannotBeRead) {
  AddResponse("https://example.org/", "HTTP/1.1 200 OK\r\n\r\n", "page");
  index_ += base::StrCat(
      {ToSurt(GURL("https://example.org/lost.css")), " ", kTimestamp,
       " {\"url\": \"https://example.org/lost.css\", \"status\": \"200\", "
       "\"length\": \"10\", \"offset\": \"0\", "
       "\"filename\": \"elsewhere.warc.gz\"}\n"});

  const MissExplanation miss = MissFor(Get("https://example.org/lost.css"));
  EXPECT_EQ(MissReason::kUnreadable, miss.reason);
  EXPECT_EQ("https://example.org/lost.css", miss.url);
}

}  // namespace
}  // namespace warc
