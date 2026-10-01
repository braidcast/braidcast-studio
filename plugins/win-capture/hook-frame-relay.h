#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Turns the hook's cumulative present and copy counters, read once per video
 * tick, into one tick's frame report for libobs. Deltas are uint32 modular, so
 * a counter wrapping is an ordinary step; a counter going down never means a
 * reset by itself, which is why a new hook info view resets the relay instead.
 *
 * A new frame is one the source could show this tick and could not before:
 * - with one shared texture, any copy since the last tick;
 * - with a frame generation ring, the host drawing a newer slot, since a burst
 *   of copies lands in one tick but is drawn out over the following ones. */
struct hook_frame_relay {
	uint32_t presents_seen;
	uint32_t copies_seen;
	uint64_t ring_frame_seen;
	bool baselined;
};

struct hook_frame_report {
	uint32_t offered;
	uint32_t delivered;
	bool new_frame;
};

static inline void hook_frame_relay_reset(struct hook_frame_relay *relay)
{
	relay->baselined = false;
}

/* ring_frame_no is the frame number of the ring slot the host draws, and is
 * read only while ring is true. The first step after a reset only baselines. */
static inline struct hook_frame_report hook_frame_relay_step(struct hook_frame_relay *relay, uint32_t presents,
							     uint32_t copies, bool ring, uint64_t ring_frame_no)
{
	struct hook_frame_report report = {0, 0, false};
	if (relay->baselined) {
		report.offered = presents - relay->presents_seen;
		report.delivered = copies - relay->copies_seen;
		report.new_frame = ring ? ring_frame_no != relay->ring_frame_seen : report.delivered != 0;
	}
	relay->presents_seen = presents;
	relay->copies_seen = copies;
	relay->ring_frame_seen = ring_frame_no;
	relay->baselined = true;
	return report;
}
