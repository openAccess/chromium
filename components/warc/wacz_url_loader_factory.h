// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_WARC_WACZ_URL_LOADER_FACTORY_H_
#define COMPONENTS_WARC_WACZ_URL_LOADER_FACTORY_H_

#include <string>

#include "base/files/file.h"
#include "base/threading/sequence_bound.h"
#include "components/warc/wacz_collection.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/cpp/self_deleting_url_loader_factory.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"

namespace warc {

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
  explicit WaczArchiveSource(base::File archive);

  WaczArchiveSource(const WaczArchiveSource&) = delete;
  WaczArchiveSource& operator=(const WaczArchiveSource&) = delete;

  ~WaczArchiveSource();

  // The response stored for `url` at `timestamp`, or nullopt -- which is also
  // the answer when the container could not be read at all, since there is
  // nothing a caller could do differently about the two.
  std::optional<ArchivedResponse> Lookup(GURL url, std::string timestamp);

 private:
  std::optional<WaczCollection> collection_;
};

class WaczUrlLoaderFactory : public network::SelfDeletingURLLoaderFactory {
 public:
  // Serves `archive` as it stood at `timestamp`.
  //
  // One moment for the whole factory, rather than nearest-to-now for each
  // request: a page and the images on it were captured seconds apart, but a
  // page captured in March asking for a script captured in September is not a
  // page that ever existed. `timestamp` may be written either way a WACZ
  // writes one.
  //
  // Always returns a remote: whether the container can be read is only known
  // once it has been read, and that is not something the caller's sequence may
  // wait for. An unreadable archive answers every request with a failure,
  // which is what an archive missing everything amounts to.
  static mojo::PendingRemote<network::mojom::URLLoaderFactory> Create(
      base::File archive,
      const std::string& timestamp);

  // Use Create(). Public only because a self-deleting class is constructed
  // for you, and the pass key is what keeps that from being done by hand.
  WaczUrlLoaderFactory(
      base::File archive,
      const std::string& timestamp,
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

  // The archive is read where it lies, so reading from it blocks, so it lives
  // on a sequence where blocking is allowed rather than on this one.
  base::SequenceBound<WaczArchiveSource> archive_;

  const std::string timestamp_;
};

}  // namespace warc

#endif  // COMPONENTS_WARC_WACZ_URL_LOADER_FACTORY_H_
