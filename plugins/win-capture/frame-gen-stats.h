#pragma once

/* Frame Generation Capture diagnostics: fixed-size counters kept one window at a
 * time, and the line each window logs. The hook side runs inside the game's
 * Present, so counting is plain increments into static storage and formatting
 * happens once per window into the caller's stack buffer: no allocation, no lock.
 * Header-only and pure so the capture hook, game-capture.c and the tests share it.
 *
 * Every line starts with FGC_STATS_TAG, and capture-rate's per-window line carries
 * it too, so one grep lines the hook, the ring and the canvas up on the log clock. */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FGC_STATS_TAG "[10s]"
#define FGC_STATS_WINDOW_NS 10000000000ULL

/* Copy-time histogram: 25 us bins; the last one also holds everything slower. */
#define FGC_COPY_BIN_NS 25000ULL
#define FGC_COPY_BINS 160

#define FGC_BURST_SIZES 4

struct fgc_hook_stats {
	uint32_t presents;
	uint32_t copies;
	/* Closed bursts of 1..4 Presents. A 4-Present burst the size cap closed
	 * while the next Present still fell inside the burst duration counts as
	 * capped instead: the game sent more than 4 at once. */
	uint32_t bursts[FGC_BURST_SIZES];
	uint32_t bursts_capped;
	/* Stalls that emptied the pacer's window (frame-gen-pacer.h). */
	uint32_t pace_resets;
	/* Stamps held to the pacer's lead cap ahead of their Present. */
	uint32_t lead_clamps;
	/* Leads that did not clear in time: the pacer re-measured and caught up. */
	uint32_t lead_resyncs;
	uint32_t bucket_skips;
	/* Copies skipped because every writable slot held a frame not yet offered
	 * to the host; slot busy is every slot held by the host instead. */
	uint32_t ring_full;
	uint32_t slot_busy;
	uint32_t copy_fails;
	uint64_t span_max_ns;
	/* From a burst's last Present to the next one's first; 0 until a burst
	 * follows another. After a capped burst it is spacing inside the cluster
	 * the cap split, which the >4 count tells apart. */
	uint64_t gap_min_ns;
	uint32_t copy_bins[FGC_COPY_BINS];
};

/* Tick ends of the host's ring slot pick. No due slot splits into nothing newer
 * published at all and newer slots whose show time is still ahead. */
struct fgc_ring_stats {
	uint32_t new_slot;
	uint32_t none_newer;
	uint32_t not_due;
	uint32_t acquire_failed;
	uint32_t seq_changed;
};

/* Opens the window on its first call; true once it has run its length. */
static inline bool fgc_stats_window_due(uint64_t *start_ns, uint64_t now_ns)
{
	if (!*start_ns) {
		*start_ns = now_ns;
		return false;
	}
	return now_ns - *start_ns >= FGC_STATS_WINDOW_NS;
}

static inline void fgc_hook_stats_burst(struct fgc_hook_stats *s, uint32_t size, bool capped)
{
	if (capped) {
		s->bursts_capped++;
	} else if (size >= 1 && size <= FGC_BURST_SIZES) {
		s->bursts[size - 1]++;
	}
}

static inline void fgc_hook_stats_gap(struct fgc_hook_stats *s, uint64_t gap_ns)
{
	if (!s->gap_min_ns || gap_ns < s->gap_min_ns) {
		s->gap_min_ns = gap_ns;
	}
}

static inline void fgc_hook_stats_span(struct fgc_hook_stats *s, uint64_t span_ns)
{
	if (span_ns > s->span_max_ns) {
		s->span_max_ns = span_ns;
	}
}

static inline void fgc_hook_stats_copy_time(struct fgc_hook_stats *s, uint64_t ns)
{
	const uint64_t bin = ns / FGC_COPY_BIN_NS;
	s->copy_bins[bin < FGC_COPY_BINS ? bin : FGC_COPY_BINS - 1]++;
}

/* The upper edge, in us, of the bin holding the pct-th percentile copy; 0 with
 * no copies. The last bin reads as its upper edge, so a p99 at 4000 us means
 * 3975 us or slower. */
static inline uint64_t fgc_hook_stats_copy_percentile_us(const struct fgc_hook_stats *s, uint32_t pct)
{
	uint64_t total = 0;
	for (size_t i = 0; i < FGC_COPY_BINS; i++) {
		total += s->copy_bins[i];
	}
	if (!total) {
		return 0;
	}
	const uint64_t rank = (total * pct + 99) / 100;
	uint64_t seen = 0;
	for (size_t i = 0; i < FGC_COPY_BINS; i++) {
		seen += s->copy_bins[i];
		if (seen >= rank) {
			return (i + 1) * FGC_COPY_BIN_NS / 1000;
		}
	}
	return FGC_COPY_BINS * FGC_COPY_BIN_NS / 1000;
}

/* count per second over window_ns, times ten and rounded, for "%u.%u" output
 * without floating point in the hook. */
static inline uint64_t fgc_rate_x10(uint32_t count, uint64_t window_ns)
{
	return window_ns ? ((uint64_t)count * 10000000000ULL + window_ns / 2) / window_ns : 0;
}

/* The window's length in tenths of a second, rounded. */
static inline uint64_t fgc_window_x10(uint64_t window_ns)
{
	return (window_ns + 50000000ULL) / 100000000ULL;
}

static inline int fgc_hook_stats_format(char *buf, size_t size, const struct fgc_hook_stats *s, uint64_t window_ns,
					uint64_t step_ns)
{
	const uint64_t window_x10 = fgc_window_x10(window_ns);
	const uint64_t presents = fgc_rate_x10(s->presents, window_ns);
	const uint64_t copies = fgc_rate_x10(s->copies, window_ns);
	return snprintf(buf, size,
			FGC_STATS_TAG
			" hook %" PRIu64 ".%" PRIu64 " s: presents %" PRIu64 ".%" PRIu64 "/s copies %" PRIu64
			".%" PRIu64 "/s, bursts 1:%u 2:%u 3:%u 4:%u >4:%u"
			", step %" PRIu64 " us, pace resets %u, lead clamps %u, lead resyncs %u"
			", bucket skips %u, ring full %u, slot busy %u, copy fails %u, burst span max %" PRIu64
			" us, burst gap min %" PRIu64 " us, copy p50 %" PRIu64 " p99 %" PRIu64 " us",
			window_x10 / 10, window_x10 % 10, presents / 10, presents % 10, copies / 10, copies % 10,
			s->bursts[0], s->bursts[1], s->bursts[2], s->bursts[3], s->bursts_capped, step_ns / 1000,
			s->pace_resets, s->lead_clamps, s->lead_resyncs, s->bucket_skips, s->ring_full, s->slot_busy,
			s->copy_fails, s->span_max_ns / 1000, s->gap_min_ns / 1000,
			fgc_hook_stats_copy_percentile_us(s, 50), fgc_hook_stats_copy_percentile_us(s, 99));
}

static inline int fgc_ring_stats_format(char *buf, size_t size, const struct fgc_ring_stats *s, uint64_t window_ns)
{
	const uint64_t window_x10 = fgc_window_x10(window_ns);
	return snprintf(buf, size,
			FGC_STATS_TAG " ring %" PRIu64 ".%" PRIu64
				      " s: ticks %u, new slot %u, no due slot %u (none newer %u, newer not due %u)"
				      ", acquire failed %u, seq changed %u",
			window_x10 / 10, window_x10 % 10,
			s->new_slot + s->none_newer + s->not_due + s->acquire_failed + s->seq_changed, s->new_slot,
			s->none_newer + s->not_due, s->none_newer, s->not_due, s->acquire_failed, s->seq_changed);
}
