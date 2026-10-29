// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/warc_tab_capture.h"

#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/stringprintf.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/warc_page_recorder.h"
#include "chrome/common/chrome_paths.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/network_service_instance.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/web_contents.h"
#include "services/network/public/cpp/network_switches.h"
#include "services/network/public/mojom/network_service.mojom.h"

namespace {

// Named as a --warc-output capture is (see network_service_instance_impl.cc),
// so that a tab's capture and a session's look the same and sort together.
constexpr char kWarcCapturePrefix[] = "chrdl";

// Where captures go when --warc-output did not say: beside the user's
// downloads, which is where a browser puts what it saves for the user.
constexpr char kDefaultCaptureDirectoryName[] = "WARC";

// The 14-digit UTC timestamp web archive tooling spells a time with.
std::string CaptureTimestamp() {
  base::Time::Exploded now;
  base::Time::Now().UTCExplode(&now);
  return base::StringPrintf("%04d%02d%02d%02d%02d%02d", now.year, now.month,
                            now.day_of_month, now.hour, now.minute, now.second);
}

}  // namespace

struct WarcTabCapture::OpenedCapture {
  base::FilePath directory;
  base::File archive;
  std::string archive_name;
  base::File spill;
};

namespace {

// Makes the capture's directory and opens its archive. Blocking, so it runs on
// the thread pool; `parent` is null when --warc-output did not name a
// directory.
std::optional<WarcTabCapture::OpenedCapture> OpenCapture(
    base::FilePath warc_output) {
  base::FilePath parent;
  if (!warc_output.empty() && base::DirectoryExists(warc_output)) {
    parent = warc_output;
  } else if (base::PathService::Get(chrome::DIR_DEFAULT_DOWNLOADS, &parent)) {
    parent = parent.AppendASCII(kDefaultCaptureDirectoryName);
  } else {
    LOG(ERROR) << "WARC tab capture: no downloads directory to write into.";
    return std::nullopt;
  }

  // Two tabs started in the same second would otherwise share a directory, and
  // with it a page list. The second gets a suffix rather than the first's
  // files.
  const std::string timestamp = CaptureTimestamp();
  const std::string stem = base::StrCat({kWarcCapturePrefix, "-", timestamp});
  base::FilePath directory = parent.AppendASCII(stem);
  for (int n = 2; base::PathExists(directory); ++n) {
    directory = parent.AppendASCII(base::StringPrintf("%s-%d", stem.c_str(), n));
  }
  if (!base::CreateDirectory(directory)) {
    LOG(ERROR) << "WARC tab capture: failed creating " << directory.value();
    return std::nullopt;
  }

  WarcTabCapture::OpenedCapture opened;
  opened.directory = directory;
  opened.archive_name = base::StrCat({stem, "-00000.warc.gz"});
  const base::FilePath archive_path =
      directory.AppendASCII(opened.archive_name);
  opened.archive = base::File(
      archive_path, base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_WRITE);
  if (!opened.archive.IsValid()) {
    LOG(ERROR) << "WARC tab capture: failed opening " << archive_path.value();
    return std::nullopt;
  }
  // Scratch space for resources too large to assemble in memory, as for
  // --warc-output. Deleted on close; nothing in it outlives the capture.
  opened.spill = base::File(
      archive_path.AddExtensionASCII(".spill"),
      base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_READ |
          base::File::FLAG_WRITE | base::File::FLAG_DELETE_ON_CLOSE);
  return opened;
}

}  // namespace

WarcTabCapture::WarcTabCapture(content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<WarcTabCapture>(*web_contents) {}

WarcTabCapture::~WarcTabCapture() = default;

// static
bool WarcTabCapture::IsCapturing(content::WebContents* web_contents) {
  return FromWebContents(web_contents) != nullptr;
}

// static
void WarcTabCapture::Start(content::WebContents* web_contents) {
  if (IsCapturing(web_contents)) {
    return;
  }
  CreateForWebContents(web_contents);
  WarcTabCapture* capture = FromWebContents(web_contents);

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&OpenCapture,
                     base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
                         network::switches::kWarcOutput)),
      base::BindOnce(&WarcTabCapture::OnCaptureOpened,
                     capture->weak_factory_.GetWeakPtr()));
}

// static
void WarcTabCapture::Stop(content::WebContents* web_contents) {
  WarcTabCapture* capture = FromWebContents(web_contents);
  if (!capture) {
    return;
  }
  capture->Finish();
  web_contents->RemoveUserData(UserDataKey());
}

// static
std::optional<base::UnguessableToken> WarcTabCapture::GetCaptureId(
    content::WebContents* web_contents) {
  WarcTabCapture* capture =
      web_contents ? FromWebContents(web_contents) : nullptr;
  if (!capture) {
    return std::nullopt;
  }
  return capture->capture_id_;
}

// static
void WarcTabCapture::OnCaptureOpened(base::WeakPtr<WarcTabCapture> self,
                                     std::optional<OpenedCapture> opened) {
  if (!self) {
    // The tab stopped archiving, or closed, while its files were opening.
    // Closing them blocks, so it is not done here.
    if (opened) {
      base::ThreadPool::PostTask(
          FROM_HERE, {base::MayBlock()},
          base::BindOnce([](OpenedCapture) {}, std::move(*opened)));
    }
    return;
  }
  if (!opened) {
    // Nothing to record into. Leaving the tab marked would claim an archive
    // that does not exist.
    Stop(self->web_contents());
    return;
  }

  self->directory_ = opened->directory;

  // From here the tab's pages are listed beside its own archive. A page that
  // committed before the capture began is written where it was headed, since
  // the archive holds none of it.
  WarcPageRecorder::CreateForWebContents(self->web_contents());
  WarcPageRecorder::FromWebContents(self->web_contents())
      ->SetCaptureDirectory(self->directory_);

  LOG(WARNING) << "WARC tab capture: archiving into "
               << self->directory_.value();
  content::GetNetworkService()->StartWarcCapture(
      self->capture_id_, std::move(opened->archive), opened->archive_name,
      /*compress_records=*/true, std::move(opened->spill),
      !base::CommandLine::ForCurrentProcess()->HasSwitch(
          network::switches::kWarcIncludeCredentials),
      base::BindOnce(&WarcTabCapture::OnCaptureStarted, self));
}

void WarcTabCapture::OnCaptureStarted() {
  // Load the page again now that its requests will be recorded, and from the
  // network: a response served from the cache never reaches the recorder.
  content::NavigationController& controller = web_contents()->GetController();
  if (web_contents()->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
    controller.Reload(content::ReloadType::BYPASSING_CACHE,
                      /*check_for_repost=*/true);
  }
}

void WarcTabCapture::Finish() {
  if (finished_) {
    return;
  }
  finished_ = true;

  // The page waiting for its title belongs to this capture, so it is written
  // here before the tab's pages go back to wherever they went before.
  if (WarcPageRecorder* pages =
          WarcPageRecorder::FromWebContents(web_contents())) {
    pages->SetCaptureDirectory(base::FilePath());
  }

  // Sent on the same pipe as StartWarcCapture, so it arrives after it even if
  // the start has not been answered yet; and it is harmless if the start was
  // never sent.
  content::GetNetworkService()->StopWarcCapture(
      capture_id_, base::BindOnce(
                       [](base::FilePath directory) {
                         if (!directory.empty()) {
                           LOG(WARNING) << "WARC tab capture: finished "
                                        << directory.value();
                         }
                       },
                       directory_));
}

void WarcTabCapture::WebContentsDestroyed() {
  Finish();
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(WarcTabCapture);
