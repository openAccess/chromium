// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/side_panel/archive/archive_side_panel_ui.h"

#include <utility>

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/side_panel/archive/archive_page_handler.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/grit/generated_resources.h"
#include "chrome/grit/side_panel_archive_resources.h"
#include "chrome/grit/side_panel_archive_resources_map.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "ui/base/webui/web_ui_util.h"
#include "ui/webui/webui_util.h"

ArchiveSidePanelUIConfig::ArchiveSidePanelUIConfig()
    : DefaultTopChromeWebUIConfig<ArchiveSidePanelUI>(
          content::kChromeUIScheme,
          chrome::kChromeUIArchiveSidePanelHost) {}

ArchiveSidePanelUI::ArchiveSidePanelUI(content::WebUI* web_ui)
    : TopChromeWebUIController(web_ui, /*enable_chrome_send=*/true) {
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      Profile::FromWebUI(web_ui), chrome::kChromeUIArchiveSidePanelHost);
  static constexpr webui::LocalizedString kStrings[] = {
      {"notArchiving", IDS_ARCHIVE_SIDE_PANEL_NOT_ARCHIVING},
      {"starting", IDS_ARCHIVE_SIDE_PANEL_STARTING},
      {"archiving", IDS_ARCHIVE_SIDE_PANEL_ARCHIVING},
      {"stopped", IDS_ARCHIVE_SIDE_PANEL_STOPPED},
      {"startArchiving", IDS_ARCHIVE_SIDE_PANEL_START},
      {"stopArchiving", IDS_ARCHIVE_SIDE_PANEL_STOP},
      {"showFolder", IDS_ARCHIVE_SIDE_PANEL_SHOW_FOLDER},
      {"filter", IDS_ARCHIVE_SIDE_PANEL_FILTER},
      {"notArchivingHint", IDS_ARCHIVE_SIDE_PANEL_NOT_ARCHIVING_HINT},
      {"requestOnly", IDS_ARCHIVE_SIDE_PANEL_REQUEST_ONLY},
      {"truncated", IDS_ARCHIVE_SIDE_PANEL_TRUNCATED},
      {"olderNotShown", IDS_ARCHIVE_SIDE_PANEL_OLDER_NOT_SHOWN},
      {"totals", IDS_ARCHIVE_SIDE_PANEL_TOTALS},
      {"statusColumn", IDS_ARCHIVE_SIDE_PANEL_STATUS_COLUMN},
      {"typeColumn", IDS_ARCHIVE_SIDE_PANEL_TYPE_COLUMN},
      {"sizeColumn", IDS_ARCHIVE_SIDE_PANEL_SIZE_COLUMN},
      {"urlColumn", IDS_ARCHIVE_SIDE_PANEL_URL_COLUMN},
      {"replaying", IDS_ARCHIVE_SIDE_PANEL_REPLAYING},
      {"replayingAsOf", IDS_ARCHIVE_SIDE_PANEL_REPLAYING_AS_OF},
      {"replayTotals", IDS_ARCHIVE_SIDE_PANEL_REPLAY_TOTALS},
      {"replayFilter", IDS_ARCHIVE_SIDE_PANEL_REPLAY_FILTER},
      {"showArchive", IDS_ARCHIVE_SIDE_PANEL_SHOW_ARCHIVE},
      {"missingStatus", IDS_ARCHIVE_SIDE_PANEL_MISSING_STATUS},
      {"missing", IDS_ARCHIVE_SIDE_PANEL_MISSING},
      {"failed", IDS_ARCHIVE_SIDE_PANEL_FAILED},
      {"answeredBy", IDS_ARCHIVE_SIDE_PANEL_ANSWERED_BY},
      {"capturedAt", IDS_ARCHIVE_SIDE_PANEL_CAPTURED_AT},
      {"missHostShort", IDS_ARCHIVE_SIDE_PANEL_MISS_HOST_SHORT},
      {"missPathShort", IDS_ARCHIVE_SIDE_PANEL_MISS_PATH_SHORT},
      {"missQueryShort", IDS_ARCHIVE_SIDE_PANEL_MISS_QUERY_SHORT},
      {"missMethodShort", IDS_ARCHIVE_SIDE_PANEL_MISS_METHOD_SHORT},
      {"missRangeShort", IDS_ARCHIVE_SIDE_PANEL_MISS_RANGE_SHORT},
      {"missUnreadableShort", IDS_ARCHIVE_SIDE_PANEL_MISS_UNREADABLE_SHORT},
      {"missHost", IDS_ARCHIVE_SIDE_PANEL_MISS_HOST},
      {"missPath", IDS_ARCHIVE_SIDE_PANEL_MISS_PATH},
      {"missQuery", IDS_ARCHIVE_SIDE_PANEL_MISS_QUERY},
      {"missMethod", IDS_ARCHIVE_SIDE_PANEL_MISS_METHOD},
      {"missRange", IDS_ARCHIVE_SIDE_PANEL_MISS_RANGE},
      {"missUnreadable", IDS_ARCHIVE_SIDE_PANEL_MISS_UNREADABLE},
      {"archiveUnreadable", IDS_ARCHIVE_SIDE_PANEL_ARCHIVE_UNREADABLE},
      {"requestsView", IDS_ARCHIVE_SIDE_PANEL_REQUESTS_VIEW},
      {"assistantView", IDS_ARCHIVE_SIDE_PANEL_ASSISTANT_VIEW},
      {"assistantHint", IDS_ARCHIVE_SIDE_PANEL_ASSISTANT_HINT},
      {"assistantPlaceholder", IDS_ARCHIVE_SIDE_PANEL_ASSISTANT_PLACEHOLDER},
      {"ask", IDS_ARCHIVE_SIDE_PANEL_ASK},
      {"stopAgent", IDS_ARCHIVE_SIDE_PANEL_STOP_AGENT},
  };
  source->AddLocalizedStrings(kStrings);
  webui::SetupWebUIDataSource(source, kSidePanelArchiveResources,
                              IDR_SIDE_PANEL_ARCHIVE_ARCHIVE_HTML);
}

ArchiveSidePanelUI::~ArchiveSidePanelUI() = default;

WEB_UI_CONTROLLER_TYPE_IMPL(ArchiveSidePanelUI)

void ArchiveSidePanelUI::BindInterface(
    mojo::PendingReceiver<archive_side_panel::mojom::PageHandlerFactory>
        receiver) {
  page_factory_receiver_.reset();
  page_factory_receiver_.Bind(std::move(receiver));
}

void ArchiveSidePanelUI::CreatePageHandler(
    mojo::PendingRemote<archive_side_panel::mojom::Page> page,
    mojo::PendingReceiver<archive_side_panel::mojom::PageHandler> receiver) {
  page_handler_ = std::make_unique<ArchivePageHandler>(
      std::move(receiver), std::move(page), *this, *web_ui());
}
