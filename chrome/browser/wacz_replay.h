// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WACZ_REPLAY_H_
#define CHROME_BROWSER_WACZ_REPLAY_H_

#include <vector>

#include "content/public/browser/storage_partition_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "url/gurl.h"

class BrowserWindowInterface;

namespace content {
class BrowserContext;
class RenderFrameHost;
}  // namespace content

namespace wacz_replay {

// Whether this browser was started to replay an archive rather than to browse
// the web: --wacz-replay=<path to a .wacz>.
//
// This answers where the archive comes from, not who is allowed to read it.
// What a request is answered from is decided by the storage partition it comes
// from -- see ServesFromArchive() -- so that replaying can become a property
// of a tab without the decision having to move.
bool IsEnabled();

// The storage partition archived content is replayed in.
//
// Serving an archived origin *as* that origin is the whole point of replaying
// in a browser, and it is also what makes a partition of its own necessary:
// the page runs as https://example.com, so in an ordinary profile its scripts
// would read the live site's cookies and its writes would land in the live
// site's storage. Here they read and write nothing but each other, and the
// partition is in memory, so a capture leaves nothing behind when the browser
// exits -- including the things an archived response header would otherwise
// set for good, an HSTS entry above all.
//
// Named for the archive, not for the browser: two archives are two captures of
// the web, and what one of them stored is no more the other's than a live
// site's would be. The name is a digest of the path as it was given, so the
// same archive opened twice is one partition; the same archive under two
// spellings is two, which costs nothing when neither survives the browser.
content::StoragePartitionConfig ReplayPartition(
    content::BrowserContext* browser_context);

// Whether `config` is a partition returned by ReplayPartition() -- for any
// archive, since what follows from it is the same for all of them.
bool IsReplayPartition(const content::StoragePartitionConfig& config);

// Whether what this frame asks for is answered out of an archive. `frame` is
// null for a worker, which belongs to a process instead; either way the
// question is the same one, since a process serves a single partition.
//
// This is the test that decides it, and it is deliberately not a test of the
// command line: a frame reaches an archive because of where it is, and a frame
// outside a replay partition cannot reach one however the browser was started.
bool ServesFromArchive(content::RenderFrameHost* frame, int render_process_id);

// Opens the replay tabs this browser was started for: one for each of `urls`,
// or -- given none -- one on the first page of the archive's own page list.
//
// A replay tab is an ordinary tab in an ordinary window, except that it lives
// in the replay partition and is answered out of the archive. A tab beside it
// browses the web as it always did.
//
// The page list settles both questions a replay has to answer before it can
// start. It is the only record of which of an archive's URLs were pages rather
// than the images and scripts that hung off them, so it says where to land
// when nothing else does; and it is the only record of when each page was
// visited, so it says which moment to serve. A URL named on the command line
// is looked up in it too: replaying a page at some other capture's time is
// exactly the sort of miss nobody notices.
//
// The partition is fixed to each tab rather than worked out per navigation,
// because a capture is somewhere you are and not a page you are on: follow a
// link out of an archived page and you are still inside the archive. It is
// also why a tab is made this way rather than opened and then moved -- a tab
// that began outside the partition would have a moment in which what it asked
// for could still be fetched.
//
// Reading the page list means reading the container, which blocks, so the tabs
// appear a moment after the window does, and `browser` may be gone by then.
void OpenReplayTabs(BrowserWindowInterface* browser, std::vector<GURL> urls);

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
