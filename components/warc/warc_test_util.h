// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_WARC_WARC_TEST_UTIL_H_
#define COMPONENTS_WARC_WARC_TEST_UTIL_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace warc {

// Walks `data` as a sequence of concatenated gzip members and returns each
// member's decompressed content, or nullopt if the bytes are not a clean
// concatenation of complete members.
//
// Tests walk member by member rather than handing the whole archive to a
// decompressor because where the member boundaries fall is the thing worth
// asserting: a ".warc.gz" is only randomly accessible if each record sits in a
// member of its own. Concatenating the members' contents would look identical
// either way.
std::optional<std::vector<std::string>> InflateGzipMembers(
    std::string_view data);

}  // namespace warc

#endif  // COMPONENTS_WARC_WARC_TEST_UTIL_H_
