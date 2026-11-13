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
#include "chrome/browser/ui/webui/side_panel/archive/archive_agent_queries.h"
#include "chrome/browser/wacz_replay/replay_partition.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/side_panel/archive/archive_side_panel_ui.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "components/tabs/public/tab_interface.h"
#include "components/warc/cdxj.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "net/base/net_errors.h"

namespace {

using archive_side_panel::mojom::AgentEvent;
using archive_side_panel::mojom::AgentEventKind;
using archive_side_panel::mojom::CaptureState;
using archive_side_panel::mojom::CaptureStatus;
using archive_side_panel::mojom::CaptureStatusPtr;
using archive_side_panel::mojom::Exchange;
using archive_side_panel::mojom::ExchangePtr;
using archive_side_panel::mojom::MissReason;

WarcTabCapture* CaptureOf(content::WebContents* contents) {
  return contents ? WarcTabCapture::FromWebContents(contents) : nullptr;
}

double ToMs(base::Time time) {
  return time.is_null() ? 0 : time.InMillisecondsFSinceUnixEpoch();
}

CaptureStatusPtr CaptureStatusOf(WarcTabCapture* capture) {
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

// A replay tab's status. `log` is null for one that has asked for nothing yet.
CaptureStatusPtr ReplayStatusOf(ReplayLog* log) {
  CaptureStatusPtr status = CaptureStatus::New();
  status->state = CaptureState::kReplaying;
  if (!log) {
    return status;
  }
  status->capture_number = log->serial();
  status->directory = log->archive().AsUTF8Unsafe();
  status->exchange_count =
      base::saturated_cast<uint32_t>(log->request_count());
  status->body_bytes = static_cast<double>(log->body_bytes());
  status->missing_count = base::saturated_cast<uint32_t>(log->missing_count());
  status->moment_ms = ToMs(log->moment());
  return status;
}

ExchangePtr ToMojom(const WarcTabCapture::Exchange& exchange, size_t index) {
  ExchangePtr result = Exchange::New();
  result->index = base::saturated_cast<uint32_t>(index);
  result->url = exchange.url.spec();
  result->method = exchange.method;
  result->status_code = exchange.status_code;
  result->mime_type = exchange.mime_type;
  result->body_bytes = static_cast<double>(exchange.body_bytes);
  result->truncated = exchange.truncated;
  result->time_ms = ToMs(exchange.time);
  return result;
}

MissReason ToMojom(warc::MissReason reason) {
  switch (reason) {
    case warc::MissReason::kUnknown:
      return MissReason::kUnknown;
    case warc::MissReason::kHostNotCaptured:
      return MissReason::kHostNotCaptured;
    case warc::MissReason::kPathNotCaptured:
      return MissReason::kPathNotCaptured;
    case warc::MissReason::kOtherQuery:
      return MissReason::kOtherQuery;
    case warc::MissReason::kOtherMethod:
      return MissReason::kOtherMethod;
    case warc::MissReason::kRangeNotHeld:
      return MissReason::kRangeNotHeld;
    case warc::MissReason::kUnreadable:
      return MissReason::kUnreadable;
  }
  return MissReason::kUnknown;
}

ExchangePtr ToMojom(const warc::ReplayOutcome& request, size_t index) {
  ExchangePtr result = Exchange::New();
  result->index = base::saturated_cast<uint32_t>(index);
  result->url = request.url.spec();
  result->method = request.method;
  result->status_code = request.status_code;
  result->mime_type = request.mime_type;
  result->body_bytes = static_cast<double>(request.body_bytes);
  result->time_ms = ToMs(request.time);
  result->missing = request.missing;
  if (request.missing) {
    result->miss_reason = ToMojom(request.miss.reason);
    result->nearest_url = request.miss.url;
    result->nearest_method = request.miss.method;
    result->nearest_captured_ms =
        ToMs(warc::ParseIndexTimestamp(request.miss.timestamp));
  }
  if (request.net_error != net::OK) {
    result->error = net::ErrorToShortString(request.net_error);
  }
  result->destination = request.destination;
  if (!request.captured_url.is_empty()) {
    result->captured_url = request.captured_url.spec();
  }
  result->captured_ms = ToMs(request.captured);
  return result;
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
  ReplayLog::AddObserver(this);
  if (tabs::TabInterface* tab = GetTab()) {
    will_discard_contents_subscription_ =
        tab->RegisterWillDiscardContents(base::BindRepeating(
            &ArchivePageHandler::OnTabWillDiscardContents,
            base::Unretained(this)));
  }
}

ArchivePageHandler::~ArchivePageHandler() {
  ReplayLog::RemoveObserver(this);
  WarcTabCapture::RemoveObserver(this);
}

void ArchivePageHandler::ShowUI() {
  if (auto embedder = panel_ui_->embedder()) {
    embedder->ShowUI();
  }
}

void ArchivePageHandler::GetCapture(GetCaptureCallback callback) {
  content::WebContents* contents = GetTabContents();
  std::vector<ExchangePtr> exchanges;
  if (contents && wacz_replay::IsReplayTab(contents)) {
    if (ReplayLog* log = ReplayLog::FromWebContents(contents)) {
      size_t index = log->request_count() - log->requests().size();
      for (const warc::ReplayOutcome& request : log->requests()) {
        exchanges.push_back(ToMojom(request, ++index));
      }
    }
    std::move(callback).Run(StatusOf(contents), std::move(exchanges));
    return;
  }
  WarcTabCapture* capture = CaptureOf(contents);
  if (capture) {
    RememberDirectory(capture);
    // The kept exchanges are the most recent; the first of them is numbered
    // after those no longer kept.
    size_t index = capture->exchange_count() - capture->exchanges().size();
    for (const WarcTabCapture::Exchange& exchange : capture->exchanges()) {
      exchanges.push_back(ToMojom(exchange, ++index));
    }
  }
  std::move(callback).Run(StatusOf(contents), std::move(exchanges));
}

void ArchivePageHandler::StartArchiving() {
  tabs::TabInterface* tab = GetTab();
  content::WebContents* contents = GetTabContents();
  // A replay tab is answered out of an archive, and archiving it would mean
  // starting it over on the web.
  if (!contents || WarcTabCapture::IsCapturing(contents) ||
      wacz_replay::IsReplayTab(contents)) {
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
  content::WebContents* contents = GetTabContents();
  if (ReplayLog* log =
          contents ? ReplayLog::FromWebContents(contents) : nullptr) {
    platform_util::ShowItemInFolder(Profile::FromWebUI(&*web_ui_),
                                    log->archive());
    return;
  }
  if (WarcTabCapture* capture = CaptureOf(contents)) {
    RememberDirectory(capture);
  }
  if (last_directory_.empty()) {
    return;
  }
  platform_util::OpenItem(Profile::FromWebUI(&*web_ui_), last_directory_,
                          platform_util::OPEN_FOLDER, base::DoNothing());
}

void ArchivePageHandler::AskAgent(const std::string& question) {
  if (!agent_) {
    agent_ = std::make_unique<archive_agent::ArchiveAgent>(*this);
  }
  agent_->Ask(question);
}

void ArchivePageHandler::StopAgent() {
  if (agent_) {
    agent_->Stop();
  }
}

void ArchivePageHandler::OnAgentText(const std::string& text) {
  page_->OnAgentEvent(AgentEvent::New(AgentEventKind::kText, text));
}

void ArchivePageHandler::OnAgentToolCall(const std::string& tool,
                                         const std::string& arguments) {
  page_->OnAgentEvent(
      AgentEvent::New(AgentEventKind::kToolCall,
                      arguments == "{}" ? tool : tool + " " + arguments));
}

void ArchivePageHandler::OnAgentIdle() {
  page_->OnAgentEvent(AgentEvent::New(AgentEventKind::kIdle, std::string()));
}

void ArchivePageHandler::OnAgentError(const std::string& message) {
  page_->OnAgentEvent(AgentEvent::New(AgentEventKind::kError, message));
}

void ArchivePageHandler::AnswerQuery(
    const std::string& tool,
    const base::DictValue& params,
    base::OnceCallback<void(std::string)> answer) {
  if (tool == kSearchArchiveTool) {
    SearchArchive(GetTabContents(), params, std::move(answer));
    return;
  }
  std::move(answer).Run(AnswerArchiveQuery(GetTabContents(), tool, params));
}

void ArchivePageHandler::OnCaptureChanged(content::WebContents* web_contents) {
  if (web_contents != GetTabContents()) {
    return;
  }
  if (WarcTabCapture* capture = CaptureOf(web_contents)) {
    RememberDirectory(capture);
  }
  page_->OnCaptureChanged(StatusOf(web_contents));
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

void ArchivePageHandler::OnReplayStarted(content::WebContents* web_contents) {
  if (web_contents != GetTabContents()) {
    return;
  }
  page_->OnCaptureChanged(StatusOf(web_contents));
}

void ArchivePageHandler::OnReplayRequest(content::WebContents* web_contents,
                                         const warc::ReplayOutcome& request) {
  if (web_contents != GetTabContents()) {
    return;
  }
  // Reported once counted, so the count is this request's number.
  page_->OnExchangeRecorded(ToMojom(
      request, ReplayLog::FromWebContents(web_contents)->request_count()));
}

CaptureStatusPtr ArchivePageHandler::StatusOf(content::WebContents* contents) {
  if (contents && wacz_replay::IsReplayTab(contents)) {
    return ReplayStatusOf(ReplayLog::FromWebContents(contents));
  }
  return CaptureStatusOf(CaptureOf(contents));
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
  if (WarcTabCapture* capture = CaptureOf(new_contents)) {
    RememberDirectory(capture);
  }
  page_->OnCaptureChanged(StatusOf(new_contents));
}

void ArchivePageHandler::RememberDirectory(WarcTabCapture* capture) {
  if (!capture->directory().empty()) {
    last_directory_ = capture->directory();
  }
}
