/******************************************************************************
    Copyright (C) 2026 by Braidcast

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#pragma once

#include "util/c99defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 *   Under game load the graphics thread's post-video_sleep region -- reported as
 * "prev tail", since it is charged to the frame after the one that ran it --
 * measured a 40.3 ms median and a 523.4 ms worst case across the 50 recorded
 * events over 5 ms -- 6.0 ms being the smallest of those, which is a floor on
 * the filter and not on the region. That region covers the once-a-second
 * rollup, stop_requested and the diagnostic's own logging together, so it bounds
 * what the logging cost rather than isolating it; no single blog() call was ever
 * timed on its own. The same region measured 0.0 ms in a clean run once GPU
 * contention was removed, which is why no mechanism is claimed here.
 *
 *   The logging moved off the thread anyway: whatever the reason for the price,
 * a diagnostic must not be able to spend frame budgets on the thread whose
 * missed frames are its subject.
 *
 *   obs_debug_logf formats the line into a fixed ring of pre-rendered slots and
 * returns; one dedicated thread drains the ring and makes the blog() calls. The
 * producer never allocates and never waits: what it charges the caller is a
 * formatted string and a couple of atomics. It is not literally syscall-free --
 * its os_gettime_ns is a QueryPerformanceCounter, which is a usermode read only
 * where the HAL is backed by an invariant TSC and traps on the HPET and ACPI-PM
 * fallbacks -- but that is one counter read against a log write.
 */

/* Both are idempotent, and either may be called redundantly from any thread.
 * They are not safe to call concurrently with each other -- the caller owns that
 * ordering, as it owns the rule that every emitting thread is joined before the
 * ring stops. */

/* Brings the drain thread up. Returns false only if the thread could not be
 * created, in which case obs_debug_logf keeps working via the fallback below. */
bool obs_debug_log_start(void);

/* Drains everything queued before returning, so it blocks the calling thread for
 * as long as that takes: up to a poll interval waiting for the drain thread to
 * wake, then a blog() per queued line, each of which is a flushed file write plus
 * the debugger and stderr sinks behind it. Tens of milliseconds on a loaded ring.
 * Callers on a UI thread should expect to feel it. */
void obs_debug_log_stop(void);

#if !defined(_MSC_VER) && !defined(SWIG)
#define OBS_DEBUG_LOG_PRINTFATTR(f, a) __attribute__((__format__(__printf__, f, a)))
#else
#define OBS_DEBUG_LOG_PRINTFATTR(f, a)
#endif

/* Never blocks. The ring is bounded, so a line offered while it is full is
 * dropped rather than queued -- drops are counted and the drain thread discloses
 * the total as its own log line, so they are never silent, but a caller that
 * needs every line delivered wants blog() instead. Each drained line is prefixed
 * with the seconds.milliseconds the producer stamped on it.
 *
 * Outside the start/stop window this falls back to blog() on the calling thread,
 * so a diagnostic can be expensive but is never lost. */
OBS_DEBUG_LOG_PRINTFATTR(2, 3)
void obs_debug_logf(int log_level, const char *format, ...);

#undef OBS_DEBUG_LOG_PRINTFATTR

#ifdef __cplusplus
}
#endif
