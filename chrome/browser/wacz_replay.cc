// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wacz_replay.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/containers/flat_set.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/thread_pool.h"
#include "base/unguessable_token.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/wacz_replay/replay_log.h"
#include "chrome/common/chrome_switches.h"
#include "components/warc/cdxj.h"
#include "components/warc/wacz_collection.h"
#include "components/warc/wacz_url_loader_factory.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace wacz_replay {

namespace {

// Between the tab and the moment in a partition's name. Neither half can
// contain it: one is hex and the other digits.
constexpr char kMomentSeparator = '-';

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

base::File OpenArchiveFile(const base::FilePath& path) {
  return base::File(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
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

// Adds what became of a request to the log of the tab that made it, if the tab
// is still there.
void ReportToTab(base::WeakPtr<content::WebContents> tab,
                 const scoped_refptr<warc::WaczArchive>& archive,
                 base::Time moment,
                 const warc::ReplayOutcome& request) {
  if (tab) {
    ReplayLog::Record(tab.get(), archive, moment, request);
  }
}

// Adds the tabs with a frame in `process` to `tabs`.
void AddTabsOf(content::RenderProcessHost& process,
               base::flat_set<content::WebContents*>& tabs) {
  process.ForEachRenderFrameHost([&tabs](content::RenderFrameHost* frame) {
    if (content::WebContents* tab =
            content::WebContents::FromRenderFrameHost(frame)) {
      tabs.insert(tab);
    }
  });
}

void Record(const base::flat_set<content::WebContents*>& tabs,
            const scoped_refptr<warc::WaczArchive>& archive,
            base::Time moment,
            const warc::ReplayOutcome& request) {
  for (content::WebContents* tab : tabs) {
    ReplayLog::Record(tab, archive, moment, request);
  }
}

// As ReportToTab(), for a worker: it belongs to a process rather than a tab, so
// what it asks for goes in the log of each tab with a frame in its process.
// A process serves one partition, so they are all replaying what it is.
void ReportToTabsInProcess(int render_process_id,
                           const scoped_refptr<warc::WaczArchive>& archive,
                           base::Time moment,
                           const warc::ReplayOutcome& request) {
  content::RenderProcessHost* process =
      content::RenderProcessHost::FromID(render_process_id);
  if (!process) {
    return;
  }
  base::flat_set<content::WebContents*> tabs;
  AddTabsOf(*process, tabs);
  Record(tabs, archive, moment, request);
}

// As ReportToTab(), for what belongs to a partition as a whole: a service
// worker's script, fetched before the worker has a process. It goes in the log
// of each tab with a frame in a process of the partition -- which, a replay
// partition being a tab's own, is the tab that registered the worker and any
// window it opened.
void ReportToTabsInPartition(const content::StoragePartitionConfig& config,
                             const scoped_refptr<warc::WaczArchive>& archive,
                             base::Time moment,
                             const warc::ReplayOutcome& request) {
  base::flat_set<content::WebContents*> tabs;
  for (auto it = content::RenderProcessHost::AllHostsIterator(); !it.IsAtEnd();
       it.Advance()) {
    content::RenderProcessHost* process = it.GetCurrentValue();
    if (process->GetStoragePartition()->GetConfig() == config) {
      AddTabsOf(*process, tabs);
    }
  }
  Record(tabs, archive, moment, request);
}

// What each of the factories below reports a request to, told the archive and
// the time the partition's clock starts at.
using Reporter =
    base::RepeatingCallback<void(const scoped_refptr<warc::WaczArchive>&,
                                 base::Time,
                                 const warc::ReplayOutcome&)>;

// Fills `receiver` with a factory serving the archive at the moment of the
// replay partition `config` -- or the earliest capture, given no partition --
// and telling `report` what became of each request.
void BindArchiveFactory(
    const content::StoragePartitionConfig* config,
    Reporter report,
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
  const base::Time clock_start =
      config ? ReplayClockStart(*config).value_or(base::Time()) : base::Time();
  Archive()->BindFactory(
      config ? ReplayMoment(*config) : std::string(), std::move(receiver),
      std::move(passthrough),
      base::BindRepeating(std::move(report), Archive(), clock_start));
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
                       profile, url,
                       ReplayPartition(profile, moment,
                                       base::UnguessableToken::Create()))));
  contents->GetController().LoadURL(url, content::Referrer(),
                                    ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                                    std::string());

  NavigateParams params(browser, std::move(contents));
  params.disposition = WindowOpenDisposition::kNewForegroundTab;
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

// A tab to open: the URL it lands on, and the moment it is replayed at.
using Landing = std::pair<GURL, std::string>;

// Where each of `urls` opens, from the archive at `path`. Opening a file and
// reading it both block, so this runs off the UI thread and the answer comes
// back to it.
//
// The whole index is read here, and read again when the archive first serves:
// the page list alone cannot say which of a URL's pages the site answered,
// and only one of them is somewhere a visit can begin.
std::vector<Landing> ReadLandings(base::FilePath path, std::vector<GURL> urls) {
  std::optional<warc::WaczCollection> collection =
      warc::WaczCollection::Open(OpenArchiveFile(path));
  // A container with no index serves nothing, but still says which pages it
  // holds, and a tab that plainly has nothing is the honest answer.
  const std::vector<warc::PageEntry> pages =
      collection ? collection->pages()
                 : warc::ReadPageList(OpenArchiveFile(path));

  // Told only which archive to open, the archive says where to start: the
  // first page of its list is where the capture began. An archive naming no
  // pages opens no tab, because there is nothing in it to land on.
  if (urls.empty()) {
    if (pages.empty()) {
      return {};
    }
    urls.push_back(GURL(pages.front().url));
  }

  // Each tab at its own page's moment. A tab left to the first request would
  // have no time to serve but the earliest the archive holds, which is the
  // same answer only for a capture made in one sitting.
  std::vector<Landing> landings;
  for (GURL& url : urls) {
    std::string moment =
        collection ? collection->LandingMoment(url) : MomentOf(pages, url);
    landings.emplace_back(std::move(url), std::move(moment));
  }
  return landings;
}

// Empties a replay tab's storage before the first request of a visit, and
// holds the request until it is empty.
class VisitThrottle : public content::NavigationThrottle {
 public:
  explicit VisitThrottle(content::NavigationThrottleRegistry& registry)
      : content::NavigationThrottle(registry) {}

  ThrottleCheckResult WillStartRequest() override {
    navigation_handle()
        ->GetWebContents()
        ->GetPrimaryMainFrame()
        ->GetStoragePartition()
        ->ClearData(content::StoragePartition::REMOVE_DATA_MASK_ALL,
                    blink::StorageKey(), base::Time(), base::Time::Max(),
                    // Posted, so that it cannot resume what has not yet
                    // been deferred.
                    base::BindPostTaskToCurrentDefault(base::BindOnce(
                        &VisitThrottle::Resume, weak_factory_.GetWeakPtr())));
    return DEFER;
  }

  const char* GetNameForLogging() override { return "WaczReplayVisitThrottle"; }

 private:
  base::WeakPtrFactory<VisitThrottle> weak_factory_{this};
};

void OpenTabs(base::WeakPtr<BrowserWindowInterface> browser,
              std::vector<Landing> landings) {
  if (!browser) {
    return;
  }
  for (const auto& [url, moment] : landings) {
    OpenReplayTab(browser.get(), url, moment);
  }
}

}  // namespace

bool IsEnabled() {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kWaczReplay);
}

content::StoragePartitionConfig ReplayPartition(
    content::BrowserContext* browser_context,
    const std::string& moment,
    const base::UnguessableToken& tab) {
  // Spelled one way, so that a moment from the page list and the same moment
  // from an index are one partition rather than two.
  return content::StoragePartitionConfig::Create(
      browser_context, kPartitionDomain,
      base::StrCat({tab.ToString(), std::string(1, kMomentSeparator),
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

std::optional<base::Time> ReplayClockStart(
    const content::StoragePartitionConfig& config) {
  const std::string moment = ReplayMoment(config);
  if (moment.empty()) {
    return std::nullopt;
  }
  const base::Time start = warc::ParseIndexTimestamp(moment);
  if (start.is_null()) {
    return std::nullopt;
  }
  return start;
}

void AppendRendererSwitches(content::RenderProcessHost& process,
                            base::CommandLine& command_line) {
  const std::optional<base::Time> start =
      ReplayClockStart(process.GetStoragePartition()->GetConfig());
  if (!start) {
    return;
  }
  command_line.AppendSwitchASCII(
      switches::kRendererClockStart,
      base::NumberToString(
          (*start - base::Time::UnixEpoch()).InMilliseconds()));
}

bool ServesFromArchive(content::RenderFrameHost* frame, int render_process_id) {
  const content::StoragePartition* partition =
      PartitionOf(frame, render_process_id);
  return partition && IsReplayPartition(partition->GetConfig());
}

void AddVisitThrottle(content::NavigationThrottleRegistry& registry) {
  content::NavigationHandle& navigation = registry.GetNavigationHandle();
  if (!navigation.IsInPrimaryMainFrame() || navigation.IsRendererInitiated() ||
      navigation.IsHistory() || !IsReplayTab(navigation.GetWebContents())) {
    return;
  }
  registry.AddThrottle(std::make_unique<VisitThrottle>(registry));
}

void OpenReplayTabs(BrowserWindowInterface* browser, std::vector<GURL> urls) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&ReadLandings, ArchivePath(), std::move(urls)),
      base::BindOnce(&OpenTabs, browser->GetWeakPtr()));
}

void BindFactory(
    content::RenderFrameHost* frame,
    int render_process_id,
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> passthrough) {
  // The moment is the partition's, so a frame and the workers beside it in
  // one process cannot be served two different ones.
  const content::StoragePartition* partition =
      PartitionOf(frame, render_process_id);
  content::WebContents* tab =
      frame ? content::WebContents::FromRenderFrameHost(frame) : nullptr;
  BindArchiveFactory(
      partition ? &partition->GetConfig() : nullptr,
      tab ? base::BindRepeating(&ReportToTab, tab->GetWeakPtr())
          : base::BindRepeating(&ReportToTabsInProcess, render_process_id),
      std::move(receiver), std::move(passthrough));
}

void ResetArchiveForTesting() {
  Archive() = nullptr;
}

void BindFactoryForPartition(
    const content::StoragePartitionConfig& config,
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> passthrough) {
  BindArchiveFactory(&config,
                     base::BindRepeating(&ReportToTabsInPartition, config),
                     std::move(receiver), std::move(passthrough));
}

}  // namespace wacz_replay
