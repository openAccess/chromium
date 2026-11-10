// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/side_panel/archive/archive_page_handler.h"

#include <stdint.h>

#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/numerics/safe_conversions.h"
#include "chrome/browser/platform_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/side_panel/archive/archive_side_panel_ui.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"

namespace {

using archive_side_panel::mojom::CaptureState;
using archive_side_panel::mojom::CaptureStatus;
using archive_side_panel::mojom::CaptureStatusPtr;
using archive_side_panel::mojom::Exchange;
using archive_side_panel::mojom::ExchangePtr;

WarcTabCapture* CaptureOf(content::WebContents* contents) {
  return contents ? WarcTabCapture::FromWebContents(contents) : nullptr;
}

CaptureStatusPtr StatusOf(WarcTabCapture* capture) {
  CaptureStatusPtr status = CaptureStatus::New();
  if (!capture) {
    status->state = CaptureState::kNotArchiving;
    return status;
  }
  status->state =
      capture->started() ? CaptureState::kArchiving : CaptureState::kStarting;
  status->capture_number = capture->serial();
  status->directory = capture->directory().AsUTF8Unsafe();
  status->exchange_count =
      base::saturated_cast<uint32_t>(capture->exchange_count());
  status->body_bytes = static_cast<double>(capture->body_bytes());
  return status;
}

ExchangePtr ToMojom(const WarcTabCapture::Exchange& exchange, size_t index) {
  return Exchange::New(base::saturated_cast<uint32_t>(index),
                       exchange.url.spec(), exchange.method,
                       exchange.status_code, exchange.mime_type,
                       static_cast<double>(exchange.body_bytes),
                       exchange.truncated,
                       exchange.time.InMillisecondsFSinceUnixEpoch());
}

}  // namespace

ArchivePageHandler::ArchivePageHandler(
    mojo::PendingReceiver<archive_side_panel::mojom::PageHandler> receiver,
    mojo::PendingRemote<archive_side_panel::mojom::Page> page,
    ArchiveSidePanelUI& panel_ui,
    content::WebUI& web_ui)
    : receiver_(this, std::move(receiver)),
      page_(std::move(page)),
      panel_ui_(panel_ui),
      web_ui_(web_ui) {
  WarcTabCapture::AddObserver(this);
  if (tabs::TabInterface* tab = GetTab()) {
    will_discard_contents_subscription_ =
        tab->RegisterWillDiscardContents(base::BindRepeating(
            &ArchivePageHandler::OnTabWillDiscardContents,
            base::Unretained(this)));
  }
}

ArchivePageHandler::~ArchivePageHandler() {
  WarcTabCapture::RemoveObserver(this);
}

void ArchivePageHandler::ShowUI() {
  if (auto embedder = panel_ui_->embedder()) {
    embedder->ShowUI();
  }
}

void ArchivePageHandler::GetCapture(GetCaptureCallback callback) {
  WarcTabCapture* capture = CaptureOf(GetTabContents());
  std::vector<ExchangePtr> exchanges;
  if (capture) {
    RememberDirectory(capture);
    // The kept exchanges are the most recent; the first of them is numbered
    // after those no longer kept.
    size_t index = capture->exchange_count() - capture->exchanges().size();
    for (const WarcTabCapture::Exchange& exchange : capture->exchanges()) {
      exchanges.push_back(ToMojom(exchange, ++index));
    }
  }
  std::move(callback).Run(StatusOf(capture), std::move(exchanges));
}

void ArchivePageHandler::StartArchiving() {
  tabs::TabInterface* tab = GetTab();
  content::WebContents* contents = GetTabContents();
  if (!contents || WarcTabCapture::IsCapturing(contents)) {
    return;
  }
  BrowserWindowInterface* window = tab->GetBrowserWindowInterface();
  TabStripModel* tab_strip = window ? window->GetTabStripModel() : nullptr;
  if (!tab_strip || tab_strip->GetIndexOfTab(tab) == TabStripModel::kNoTab) {
    return;
  }
  // As "Archive tab" does: the tab is replaced where it stands by one that
  // starts from nothing, and the contents it showed go.
  tab_strip->DiscardWebContents(contents,
                                WarcTabCapture::StartInEmptyState(contents));
}

void ArchivePageHandler::StopArchiving() {
  if (content::WebContents* contents = GetTabContents()) {
    WarcTabCapture::Stop(contents);
  }
}

void ArchivePageHandler::ShowDirectory() {
  if (WarcTabCapture* capture = CaptureOf(GetTabContents())) {
    RememberDirectory(capture);
  }
  if (last_directory_.empty()) {
    return;
  }
  platform_util::OpenItem(Profile::FromWebUI(&*web_ui_), last_directory_,
                          platform_util::OPEN_FOLDER, base::DoNothing());
}

void ArchivePageHandler::OnCaptureChanged(content::WebContents* web_contents) {
  if (web_contents != GetTabContents()) {
    return;
  }
  WarcTabCapture* capture = CaptureOf(web_contents);
  if (capture) {
    RememberDirectory(capture);
  }
  page_->OnCaptureChanged(StatusOf(capture));
}

void ArchivePageHandler::OnExchangeRecorded(
    content::WebContents* web_contents,
    const WarcTabCapture::Exchange& exchange) {
  if (web_contents != GetTabContents()) {
    return;
  }
  // Reported once counted, so the count is this exchange's number.
  page_->OnExchangeRecorded(
      ToMojom(exchange, CaptureOf(web_contents)->exchange_count()));
}

tabs::TabInterface* ArchivePageHandler::GetTab() {
  return webui::GetTabInterface(web_ui_->GetWebContents());
}

content::WebContents* ArchivePageHandler::GetTabContents() {
  tabs::TabInterface* tab = GetTab();
  return tab ? tab->GetContents() : nullptr;
}

void ArchivePageHandler::OnTabWillDiscardContents(
    tabs::TabInterface* tab,
    content::WebContents* old_contents,
    content::WebContents* new_contents) {
  // The capture a tab starts belongs to the contents it is about to show, so
  // this is the first the page can hear of it.
  WarcTabCapture* capture = CaptureOf(new_contents);
  if (capture) {
    RememberDirectory(capture);
  }
  page_->OnCaptureChanged(StatusOf(capture));
}

void ArchivePageHandler::RememberDirectory(WarcTabCapture* capture) {
  if (!capture->directory().empty()) {
    last_directory_ = capture->directory();
  }
}
