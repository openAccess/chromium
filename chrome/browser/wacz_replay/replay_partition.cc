// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wacz_replay/replay_partition.h"

#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"

namespace wacz_replay {

bool IsReplayPartition(const content::StoragePartitionConfig& config) {
  return config.partition_domain() == kPartitionDomain;
}

bool IsReplayTab(content::WebContents* web_contents) {
  return IsReplayPartition(
      web_contents->GetPrimaryMainFrame()->GetStoragePartition()->GetConfig());
}

}  // namespace wacz_replay
