// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wacz_replay/replay_log.h"

#include <utility>

#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"

namespace {

base::ObserverList<ReplayLog::Observer>& Observers() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  static base::NoDestructor<base::ObserverList<ReplayLog::Observer>> observers;
  return *observers;
}

int NextSerial() {
  static int serial = 0;
  return ++serial;
}

}  // namespace

// static
void ReplayLog::AddObserver(Observer* observer) {
  Observers().AddObserver(observer);
}

// static
void ReplayLog::RemoveObserver(Observer* observer) {
  Observers().RemoveObserver(observer);
}

// static
void ReplayLog::Record(content::WebContents* web_contents,
                       scoped_refptr<warc::WaczArchive> archive,
                       base::Time moment,
                       const warc::ReplayOutcome& request) {
  ReplayLog* log = FromWebContents(web_contents);
  if (!log) {
    CreateForWebContents(web_contents, std::move(archive), moment);
    log = FromWebContents(web_contents);
    for (Observer& observer : Observers()) {
      observer.OnReplayStarted(web_contents);
    }
  }
  log->Add(request);
}

ReplayLog::ReplayLog(content::WebContents* web_contents,
                     scoped_refptr<warc::WaczArchive> archive,
                     base::Time moment)
    : content::WebContentsUserData<ReplayLog>(*web_contents),
      serial_(NextSerial()),
      archive_(std::move(archive)),
      moment_(moment) {}

ReplayLog::~ReplayLog() = default;

void ReplayLog::Add(const warc::ReplayOutcome& request) {
  ++request_count_;
  if (request.missing) {
    ++missing_count_;
  }
  body_bytes_ += request.body_bytes;
  requests_.push_back(request);
  if (requests_.size() > kMaxRequestsKept) {
    requests_.pop_front();
  }
  for (Observer& observer : Observers()) {
    observer.OnReplayRequest(&GetWebContents(), request);
  }
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(ReplayLog);
