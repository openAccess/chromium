// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_SIDE_PANEL_ARCHIVE_ARCHIVE_SIDE_PANEL_UI_H_
#define CHROME_BROWSER_UI_WEBUI_SIDE_PANEL_ARCHIVE_ARCHIVE_SIDE_PANEL_UI_H_

#include <memory>
#include <string_view>

#include "chrome/browser/ui/webui/side_panel/archive/archive.mojom.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"

class ArchivePageHandler;
class ArchiveSidePanelUI;

class ArchiveSidePanelUIConfig
    : public DefaultTopChromeWebUIConfig<ArchiveSidePanelUI> {
 public:
  ArchiveSidePanelUIConfig();
};

// The Archive side panel: what the tab it belongs to is archiving, as it
// happens. See WarcTabCapture.
class ArchiveSidePanelUI : public TopChromeWebUIController,
                           public archive_side_panel::mojom::PageHandlerFactory {
 public:
  explicit ArchiveSidePanelUI(content::WebUI* web_ui);
  ArchiveSidePanelUI(const ArchiveSidePanelUI&) = delete;
  ArchiveSidePanelUI& operator=(const ArchiveSidePanelUI&) = delete;
  ~ArchiveSidePanelUI() override;

  static constexpr std::string_view GetWebUIName() {
    return "ArchiveSidePanel";
  }

  void BindInterface(
      mojo::PendingReceiver<archive_side_panel::mojom::PageHandlerFactory>
          receiver);

 private:
  WEB_UI_CONTROLLER_TYPE_DECL();

  // archive_side_panel::mojom::PageHandlerFactory:
  void CreatePageHandler(
      mojo::PendingRemote<archive_side_panel::mojom::Page> page,
      mojo::PendingReceiver<archive_side_panel::mojom::PageHandler> receiver)
      override;

  std::unique_ptr<ArchivePageHandler> page_handler_;
  mojo::Receiver<archive_side_panel::mojom::PageHandlerFactory>
      page_factory_receiver_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_SIDE_PANEL_ARCHIVE_ARCHIVE_SIDE_PANEL_UI_H_
