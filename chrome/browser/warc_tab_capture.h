// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WARC_TAB_CAPTURE_H_
#define CHROME_BROWSER_WARC_TAB_CAPTURE_H_

#include <memory>
#include <optional>

#include "base/files/file_path.h"
#include "base/memory/weak_ptr.h"
#include "base/unguessable_token.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
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
class WarcTabCapture : public content::WebContentsObserver,
                       public content::WebContentsUserData<WarcTabCapture> {
 public:
  WarcTabCapture(const WarcTabCapture&) = delete;
  WarcTabCapture& operator=(const WarcTabCapture&) = delete;

  ~WarcTabCapture() override;

  // Whether `web_contents` is being archived on its own.
  static bool IsCapturing(content::WebContents* web_contents);

  // Starts archiving `web_contents` as it is, unless it already is: its
  // current page is reloaded from the network, but in whatever state the tab
  // already has.
  static void Start(content::WebContents* web_contents);

  // Makes the tab that archives what `web_contents` shows, from an empty state
  // (see above), and starts archiving it. The caller puts the returned tab in
  // place of `web_contents`; it loads the page once its capture has started.
  // Null when `web_contents` shows nothing a capture could hold.
  static std::unique_ptr<content::WebContents> StartInEmptyState(
      content::WebContents* web_contents);

  // Stops archiving `web_contents` and closes its archive, if it is being
  // archived.
  static void Stop(content::WebContents* web_contents);

  // The capture `web_contents` records into, if it is being archived on its
  // own. Null-safe, since a navigation can be asked about before it has a tab.
  static std::optional<base::UnguessableToken> GetCaptureId(
      content::WebContents* web_contents);

  // A capture's directory and its files, opened off the UI thread.
  struct OpenedCapture;

 private:
  friend class content::WebContentsUserData<WarcTabCapture>;

  explicit WarcTabCapture(content::WebContents* web_contents);

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

  const base::UnguessableToken capture_id_ = base::UnguessableToken::Create();

  // Empty until the capture's files are open.
  base::FilePath directory_;

  bool finished_ = false;

  // For a tab made by StartInEmptyState(): the page it is to load, which it
  // does not until its requests will be recorded.
  std::optional<GURL> initial_url_;

  base::WeakPtrFactory<WarcTabCapture> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

#endif  // CHROME_BROWSER_WARC_TAB_CAPTURE_H_
