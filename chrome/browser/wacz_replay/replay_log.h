// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WACZ_REPLAY_REPLAY_LOG_H_
#define CHROME_BROWSER_WACZ_REPLAY_REPLAY_LOG_H_

#include <stddef.h>
#include <stdint.h>

#include "base/containers/circular_deque.h"
#include "base/files/file_path.h"
#include "base/memory/scoped_refptr.h"
#include "base/observer_list_types.h"
#include "base/time/time.h"
#include "components/warc/wacz_url_loader_factory.h"
#include "content/public/browser/web_contents_user_data.h"

// What a replay tab has asked its archive for, and what became of each
// request: what answered it, or that nothing did.
//
// A replay that looks right can still be missing what the page went on to
// want -- a script that never ran, an image that never came -- and nothing on
// the page says so. This is where it is said, for whatever shows it.
class ReplayLog : public content::WebContentsUserData<ReplayLog> {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // `web_contents` began replaying: its log was made.
    virtual void OnReplayStarted(content::WebContents* web_contents) {}

    // `web_contents` asked its archive for something, and `request` is what
    // became of it.
    virtual void OnReplayRequest(content::WebContents* web_contents,
                                 const warc::ReplayOutcome& request) {}
  };

  // The most requests kept to show. Older ones are counted but not kept.
  static constexpr size_t kMaxRequestsKept = 10000;

  static void AddObserver(Observer* observer);
  static void RemoveObserver(Observer* observer);

  // Adds `request` to the log of `web_contents`, a replay tab replaying
  // `archive` at `moment` (null when it serves the earliest capture of each
  // URL), starting the log if this is the tab's first request.
  static void Record(content::WebContents* web_contents,
                     scoped_refptr<warc::WaczArchive> archive,
                     base::Time moment,
                     const warc::ReplayOutcome& request);

  ReplayLog(const ReplayLog&) = delete;
  ReplayLog& operator=(const ReplayLog&) = delete;
  ~ReplayLog() override;

  // Different for each log made in this browser, so that one log can be told
  // from the next.
  int serial() const { return serial_; }

  // The archive replayed: where it is, and the archive itself, for asking
  // what it holds beyond what the tab asked it for.
  const base::FilePath& archive() const { return archive_->path(); }
  const scoped_refptr<warc::WaczArchive>& opened_archive() const {
    return archive_;
  }
  base::Time moment() const { return moment_; }

  // The latest requests, oldest first.
  const base::circular_deque<warc::ReplayOutcome>& requests() const {
    return requests_;
  }

  // Every request, including those no longer kept.
  size_t request_count() const { return request_count_; }
  // Of those, the ones the archive held nothing for.
  size_t missing_count() const { return missing_count_; }
  // Bodies served, in bytes.
  uint64_t body_bytes() const { return body_bytes_; }

 private:
  friend class content::WebContentsUserData<ReplayLog>;

  ReplayLog(content::WebContents* web_contents,
            scoped_refptr<warc::WaczArchive> archive,
            base::Time moment);

  void Add(const warc::ReplayOutcome& request);

  const int serial_;
  const scoped_refptr<warc::WaczArchive> archive_;
  const base::Time moment_;

  base::circular_deque<warc::ReplayOutcome> requests_;
  size_t request_count_ = 0;
  size_t missing_count_ = 0;
  uint64_t body_bytes_ = 0;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

#endif  // CHROME_BROWSER_WACZ_REPLAY_REPLAY_LOG_H_
