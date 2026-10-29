// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WARC_TAB_CAPTURE_H_
#define CHROME_BROWSER_WARC_TAB_CAPTURE_H_

#include <optional>

#include "base/files/file_path.h"
#include "base/memory/weak_ptr.h"
#include "base/unguessable_token.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

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
// Starting a capture reloads the tab, bypassing the cache: what a page already
// loaded is not recorded after the fact, and a response served from the cache
// never reaches the network service at all.
class WarcTabCapture : public content::WebContentsObserver,
                       public content::WebContentsUserData<WarcTabCapture> {
 public:
  WarcTabCapture(const WarcTabCapture&) = delete;
  WarcTabCapture& operator=(const WarcTabCapture&) = delete;

  ~WarcTabCapture() override;

  // Whether `web_contents` is being archived on its own.
  static bool IsCapturing(content::WebContents* web_contents);

  // Starts archiving `web_contents`, unless it already is.
  static void Start(content::WebContents* web_contents);

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

  // Writes the page waiting for its title, and closes the archive.
  void Finish();

  // content::WebContentsObserver:
  void WebContentsDestroyed() override;

  const base::UnguessableToken capture_id_ = base::UnguessableToken::Create();

  // Empty until the capture's files are open.
  base::FilePath directory_;

  bool finished_ = false;

  base::WeakPtrFactory<WarcTabCapture> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

#endif  // CHROME_BROWSER_WARC_TAB_CAPTURE_H_
