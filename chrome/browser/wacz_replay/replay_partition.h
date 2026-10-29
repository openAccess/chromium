// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WACZ_REPLAY_REPLAY_PARTITION_H_
#define CHROME_BROWSER_WACZ_REPLAY_REPLAY_PARTITION_H_

#include "content/public/browser/storage_partition_config.h"

namespace content {
class WebContents;
}  // namespace content

namespace wacz_replay {

// The domain every replay partition lives in, whatever archive and moment it
// holds. It is what IsReplayPartition() recognises, so one test answers for
// all of them.
inline constexpr char kPartitionDomain[] = "wacz-replay";

// Whether `config` is a partition returned by ReplayPartition() -- for any
// archive, since what follows from it is the same for all of them.
bool IsReplayPartition(const content::StoragePartitionConfig& config);

// Whether `web_contents` is a replay tab. A tab's partition is fixed when it is
// made, so a tab that is not a replay tab now never becomes one.
bool IsReplayTab(content::WebContents* web_contents);

}  // namespace wacz_replay

#endif  // CHROME_BROWSER_WACZ_REPLAY_REPLAY_PARTITION_H_
