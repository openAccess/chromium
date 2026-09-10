// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/warc_page_recorder.h"

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

// Owns the file. Lives on a blocking sequence for the life of the process,
// since a capture lasts as long as the browser does and every tab appends to
// the same file.
class PagesFile {
 public:
  PagesFile() = default;
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
  // Where the page list goes, decided once: inside the directory this session
  // is capturing into, beside the archives it describes. That directory belongs
  // to this session alone, so the list is never anything but this session's and
  // never has to be cleared out of another's way.
  //
  // A capture written to a file the user named has no such place, and inventing
  // a neighbour for a named file is worse than leaving the page list to be
  // supplied another way.
  void Resolve() {
    const base::FilePath capture_directory = content::GetWarcCaptureDirectory();
    if (capture_directory.empty()) {
      return;
    }
    path_ = capture_directory.Append(kPagesFileName);
    // Created empty rather than appended to blind: appending requires the file
    // to be there already. The directory belongs to this capture alone, so
    // there is never an earlier list here to preserve or to overwrite.
    if (!base::WriteFile(path_, std::string_view())) {
      LOG(ERROR) << "WARC pages: failed creating " << path_.value();
      path_.clear();
    }
  }

  bool resolved_ = false;
  bool header_written_ = false;
  base::FilePath path_;
};

PagesFile& GetPagesFile() {
  static base::NoDestructor<PagesFile> pages_file;
  return *pages_file;
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

void WarcPageRecorder::DidFinishNavigation(content::NavigationHandle* handle) {
  if (!handle->IsInPrimaryMainFrame() || !handle->HasCommitted()) {
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

  pending_ = PendingPage{handle->GetURL(), base::Time::Now(),
                         base::Uuid::GenerateRandomV4().AsLowercaseString()};
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
      base::BindOnce([](std::string line) { GetPagesFile().Append(line); },
                     base::StrCat({*json, "\n"})));
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(WarcPageRecorder);
