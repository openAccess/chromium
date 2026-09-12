// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wacz_replay.h"

#include <utility>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "chrome/common/chrome_switches.h"
#include "components/warc/wacz_url_loader_factory.h"

namespace wacz_replay {

namespace {

// The archive this browser is replaying, opened once. Shared by every factory
// so that the container is read, and its index parsed, once rather than once
// per frame.
scoped_refptr<warc::WaczArchive>& Archive() {
  static base::NoDestructor<scoped_refptr<warc::WaczArchive>> archive;
  return *archive;
}

}  // namespace

bool IsEnabled() {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kWaczReplay);
}

void BindFactory(
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> passthrough) {
  if (!Archive()) {
    // Nothing is opened or read here: the archive opens itself on the sequence
    // that may block, so the first request does not wait on a file open on
    // this one. An archive that turns out to be unreadable still has a
    // factory, which answers every request with a miss -- better a browser
    // that plainly has nothing than one that quietly goes to the network.
    Archive() = warc::WaczArchive::Open(
        base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
            switches::kWaczReplay),
        std::string());
  }
  Archive()->BindFactory(std::move(receiver), std::move(passthrough));
}

}  // namespace wacz_replay
