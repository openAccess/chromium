// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WARC_PAGE_RECORDER_H_
#define CHROME_BROWSER_WARC_PAGE_RECORDER_H_

#include <optional>
#include <string>

#include "base/time/time.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "url/gurl.h"

// Records the pages a --warc-output capture visited, as the "pages.jsonl" a
// WACZ requires beside the archives themselves.
//
// The archive can be built from the WARC files alone, but not this: a WARC
// says which resources were fetched, never which of them a person navigated to
// or what the result was called. Deriving it afterwards means guessing -- the
// tooling picks likely-looking HTML responses and uses the URL where the title
// belongs. Only the browser knows, so only the browser can write it.
//
// One of these is attached per tab, and each appends to the one file the
// capture shares.
class WarcPageRecorder : public content::WebContentsObserver,
                         public content::WebContentsUserData<WarcPageRecorder> {
 public:
  WarcPageRecorder(const WarcPageRecorder&) = delete;
  WarcPageRecorder& operator=(const WarcPageRecorder&) = delete;

  ~WarcPageRecorder() override;

  // Attaches a recorder if this browser is capturing to a WARC. Costs nothing
  // when it is not, which is every ordinary run.
  static void MaybeCreateForWebContents(content::WebContents* web_contents);

 private:
  friend class content::WebContentsUserData<WarcPageRecorder>;

  explicit WarcPageRecorder(content::WebContents* web_contents);

  // content::WebContentsObserver:
  void DidFinishNavigation(content::NavigationHandle* handle) override;
  void DidStopLoading() override;
  void WebContentsDestroyed() override;

  // Writes the page that was waiting for a title, if there is one.
  //
  // A page is written when it stops loading rather than when it commits,
  // because its title is not known at commit -- the document has not been
  // parsed yet. Anything that ends the wait writes what is known by then: the
  // next navigation, the tab closing, or the browser exiting.
  void WritePendingPage();

  // A page that has committed but is not written yet. Its URL and time are
  // fixed at commit, so a slow page is dated when it was requested rather than
  // when it finished.
  struct PendingPage {
    GURL url;
    base::Time at;
    std::string id;
  };
  std::optional<PendingPage> pending_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

#endif  // CHROME_BROWSER_WARC_PAGE_RECORDER_H_
