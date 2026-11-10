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
