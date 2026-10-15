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

#include "obs-audio-voice.h"

#include <stdlib.h>

/* Matches a member on any side, for best_member. */
#define ANY_SIDE (-1)

static int compare_members(const void *a, const void *b)
{
	const struct audio_voice_member *x = a;
	const struct audio_voice_member *y = b;

	if (x->group != y->group) {
		return x->group < y->group ? -1 : 1;
	}
	if (x->rank != y->rank) {
		return x->rank < y->rank ? -1 : 1;
	}
	/* qsort is not stable: a tie broken by the input order would flip between ticks. */
	if (x->source != y->source) {
		return (uintptr_t)x->source < (uintptr_t)y->source ? -1 : 1;
	}
	return 0;
}

static inline bool is_candidate(const struct audio_voice_member *m, enum audio_voice_presence level)
{
	return m->enabled && m->presence == level;
}

/* The pick order: a showing member first, then one whose page keeps running while hidden, then
 * the lower rank. */
static bool preferred(const struct audio_voice_member *a, const struct audio_voice_member *b)
{
	if (a->showing != b->showing) {
		return a->showing;
	}
	if (a->shutdown_when_hidden != b->shutdown_when_hidden) {
		return !a->shutdown_when_hidden;
	}
	return a->rank < b->rank;
}

/* The best candidate at `level` on `side` (of `transition`, unless unsided), or `count` when
 * there is none. `prefer_open` ranks a member that is already open above the pick order, so a
 * crossfade partner stays the one that was already playing. */
static size_t best_member(const struct audio_voice_member *g, size_t count, enum audio_voice_presence level, int side,
			  const void *transition, bool prefer_open)
{
	size_t found = count;

	for (size_t i = 0; i < count; i++) {
		const struct audio_voice_member *m = &g[i];

		if (!is_candidate(m, level)) {
			continue;
		}
		if (side != ANY_SIDE) {
			if ((int)m->side != side) {
				continue;
			}
			if (side != AUDIO_VOICE_UNSIDED && m->transition != transition) {
				continue;
			}
		}
		if (found == count) {
			found = i;
		} else if (prefer_open && m->open != g[found].open) {
			if (m->open) {
				found = i;
			}
		} else if (preferred(m, &g[found])) {
			found = i;
		}
	}
	return found;
}

static bool any_candidate(const struct audio_voice_member *g, size_t count, enum audio_voice_presence level,
			  bool showing_only)
{
	for (size_t i = 0; i < count; i++) {
		if (is_candidate(&g[i], level) && (!showing_only || g[i].showing)) {
			return true;
		}
	}
	return false;
}

/* One group's members, sorted by rank. */
static void pick_group(struct audio_voice_member *g, size_t count)
{
	/* The in-tree members when there are any; otherwise the active ones, so the monitor still
	 * hears exactly one. With neither, nothing is playing and the gates stay as they are. */
	enum audio_voice_presence level;
	if (any_candidate(g, count, AUDIO_VOICE_IN_TREE, false)) {
		level = AUDIO_VOICE_IN_TREE;
	} else if (any_candidate(g, count, AUDIO_VOICE_ACTIVE, false)) {
		level = AUDIO_VOICE_ACTIVE;
	} else {
		return;
	}

	size_t current = count;
	for (size_t i = 0; i < count; i++) {
		if (g[i].voice) {
			current = i;
			break;
		}
	}
	const bool current_candidate = current < count && is_candidate(&g[current], level);

	size_t voice;
	size_t partner = count;

	if (current_candidate && (g[current].showing || !any_candidate(g, count, level, true /* showing_only */))) {
		voice = current;
	} else {
		voice = best_member(g, count, level, ANY_SIDE, NULL, false);
	}

	if (level == AUDIO_VOICE_IN_TREE && g[voice].side == AUDIO_VOICE_SIDE_A) {
		/* The voice fades out with its transition's outgoing side. Hand off to a member at full
		 * gain if there is one; otherwise to the incoming side, keeping the outgoing voice open
		 * until the transition ends so the two path gains still sum to one. With neither, the
		 * fade is the transition's own. */
		const size_t unsided = best_member(g, count, level, AUDIO_VOICE_UNSIDED, NULL, false);
		if (unsided < count) {
			voice = unsided;
		} else {
			const size_t incoming =
				best_member(g, count, level, AUDIO_VOICE_SIDE_B, g[voice].transition, false);
			if (incoming < count) {
				partner = voice;
				voice = incoming;
			}
		}
	}

	/* A voice on an incoming side rises with the transition; the outgoing side's member falls with
	 * it, so it stays open beside the voice until the transition ends. Only this voice's own
	 * transition is considered: a member under another transition never opens. */
	if (partner == count && level == AUDIO_VOICE_IN_TREE && g[voice].side == AUDIO_VOICE_SIDE_B) {
		partner = best_member(g, count, level, AUDIO_VOICE_SIDE_A, g[voice].transition, true);
	}

	for (size_t i = 0; i < count; i++) {
		g[i].voice = i == voice;
		g[i].open = i == voice || i == partner;
	}
}

void audio_voice_pick(struct audio_voice_member *members, size_t count)
{
	if (!count) {
		return;
	}

	qsort(members, count, sizeof(*members), compare_members);

	for (size_t start = 0; start < count;) {
		size_t end = start + 1;
		while (end < count && members[end].group == members[start].group) {
			end++;
		}

		if (members[start].group == 0) {
			for (size_t i = start; i < end; i++) {
				audio_voice_ungrouped(&members[i]);
			}
		} else {
			pick_group(members + start, end - start);
		}
		start = end;
	}
}
