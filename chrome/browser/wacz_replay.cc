// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wacz_replay.h"

#include <memory>
#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/common/chrome_switches.h"
#include "components/warc/wacz_url_loader_factory.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "crypto/hash.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace wacz_replay {

namespace {

// The domain every replay partition lives in, whatever archive it holds. It is
// what IsReplayPartition() recognises, so one test answers for all of them.
constexpr char kPartitionDomain[] = "wacz-replay";

// Enough of a digest to name an archive by. Collisions would merge two
// captures' storage; sixty-four bits puts that out of reach without making the
// name unreadable in a trace.
std::string ArchiveKey(const base::FilePath& path) {
  const auto digest = crypto::hash::Sha256(path.value());
  return base::HexEncodeLower(base::span(digest).first(8u));
}

base::FilePath ArchivePath() {
  return base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
      switches::kWaczReplay);
}

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

content::StoragePartitionConfig ReplayPartition(
    content::BrowserContext* browser_context) {
  return content::StoragePartitionConfig::Create(
      browser_context, kPartitionDomain, ArchiveKey(ArchivePath()),
      /*in_memory=*/true);
}

bool IsReplayPartition(const content::StoragePartitionConfig& config) {
  return config.partition_domain() == kPartitionDomain;
}

bool ServesFromArchive(content::RenderFrameHost* frame, int render_process_id) {
  const content::StoragePartition* partition = nullptr;
  if (frame) {
    partition = frame->GetStoragePartition();
  } else if (auto* process =
                 content::RenderProcessHost::FromID(render_process_id)) {
    partition = process->GetStoragePartition();
  }
  return partition && IsReplayPartition(partition->GetConfig());
}

void OpenReplayTab(BrowserWindowInterface* browser, const GURL& url) {
  Profile* profile = browser->GetProfile();

  // Fixed, rather than chosen again at each navigation: the tab keeps the
  // partition wherever the archived pages lead it, which is what makes a
  // capture somewhere you are rather than a page you are on. Fixing it also
  // fixes it from the tab's first frame, before anything has been asked for,
  // so there is no moment at which the tab exists outside the partition and
  // could be answered from the network instead.
  std::unique_ptr<content::WebContents> contents =
      content::WebContents::Create(content::WebContents::CreateParams(
          profile, content::SiteInstance::CreateForFixedStoragePartition(
                       profile, url, ReplayPartition(profile))));
  contents->GetController().LoadURL(url, content::Referrer(),
                                    ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                                    std::string());

  NavigateParams params(browser, std::move(contents));
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  Navigate(&params);
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
    Archive() = warc::WaczArchive::Open(ArchivePath(), std::string());
  }
  Archive()->BindFactory(std::move(receiver), std::move(passthrough));
}

}  // namespace wacz_replay
