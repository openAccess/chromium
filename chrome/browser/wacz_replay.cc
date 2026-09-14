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

// Reads the archive's page list. Opening a file and reading it both block, so
// this runs off the UI thread and the answer comes back to it.
std::vector<warc::PageEntry> ReadPageList(base::FilePath path) {
  return warc::ReadPageList(
      base::File(path, base::File::FLAG_OPEN | base::File::FLAG_READ));
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

  // Opened here rather than left to the first request, because this is where
  // its moment is settled. An archive that opens itself later has no time to
  // serve but the earliest it holds, which is the same answer only for a
  // capture made in one sitting.
  //
  // One moment for the archive, so the first page named settles it. That is
  // exact for the single tab a replay usually is, and it is why a moment will
  // have to belong to a tab before two pages captured weeks apart can be
  // replayed beside each other.
  Archive() =
      warc::WaczArchive::Open(ArchivePath(), MomentOf(pages, urls.front()));

  for (const GURL& url : urls) {
    OpenReplayTab(browser.get(), url);
  }
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

void OpenReplayTabs(BrowserWindowInterface* browser, std::vector<GURL> urls) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&ReadPageList, ArchivePath()),
      base::BindOnce(&OpenTabsForPages, browser->GetWeakPtr(),
                     std::move(urls)));
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
