// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_RENDERER_RENDERER_CLOCK_H_
#define CONTENT_RENDERER_RENDERER_CLOCK_H_

#include <memory>

#include "base/time/time_override.h"
#include "content/common/content_export.h"

namespace base {
class CommandLine;
}  // namespace base

namespace content {

// Starts this process's wall clock at the time --renderer-clock-start names,
// so that base::Time::Now() reads that time now and runs on from it at the
// real clock's pace. Returns null, and leaves the clock alone, when the switch
// is absent or is not a time.
//
// Every wall clock a page can read is base::Time::Now() underneath: Date and
// Temporal through gin, performance.timeOrigin, document.lastModified. Moving
// that one clock moves all of them together, in every frame and worker the
// process runs, before any script has read the real one -- which a script
// injected into each page could not promise. Only the wall clock moves.
// TimeTicks are untouched, so timers, animation and anything measuring a
// duration run exactly as they would have.
//
// The clock is the process's from its first moment, so a page that compares a
// stored time with Date.now() sees the two agree as they did when the page was
// written: a replayed page whose cached data expires five minutes after it was
// fetched finds it fresh, rather than stale and in need of a request the
// archive never captured.
//
// The override lasts as long as the returned object, which the renderer keeps
// for its whole life. Call it before the process reads the time, and only once:
// base allows a single override at a time.
CONTENT_EXPORT std::unique_ptr<base::subtle::ScopedTimeClockOverrides>
StartClockFromCommandLine(const base::CommandLine& command_line);

}  // namespace content

#endif  // CONTENT_RENDERER_RENDERER_CLOCK_H_
