// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_RENDERER_RENDERER_CLOCK_H_
#define CONTENT_RENDERER_RENDERER_CLOCK_H_

#include <memory>

#include "base/time/time.h"
#include "base/time/time_override.h"
#include "content/common/content_export.h"

namespace base {
class CommandLine;
}  // namespace base

namespace content {

// A wall clock moved to another time, for as long as this object lives.
class CONTENT_EXPORT RendererClock {
 public:
  RendererClock(const RendererClock&) = delete;
  RendererClock& operator=(const RendererClock&) = delete;
  ~RendererClock();

 private:
  friend std::unique_ptr<RendererClock> StartClockFromCommandLine(
      const base::CommandLine& command_line);

  RendererClock();

  base::subtle::ScopedTimeClockOverrides override_;
};

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
// A page that compares a stored time with Date.now() then sees the two agree
// as they did when the page was written: a replayed page whose cached data
// expires five minutes after it was fetched finds it fresh, rather than stale
// and in need of a request the archive never captured. RestartClockIfAlone()
// keeps it so for each new document, not only the process's first.
//
// The override lasts as long as the returned object, which the renderer keeps
// for its whole life. Call it before the process reads the time, and only once:
// base allows a single override at a time.
CONTENT_EXPORT std::unique_ptr<RendererClock> StartClockFromCommandLine(
    const base::CommandLine& command_line);

// Starts the moved clock over for a document about to commit, so that the
// moment its navigation started -- `navigation_start`, or now if that is null
// or yet to come -- reads as the time the clock was started at. Does nothing,
// and returns false, unless the clock was moved, `only_document` is true, and
// no shared or service worker is running in the process.
//
// A clock that only ran on from the process's start would leave a page fresh
// on its first load and stale on a reload a few minutes later, when what a
// replay tool does, and what the page saw when it was captured, is to start
// each load at the moment. But the clock is the process's, and every document
// in the process reads it; one that is still open would see time go backwards.
// So it starts over only when nothing could notice: `only_document` says the
// committing frame and the frames below it -- all of which the commit
// replaces -- are the only frames in the process, and a worker that is no
// frame's keeps it running too. A process shared with another open page keeps
// its clock running, as it did before.
//
// A page kept for the back/forward cache is still open in this sense, and
// rightly: it comes back reading the clock it left with, never an earlier
// one. So a reload starts the clock over, but following a link usually does
// not, since the page left behind is kept in the same process.
//
// A dedicated worker of the document being replaced may read the time once
// more before it stops. It is going away with its document, and is not
// counted.
CONTENT_EXPORT bool RestartClockIfAlone(base::TimeTicks navigation_start,
                                        bool only_document);

// Held by a shared or service worker for as long as it runs, so that the
// clock does not start over under it. Any thread.
class CONTENT_EXPORT ScopedClockKeeper {
 public:
  ScopedClockKeeper();
  ScopedClockKeeper(const ScopedClockKeeper&) = delete;
  ScopedClockKeeper& operator=(const ScopedClockKeeper&) = delete;
  ~ScopedClockKeeper();
};

}  // namespace content

#endif  // CONTENT_RENDERER_RENDERER_CLOCK_H_
