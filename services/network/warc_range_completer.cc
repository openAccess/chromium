// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/warc_range_completer.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "net/base/io_buffer.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/base/network_handle.h"
#include "net/base/request_priority.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_response_info.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "services/network/warc_recorder.h"

namespace network {

namespace {

// Headers that describe the partial request rather than the resource. Reissuing
// them would ask for the same fragment again.
constexpr const char* kRangeHeaders[] = {"Range", "If-Range"};

constexpr size_t kReadBufferSize = 64 * 1024;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("warc_range_completion", R"(
      semantics {
        sender: "WARC recorder"
        description:
          "When --warc-output is recording and a page requests only part of a "
          "resource, the network service fetches the whole resource so the "
          "archive contains a complete copy rather than fragments."
        trigger:
          "A 206 Partial Content response while WARC recording is active."
        data:
          "The same request the page made, without its Range header."
        destination: WEBSITE
      }
      policy {
        cookies_allowed: YES
        cookies_store: "user"
        setting:
          "Only active when the --warc-output command line switch is given."
        policy_exception_justification:
          "Not implemented; the feature requires an explicit command line "
          "switch and is not otherwise reachable."
      })");

}  // namespace

// One whole-resource fetch, recorded as its own exchange.
class WarcRangeCompleter::Fetch : public net::URLRequest::Delegate {
 public:
  Fetch(const Queued& queued,
        net::URLRequestContext* context,
        std::unique_ptr<WarcExchangeRecorder> recorder,
        base::OnceCallback<void(Fetch*)> done)
      : recorder_(std::move(recorder)), done_(std::move(done)) {
    request_ =
        context->CreateRequest(queued.url, net::IDLE, this, kTrafficAnnotation,
                               net::handles::kInvalidNetworkHandle);
    request_->set_method("GET");
    request_->SetExtraRequestHeaders(queued.headers);
    request_->set_isolation_info(queued.isolation_info);
    request_->set_site_for_cookies(queued.site_for_cookies);
    request_->set_initiator(queued.initiator);

    // The archive wants the resource as served, not a copy the cache may hold
    // only part of; a ranged load leaves partial entries behind. Writing whole
    // videos back into the cache would evict everything else, too.
    request_->SetLoadFlags(net::LOAD_DISABLE_CACHE);

    // Nothing downstream consumes this body, so the transfer encoding can be
    // left on it unconditionally -- a completion record is always byte-faithful
    // regardless of how the page's own loads were configured.
    request_->set_client_side_content_decoding_enabled(true);

    recorder_->SetTargetUrl(queued.url);
    recorder_->SetBodyIsWireFormat(true);
    request_->SetRequestHeadersCallback(base::BindRepeating(
        [](base::WeakPtr<Fetch> self, net::HttpRawRequestHeaders h) {
          if (self && self->recorder_) {
            self->recorder_->SetRequestHeaders(h, "GET");
          }
        },
        weak_factory_.GetWeakPtr()));
    request_->SetResponseHeadersCallback(base::BindRepeating(
        [](base::WeakPtr<Fetch> self,
           scoped_refptr<const net::HttpResponseHeaders> h) {
          if (self && self->recorder_) {
            self->recorder_->SetResponseHeaders(std::move(h));
          }
        },
        weak_factory_.GetWeakPtr()));

    buffer_ = base::MakeRefCounted<net::IOBufferWithSize>(kReadBufferSize);
  }

  // Separate from construction so the completer owns this object before it can
  // possibly report itself finished.
  void Start() {
    ArmStallTimer();
    request_->Start();
  }

  Fetch(const Fetch&) = delete;
  Fetch& operator=(const Fetch&) = delete;

  ~Fetch() override = default;

  // net::URLRequest::Delegate:
  void OnResponseStarted(net::URLRequest* request, int net_error) override {
    if (net_error != net::OK) {
      Finish("unspecified");
      return;
    }
    const net::HttpResponseInfo& info = request->response_info();
    recorder_->SetRemoteEndpoint(info.remote_endpoint);
    if (info.DidUseQuic()) {
      recorder_->SetProtocol("h3");
    } else if (info.was_fetched_via_spdy) {
      recorder_->SetProtocol("h2");
    } else {
      recorder_->SetProtocol("http/1.1");
    }
    ReadMore();
  }

  void OnReadCompleted(net::URLRequest* request, int bytes_read) override {
    // Keep reading: an asynchronous read delivers one buffer, and without
    // pumping the loop again nothing further is ever requested.
    if (DidRead(bytes_read)) {
      ReadMore();
    }
  }

 private:
  void ReadMore() {
    while (true) {
      const int result = request_->Read(buffer_.get(), buffer_->size());
      if (result == net::ERR_IO_PENDING) {
        return;
      }
      if (!DidRead(result)) {
        return;
      }
    }
  }

  // Returns false once the fetch is over, at which point `this` is destroyed.
  bool DidRead(int bytes_read) {
    if (bytes_read < 0) {
      Finish("unspecified");
      return false;
    }
    if (bytes_read == 0) {
      Finish(std::string());
      return false;
    }
    recorder_->AddBodyBytes(
        buffer_->span().first(static_cast<size_t>(bytes_read)));
    ArmStallTimer();
    return true;
  }

  // A completion has no size limit, so a server that stops sending would
  // otherwise hold the fetch open for the life of the browser.
  void ArmStallTimer() {
    stall_timer_.Start(
        FROM_HERE, kStallTimeout,
        base::BindOnce(&Fetch::OnStalled, base::Unretained(this)));
  }

  void OnStalled() { Finish("time"); }

  void Finish(std::string_view truncated_reason) {
    stall_timer_.Stop();
    if (!truncated_reason.empty() && !recorder_->body_truncated()) {
      recorder_->SetTruncated(std::string(truncated_reason));
    }
    recorder_->Finish();
    request_.reset();
    // Destroys `this`.
    std::move(done_).Run(this);
  }

  std::unique_ptr<net::URLRequest> request_;
  std::unique_ptr<WarcExchangeRecorder> recorder_;
  scoped_refptr<net::IOBufferWithSize> buffer_;
  base::OneShotTimer stall_timer_;
  base::OnceCallback<void(Fetch*)> done_;

  base::WeakPtrFactory<Fetch> weak_factory_{this};
};

WarcRangeCompleter::Queued::Queued() = default;
WarcRangeCompleter::Queued::Queued(const Queued&) = default;
WarcRangeCompleter::Queued& WarcRangeCompleter::Queued::operator=(
    const Queued&) = default;
WarcRangeCompleter::Queued::~Queued() = default;

WarcRangeCompleter::WarcRangeCompleter(
    net::URLRequestContext* url_request_context)
    : url_request_context_(url_request_context) {}

WarcRangeCompleter::~WarcRangeCompleter() = default;

void WarcRangeCompleter::CompleteIfNeeded(const net::URLRequest& partial,
                                          WarcRecorder* recorder) {
  if (!recorder || !seen_.insert(partial.url()).second) {
    return;
  }

  Queued queued;
  queued.url = partial.url();
  queued.isolation_info = partial.isolation_info();
  queued.site_for_cookies = partial.site_for_cookies();
  queued.initiator = partial.initiator();
  queued.headers = partial.extra_request_headers();
  for (const char* name : kRangeHeaders) {
    queued.headers.RemoveHeader(name);
  }

  pending_.push_back(std::move(queued));
  recorder_ = recorder;
  MaybeStartNext();
}

void WarcRangeCompleter::MaybeStartNext() {
  while (!pending_.empty() && active_.size() < kMaxConcurrentFetches) {
    Queued queued = std::move(pending_.front());
    pending_.pop_front();
    if (!recorder_) {
      continue;
    }
    ++started_;
    active_.push_back(std::make_unique<Fetch>(
        queued, url_request_context_, recorder_->CreateCompletionRecorder(),
        base::BindOnce(&WarcRangeCompleter::OnFetchDone,
                       weak_factory_.GetWeakPtr())));
    active_.back()->Start();
  }
}

void WarcRangeCompleter::OnFetchDone(Fetch* fetch) {
  ++finished_;
  auto it = std::ranges::find_if(
      active_,
      [fetch](const std::unique_ptr<Fetch>& f) { return f.get() == fetch; });
  if (it != active_.end()) {
    // Deleted after returning to the caller, which is still inside `fetch`.
    std::unique_ptr<Fetch> owned = std::move(*it);
    active_.erase(it);
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(owned));
  }
  MaybeStartNext();
}

}  // namespace network
