// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_SIDE_PANEL_ARCHIVE_ARCHIVE_AGENT_QUERIES_H_
#define CHROME_BROWSER_UI_WEBUI_SIDE_PANEL_ARCHIVE_ARCHIVE_AGENT_QUERIES_H_

#include <string>

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/values.h"

namespace content {
class WebContents;
}  // namespace content

namespace warc {
struct CdxjEntry;
}  // namespace warc

// Answers the archive assistant's `tool` (see archive_agent::kTools), asked
// with `params`, about the tab showing `contents`: what its capture wrote, or
// what its replay asked the archive for. Returns JSON for the model to read.
//
// Only looks. What it returns is what the panel shows, in a form a model can
// read, and no more: the tab's page, its requests and their outcomes.
std::string AnswerArchiveQuery(content::WebContents* contents,
                               const std::string& tool,
                               const base::DictValue& params);

// The tool that looks in the archive itself rather than at what the tab asked
// of it. Answered by SearchArchive(), since the archive is read on a sequence
// of its own.
inline constexpr char kSearchArchiveTool[] = "search_archive";

// Answers search_archive, asked with `params`, about the archive the tab
// showing `contents` replays: runs `answer` with the JSON, later, on this
// sequence.
void SearchArchive(content::WebContents* contents,
                   const base::DictValue& params,
                   base::OnceCallback<void(std::string)> answer);

// What SearchArchive() answers from `index`, an archive's index sorted as an
// index is. Exposed for testing.
std::string SearchArchiveIndex(base::span<const warc::CdxjEntry> index,
                               const base::DictValue& params);

#endif  // CHROME_BROWSER_UI_WEBUI_SIDE_PANEL_ARCHIVE_ARCHIVE_AGENT_QUERIES_H_
