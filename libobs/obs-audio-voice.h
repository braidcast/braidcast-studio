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

/*
 * The per-group voice pick behind obs_source_set_audio_group: among the members of one audio
 * group, which one is the voice and which are open this tick. Pure -- it reads and writes only
 * the array it is given, takes no lock and allocates nothing -- so the audio thread can run it
 * every tick and a unit test can drive it with a table.
 */

#include "util/c99defs.h"

#ifdef __cplusplus
extern "C" {
#endif

enum audio_voice_presence {
	AUDIO_VOICE_ABSENT,  /* neither in a mixed tree nor active */
	AUDIO_VOICE_ACTIVE,  /* active, but in no tree that is mixed */
	AUDIO_VOICE_IN_TREE, /* in the active tree of a mix whose audio is mixed this tick */
};

enum audio_voice_side {
	AUDIO_VOICE_UNSIDED, /* reaches the mix at full gain */
	AUDIO_VOICE_SIDE_A,  /* only under the outgoing side of a transition mixing audio */
	AUDIO_VOICE_SIDE_B,  /* only under the incoming side of that transition */
};

struct audio_voice_member {
	/* Filled by the caller every tick. Group 0 means ungrouped. */
	uint16_t group;
	uint32_t rank; /* unique among live members; equal ranks fall back to `source` */
	bool shutdown_when_hidden;
	bool enabled;
	bool showing;
	enum audio_voice_presence presence;
	enum audio_voice_side side;
	const void *transition; /* the transition `side` refers to; ignored when unsided */

	/* In: the previous tick's result. Out: this tick's. */
	bool voice;
	bool open;

	/* The caller's own back-pointer; never read here. */
	void *source;
};

/* An ungrouped source is never a voice and never gated. */
static inline void audio_voice_ungrouped(struct audio_voice_member *member)
{
	member->voice = false;
	member->open = true;
}

/* Sorts `members` by group, then decides every group's voice and open members. A group with no
 * enabled active member keeps last tick's result, so its gates do not move. */
void audio_voice_pick(struct audio_voice_member *members, size_t count);

#ifdef __cplusplus
}
#endif
