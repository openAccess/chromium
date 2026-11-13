// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_WARC_WACZ_URL_LOADER_FACTORY_H_
#define COMPONENTS_WARC_WACZ_URL_LOADER_FACTORY_H_

#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/types/expected.h"
#include "base/memory/ref_counted.h"
#include "base/threading/sequence_bound.h"
#include "base/time/time.h"
#include "components/warc/wacz_collection.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/cpp/self_deleting_url_loader_factory.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "url/gurl.h"

namespace warc {

// What became of one request a replay was asked to answer: what the archive
// held for it, or that it held nothing. A redirect is one answer and the
// request it sends the page on to is another, as on the network.
struct ReplayOutcome {
  ReplayOutcome();
  ReplayOutcome(const ReplayOutcome&);
  ReplayOutcome& operator=(const ReplayOutcome&);
  ~ReplayOutcome();

  GURL url;
  std::string method;
  // What the page wanted it for, as Fetch names it -- "document", "script",
  // "image" -- or empty for a fetch() or an XMLHttpRequest.
  std::string destination;

  // The archive holds nothing that answers it. A page missing this way is
  // shown a page saying so, and anything else fails as if the network had.
  bool missing = false;
  // Why the archive held nothing, when it did not, and what came nearest.
  MissExplanation miss;
  // Why it failed other than by missing -- a redirect the replay would not
  // follow, say -- or net::OK.
  int net_error = 0;

  // What was served, when something was: the stored status, the declared MIME
  // type lower-cased without parameters, and the body's size as sent.
  int status_code = 0;
  std::string mime_type;
  uint64_t body_bytes = 0;

  // The capture that answered: when it was taken, and the URL it was taken
  // for where that is not `url` -- another URL filed under the same key, or
  // the closest capture of a subresource asked for in other words.
  base::Time captured;
  GURL captured_url;

  // When the answer was given.
  base::Time time;
};

// Told of every request a factory answers or cannot, on the sequence the
// factory serves on.
using ReplayReporter = base::RepeatingCallback<void(const ReplayOutcome&)>;

// Serves requests from a WACZ instead of from the network.
//
// This is the whole of replay in the browser: give a frame this factory and
// every request it makes -- the page, and the images, styles and scripts the
// page asks for afterwards -- is answered out of the archive. Because it sits
// where the network would, the archived site keeps its own URLs and its own
// origin, and nothing has to rewrite the page's links or shim its fetches to
// point somewhere else. That is the one thing a browser can do here that a
// replay tool outside one cannot.
//
// It is also why there is nothing to fall through to. A factory that served
// what it had and let the rest go to the network would present an incomplete
// archive as a complete page, which is the failure worth preventing most: a
// missing resource is visibly missing, and a live one is not.
// The opened archive, on a sequence where blocking is allowed.
//
// Reading a record means reading the container where it lies, which blocks,
// and a factory is called on a sequence that may not. So the archive lives
// here and is asked across sequences.
class WaczArchiveSource {
 public:
  // Opens `path` here, on this sequence, rather than being handed a file
  // opened on the caller's: opening blocks too, and a caller that may not
  // block should not have to find somewhere that may just to hand one over.
  explicit WaczArchiveSource(base::FilePath path);

  WaczArchiveSource(const WaczArchiveSource&) = delete;
  WaczArchiveSource& operator=(const WaczArchiveSource&) = delete;

  ~WaczArchiveSource();

  // The response stored for `request` at `timestamp`, or why there is none
  // -- which is kUnreadable when the container could not be read at all.
  base::expected<ArchivedResponse, MissExplanation> Lookup(
      ReplayRequest request,
      std::string timestamp);

  // The archive, or null if it could not be read. For asking what it holds
  // beyond what a page asks for; only on this sequence.
  const WaczCollection* collection() const {
    return collection_ ? &*collection_ : nullptr;
  }

 private:
  std::optional<WaczCollection> collection_;
};

// One opened archive.
//
// Shared by every factory serving from it: a frame, its iframes and its
// workers are all reading one container, and so are tabs replaying it at
// different moments. Opening it once means reading its index once.
//
// The moment is not the archive's but each factory's, since two tabs can be
// replaying one archive as it stood weeks apart.
class WaczArchive : public base::RefCountedThreadSafe<WaczArchive> {
 public:
  static scoped_refptr<WaczArchive> Open(base::FilePath path);

  WaczArchive(const WaczArchive&) = delete;
  WaczArchive& operator=(const WaczArchive&) = delete;

  // A factory serving this archive as it stood at `timestamp`, which may be
  // written either way a WACZ writes one, or be empty for the earliest
  // capture of each URL. Hand one to a frame and everything that frame asks
  // for comes from here.
  //
  // `reporter`, if given, hears what became of each request.
  mojo::PendingRemote<network::mojom::URLLoaderFactory> CreateFactory(
      const std::string& timestamp,
      ReplayReporter reporter = ReplayReporter());

  // As above, for a caller that already has a receiver to fill -- which is
  // what a request interception hook hands over.
  //
  // `passthrough` receives the requests this archive has no business
  // answering. An archive holds what a page fetched over the web, so http and
  // https requests are answered from it and never go anywhere else; but a
  // browser also asks for its own furniture -- chrome:// pages, the theme it
  // draws itself with -- and those have nothing to do with the archive and
  // would be broken by being refused. May be empty, in which case anything
  // not answered here fails.
  void BindFactory(
      const std::string& timestamp,
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      mojo::PendingRemote<network::mojom::URLLoaderFactory> passthrough,
      ReplayReporter reporter = ReplayReporter());

  base::SequenceBound<WaczArchiveSource>& source() { return source_; }

  // The container this reads.
  const base::FilePath& path() const { return path_; }

 private:
  friend class base::RefCountedThreadSafe<WaczArchive>;

  explicit WaczArchive(base::FilePath path);
  ~WaczArchive();

  const base::FilePath path_;
  base::SequenceBound<WaczArchiveSource> source_;
};

class WaczUrlLoaderFactory : public network::SelfDeletingURLLoaderFactory {
 public:
  // Use WaczArchive::CreateFactory(). Public only because a self-deleting
  // class is constructed for you, and the pass key is what keeps that from
  // being done by hand.
  WaczUrlLoaderFactory(
      scoped_refptr<WaczArchive> archive,
      const std::string& timestamp,
      mojo::PendingRemote<network::mojom::URLLoaderFactory> passthrough,
      ReplayReporter reporter,
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      base::SelfDeletingPassKey key);

  WaczUrlLoaderFactory(const WaczUrlLoaderFactory&) = delete;
  WaczUrlLoaderFactory& operator=(const WaczUrlLoaderFactory&) = delete;

  // network::mojom::URLLoaderFactory:
  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override;

 private:
  // Private, as a self-deleting class requires: it goes when its receivers do.
  ~WaczUrlLoaderFactory() override;

  // Held, not owned: several factories may serve one archive, and the archive
  // outlives any of them.
  const scoped_refptr<WaczArchive> archive_;

  // The moment every request through this factory is answered at.
  const std::string timestamp_;

  // Where a request the archive has no business answering goes instead.
  mojo::Remote<network::mojom::URLLoaderFactory> passthrough_;

  // Told what became of each request answered here; may be null.
  const ReplayReporter reporter_;
};

}  // namespace warc

#endif  // COMPONENTS_WARC_WACZ_URL_LOADER_FACTORY_H_
