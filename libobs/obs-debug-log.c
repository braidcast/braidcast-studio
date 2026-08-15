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

#include "obs-debug-log.h"

#include "util/base.h"
#include "util/platform.h"
#include "util/threading.h"

#include <stdarg.h>
#include <stdio.h>

/* Power of two, so a slot index survives the position counters wrapping through
 * LONG_MIN: two's-complement increment is addition modulo 2^N and the mask
 * divides that period exactly, leaving the slot walk contiguous across the wrap.
 * Every position arithmetic below is done unsigned for the same reason -- signed
 * overflow is undefined, and these counters do reach LONG_MAX. */
#define DEBUG_LOG_SLOTS 256
#define DEBUG_LOG_SLOT_MASK (DEBUG_LOG_SLOTS - 1)

/* Sized for the longest line whose length is bounded: the per-frame segment
 * breakdown in obs-video.c, where a 159 byte head, 16 values that a %.1f of a
 * double converted from a uint64 nanosecond count caps at 16 characters
 * (UINT64_MAX nanoseconds prints as "18446744073709.6"), and two optional
 * annotations at 159 and 223 bytes total 1022 bytes with the fixed text. That
 * clears the buffer by two bytes, so a new field on that line needs this raised
 * with it.
 *
 * The per-source rollup is not bounded -- it interpolates a source name straight
 * from obs_source_get_name -- so a long enough name is truncated here at 1023,
 * below what either installed sink would have kept: 8191 into the log file
 * (SessionLogHandler) and 4095 to stderr and the debugger (ObsLogHandler).
 * Losing the tail of one diagnostic line is worth far less than sizing every
 * slot for the largest of those. */
#define DEBUG_LOG_LINE_SIZE 1024

/* Poll interval. A line published just after the drain thread goes to sleep
 * waits this long to reach the log -- bounded and small, but not zero, which is
 * why the producer stamps the line rather than letting its position in the log
 * stand in for its time. */
#define DEBUG_LOG_POLL_MS 20

/* A producer that has claimed a slot is microseconds from publishing it, so the
 * drain waits it out rather than paying a whole poll interval for that line. */
#define DEBUG_LOG_SPIN_TRIES 4096

struct debug_log_slot {
	/* Vyukov sequence: the position this slot is free for. It equals that
	 * position when free, position + 1 once published, and is advanced to
	 * position + DEBUG_LOG_SLOTS when consumed -- the value the next lap's
	 * producer looks for. Carrying the position rather than a free/busy state
	 * is what makes a claim unambiguous across a lap: a slot claimed but not
	 * yet published still reads as its own position, so a producer arriving a
	 * lap later computes a negative difference and reports the ring full
	 * instead of writing on top of a live slot.
	 *
	 * It is also the only field ordering the two threads: storing position + 1
	 * releases time_ns, level and text to the drain thread, and storing
	 * position + DEBUG_LOG_SLOTS releases the slot back to the producers. */
	volatile long seq;
	int level;
	uint64_t time_ns;
	char text[DEBUG_LOG_LINE_SIZE];
};

static struct {
	struct debug_log_slot slots[DEBUG_LOG_SLOTS];

	/* Producer claim position. Its low bits are the slot. */
	volatile long write;

	/* Never reset by start: the drain thread reads it to zero on every pass and
	 * again in the flush that stop performs, so an arm begins at zero without
	 * being told to. A drop that a disarming race records after that last flush
	 * survives to the next arm and is disclosed there rather than erased. */
	volatile long dropped;

	/* Start/stop ownership, exchanged rather than read as a gate. One boolean
	 * cannot serve both roles: the exclusion has to be taken before the ring
	 * is seeded and the producer gate has to be published after it. */
	volatile bool started;

	/* Producer gate and drain-loop condition. Published last by start,
	 * cleared first by stop. */
	volatile bool running;

	/* Whether the sequences have ever been seeded. Atomic so a start on one
	 * thread observes a seed performed on another; what keeps two callers from
	 * interleaving the seed itself is the ownership latch above plus the
	 * header's precondition that start and stop are never called concurrently.
	 * The latch cannot order this on its own -- it is exchanged before the seed
	 * runs, so it releases nothing the seed writes. */
	volatile bool seeded;

	/* Owned by whichever thread is currently draining: the drain thread while
	 * it runs, and the stopping thread during the flush that follows the join.
	 * Never two at once -- the create/join chain is what orders the handoff. */
	long read_pos;
	pthread_t thread;
} debug_log;

/* Emits every published line, in order, up to the first slot not yet published.
 * Returns whether anything was emitted. */
static bool debug_log_emit_pending(void)
{
	bool emitted = false;

	for (;;) {
		const long pos = debug_log.read_pos;
		const long ready = (long)((unsigned long)pos + 1);
		struct debug_log_slot *slot = &debug_log.slots[(unsigned long)pos & DEBUG_LOG_SLOT_MASK];
		long seq = os_atomic_load_long(&slot->seq);

		/* A claimed slot still reads as free, so an empty ring and a
		 * producer part-way through its vsnprintf are identical from here.
		 * The claim position is what separates them. */
		if (seq != ready && os_atomic_load_long(&debug_log.write) != pos) {
			for (int i = 0; seq != ready && i < DEBUG_LOG_SPIN_TRIES; i++) {
				seq = os_atomic_load_long(&slot->seq);
			}
		}

		if (seq != ready) {
			break;
		}

		/* Producer-side milliseconds off the monotonic clock the render
		 * diagnostics already measure with. Neither installed log handler
		 * stamps a time, so without this the line's position in the log
		 * would be its only ordering signal -- and that position is now up
		 * to a poll interval later than the event it describes, while lines
		 * logged directly by other threads keep arriving in place. */
		const uint64_t ms = slot->time_ns / 1000000ULL;

		/* The line is already formatted, so it is passed as an argument: a
		 * percent sign inside a source name must not be read as a
		 * conversion here. */
		blog(slot->level, "[%llu.%03llu] %s", (unsigned long long)(ms / 1000ULL),
		     (unsigned long long)(ms % 1000ULL), slot->text);

		os_atomic_set_long(&slot->seq, (long)((unsigned long)pos + DEBUG_LOG_SLOTS));
		debug_log.read_pos = ready;
		emitted = true;
	}

	return emitted;
}

/* Discloses what the ring turned away. Read and clear in one operation, so a
 * drop landing during the report is carried into the next one rather than lost. */
static bool debug_log_report_drops(void)
{
	const long dropped = os_atomic_set_long(&debug_log.dropped, 0);

	if (dropped <= 0) {
		return false;
	}

	blog(LOG_WARNING, "[render-debug] dropped %ld line(s)", dropped);
	return true;
}

static bool debug_log_flush(void)
{
	const bool emitted = debug_log_emit_pending();
	return debug_log_report_drops() || emitted;
}

static void *debug_log_drain_thread(void *param)
{
	UNUSED_PARAMETER(param);

	os_set_thread_name("libobs: debug log");

	while (os_atomic_load_bool(&debug_log.running)) {
		if (!debug_log_flush()) {
			os_sleep_ms(DEBUG_LOG_POLL_MS);
		}
	}

	/* Whatever was published between the last pass and the flag clearing. */
	debug_log_flush();
	return NULL;
}

bool obs_debug_log_start(void)
{
	/* The exchange is both the idempotence and the mutual exclusion: only the
	 * caller that observes a false owns the start, so two concurrent calls
	 * cannot each create a thread and orphan one of them. */
	if (os_atomic_set_bool(&debug_log.started, true)) {
		return os_atomic_load_bool(&debug_log.running);
	}

	/* Seeded once for the process, not once per arm, and this is load-bearing
	 * now that the arming flag is a runtime toggle: a re-seed would rewrite the
	 * sequences under a producer that had already passed the running check and
	 * was still mid-write, which is the one route left to two writers in one
	 * slot. It is also unnecessary. A stop drains the ring and leaves every
	 * sequence at the position its slot is next free for, with write and
	 * read_pos equal -- precisely the state a resume needs, whatever lap the
	 * positions happen to be on. A line that a disarming race published after
	 * the final flush is not lost either: it stays in its slot and the next
	 * arm's drain thread emits it.
	 *
	 * The initial static zeroing already puts write and read_pos at 0, which is
	 * the position this seeding pairs with, so they are set here rather than on
	 * every start for the same reason. Only the caller holding the ownership
	 * latch runs any of this, and all of it lands before the producer gate is
	 * published -- which is why that latch is a separate flag from the gate. */
	if (!os_atomic_load_bool(&debug_log.seeded)) {
		for (long i = 0; i < DEBUG_LOG_SLOTS; i++) {
			os_atomic_set_long(&debug_log.slots[i].seq, i);
		}

		os_atomic_set_long(&debug_log.write, 0);
		debug_log.read_pos = 0;
		os_atomic_set_bool(&debug_log.seeded, true);
	}

	os_atomic_set_bool(&debug_log.running, true);

	if (pthread_create(&debug_log.thread, NULL, debug_log_drain_thread, NULL) != 0) {
		os_atomic_set_bool(&debug_log.running, false);
		os_atomic_set_bool(&debug_log.started, false);
		blog(LOG_WARNING, "Could not start the debug log thread; diagnostics will log inline");
		return false;
	}

	return true;
}

void obs_debug_log_stop(void)
{
	/* Mirrors start: only the caller that observes a true owns the stop, so a
	 * redundant call cannot join a thread that was already joined. */
	if (!os_atomic_set_bool(&debug_log.started, false)) {
		return;
	}

	os_atomic_set_bool(&debug_log.running, false);
	pthread_join(debug_log.thread, NULL);

	/* Catches whatever the drain thread's own parting flush raced past. It
	 * cannot catch a line published after this call: obs_debug_logf reads the
	 * running flag before it claims a slot, so an emitter that had already
	 * passed that read can still publish behind this flush. Disarming the render
	 * debug flag while the graphics thread is mid-emit is exactly that case.
	 *
	 * Nothing here closes the window and nothing needs to. Because start never
	 * re-seeds, such a line stays in its slot in a coherent ring and the next
	 * arm's drain thread emits it. The last stop of a session is the one that
	 * cannot be followed by an arm, and that stop is the one in obs.c
	 * stop_video, which runs after the graphics thread has been joined -- so no
	 * emitter is left to lose a line to it. What this window cannot do, in any
	 * case, is corrupt a slot or tear a line: that is the property worth having,
	 * and the seeding rule in start is what provides it. */
	debug_log_flush();
}

void obs_debug_logf(int log_level, const char *format, ...)
{
	struct debug_log_slot *slot;
	va_list args;
	long pos;

	va_start(args, format);

	/* Before the ring is up or after it is down there is nothing to drain the
	 * line, so the caller pays for it. That is the cost this module exists to
	 * remove, but a diagnostic that is expensive beats one that is missing. */
	if (!os_atomic_load_bool(&debug_log.running)) {
		blogva(log_level, format, args);
		va_end(args);
		return;
	}

	/* Stamped before the claim, so the line carries when the diagnostic
	 * happened rather than when a contended claim finished. */
	const uint64_t time_ns = os_gettime_ns();

	/* Vyukov bounded-queue claim. Winning the compare-and-swap on the claim
	 * position is what makes the slot exclusively ours, and the slot's own
	 * sequence is what proves the previous lap has left it -- neither check
	 * alone is enough. Uncontended, which is every call today because only the
	 * graphics thread emits, this is a single locked compare-and-swap and the
	 * loop never turns over. */
	pos = os_atomic_load_long(&debug_log.write);

	for (;;) {
		slot = &debug_log.slots[(unsigned long)pos & DEBUG_LOG_SLOT_MASK];

		const long seq = os_atomic_load_long(&slot->seq);
		const long dif = (long)((unsigned long)seq - (unsigned long)pos);

		/* Behind our position: the slot still holds a line from the
		 * previous lap that the drain thread has not emitted. */
		if (dif < 0) {
			os_atomic_inc_long(&debug_log.dropped);
			va_end(args);
			return;
		}

		if (dif == 0 && os_atomic_compare_swap_long(&debug_log.write, pos, (long)((unsigned long)pos + 1))) {
			break;
		}

		/* Lost the claim, or read a position another producer had already
		 * moved past. Either way our position is stale. */
		pos = os_atomic_load_long(&debug_log.write);
	}

	slot->time_ns = time_ns;
	slot->level = log_level;
	vsnprintf(slot->text, sizeof(slot->text), format, args);
	va_end(args);

	os_atomic_set_long(&slot->seq, (long)((unsigned long)pos + 1));
}
