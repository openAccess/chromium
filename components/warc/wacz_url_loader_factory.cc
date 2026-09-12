// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/wacz_url_loader_factory.h"

#include <utility>

#include "base/byte_size.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_producer.h"
#include "mojo/public/cpp/system/string_data_source.h"
#include "net/base/net_errors.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/http_request_headers_update_params.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace warc {

namespace {

// Enough for a page or a script in one go; a larger body is written in pieces
// as the renderer consumes it.
constexpr uint32_t kDataPipeCapacity = 64 * 1024;

// What a navigation to something the archive does not hold arrives at.
//
// A subresource that is missing should look missing -- a broken image is
// exactly the right impression. A document that is missing has nowhere to
// show that, and an empty error page leaves a visitor guessing whether the
// archive is broken, the URL is wrong, or replay has failed. So a navigation
// gets a page that says which URL was asked for and that this archive does
// not contain it.
std::string MissingPageBody(const GURL& url) {
  return base::StrCat({
      "<!doctype html><meta charset=\"utf-8\">"
      "<title>Not in this archive</title>"
      "<body style=\"font:14px system-ui;margin:3em;max-width:40em\">"
      "<h1>Not in this archive</h1><p>This capture does not contain</p>"
      "<p><code>",
      base::EscapeForHTML(url.spec()),
      "</code></p><p>Nothing was fetched from the network to find out. An "
      "archive holds what was captured and no more.</p>",
  });
}

// Serves one request, then deletes itself.
//
// Owns itself for as long as its bindings live, as a loader does: the client
// may give up at any point, and the archive read it is waiting on outlives
// nothing.
class WaczUrlLoader : public network::mojom::URLLoader {
 public:
  WaczUrlLoader(mojo::PendingReceiver<network::mojom::URLLoader> loader,
                mojo::PendingRemote<network::mojom::URLLoaderClient> client)
      : receiver_(this, std::move(loader)), client_(std::move(client)) {
    receiver_.set_disconnect_handler(
        base::BindOnce(&WaczUrlLoader::DeleteSelf, base::Unretained(this)));
  }

  WaczUrlLoader(const WaczUrlLoader&) = delete;
  WaczUrlLoader& operator=(const WaczUrlLoader&) = delete;

  ~WaczUrlLoader() override = default;

  base::WeakPtr<WaczUrlLoader> AsWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // Remembers what to say if the archive has nothing.
  void set_missing_page(std::optional<GURL> url) {
    missing_page_for_ = std::move(url);
  }

  // What the archive had to say.
  void Serve(std::optional<ArchivedResponse> response) {
    if (!response) {
      // Nothing else to offer, and nowhere else to look. Saying so is the
      // point: a resource the archive lacks must be seen to be missing.
      if (missing_page_for_) {
        ServeMissingPage(*missing_page_for_);
      } else {
        Fail(net::ERR_FILE_NOT_FOUND);
      }
      return;
    }

    auto head = network::mojom::URLResponseHead::New();
    head->headers = std::move(response->headers);
    head->headers->GetMimeTypeAndCharset(&head->mime_type, &head->charset);
    head->content_length = static_cast<int64_t>(response->body.size());
    head->encoded_data_length = static_cast<int64_t>(response->body.size());
    const base::TimeTicks now = base::TimeTicks::Now();
    head->request_start = now;
    head->response_start = now;
    // The response was not fetched now, whatever the clock says, and a reader
    // of an archive is entitled to know when it was.
    head->request_time = base::Time::Now();
    head->response_time = base::Time::Now();

    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    if (mojo::CreateDataPipe(kDataPipeCapacity, producer, consumer) !=
        MOJO_RESULT_OK) {
      Fail(net::ERR_INSUFFICIENT_RESOURCES);
      return;
    }

    body_size_ = response->body.size();
    client_->OnReceiveResponse(std::move(head), std::move(consumer),
                               std::nullopt);

    // The body is written after the head, and asynchronously, because a
    // response larger than the pipe cannot be handed over in one piece.
    body_ = std::string(response->body.begin(), response->body.end());
    producer_ = std::make_unique<mojo::DataPipeProducer>(std::move(producer));
    producer_->Write(std::make_unique<mojo::StringDataSource>(
                         body_, mojo::StringDataSource::AsyncWritingMode::
                                    STRING_STAYS_VALID_UNTIL_COMPLETION),
                     base::BindOnce(&WaczUrlLoader::OnBodyWritten,
                                    weak_factory_.GetWeakPtr()));
  }

  // network::mojom::URLLoader:
  void FollowRedirect(
      network::HttpRequestHeadersUpdateParams headers_update_params,
      const std::optional<GURL>& new_url) override {
    // An archived redirect is answered as the redirect it was, and the browser
    // asks again for wherever it points -- which this factory serves too, out
    // of the same archive. So there is nothing to follow here.
    DeleteSelf();
  }
  void SetPriority(net::RequestPriority priority,
                   int32_t intra_priority_value) override {}

 private:
  void ServeMissingPage(const GURL& url) {
    ArchivedResponse response;
    response.headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        net::HttpUtil::AssembleRawHeaders(
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html; charset=utf-8\r\n\r\n"));
    const std::string body = MissingPageBody(url);
    response.body.assign(body.begin(), body.end());
    missing_page_for_.reset();
    Serve(std::move(response));
  }

  void OnBodyWritten(MojoResult result) {
    if (result != MOJO_RESULT_OK) {
      Fail(net::ERR_FAILED);
      return;
    }
    network::URLLoaderCompletionStatus status(net::OK);
    status.encoded_data_length = base::ByteSize(body_size_);
    status.decoded_body_length = base::ByteSize(body_size_);
    client_->OnComplete(status);
    DeleteSelf();
  }

  void Fail(int net_error) {
    client_->OnComplete(network::URLLoaderCompletionStatus(net_error));
    DeleteSelf();
  }

  void DeleteSelf() { delete this; }

  mojo::Receiver<network::mojom::URLLoader> receiver_;
  mojo::Remote<network::mojom::URLLoaderClient> client_;

  std::optional<GURL> missing_page_for_;
  std::string body_;
  size_t body_size_ = 0;
  std::unique_ptr<mojo::DataPipeProducer> producer_;

  base::WeakPtrFactory<WaczUrlLoader> weak_factory_{this};
};

}  // namespace

WaczArchiveSource::WaczArchiveSource(base::File archive)
    : collection_(WaczCollection::Open(std::move(archive))) {}

WaczArchiveSource::~WaczArchiveSource() = default;

std::optional<ArchivedResponse> WaczArchiveSource::Lookup(
    GURL url,
    std::string timestamp) {
  if (!collection_) {
    return std::nullopt;
  }
  return collection_->Lookup(url, timestamp);
}

// static
scoped_refptr<WaczArchive> WaczArchive::Open(base::File archive,
                                             const std::string& timestamp) {
  return base::WrapRefCounted(new WaczArchive(std::move(archive), timestamp));
}

WaczArchive::WaczArchive(base::File archive, const std::string& timestamp)
    : source_(base::ThreadPool::CreateSequencedTaskRunner(
                  {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
                   base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN}),
              std::move(archive)),
      timestamp_(timestamp) {}

WaczArchive::~WaczArchive() = default;

mojo::PendingRemote<network::mojom::URLLoaderFactory>
WaczArchive::CreateFactory() {
  mojo::PendingRemote<network::mojom::URLLoaderFactory> pending_remote;
  // Deletes itself once nothing holds a receiver, as its base class arranges.
  base::MakeSelfDeleting<WaczUrlLoaderFactory>(
      base::WrapRefCounted(this),
      pending_remote.InitWithNewPipeAndPassReceiver());
  return pending_remote;
}

WaczUrlLoaderFactory::WaczUrlLoaderFactory(
    scoped_refptr<WaczArchive> archive,
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
    base::SelfDeletingPassKey key)
    : network::SelfDeletingURLLoaderFactory(std::move(receiver), key),
      archive_(std::move(archive)) {}

WaczUrlLoaderFactory::~WaczUrlLoaderFactory() = default;

void WaczUrlLoaderFactory::CreateLoaderAndStart(
    mojo::PendingReceiver<network::mojom::URLLoader> loader,
    int32_t request_id,
    uint32_t options,
    const network::ResourceRequest& request,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    const net::MutableNetworkTrafficAnnotationTag& traffic_annotation) {
  auto* url_loader = new WaczUrlLoader(std::move(loader), std::move(client));

  // A document that is missing needs somewhere to say so; a subresource does
  // not, and should simply be missing.
  if (request.destination == network::mojom::RequestDestination::kDocument) {
    url_loader->set_missing_page(request.url);
  }

  // Only what an archive can answer. A capture records what the browser was
  // given in reply to a request it made; there is no reply stored for a
  // request nobody made.
  if (request.method != net::HttpRequestHeaders::kGetMethod) {
    url_loader->Serve(std::nullopt);
    return;
  }

  archive_->source()
      .AsyncCall(&WaczArchiveSource::Lookup)
      .WithArgs(request.url, archive_->timestamp())
      .Then(base::BindOnce(&WaczUrlLoader::Serve, url_loader->AsWeakPtr()));
}

}  // namespace warc
