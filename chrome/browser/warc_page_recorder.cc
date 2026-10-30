// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/warc_page_recorder.h"

#include <map>
#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/uuid.h"
#include "base/values.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_handle_timing.h"
#include "content/public/browser/network_service_instance.h"
#include "content/public/browser/web_contents.h"
#include "services/network/public/cpp/network_switches.h"

namespace {

// The file WACZ expects a capture's page list in, and the header line that
// declares what it is.
constexpr base::FilePath::CharType kPagesFileName[] =
    FILE_PATH_LITERAL("pages.jsonl");
constexpr char kPagesHeader[] =
    R"({"format": "json-pages-1.0", "id": "pages", "title": "All Pages"})"
    "\n";

// Owns the page list of one capture. Lives on a blocking sequence for the life
// of the process: a session's capture lasts as long as the browser does, and a
// tab's costs one small entry once it is over.
class PagesFile {
 public:
  explicit PagesFile(const base::FilePath& capture_directory)
      : capture_directory_(capture_directory) {}
  PagesFile(const PagesFile&) = delete;
  PagesFile& operator=(const PagesFile&) = delete;

  void Append(const std::string& line) {
    if (!resolved_) {
      Resolve();
      resolved_ = true;
    }
    if (path_.empty()) {
      return;
    }
    if (!header_written_) {
      // Only once the capture has a page to record: a header alone would claim
      // a page list where there is none.
      if (!base::AppendToFile(path_, kPagesHeader)) {
        LOG(ERROR) << "WARC pages: failed writing " << path_.value();
        path_.clear();
        return;
      }
      header_written_ = true;
    }
    if (!base::AppendToFile(path_, line)) {
      LOG(ERROR) << "WARC pages: failed writing " << path_.value();
      path_.clear();
    }
  }

 private:
  // Where the page list goes, decided once: inside the directory the capture
  // is writing into, beside the archives it describes. That directory belongs
  // to the capture alone, so the list is never anything but its own and never
  // has to be cleared out of another's way.
  void Resolve() {
    path_ = capture_directory_.Append(kPagesFileName);
    // Created empty rather than appended to blind: appending requires the file
    // to be there already. The directory belongs to this capture alone, so
    // there is never an earlier list here to preserve or to overwrite.
    if (!base::WriteFile(path_, std::string_view())) {
      LOG(ERROR) << "WARC pages: failed creating " << path_.value();
      path_.clear();
    }
  }

  const base::FilePath capture_directory_;
  bool resolved_ = false;
  bool header_written_ = false;
  base::FilePath path_;
};

// Only touched on the file sequence.
PagesFile& GetPagesFile(const base::FilePath& capture_directory) {
  static base::NoDestructor<std::map<base::FilePath, PagesFile>> pages_files;
  return pages_files->try_emplace(capture_directory, capture_directory)
      .first->second;
}

scoped_refptr<base::SequencedTaskRunner> GetFileTaskRunner() {
  static base::NoDestructor<scoped_refptr<base::SequencedTaskRunner>> runner(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN}));
  return *runner;
}

// RFC 3339 in UTC, which is how WACZ dates a page.
std::string FormatPageTime(base::Time time) {
  base::Time::Exploded exploded;
  time.UTCExplode(&exploded);
  return base::StringPrintf("%04d-%02d-%02dT%02d:%02d:%02dZ", exploded.year,
                            exploded.month, exploded.day_of_month,
                            exploded.hour, exploded.minute, exploded.second);
}

// When the page's document was requested, if it came without a redirect, as
// near as the browser can say to the date the network service gives its
// records.
//
// That date is what the page has to carry. A WACZ ties a page to its record by
// URL and time to the second, so a page dated a moment later than its record
// -- at commit, say, after a slow response -- can land in the next second and
// match nothing; the tooling then drops it from the page list. The network
// service dates a navigation's records when its loader is created, and starts
// the request in the same breath, so the request's start is the record's date
// to within tens of microseconds. Only a second boundary falling in that gap
// parts them.
base::Time RequestTime(content::NavigationHandle* handle) {
  const content::NavigationHandleTiming& timing =
      handle->GetNavigationHandleTiming();
  const base::TimeTicks start =
      timing.first_fetch_start_time.value_or(timing.loader_start_time);
  if (start.is_null()) {
    return base::Time::Now();
  }
  return base::Time::Now() - (base::TimeTicks::Now() - start);
}

}  // namespace

WarcPageRecorder::WarcPageRecorder(content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<WarcPageRecorder>(*web_contents) {}

WarcPageRecorder::~WarcPageRecorder() = default;

// static
void WarcPageRecorder::MaybeCreateForWebContents(
    content::WebContents* web_contents) {
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          network::switches::kWarcOutput)) {
    return;
  }
  CreateForWebContents(web_contents);
}

void WarcPageRecorder::DidRedirectNavigation(
    content::NavigationHandle* handle) {
  // Each hop of a redirect chain is an exchange of its own, dated when the
  // network service follows the redirect -- which it is told to do right
  // after this. The hop the page lands on is the last of these.
  if (handle->IsInPrimaryMainFrame()) {
    last_redirect_ = {handle->GetNavigationId(), base::Time::Now()};
  }
}

void WarcPageRecorder::DidFinishNavigation(content::NavigationHandle* handle) {
  if (!handle->IsInPrimaryMainFrame()) {
    return;
  }
  std::optional<base::Time> redirected_at;
  if (last_redirect_ && last_redirect_->first == handle->GetNavigationId()) {
    redirected_at = last_redirect_->second;
    last_redirect_.reset();
  }
  if (!handle->HasCommitted()) {
    return;
  }

  // Whatever was waiting for a title has run out of time: this page is
  // replacing it.
  WritePendingPage();

  // A same-document navigation fetches no new document, so the records it
  // produces belong to the page already recorded. An error page has a document
  // but not one the site served, and nothing about it is worth replaying.
  if (handle->IsSameDocument() || handle->IsErrorPage()) {
    return;
  }
  // Only what a WARC would hold. chrome:// and about: pages are not archived,
  // so a page list naming them would point at nothing.
  if (!handle->GetURL().SchemeIsHTTPOrHTTPS()) {
    return;
  }

  pending_ =
      PendingPage{handle->GetURL(), redirected_at.value_or(RequestTime(handle)),
                  base::Uuid::GenerateRandomV4().AsLowercaseString()};
}

void WarcPageRecorder::SetCaptureDirectory(const base::FilePath& directory) {
  WritePendingPage();
  capture_directory_ = directory;
}

void WarcPageRecorder::DidStopLoading() {
  WritePendingPage();
}

void WarcPageRecorder::WebContentsDestroyed() {
  WritePendingPage();
}

void WarcPageRecorder::WritePendingPage() {
  if (!pending_) {
    return;
  }
  const PendingPage page = std::move(*pending_);
  pending_.reset();

  // A capture written to a file the user named has no directory, and
  // inventing a neighbour for a named file is worse than leaving the page list
  // to be supplied another way.
  const base::FilePath capture_directory =
      capture_directory_.empty() ? content::GetWarcCaptureDirectory()
                                 : capture_directory_;
  if (capture_directory.empty()) {
    return;
  }

  base::DictValue entry;
  entry.Set("id", page.id);
  entry.Set("url", page.url.spec());
  entry.Set("ts", FormatPageTime(page.at));
  // The title as it stands now. A page that renames itself later keeps the name
  // it had when it finished loading, which is the one it was archived under.
  const std::string title = base::UTF16ToUTF8(web_contents()->GetTitle());
  if (!title.empty()) {
    entry.Set("title", title);
  }

  std::optional<std::string> json = base::WriteJson(entry);
  if (!json) {
    return;
  }
  GetFileTaskRunner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](const base::FilePath& capture_directory, std::string line) {
            GetPagesFile(capture_directory).Append(line);
          },
          capture_directory, base::StrCat({*json, "\n"})));
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(WarcPageRecorder);
