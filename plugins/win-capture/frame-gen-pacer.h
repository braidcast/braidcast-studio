#pragma once

/* Frame Generation Capture show times. Each Present gets a stamp one display step
 * after the previous one, the step being the game's mean Present interval over a
 * sliding window. Frame generation hands its real and generated frames to Present
 * in clusters whose sizes and spacing vary frame to frame; pacing by rate keeps
 * every Present about one step apart however the CPU side clusters them. Stamps
 * derive from Present times alone, so the capture's own cost never moves one.
 *
 * A stamp's lead over its Present leaks away a little each Present, is capped at
 * what the shared-texture ring can hold, and must return to about zero within
 * FGC_PACER_CLEAR_NS. A lead that does not is a step measured before the game
 * sped up: the window then keeps only the Presents since the lead last cleared,
 * and stamps hold until real time passes the last one, then follow it again.
 *
 * Runs inside the game's Present: fixed-size storage, no allocation, no lock.
 * Integer math only. Zeroed storage is a valid empty pacer with no lead allowed;
 * fgc_pacer_init sets the cap. Header-only and pure so the capture hook and the
 * tests share it. */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define FGC_PACER_INTERVALS 32
/* Intervals the window needs before pacing starts; below it the step is 0. */
#define FGC_PACER_MIN_INTERVALS 4
/* Each stamp gives back this fraction of the previous stamp's lead. */
#define FGC_PACER_LEAK_DEN 32ULL
/* A gap between Presents this long is a stall, not a display step. */
#define FGC_PACER_STALL_NS 100000000ULL
/* The lead must clear at least this often, or the step is taken as stale. */
#define FGC_PACER_CLEAR_NS 100000000ULL
/* A lead at most this, or half a step if larger, counts as cleared. */
#define FGC_PACER_CLEAR_EPS_NS 1000000ULL
/* The lead cap never drops below this: a 30 fps game's frame. */
#define FGC_PACER_MIN_LEAD_CAP_NS 33333333ULL
/* Ring slots the cap leaves to frames whose stamps have passed: the hook keeps
 * a slot until its stamp is two canvas intervals old, four half-interval buckets
 * at one copy per bucket. */
#define FGC_PACER_RING_RESERVE 4

struct fgc_pacer {
	/* The last FGC_PACER_INTERVALS + 1 Present times; head is the next write and
	 * count the times held, newest first back from head. */
	uint64_t times[FGC_PACER_INTERVALS + 1];
	uint32_t head;
	uint32_t count;
	uint64_t last_stamp;
	uint64_t last_lead;
	/* When the lead last cleared, and the Presents since. */
	uint64_t clear_t;
	uint32_t since_clear;
	/* Stamps hold just past the last one until real time passes it. */
	bool catch_up;
	uint64_t lead_cap_ns;
};

struct fgc_pace {
	uint64_t stamp;
	/* A stall emptied the window before this Present. */
	bool reset;
	/* The lead cap pulled the stamp back; strict increase can still leave it
	 * just past the cap. */
	bool lead_clamped;
	/* The lead had not cleared within FGC_PACER_CLEAR_NS: the window was cut
	 * back and stamps hold until real time catches up. */
	bool resync;
};

/* The most lead the ring holds at one copy per half canvas interval, so a run of
 * led-ahead frames never fills every slot the hook may write. Above a 60 fps
 * canvas the 30 fps floor exceeds that, and the hook's slot guard alone keeps a
 * frame still to be shown from being overwritten. */
static inline uint64_t fgc_pacer_lead_cap(uint64_t canvas_interval_ns, uint32_t ring_slots)
{
	const uint64_t slots = ring_slots > FGC_PACER_RING_RESERVE ? ring_slots - FGC_PACER_RING_RESERVE : 0;
	const uint64_t cap = slots * canvas_interval_ns / 2;
	return cap > FGC_PACER_MIN_LEAD_CAP_NS ? cap : FGC_PACER_MIN_LEAD_CAP_NS;
}

static inline void fgc_pacer_init(struct fgc_pacer *p, uint64_t lead_cap_ns)
{
	memset(p, 0, sizeof(*p));
	p->lead_cap_ns = lead_cap_ns;
}

static inline uint64_t fgc_pacer_time(const struct fgc_pacer *p, uint32_t back)
{
	return p->times[(p->head + FGC_PACER_INTERVALS - back) % (FGC_PACER_INTERVALS + 1)];
}

/* The display step in ns: the mean interval over the window, 0 until it holds
 * FGC_PACER_MIN_INTERVALS. A full window trims its largest interval down to the
 * second largest: one hitch is a lone outlier, while a frame generation gap
 * recurs and so has a twin. */
static inline uint64_t fgc_pacer_step(const struct fgc_pacer *p)
{
	if (p->count < FGC_PACER_MIN_INTERVALS + 1) {
		return 0;
	}
	const uint32_t n = p->count - 1;
	uint64_t span = fgc_pacer_time(p, 0) - fgc_pacer_time(p, n);
	if (p->count == FGC_PACER_INTERVALS + 1) {
		uint64_t m1 = 0;
		uint64_t m2 = 0;
		for (uint32_t k = 0; k < n; k++) {
			const uint64_t d = fgc_pacer_time(p, k) - fgc_pacer_time(p, k + 1);
			if (d > m1) {
				m2 = m1;
				m1 = d;
			} else if (d > m2) {
				m2 = d;
			}
		}
		span -= m1 - m2;
	}
	return span / n;
}

static inline struct fgc_pace fgc_pacer_stamp(struct fgc_pacer *p, uint64_t t_ns)
{
	struct fgc_pace pace = {0, false, false, false};

	if (p->count) {
		const uint64_t prev = fgc_pacer_time(p, 0);
		if (t_ns > prev && t_ns - prev > FGC_PACER_STALL_NS) {
			p->count = 0;
			p->catch_up = false;
			p->since_clear = 0;
			p->clear_t = t_ns;
			pace.reset = true;
		} else if (t_ns < prev) {
			/* A clock that stepped back reads as a zero interval, keeping the
			 * window ordered. */
			t_ns = prev;
		}
	} else {
		p->clear_t = t_ns;
	}

	p->times[p->head] = t_ns;
	p->head = (p->head + 1) % (FGC_PACER_INTERVALS + 1);
	if (p->count < FGC_PACER_INTERVALS + 1) {
		p->count++;
	}

	const uint64_t step = fgc_pacer_step(p);
	uint64_t stamp;
	if (p->catch_up) {
		/* Equal is not past: a stamp equal to the last must still rise. */
		if (t_ns > p->last_stamp) {
			p->catch_up = false;
			stamp = t_ns;
		} else {
			stamp = p->last_stamp + 1;
		}
	} else {
		const uint64_t paced = p->last_stamp + step - p->last_lead / FGC_PACER_LEAK_DEN;
		stamp = paced > t_ns ? paced : t_ns;
		if (stamp - t_ns > p->lead_cap_ns) {
			stamp = t_ns + p->lead_cap_ns;
			pace.lead_clamped = true;
		}
		if (stamp <= p->last_stamp) {
			stamp = p->last_stamp + 1;
		}
	}

	const uint64_t lead = stamp - t_ns;
	p->last_stamp = stamp;
	p->last_lead = lead;
	p->since_clear++;

	const uint64_t clear_eps = step / 2 > FGC_PACER_CLEAR_EPS_NS ? step / 2 : FGC_PACER_CLEAR_EPS_NS;
	if (lead <= clear_eps) {
		p->clear_t = t_ns;
		p->since_clear = 0;
	} else if (t_ns - p->clear_t > FGC_PACER_CLEAR_NS) {
		/* Keep the newest since_clear + 1 times; the oldest drop off. */
		if (p->count > p->since_clear + 1) {
			p->count = p->since_clear + 1;
		}
		p->catch_up = true;
		p->clear_t = t_ns;
		p->since_clear = 0;
		pace.resync = true;
	}

	pace.stamp = stamp;
	return pace;
}
