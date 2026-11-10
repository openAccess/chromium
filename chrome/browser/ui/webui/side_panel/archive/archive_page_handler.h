// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_SIDE_PANEL_ARCHIVE_ARCHIVE_PAGE_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_SIDE_PANEL_ARCHIVE_ARCHIVE_PAGE_HANDLER_H_

#include "base/callback_list.h"
#include "base/files/file_path.h"
#include "base/memory/raw_ref.h"
#include "chrome/browser/ui/webui/side_panel/archive/archive.mojom.h"
#include "chrome/browser/warc_tab_capture.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

class ArchiveSidePanelUI;

namespace content {
class WebContents;
class WebUI;
}  // namespace content

namespace tabs {
class TabInterface;
}  // namespace tabs

// The browser side of the Archive side panel. Speaks for the tab the panel
// belongs to: reports that tab's capture to the page, and starts and stops it
// on the page's behalf.
class ArchivePageHandler : public archive_side_panel::mojom::PageHandler,
                           public WarcTabCapture::Observer {
 public:
  ArchivePageHandler(
      mojo::PendingReceiver<archive_side_panel::mojom::PageHandler> receiver,
      mojo::PendingRemote<archive_side_panel::mojom::Page> page,
      ArchiveSidePanelUI& panel_ui,
      content::WebUI& web_ui);
  ArchivePageHandler(const ArchivePageHandler&) = delete;
  ArchivePageHandler& operator=(const ArchivePageHandler&) = delete;
  ~ArchivePageHandler() override;

  // archive_side_panel::mojom::PageHandler:
  void ShowUI() override;
  void GetCapture(GetCaptureCallback callback) override;
  void StartArchiving() override;
  void StopArchiving() override;
  void ShowDirectory() override;

  // WarcTabCapture::Observer:
  void OnCaptureChanged(content::WebContents* web_contents) override;
  void OnExchangeRecorded(content::WebContents* web_contents,
                          const WarcTabCapture::Exchange& exchange) override;

 private:
  // The tab the panel belongs to; null outside one.
  tabs::TabInterface* GetTab();

  // What the tab shows now. A tab that starts archiving is given new
  // contents, so this is asked each time rather than kept.
  content::WebContents* GetTabContents();

  // The tab is about to show `new_contents` in place of what it shows now.
  void OnTabWillDiscardContents(tabs::TabInterface* tab,
                                content::WebContents* old_contents,
                                content::WebContents* new_contents);

  // Keeps the directory of the capture shown, which outlives the capture: a
  // stopped one still has its archive to show.
  void RememberDirectory(WarcTabCapture* capture);

  mojo::Receiver<archive_side_panel::mojom::PageHandler> receiver_;
  mojo::Remote<archive_side_panel::mojom::Page> page_;
  const raw_ref<ArchiveSidePanelUI> panel_ui_;
  const raw_ref<content::WebUI> web_ui_;
  base::CallbackListSubscription will_discard_contents_subscription_;
  base::FilePath last_directory_;
};

#endif  // CHROME_BROWSER_UI_WEBUI_SIDE_PANEL_ARCHIVE_ARCHIVE_PAGE_HANDLER_H_
