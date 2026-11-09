// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/renderer_clock.h"

#include <atomic>
#include <cstdint>
#include <string>

#include "base/check_op.h"
#include "base/command_line.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "content/public/common/content_switches.h"

namespace content {

namespace {

// How far the clock is moved, in microseconds. Read on every thread that asks
// the time; written before the override that reads it is installed, and again
// whenever the clock starts over.
std::atomic<int64_t> g_clock_offset_us{0};

// The time the clock starts at, in microseconds since the Unix epoch, or 0
// when it is not moved.
std::atomic<int64_t> g_clock_start_us{0};

// Shared and service workers running in the process.
std::atomic<int> g_clock_keepers{0};

base::Time ShiftedNow() {
  return base::subtle::TimeNowIgnoringOverride() +
         base::Microseconds(g_clock_offset_us.load(std::memory_order_relaxed));
}

// Moves the clock so that it read `start` at the real time `real_then`.
void AnchorClock(base::Time start, base::Time real_then) {
  g_clock_offset_us.store((start - real_then).InMicroseconds(),
                          std::memory_order_relaxed);
}

}  // namespace

RendererClock::RendererClock()
    : override_(&ShiftedNow,
                /*time_ticks_override=*/nullptr,
                /*thread_ticks_override=*/nullptr) {}

RendererClock::~RendererClock() {
  g_clock_start_us.store(0, std::memory_order_relaxed);
  g_clock_offset_us.store(0, std::memory_order_relaxed);
}

std::unique_ptr<RendererClock> StartClockFromCommandLine(
    const base::CommandLine& command_line) {
  const std::string value =
      command_line.GetSwitchValueASCII(switches::kRendererClockStart);
  int64_t start_ms;
  if (value.empty() || !base::StringToInt64(value, &start_ms) ||
      start_ms <= 0) {
    return nullptr;
  }
  const base::Time start =
      base::Time::UnixEpoch() + base::Milliseconds(start_ms);
  AnchorClock(start, base::subtle::TimeNowIgnoringOverride());
  g_clock_start_us.store((start - base::Time::UnixEpoch()).InMicroseconds(),
                         std::memory_order_relaxed);
  // base answers Time::NowFromSystemTime() with the same override, so the two
  // cannot disagree.
  return base::WrapUnique(new RendererClock());
}

bool RestartClockIfAlone(base::TimeTicks navigation_start, bool only_document) {
  const int64_t start_us = g_clock_start_us.load(std::memory_order_relaxed);
  if (!start_us || !only_document ||
      g_clock_keepers.load(std::memory_order_relaxed) > 0) {
    return false;
  }
  const base::TimeTicks now_ticks = base::TimeTicks::Now();
  const base::TimeDelta since_navigation_start =
      navigation_start.is_null() || navigation_start > now_ticks
          ? base::TimeDelta()
          : now_ticks - navigation_start;
  AnchorClock(base::Time::UnixEpoch() + base::Microseconds(start_us),
              base::subtle::TimeNowIgnoringOverride() - since_navigation_start);
  return true;
}

ScopedClockKeeper::ScopedClockKeeper() {
  g_clock_keepers.fetch_add(1, std::memory_order_relaxed);
}

ScopedClockKeeper::~ScopedClockKeeper() {
  const int before = g_clock_keepers.fetch_sub(1, std::memory_order_relaxed);
  DCHECK_GT(before, 0);
}

}  // namespace content
