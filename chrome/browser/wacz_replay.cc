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
#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/common/chrome_switches.h"
#include "components/warc/wacz_collection.h"
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

// Between the archive's key and the moment in a partition's name. Neither
// half can contain it: one is hex and the other digits.
constexpr char kMomentSeparator = '-';

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

// The archive this browser is replaying, opened once. Shared by every factory,
// whatever moment it serves, so that the container is read, and its index
// parsed, once rather than once per frame or per tab.
scoped_refptr<warc::WaczArchive>& Archive() {
  static base::NoDestructor<scoped_refptr<warc::WaczArchive>> archive;
  return *archive;
}

// Reads the archive's page list. Opening a file and reading it both block, so
// this runs off the UI thread and the answer comes back to it.
std::vector<warc::PageEntry> ReadPageList(base::FilePath path) {
  return warc::ReadPageList(
      base::File(path, base::File::FLAG_OPEN | base::File::FLAG_READ));
}

// The partition `frame` is in, or for a worker the one its process serves.
const content::StoragePartition* PartitionOf(content::RenderFrameHost* frame,
                                             int render_process_id) {
  if (frame) {
    return frame->GetStoragePartition();
  }
  if (auto* process = content::RenderProcessHost::FromID(render_process_id)) {
    return process->GetStoragePartition();
  }
  return nullptr;
}

void OpenReplayTab(BrowserWindowInterface* browser,
                   const GURL& url,
                   const std::string& moment) {
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
                       profile, url, ReplayPartition(profile, moment))));
  contents->GetController().LoadURL(url, content::Referrer(),
                                    ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                                    std::string());

  NavigateParams params(browser, std::move(contents));
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  Navigate(&params);
}

// The time the page list gives for `url`, or empty if it names no such page.
// Empty means the earliest capture the archive holds, which is the only answer
// available when nothing says otherwise.
std::string MomentOf(const std::vector<warc::PageEntry>& pages,
                     const GURL& url) {
  for (const warc::PageEntry& page : pages) {
    if (GURL(page.url) == url) {
      return page.timestamp;
    }
  }
  return std::string();
}

void OpenTabsForPages(base::WeakPtr<BrowserWindowInterface> browser,
                      std::vector<GURL> urls,
                      std::vector<warc::PageEntry> pages) {
  if (!browser) {
    return;
  }

  // Told only which archive to open, the archive says where to start: the
  // first page of its list is where the capture began. An archive naming no
  // pages opens no tab, because there is nothing in it to land on.
  if (urls.empty()) {
    if (pages.empty()) {
      return;
    }
    urls.push_back(GURL(pages.front().url));
  }

  // Each tab at its own page's moment. A tab left to the first request would
  // have no time to serve but the earliest the archive holds, which is the
  // same answer only for a capture made in one sitting.
  for (const GURL& url : urls) {
    OpenReplayTab(browser.get(), url, MomentOf(pages, url));
  }
}

}  // namespace

bool IsEnabled() {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kWaczReplay);
}

content::StoragePartitionConfig ReplayPartition(
    content::BrowserContext* browser_context,
    const std::string& moment) {
  // Spelled one way, so that a moment from the page list and the same moment
  // from an index are one partition rather than two.
  return content::StoragePartitionConfig::Create(
      browser_context, kPartitionDomain,
      base::StrCat({ArchiveKey(ArchivePath()), std::string(1, kMomentSeparator),
                    warc::ToIndexTimestamp(moment)}),
      /*in_memory=*/true);
}

std::string ReplayMoment(const content::StoragePartitionConfig& config) {
  if (!IsReplayPartition(config)) {
    return std::string();
  }
  const std::string& name = config.partition_name();
  const size_t separator = name.find(kMomentSeparator);
  return separator == std::string::npos ? std::string()
                                        : name.substr(separator + 1);
}

bool ServesFromArchive(content::RenderFrameHost* frame, int render_process_id) {
  const content::StoragePartition* partition =
      PartitionOf(frame, render_process_id);
  return partition && IsReplayPartition(partition->GetConfig());
}

void OpenReplayTabs(BrowserWindowInterface* browser, std::vector<GURL> urls) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&ReadPageList, ArchivePath()),
      base::BindOnce(&OpenTabsForPages, browser->GetWeakPtr(),
                     std::move(urls)));
}

void BindFactory(
    content::RenderFrameHost* frame,
    int render_process_id,
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> passthrough) {
  if (!Archive()) {
    // Nothing is opened or read here: the archive opens itself on the sequence
    // that may block, so the first request does not wait on a file open on
    // this one. An archive that turns out to be unreadable still has a
    // factory, which answers every request with a miss -- better a browser
    // that plainly has nothing than one that quietly goes to the network.
    Archive() = warc::WaczArchive::Open(ArchivePath());
  }
  // The moment is the partition's, so a frame and the workers beside it in
  // one process cannot be served two different ones.
  const content::StoragePartition* partition =
      PartitionOf(frame, render_process_id);
  Archive()->BindFactory(
      partition ? ReplayMoment(partition->GetConfig()) : std::string(),
      std::move(receiver), std::move(passthrough));
}

}  // namespace wacz_replay
