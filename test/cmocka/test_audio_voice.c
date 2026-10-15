#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <obs-audio-voice.h>

/* Two transitions, by address only: the pick compares them and never reads them. Writable, so
 * the linker cannot fold them into one. */
static char transition_t;
static char transition_u;
#define T ((const void *)&transition_t)
#define U ((const void *)&transition_u)

enum { IN_TREE = AUDIO_VOICE_IN_TREE, ACTIVE = AUDIO_VOICE_ACTIVE, ABSENT = AUDIO_VOICE_ABSENT };
enum { NONE = AUDIO_VOICE_UNSIDED, SIDE_A = AUDIO_VOICE_SIDE_A, SIDE_B = AUDIO_VOICE_SIDE_B };

/* One member of group 1 unless the test says otherwise: enabled, showing, gated, not the voice. */
static struct audio_voice_member member(uint32_t rank, enum audio_voice_presence presence)
{
	struct audio_voice_member m = {
		.group = 1,
		.rank = rank,
		.enabled = true,
		.showing = true,
		.presence = presence,
		.side = AUDIO_VOICE_UNSIDED,
	};
	return m;
}

static struct audio_voice_member sided(uint32_t rank, enum audio_voice_side side, const void *transition)
{
	struct audio_voice_member m = member(rank, AUDIO_VOICE_IN_TREE);
	m.side = side;
	m.transition = transition;
	return m;
}

/* The member of the given rank, wherever the pick's sort moved it. */
static struct audio_voice_member *find(struct audio_voice_member *members, size_t count, uint32_t rank)
{
	size_t i = 0;
	while (i < count && members[i].rank != rank) {
		i++;
	}
	if (i == count) {
		fail_msg("no member of rank %u", rank);
	}
	return &members[i];
}

static size_t count_voices(const struct audio_voice_member *members, size_t count)
{
	size_t voices = 0;
	for (size_t i = 0; i < count; i++) {
		voices += members[i].voice;
	}
	return voices;
}

static void assert_voice(struct audio_voice_member *members, size_t count, uint32_t rank)
{
	assert_int_equal(count_voices(members, count), 1);
	assert_true(find(members, count, rank)->voice);
	assert_true(find(members, count, rank)->open);
}

static void assert_open(struct audio_voice_member *members, size_t count, uint32_t rank, bool open)
{
	assert_int_equal(find(members, count, rank)->open, open);
}

#define PICK(arr) audio_voice_pick((arr), sizeof(arr) / sizeof((arr)[0]))
#define N(arr) (sizeof(arr) / sizeof((arr)[0]))

static void two_in_tree_one_voice(void **state)
{
	struct audio_voice_member m[] = {member(2, IN_TREE), member(1, IN_TREE)};
	PICK(m);
	assert_voice(m, N(m), 1);
	assert_open(m, N(m), 2, false);
	(void)state;
}

static void current_voice_is_kept(void **state)
{
	struct audio_voice_member m[] = {member(1, IN_TREE), member(2, IN_TREE)};
	m[1].voice = true;
	m[1].open = true;
	PICK(m);
	assert_voice(m, N(m), 2);
	assert_open(m, N(m), 1, false);
	(void)state;
}

static void hidden_voice_hands_off_to_a_showing_member(void **state)
{
	struct audio_voice_member m[] = {member(1, IN_TREE), member(2, IN_TREE)};
	m[0].voice = true;
	m[0].open = true;
	m[0].showing = false;
	PICK(m);
	assert_voice(m, N(m), 2);
	(void)state;
}

static void hidden_voice_is_kept_when_nothing_shows(void **state)
{
	struct audio_voice_member m[] = {member(1, IN_TREE), member(2, IN_TREE)};
	m[0].showing = false;
	m[1].showing = false;
	m[1].voice = true;
	m[1].open = true;
	PICK(m);
	assert_voice(m, N(m), 2);
	(void)state;
}

static void showing_beats_rank(void **state)
{
	struct audio_voice_member m[] = {member(1, IN_TREE), member(2, IN_TREE)};
	m[0].showing = false;
	PICK(m);
	assert_voice(m, N(m), 2);
	(void)state;
}

static void shutdown_when_hidden_loses(void **state)
{
	struct audio_voice_member m[] = {member(1, IN_TREE), member(2, IN_TREE)};
	m[0].shutdown_when_hidden = true;
	PICK(m);
	assert_voice(m, N(m), 2);
	(void)state;
}

static void disabled_member_is_no_candidate(void **state)
{
	struct audio_voice_member m[] = {member(1, IN_TREE), member(2, IN_TREE)};
	m[0].enabled = false;
	PICK(m);
	assert_voice(m, N(m), 2);
	(void)state;
}

static void active_member_is_gated_beside_an_in_tree_voice(void **state)
{
	struct audio_voice_member m[] = {member(1, ACTIVE), member(2, IN_TREE)};
	m[0].voice = true;
	m[0].open = true;
	PICK(m);
	assert_voice(m, N(m), 2);
	assert_open(m, N(m), 1, false);
	(void)state;
}

static void active_members_alone_still_get_one_voice(void **state)
{
	struct audio_voice_member m[] = {member(1, ACTIVE), member(2, ACTIVE), member(3, ABSENT)};
	PICK(m);
	assert_voice(m, N(m), 1);
	assert_open(m, N(m), 2, false);
	assert_open(m, N(m), 3, false);
	(void)state;
}

static void no_active_member_leaves_everything(void **state)
{
	struct audio_voice_member m[] = {member(1, ABSENT), member(2, ABSENT)};
	m[1].voice = true;
	m[1].open = true;
	m[0].open = true; /* mid-handoff state that must not move */
	PICK(m);
	assert_true(find(m, N(m), 2)->voice);
	assert_false(find(m, N(m), 1)->voice);
	assert_open(m, N(m), 1, true);
	assert_open(m, N(m), 2, true);
	(void)state;
}

static void ungrouped_is_open(void **state)
{
	struct audio_voice_member m[] = {member(1, IN_TREE), member(2, ABSENT), member(3, IN_TREE)};
	m[0].group = 0;
	m[0].voice = true; /* left over from a group it has since left */
	m[1].group = 0;
	m[2].group = 0;
	PICK(m);
	for (size_t i = 0; i < N(m); i++) {
		assert_false(m[i].voice);
		assert_true(m[i].open);
	}

	struct audio_voice_member single = member(1, IN_TREE);
	audio_voice_ungrouped(&single);
	assert_false(single.voice);
	assert_true(single.open);
	(void)state;
}

static void groups_are_picked_apart(void **state)
{
	struct audio_voice_member m[] = {member(1, IN_TREE), member(2, IN_TREE), member(3, IN_TREE),
					 member(4, IN_TREE)};
	m[1].group = 2;
	m[3].group = 2;
	PICK(m);
	assert_int_equal(count_voices(m, N(m)), 2);
	assert_true(find(m, N(m), 1)->voice);
	assert_true(find(m, N(m), 2)->voice);
	assert_open(m, N(m), 3, false);
	assert_open(m, N(m), 4, false);
	(void)state;
}

/* Equal ranks break on the source, never on the order the members arrive in. */
static void equal_ranks_break_on_the_source(void **state)
{
	static char first, second;
	struct audio_voice_member forward[] = {member(1, IN_TREE), member(1, IN_TREE)};
	forward[0].source = &first;
	forward[1].source = &second;
	struct audio_voice_member backward[] = {forward[1], forward[0]};
	PICK(forward);
	PICK(backward);
	void *voice = forward[0].voice ? forward[0].source : forward[1].source;
	assert_int_equal(count_voices(forward, N(forward)), 1);
	assert_int_equal(count_voices(backward, N(backward)), 1);
	assert_ptr_equal(backward[0].voice ? backward[0].source : backward[1].source, voice);
	(void)state;
}

/* Transition rule 1: a voice on the outgoing side hands off at once to a member at full gain. */
static void outgoing_voice_hands_off_to_unsided(void **state)
{
	struct audio_voice_member m[] = {sided(1, SIDE_A, T), member(2, IN_TREE), sided(3, SIDE_B, T)};
	m[0].voice = true;
	m[0].open = true;
	PICK(m);
	assert_voice(m, N(m), 2);
	assert_open(m, N(m), 1, false);
	assert_open(m, N(m), 3, false);
	(void)state;
}

/* Rule 2: with no member at full gain, the incoming side's member becomes the voice and the
 * outgoing voice stays open beside it, and that pair holds on the next tick. */
static void outgoing_voice_crossfades_with_incoming(void **state)
{
	struct audio_voice_member m[] = {sided(1, SIDE_A, T), sided(2, SIDE_B, T)};
	m[0].voice = true;
	m[0].open = true;
	PICK(m);
	assert_voice(m, N(m), 2);
	assert_open(m, N(m), 1, true);

	PICK(m);
	assert_voice(m, N(m), 2);
	assert_open(m, N(m), 1, true);

	/* The transition ends: the outgoing member leaves the tree and closes. */
	find(m, N(m), 1)->presence = AUDIO_VOICE_ACTIVE;
	struct audio_voice_member *incoming = find(m, N(m), 2);
	incoming->side = AUDIO_VOICE_UNSIDED;
	incoming->transition = NULL;
	PICK(m);
	assert_voice(m, N(m), 2);
	assert_open(m, N(m), 1, false);
	(void)state;
}

/* Rule 3: nothing to hand off to, so the voice fades with its side. */
static void outgoing_voice_alone_fades(void **state)
{
	struct audio_voice_member m[] = {sided(1, SIDE_A, T), member(2, ACTIVE)};
	m[0].voice = true;
	m[0].open = true;
	PICK(m);
	assert_voice(m, N(m), 1);
	assert_open(m, N(m), 2, false);
	(void)state;
}

/* A fresh voice on an incoming side opens the outgoing side's member beside it. */
static void incoming_voice_opens_outgoing_partner(void **state)
{
	struct audio_voice_member m[] = {sided(1, SIDE_B, T), sided(2, SIDE_A, T), sided(3, SIDE_A, T)};
	m[2].open = true; /* already playing, so it is the partner over the better-ranked one */
	PICK(m);
	assert_voice(m, N(m), 1);
	assert_open(m, N(m), 3, true);
	assert_open(m, N(m), 2, false);
	(void)state;
}

/* Rule 2 needs the incoming member to be under the voice's own transition. */
static void other_transition_opens_nothing(void **state)
{
	struct audio_voice_member m[] = {sided(1, SIDE_A, T), sided(2, SIDE_B, U), sided(3, SIDE_A, U)};
	m[0].voice = true;
	m[0].open = true;
	PICK(m);
	assert_voice(m, N(m), 1);
	assert_open(m, N(m), 2, false);
	assert_open(m, N(m), 3, false);
	(void)state;
}

/* An unsided voice keeps the voice through someone else's transition. */
static void unsided_voice_ignores_transitions(void **state)
{
	struct audio_voice_member m[] = {sided(1, SIDE_A, T), member(2, IN_TREE), sided(3, SIDE_B, T)};
	m[1].voice = true;
	m[1].open = true;
	PICK(m);
	assert_voice(m, N(m), 2);
	assert_open(m, N(m), 1, false);
	assert_open(m, N(m), 3, false);
	(void)state;
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(two_in_tree_one_voice),
		cmocka_unit_test(current_voice_is_kept),
		cmocka_unit_test(hidden_voice_hands_off_to_a_showing_member),
		cmocka_unit_test(hidden_voice_is_kept_when_nothing_shows),
		cmocka_unit_test(showing_beats_rank),
		cmocka_unit_test(shutdown_when_hidden_loses),
		cmocka_unit_test(disabled_member_is_no_candidate),
		cmocka_unit_test(active_member_is_gated_beside_an_in_tree_voice),
		cmocka_unit_test(active_members_alone_still_get_one_voice),
		cmocka_unit_test(no_active_member_leaves_everything),
		cmocka_unit_test(ungrouped_is_open),
		cmocka_unit_test(groups_are_picked_apart),
		cmocka_unit_test(equal_ranks_break_on_the_source),
		cmocka_unit_test(outgoing_voice_hands_off_to_unsided),
		cmocka_unit_test(outgoing_voice_crossfades_with_incoming),
		cmocka_unit_test(outgoing_voice_alone_fades),
		cmocka_unit_test(incoming_voice_opens_outgoing_partner),
		cmocka_unit_test(other_transition_opens_nothing),
		cmocka_unit_test(unsided_voice_ignores_transitions),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
