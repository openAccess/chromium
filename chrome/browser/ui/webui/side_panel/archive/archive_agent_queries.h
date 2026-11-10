// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_SIDE_PANEL_ARCHIVE_ARCHIVE_AGENT_QUERIES_H_
#define CHROME_BROWSER_UI_WEBUI_SIDE_PANEL_ARCHIVE_ARCHIVE_AGENT_QUERIES_H_

#include <string>

#include "base/values.h"

namespace content {
class WebContents;
}  // namespace content

// Answers the archive assistant's `tool` (see archive_agent::kTools), asked
// with `params`, about the tab showing `contents`: what its capture wrote, or
// what its replay asked the archive for. Returns JSON for the model to read.
//
// Only looks. What it returns is what the panel shows, in a form a model can
// read, and no more: the tab's page, its requests and their outcomes.
std::string AnswerArchiveQuery(content::WebContents* contents,
                               const std::string& tool,
                               const base::DictValue& params);

#endif  // CHROME_BROWSER_UI_WEBUI_SIDE_PANEL_ARCHIVE_ARCHIVE_AGENT_QUERIES_H_
