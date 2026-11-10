// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WARC_TAB_CAPTURE_H_
#define CHROME_BROWSER_WARC_TAB_CAPTURE_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <optional>
#include <string>

#include "base/containers/circular_deque.h"
#include "base/files/file_path.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list_types.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "services/network/public/mojom/network_service.mojom.h"
#include "url/gurl.h"

// Archives one tab to a WARC capture of its own, without --warc-output and
// without recording anything else the browser does.
//
// The network service records a request into a capture when the request is
// marked with that capture's id. The browser marks a tab's navigations and the
// factories its frames load through (ChromeContentBrowserClient::
// GetWarcCaptureId asks this class), so the archive holds what the tab loaded
// and nothing from the tabs beside it.
//
// Each capture gets a directory of its own, named as a --warc-output capture
// is, holding its archive and the pages.jsonl that turns it into a WACZ. It
// goes inside the --warc-output directory when one was given, and otherwise in
// a "WARC" directory under the default downloads directory.
//
// A capture starts from nothing: the tab is replaced by one in a storage
// partition of its own, in memory and empty -- no cookies, no site storage, no
// cache, no renderer that has loaded anything -- which then loads the page
// afresh. Replay begins from the same nothing, so the page's scripts take the
// same path in both; captured with the profile's own state, a site that
// remembered a visitor took a path at capture that replay, knowing no one,
// could not follow, and asked for what had never been fetched. The tab stays
// in that partition until it closes, archiving or not, and is signed in to
// nothing it was not signed in to there.
//
// The network service reports each exchange it writes, and the capture keeps
// them, so that what is going into the archive can be shown as it happens.
class WarcTabCapture : public content::WebContentsObserver,
                       public content::WebContentsUserData<WarcTabCapture>,
                       public network::mojom::WarcCaptureObserver {
 public:
  // One exchange the capture wrote, as the network service reported it.
  struct Exchange {
    Exchange();
    Exchange(const Exchange&);
    Exchange& operator=(const Exchange&);
    ~Exchange();

    GURL url;
    std::string method;
    // 0 when the archive holds the request alone.
    int status_code = 0;
    std::string mime_type;
    uint64_t body_bytes = 0;
    bool truncated = false;
    base::Time time;
  };

  // Told about every tab's capture, on the UI thread. A tab's WebContents is
  // replaced when it starts archiving, so an observer of one tab compares
  // against whatever the tab holds now rather than keeping a WebContents.
  class Observer : public base::CheckedObserver {
   public:
    // `web_contents` started or stopped archiving, or its capture moved on --
    // its directory became known, or recording began.
    virtual void OnCaptureChanged(content::WebContents* web_contents) {}

    // The capture of `web_contents` wrote `exchange`.
    virtual void OnExchangeRecorded(content::WebContents* web_contents,
                                    const Exchange& exchange) {}
  };

  // How many of the most recent exchanges a capture keeps to show. The count
  // and size below cover every exchange, kept or not.
  static constexpr size_t kMaxExchangesKept = 10000;

  WarcTabCapture(const WarcTabCapture&) = delete;
  WarcTabCapture& operator=(const WarcTabCapture&) = delete;

  ~WarcTabCapture() override;

  // Whether `web_contents` is being archived on its own.
  static bool IsCapturing(content::WebContents* web_contents);

  // Makes the tab that archives what `web_contents` shows, from an empty state
  // (see above), and starts archiving it. The caller puts the returned tab in
  // place of `web_contents`; it loads the page once its capture has started.
  // A tab showing a page that is not on the web -- a new tab page, say -- is
  // replaced all the same, so that whatever is browsed to from it starts
  // empty too.
  static std::unique_ptr<content::WebContents> StartInEmptyState(
      content::WebContents* web_contents);

  // Stops archiving `web_contents` and closes its archive, if it is being
  // archived.
  static void Stop(content::WebContents* web_contents);

  // The capture `web_contents` records into, if it is being archived on its
  // own. Null-safe, since a navigation can be asked about before it has a tab.
  static std::optional<base::UnguessableToken> GetCaptureId(
      content::WebContents* web_contents);

  static void AddObserver(Observer* observer);
  static void RemoveObserver(Observer* observer);

  // Different for each capture the browser starts, so that something showing
  // one can tell when it has been replaced by another.
  int serial() const { return serial_; }

  // The capture's directory, empty until its files are open.
  const base::FilePath& directory() const { return directory_; }

  // Whether the network service is recording yet. Until it is, the tab waits
  // to load what it was started on.
  bool started() const { return started_; }

  // The most recent exchanges, oldest first.
  const base::circular_deque<Exchange>& exchanges() const {
    return exchanges_;
  }
  size_t exchange_count() const { return exchange_count_; }
  uint64_t body_bytes() const { return body_bytes_; }

  // A capture's directory and its files, opened off the UI thread.
  struct OpenedCapture;

 private:
  friend class content::WebContentsUserData<WarcTabCapture>;

  explicit WarcTabCapture(content::WebContents* web_contents);

  // Starts archiving `web_contents`, a tab made by StartInEmptyState().
  static void Start(content::WebContents* web_contents);

  // Hands the opened files to the network service. Static so that a tab that
  // stopped archiving, or closed, while they were opening still gets them
  // closed off the UI thread.
  static void OnCaptureOpened(base::WeakPtr<WarcTabCapture> self,
                              std::optional<OpenedCapture> opened);

  void OnCaptureStarted();

  // Loads the page a tab made by StartInEmptyState() is waiting to show, if it
  // still is.
  void LoadInitialUrl();

  // Writes the page waiting for its title, and closes the archive.
  void Finish();

  // content::WebContentsObserver:
  void WebContentsDestroyed() override;

  // network::mojom::WarcCaptureObserver:
  void OnExchangeRecorded(
      network::mojom::WarcRecordedExchangePtr exchange) override;

  static void NotifyCaptureChanged(content::WebContents* web_contents);

  const base::UnguessableToken capture_id_ = base::UnguessableToken::Create();
  const int serial_;

  // Empty until the capture's files are open.
  base::FilePath directory_;

  bool started_ = false;
  bool finished_ = false;

  base::circular_deque<Exchange> exchanges_;
  size_t exchange_count_ = 0;
  uint64_t body_bytes_ = 0;

  // Hears from the network service until the capture stops.
  mojo::Receiver<network::mojom::WarcCaptureObserver> observer_receiver_{this};

  // The page the tab is to load, which it does not until its requests will be
  // recorded. Empty for a tab that had nothing to show.
  std::optional<GURL> initial_url_;

  base::WeakPtrFactory<WarcTabCapture> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

#endif  // CHROME_BROWSER_WARC_TAB_CAPTURE_H_
