// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_ARCHIVE_ARCHIVE_SIDE_PANEL_CONTROLLER_H_
#define CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_ARCHIVE_ARCHIVE_SIDE_PANEL_CONTROLLER_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/wacz_replay/replay_log.h"
#include "chrome/browser/warc_tab_capture.h"

class SidePanelEntryScope;
class SidePanelRegistry;

namespace tabs {
class TabInterface;
}  // namespace tabs

namespace views {
class View;
}  // namespace views

// The Archive side panel of one tab: registers it with the tab's side panels,
// and opens it when the tab starts archiving while in front, so that what is
// being recorded is in view from the start.
class ArchiveSidePanelController : public WarcTabCapture::Observer,
                                   public ReplayLog::Observer {
 public:
  ArchiveSidePanelController(tabs::TabInterface* tab,
                             SidePanelRegistry* registry);
  ArchiveSidePanelController(const ArchiveSidePanelController&) = delete;
  ArchiveSidePanelController& operator=(const ArchiveSidePanelController&) =
      delete;
  ~ArchiveSidePanelController() override;

  // WarcTabCapture::Observer:
  void OnCaptureChanged(content::WebContents* web_contents) override;

  // ReplayLog::Observer:
  void OnReplayStarted(content::WebContents* web_contents) override;

 private:
  // Opens the panel if the tab is in front.
  void ShowIfActive();

  std::unique_ptr<views::View> CreateView(SidePanelEntryScope& scope);

  const raw_ptr<tabs::TabInterface> tab_;

  // The capture the panel was last opened for, so that it opens once for each
  // and is not forced back open after the user closes it.
  int opened_for_capture_ = 0;
  // Likewise for a replay, which opens it once when the tab starts replaying.
  int opened_for_replay_ = 0;
};

#endif  // CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_ARCHIVE_ARCHIVE_SIDE_PANEL_CONTROLLER_H_
