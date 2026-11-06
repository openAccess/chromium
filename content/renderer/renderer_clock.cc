// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/renderer_clock.h"

#include <atomic>
#include <cstdint>
#include <string>

#include "base/command_line.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "content/public/common/content_switches.h"

namespace content {

namespace {

// How far the clock is moved, in microseconds. Read on every thread that asks
// the time; written once, before the override that reads it is installed.
std::atomic<int64_t> g_clock_offset_us{0};

base::Time ShiftedNow() {
  return base::subtle::TimeNowIgnoringOverride() +
         base::Microseconds(g_clock_offset_us.load(std::memory_order_relaxed));
}

}  // namespace

std::unique_ptr<base::subtle::ScopedTimeClockOverrides>
StartClockFromCommandLine(const base::CommandLine& command_line) {
  const std::string value =
      command_line.GetSwitchValueASCII(switches::kRendererClockStart);
  int64_t start_ms;
  if (value.empty() || !base::StringToInt64(value, &start_ms) ||
      start_ms <= 0) {
    return nullptr;
  }
  const base::Time start =
      base::Time::UnixEpoch() + base::Milliseconds(start_ms);
  g_clock_offset_us.store(
      (start - base::subtle::TimeNowIgnoringOverride()).InMicroseconds(),
      std::memory_order_relaxed);
  // base answers Time::NowFromSystemTime() with the same override, so the two
  // cannot disagree.
  return std::make_unique<base::subtle::ScopedTimeClockOverrides>(
      &ShiftedNow, /*time_ticks_override=*/nullptr,
      /*thread_ticks_override=*/nullptr);
}

}  // namespace content
