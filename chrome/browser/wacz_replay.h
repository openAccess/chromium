// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WACZ_REPLAY_H_
#define CHROME_BROWSER_WACZ_REPLAY_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"

namespace wacz_replay {

// Whether this browser was started to replay an archive rather than to browse
// the web: --wacz-replay=<path to a .wacz>.
//
// It is the whole browser rather than a tab, which is the honest shape for a
// first one. Replaying in one tab of an ordinary browser means a tab whose
// storage, cookies and network are separated from the rest, and that
// separation is the part that has to be got right rather than got working.
// Until then, a browser started this way reaches nothing but the archive --
// so pair it with a --user-data-dir of its own.
bool IsEnabled();

// Fills `receiver` with a factory serving the archive. Every http and https
// request every frame makes goes through it, so the archived site keeps its
// own URLs and its own origin, and nothing reaches the network.
//
// `passthrough` takes what the archive has no business answering -- the
// browser's own chrome:// pages and furniture, which are no part of any
// capture and would only be broken by being refused.
void BindFactory(
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> passthrough);

}  // namespace wacz_replay

#endif  // CHROME_BROWSER_WACZ_REPLAY_H_
