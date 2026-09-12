// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_WARC_WACZ_URL_LOADER_FACTORY_H_
#define COMPONENTS_WARC_WACZ_URL_LOADER_FACTORY_H_

#include <string>

#include "base/files/file.h"
#include "base/memory/ref_counted.h"
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

// One opened archive, and the moment it is being replayed at.
//
// Shared by every factory serving from it: a frame, its iframes and its
// workers are all reading one container, and opening it once means reading
// its index once.
class WaczArchive : public base::RefCountedThreadSafe<WaczArchive> {
 public:
  // Serves `archive` as it stood at `timestamp`, which may be written either
  // way a WACZ writes one.
  static scoped_refptr<WaczArchive> Open(base::File archive,
                                         const std::string& timestamp);

  WaczArchive(const WaczArchive&) = delete;
  WaczArchive& operator=(const WaczArchive&) = delete;

  // A factory serving from this archive. Hand one to a frame and everything
  // that frame asks for comes from here.
  mojo::PendingRemote<network::mojom::URLLoaderFactory> CreateFactory();

  base::SequenceBound<WaczArchiveSource>& source() { return source_; }
  const std::string& timestamp() const { return timestamp_; }

 private:
  friend class base::RefCountedThreadSafe<WaczArchive>;

  WaczArchive(base::File archive, const std::string& timestamp);
  ~WaczArchive();

  base::SequenceBound<WaczArchiveSource> source_;
  const std::string timestamp_;
};

class WaczUrlLoaderFactory : public network::SelfDeletingURLLoaderFactory {
 public:
  // Use WaczArchive::CreateFactory(). Public only because a self-deleting
  // class is constructed for you, and the pass key is what keeps that from
  // being done by hand.
  WaczUrlLoaderFactory(
      scoped_refptr<WaczArchive> archive,
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
};

}  // namespace warc

#endif  // COMPONENTS_WARC_WACZ_URL_LOADER_FACTORY_H_
