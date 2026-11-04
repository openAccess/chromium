// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/wacz_url_loader_factory.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "base/byte_size.h"
#include "base/files/file.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_producer.h"
#include "mojo/public/cpp/system/string_data_source.h"
#include "net/base/net_errors.h"
#include "net/http/http_byte_range.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "net/url_request/redirect_info.h"
#include "net/url_request/redirect_util.h"
#include "net/url_request/url_request.h"
#include "services/network/public/cpp/data_element.h"
#include "services/network/public/cpp/http_request_headers_update_params.h"
#include "services/network/public/cpp/request_destination.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/resource_request_body.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/origin.h"

namespace warc {

namespace {

// Enough for a page or a script in one go; a larger body is written in pieces
// as the renderer consumes it.
constexpr uint32_t kDataPipeCapacity = 64 * 1024;

// What decides which capture answers `request`.
ReplayRequest ToReplayRequest(const network::ResourceRequest& request) {
  ReplayRequest replay;
  replay.url = request.url;
  replay.method = request.method;
  replay.content_type =
      request.headers.GetHeader(net::HttpRequestHeaders::kContentType)
          .value_or(std::string());
  // The body as sent, if it was sent as bytes. One built from a file or a
  // blob is not known here, and is left out of the lookup rather than read.
  replay.body.emplace();
  if (request.request_body) {
    for (const network::DataElement& element :
         *request.request_body->elements()) {
      if (element.type() != network::DataElement::Tag::kBytes) {
        replay.body.reset();
        break;
      }
      replay.body->append(
          element.As<network::DataElementBytes>().AsStringView());
    }
  }
  replay.accept_closest =
      request.destination != network::mojom::RequestDestination::kDocument &&
      !network::IsRequestDestinationEmbeddedFrame(request.destination);
  return replay;
}

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

// The single byte range `request` asks for, or nullopt if it asks for the
// whole resource. A request for several ranges at once is answered with the
// whole resource, as a server may: assembling a multipart answer would serve
// the same bytes, wrapped in a format no player of archived media relies on.
std::optional<net::HttpByteRange> RequestedRange(
    const network::ResourceRequest& request) {
  std::optional<std::string> header =
      request.headers.GetHeader(net::HttpRequestHeaders::kRange);
  std::vector<net::HttpByteRange> ranges;
  if (!header || !net::HttpUtil::ParseRangeHeader(*header, &ranges) ||
      ranges.size() != 1) {
    return std::nullopt;
  }
  return ranges.front();
}

// `stored` answered for `range` alone, as the server that sent it would have
// answered: a 206 holding those bytes, or a 416 if the resource has no such
// bytes. Nullopt if the archive does not hold them -- a capture of part of a
// resource answers only for the part it holds.
//
// What the archive holds for a media file is the whole of it, fetched beside
// the ranges a player asked for, and a player replaying it asks for ranges
// again -- often not the ones it asked for live. Handed the whole file instead
// of the bytes it asked for, a player that appends what it is given to its
// buffer appends the wrong bytes, and the video does not play.
std::optional<ArchivedResponse> AnswerRange(ArchivedResponse stored,
                                            net::HttpByteRange range) {
  net::HttpResponseHeaders& headers = *stored.headers;
  const int64_t held = static_cast<int64_t>(stored.body.size());
  int64_t first_held = 0;
  int64_t length = 0;
  if (headers.response_code() == 200) {
    // A truncated capture holds less than the resource it was a capture of,
    // and the resource's length is what a range is measured against.
    const std::optional<base::ByteSize> declared = headers.GetContentLength();
    length =
        declared
            ? std::max(held, base::checked_cast<int64_t>(declared->InBytes()))
            : held;
  } else if (headers.response_code() == 206) {
    int64_t last_held = 0;
    if (!headers.GetContentRangeFor206(&first_held, &last_held, &length) ||
        length < 0 || last_held - first_held + 1 != held) {
      return std::nullopt;
    }
  } else {
    // A redirect or an error has no bytes to take a range of, and is what the
    // server said whatever range was asked for.
    return stored;
  }

  if (!range.ComputeBounds(length)) {
    headers.ReplaceStatusLine("HTTP/1.1 416 Range Not Satisfiable");
    headers.SetHeader("Content-Range",
                      base::StrCat({"bytes */", base::NumberToString(length)}));
    headers.SetHeader("Content-Length", "0");
    stored.body.clear();
    return stored;
  }
  const int64_t first = range.first_byte_position();
  const int64_t last = range.last_byte_position();
  if (first < first_held || last >= first_held + held) {
    return std::nullopt;
  }

  stored.body =
      std::vector<uint8_t>(stored.body.begin() + (first - first_held),
                           stored.body.begin() + (last - first_held + 1));
  headers.ReplaceStatusLine("HTTP/1.1 206 Partial Content");
  headers.SetHeader("Content-Range",
                    base::StrCat({"bytes ", base::NumberToString(first), "-",
                                  base::NumberToString(last), "/",
                                  base::NumberToString(length)}));
  headers.SetHeader("Content-Length", base::NumberToString(last - first + 1));
  return stored;
}

// Serves one request, and the redirects it is sent on, then deletes itself.
//
// Owns itself for as long as its bindings live, as a loader does: the client
// may give up at any point, and the archive read it is waiting on outlives
// nothing.
class WaczUrlLoader : public network::mojom::URLLoader {
 public:
  WaczUrlLoader(mojo::PendingReceiver<network::mojom::URLLoader> loader,
                mojo::PendingRemote<network::mojom::URLLoaderClient> client,
                scoped_refptr<WaczArchive> archive,
                std::string timestamp,
                const network::ResourceRequest& request)
      : receiver_(this, std::move(loader)),
        client_(std::move(client)),
        archive_(std::move(archive)),
        timestamp_(std::move(timestamp)),
        request_(request) {
    receiver_.set_disconnect_handler(
        base::BindOnce(&WaczUrlLoader::DeleteSelf, base::Unretained(this)));
  }

  WaczUrlLoader(const WaczUrlLoader&) = delete;
  WaczUrlLoader& operator=(const WaczUrlLoader&) = delete;

  ~WaczUrlLoader() override = default;

  // Asks the archive for the request as it now stands: as it was made, or as
  // a redirect has since sent it.
  void Start() {
    archive_->source()
        .AsyncCall(&WaczArchiveSource::Lookup)
        .WithArgs(ToReplayRequest(request_), timestamp_)
        .Then(
            base::BindOnce(&WaczUrlLoader::Serve, weak_factory_.GetWeakPtr()));
  }

  // network::mojom::URLLoader:
  void FollowRedirect(
      network::HttpRequestHeadersUpdateParams headers_update_params,
      const std::optional<GURL>& new_url) override {
    if (!redirect_) {
      // Only a redirect can be followed, and none is waiting.
      Fail(net::ERR_UNEXPECTED);
      return;
    }
    // The client may only move the redirect within the origin it points to.
    if (new_url &&
        !url::Origin::Create(redirect_->new_url).IsSameOriginWith(*new_url)) {
      Fail(net::ERR_UNSAFE_REDIRECT);
      return;
    }
    // The request goes on as the network would send it on: under the method
    // the redirect calls for, without a body it no longer has any use for,
    // and with whatever headers the client has added or taken away.
    bool should_clear_upload = false;
    net::RedirectUtil::UpdateHttpRequest(
        request_.url, request_.method, *redirect_,
        headers_update_params.removed_headers,
        headers_update_params.modified_headers, &request_.headers,
        &should_clear_upload);
    if (should_clear_upload) {
      request_.request_body = nullptr;
    }
    request_.url = new_url.value_or(redirect_->new_url);
    request_.method = redirect_->new_method;
    request_.site_for_cookies = redirect_->new_site_for_cookies;
    request_.referrer = GURL(redirect_->new_referrer);
    request_.referrer_policy = redirect_->new_referrer_policy;
    redirect_.reset();
    // An archive holds what was fetched over the web, and nothing it points
    // to anywhere else is in it -- as the network would not follow it there
    // either.
    if (!request_.url.SchemeIsHTTPOrHTTPS()) {
      Fail(net::ERR_UNSAFE_REDIRECT);
      return;
    }
    Start();
  }
  void SetPriority(net::RequestPriority priority,
                   int32_t intra_priority_value) override {}

 private:
  // What the archive had to say.
  void Serve(std::optional<ArchivedResponse> response) {
    if (response) {
      if (std::optional<net::HttpByteRange> range = RequestedRange(request_)) {
        response = AnswerRange(std::move(*response), *range);
      }
    }
    if (!response) {
      // Nothing else to offer, and nowhere else to look. Saying so is the
      // point: a resource the archive lacks must be seen to be missing. A
      // document that is missing needs somewhere to say so; a subresource does
      // not, and should simply be missing.
      if (request_.destination ==
          network::mojom::RequestDestination::kDocument) {
        Respond(MissingPage(request_.url));
      } else {
        Fail(net::ERR_FILE_NOT_FOUND);
      }
      return;
    }

    // An archived redirect is handed over as one, as the server's was. The
    // client decides whether to follow it, and if it does, wherever it points
    // is asked of the archive in turn.
    std::string location;
    if (response->headers->IsRedirect(&location)) {
      Redirect(std::move(*response), request_.url.Resolve(location));
      return;
    }
    Respond(std::move(*response));
  }

  void Redirect(ArchivedResponse response, const GURL& location) {
    if (!location.is_valid()) {
      Fail(net::ERR_INVALID_REDIRECT);
      return;
    }
    // Captures can point at each other in a circle that no server ever did --
    // two redirects captured at different moments, or found as each other's
    // nearest -- so a chain is cut off where the network would cut it.
    if (redirects_left_-- == 0) {
      Fail(net::ERR_TOO_MANY_REDIRECTS);
      return;
    }
    redirect_ = net::RedirectInfo::ComputeRedirectInfo(
        request_.method, request_.url, request_.site_for_cookies,
        request_.update_first_party_url_on_redirect
            ? net::RedirectInfo::FirstPartyURLPolicy::UPDATE_URL_ON_REDIRECT
            : net::RedirectInfo::FirstPartyURLPolicy::NEVER_CHANGE_URL,
        request_.referrer_policy, request_.referrer.spec(),
        request_.request_initiator, response.headers->response_code(), location,
        net::RedirectUtil::GetReferrerPolicyHeader(response.headers.get()),
        /*insecure_scheme_was_upgraded=*/false);
    // A redirect's body is never shown, so none is sent.
    response.body.clear();
    client_->OnReceiveRedirect(*redirect_, Head(std::move(response)));
  }

  void Respond(ArchivedResponse response) {
    if (request_.method == net::HttpRequestHeaders::kHeadMethod) {
      // A HEAD is told the headers alone.
      response.body.clear();
    }

    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    if (mojo::CreateDataPipe(kDataPipeCapacity, producer, consumer) !=
        MOJO_RESULT_OK) {
      Fail(net::ERR_INSUFFICIENT_RESOURCES);
      return;
    }

    body_ = std::string(response.body.begin(), response.body.end());
    client_->OnReceiveResponse(Head(std::move(response)), std::move(consumer),
                               std::nullopt);

    // The body is written after the head, and asynchronously, because a
    // response larger than the pipe cannot be handed over in one piece.
    producer_ = std::make_unique<mojo::DataPipeProducer>(std::move(producer));
    producer_->Write(std::make_unique<mojo::StringDataSource>(
                         body_, mojo::StringDataSource::AsyncWritingMode::
                                    STRING_STAYS_VALID_UNTIL_COMPLETION),
                     base::BindOnce(&WaczUrlLoader::OnBodyWritten,
                                    weak_factory_.GetWeakPtr()));
  }

  static network::mojom::URLResponseHeadPtr Head(ArchivedResponse response) {
    auto head = network::mojom::URLResponseHead::New();
    head->headers = std::move(response.headers);
    head->headers->GetMimeTypeAndCharset(&head->mime_type, &head->charset);
    head->content_length = static_cast<int64_t>(response.body.size());
    head->encoded_data_length = static_cast<int64_t>(response.body.size());
    const base::TimeTicks now = base::TimeTicks::Now();
    head->request_start = now;
    head->response_start = now;
    // The response was not fetched now, whatever the clock says, and a reader
    // of an archive is entitled to know when it was.
    head->request_time = base::Time::Now();
    head->response_time = base::Time::Now();
    return head;
  }

  static ArchivedResponse MissingPage(const GURL& url) {
    ArchivedResponse response;
    response.headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        net::HttpUtil::AssembleRawHeaders(
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html; charset=utf-8\r\n\r\n"));
    const std::string body = MissingPageBody(url);
    response.body.assign(body.begin(), body.end());
    return response;
  }

  void OnBodyWritten(MojoResult result) {
    if (result != MOJO_RESULT_OK) {
      Fail(net::ERR_FAILED);
      return;
    }
    network::URLLoaderCompletionStatus status(net::OK);
    status.encoded_data_length = base::ByteSize(body_.size());
    status.decoded_body_length = base::ByteSize(body_.size());
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

  const scoped_refptr<WaczArchive> archive_;
  const std::string timestamp_;

  // The request as it now stands, changed by each redirect followed.
  network::ResourceRequest request_;

  // The redirect handed to the client, until it is followed.
  std::optional<net::RedirectInfo> redirect_;
  int redirects_left_ = net::URLRequest::kMaxRedirects;

  std::string body_;
  std::unique_ptr<mojo::DataPipeProducer> producer_;

  base::WeakPtrFactory<WaczUrlLoader> weak_factory_{this};
};

}  // namespace

WaczArchiveSource::WaczArchiveSource(base::FilePath path)
    : collection_(WaczCollection::Open(
          base::File(path, base::File::FLAG_OPEN | base::File::FLAG_READ))) {
  if (!collection_) {
    LOG(ERROR) << "Cannot replay from " << path.value()
               << ": not a readable WACZ. Every request will miss.";
  }
}

WaczArchiveSource::~WaczArchiveSource() = default;

std::optional<ArchivedResponse> WaczArchiveSource::Lookup(
    ReplayRequest request,
    std::string timestamp) {
  if (!collection_) {
    return std::nullopt;
  }
  return collection_->Lookup(request, timestamp);
}

// static
scoped_refptr<WaczArchive> WaczArchive::Open(base::FilePath path) {
  return base::WrapRefCounted(new WaczArchive(std::move(path)));
}

WaczArchive::WaczArchive(base::FilePath path)
    : source_(base::ThreadPool::CreateSequencedTaskRunner(
                  {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
                   base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN}),
              std::move(path)) {}

WaczArchive::~WaczArchive() = default;

mojo::PendingRemote<network::mojom::URLLoaderFactory>
WaczArchive::CreateFactory(const std::string& timestamp) {
  mojo::PendingRemote<network::mojom::URLLoaderFactory> pending_remote;
  BindFactory(timestamp, pending_remote.InitWithNewPipeAndPassReceiver(),
              mojo::NullRemote());
  return pending_remote;
}

void WaczArchive::BindFactory(
    const std::string& timestamp,
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> passthrough) {
  // Deletes itself once nothing holds a receiver, as its base class arranges.
  base::MakeSelfDeleting<WaczUrlLoaderFactory>(
      base::WrapRefCounted(this), timestamp, std::move(passthrough),
      std::move(receiver));
}

WaczUrlLoaderFactory::WaczUrlLoaderFactory(
    scoped_refptr<WaczArchive> archive,
    const std::string& timestamp,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> passthrough,
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
    base::SelfDeletingPassKey key)
    : network::SelfDeletingURLLoaderFactory(std::move(receiver), key),
      archive_(std::move(archive)),
      timestamp_(timestamp),
      passthrough_(std::move(passthrough)) {}

WaczUrlLoaderFactory::~WaczUrlLoaderFactory() = default;

void WaczUrlLoaderFactory::CreateLoaderAndStart(
    mojo::PendingReceiver<network::mojom::URLLoader> loader,
    int32_t request_id,
    uint32_t options,
    const network::ResourceRequest& request,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    const net::MutableNetworkTrafficAnnotationTag& traffic_annotation) {
  // An archive holds what a page fetched over the web. A browser asks for
  // other things too -- its own chrome:// pages, the theme it draws itself
  // with -- and those are no part of any capture. Refusing them does not make
  // replay any more faithful; it just breaks the browser around the archive.
  if (!request.url.SchemeIsHTTPOrHTTPS()) {
    if (passthrough_) {
      passthrough_->CreateLoaderAndStart(std::move(loader), request_id, options,
                                         request, std::move(client),
                                         traffic_annotation);
      return;
    }
    mojo::Remote<network::mojom::URLLoaderClient>(std::move(client))
        ->OnComplete(
            network::URLLoaderCompletionStatus(net::ERR_FILE_NOT_FOUND));
    return;
  }

  // Deletes itself once it has answered, or once the client stops listening.
  (new WaczUrlLoader(std::move(loader), std::move(client), archive_, timestamp_,
                     request))
      ->Start();
}

}  // namespace warc
