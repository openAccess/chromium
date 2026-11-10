// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/side_panel/archive/archive_side_panel_controller.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/side_panel/side_panel_entry.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_id.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_scope.h"
#include "chrome/browser/ui/side_panel/side_panel_registry.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "chrome/browser/ui/views/side_panel/side_panel_web_ui_view.h"
#include "chrome/browser/ui/webui/side_panel/archive/archive_side_panel_ui.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/grit/generated_resources.h"
#include "components/tabs/public/tab_interface.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "url/gurl.h"

using SidePanelWebUIViewT_ArchiveSidePanelUI =
    SidePanelWebUIViewT<ArchiveSidePanelUI>;
BEGIN_TEMPLATE_METADATA(SidePanelWebUIViewT_ArchiveSidePanelUI,
                        SidePanelWebUIViewT)
END_METADATA

ArchiveSidePanelController::ArchiveSidePanelController(
    tabs::TabInterface* tab,
    SidePanelRegistry* registry)
    : tab_(tab) {
  registry->Register(std::make_unique<SidePanelEntry>(
      SidePanelEntry::Key(SidePanelEntry::Id::kArchive),
      base::BindRepeating(&ArchiveSidePanelController::CreateView,
                          base::Unretained(this)),
      /*default_content_width_callback=*/base::NullCallback()));
  WarcTabCapture::AddObserver(this);
}

// The tab's registry goes with the tab, straight after this, so the entry
// whose callback points here is not left behind; other tab-scoped panels rely
// on the same.
ArchiveSidePanelController::~ArchiveSidePanelController() {
  WarcTabCapture::RemoveObserver(this);
}

void ArchiveSidePanelController::OnCaptureChanged(
    content::WebContents* web_contents) {
  if (web_contents != tab_->GetContents()) {
    return;
  }
  WarcTabCapture* capture = WarcTabCapture::FromWebContents(web_contents);
  if (!capture || capture->serial() == opened_for_capture_) {
    return;
  }
  opened_for_capture_ = capture->serial();
  // A tab archived from the back stays out of the way; its panel opens when
  // asked for.
  if (!tab_->IsActivated()) {
    return;
  }
  if (SidePanelUI* side_panel =
          SidePanelUI::From(tab_->GetBrowserWindowInterface())) {
    side_panel->Show(SidePanelEntry::Id::kArchive);
  }
}

std::unique_ptr<views::View> ArchiveSidePanelController::CreateView(
    SidePanelEntryScope& scope) {
  return std::make_unique<SidePanelWebUIViewT<ArchiveSidePanelUI>>(
      scope, base::RepeatingClosure(), base::RepeatingClosure(),
      std::make_unique<WebUIContentsWrapperT<ArchiveSidePanelUI>>(
          GURL(chrome::kChromeUIArchiveSidePanelURL),
          scope.GetBrowserWindowInterface().GetProfile(),
          IDS_ARCHIVE_SIDE_PANEL_TITLE,
          /*esc_closes_ui=*/false));
}
