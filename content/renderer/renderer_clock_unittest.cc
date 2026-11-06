// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/renderer_clock.h"

#include "base/command_line.h"
#include "base/strings/string_number_conversions.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/time/time_override.h"
#include "content/public/common/content_switches.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace content {
namespace {

// 2026-10-30 22:56:29 UTC.
constexpr int64_t kStartMs = 1793400989000;

base::CommandLine WithClockStart(const std::string& value) {
  base::CommandLine command_line(base::CommandLine::NO_PROGRAM);
  command_line.AppendSwitchASCII(switches::kRendererClockStart, value);
  return command_line;
}

base::Time Start() {
  return base::Time::UnixEpoch() + base::Milliseconds(kStartMs);
}

TEST(RendererClockTest, NoSwitchLeavesTheClockAlone) {
  EXPECT_FALSE(StartClockFromCommandLine(
      base::CommandLine(base::CommandLine::NO_PROGRAM)));
  EXPECT_LT(
      (base::Time::Now() - base::subtle::TimeNowIgnoringOverride()).magnitude(),
      base::Seconds(1));
}

TEST(RendererClockTest, SomethingNotATimeLeavesTheClockAlone) {
  for (const char* value :
       {"", "yesterday", "20261030225629Z", "-5", "0", "1793400989000.5"}) {
    SCOPED_TRACE(value);
    EXPECT_FALSE(StartClockFromCommandLine(WithClockStart(value)));
  }
  EXPECT_LT(
      (base::Time::Now() - base::subtle::TimeNowIgnoringOverride()).magnitude(),
      base::Seconds(1));
}

TEST(RendererClockTest, TheClockStartsAtTheTimeNamed) {
  auto clock =
      StartClockFromCommandLine(WithClockStart(base::NumberToString(kStartMs)));
  ASSERT_TRUE(clock);
  EXPECT_LT((base::Time::Now() - Start()).magnitude(), base::Seconds(1));
  EXPECT_LT((base::Time::NowFromSystemTime() - Start()).magnitude(),
            base::Seconds(1));
}

TEST(RendererClockTest, TheClockRunsOnFromThere) {
  auto clock =
      StartClockFromCommandLine(WithClockStart(base::NumberToString(kStartMs)));
  ASSERT_TRUE(clock);
  const base::Time before = base::Time::Now();
  const base::Time real_before = base::subtle::TimeNowIgnoringOverride();
  base::PlatformThread::Sleep(base::Milliseconds(50));
  const base::TimeDelta moved = base::Time::Now() - before;
  const base::TimeDelta real_moved =
      base::subtle::TimeNowIgnoringOverride() - real_before;
  EXPECT_GE(moved, base::Milliseconds(50));
  EXPECT_LT((moved - real_moved).magnitude(), base::Milliseconds(20));
}

TEST(RendererClockTest, TicksDoNotMove) {
  auto clock =
      StartClockFromCommandLine(WithClockStart(base::NumberToString(kStartMs)));
  ASSERT_TRUE(clock);
  EXPECT_LT(
      (base::TimeTicks::Now() - base::subtle::TimeTicksNowIgnoringOverride())
          .magnitude(),
      base::Seconds(1));
}

TEST(RendererClockTest, TheClockComesBackWithTheOverride) {
  StartClockFromCommandLine(WithClockStart(base::NumberToString(kStartMs)));
  EXPECT_LT(
      (base::Time::Now() - base::subtle::TimeNowIgnoringOverride()).magnitude(),
      base::Seconds(1));
}

}  // namespace
}  // namespace content
