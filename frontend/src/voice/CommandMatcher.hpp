#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_COMMAND_MATCHER_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_COMMAND_MATCHER_HPP_

#include "voice/TextNormalize.hpp"

#include <string>
#include <vector>

namespace Voice {

// What the studio currently contains, supplied by CommandRegistry. Pure data, so the
// whole pattern table can be tested against a fixed studio.
struct CommandCandidates {
	std::vector<std::string> scenes;
	std::vector<std::string> sources;      // scene items in the current scene
	std::vector<std::string> audioSources; // global audio channels
	// The source on the user's microphone channel (GlobalAudio::PrimaryMicChannel), ""
	// when none is bound: what "unmute mic" has to resolve to before it may pass a
	// muted mic in always-listen mode.
	std::string micSource;
	// The people who have spoken in chat lately, as they display, most recent first and one
	// per name (the chat hub's recent-chatter ring, Chat::DistinctByName), for "reply to ...".
	std::vector<std::string> people;
	// The platforms with a live chat, as provider ids ("twitch", "youtube"), for "send to
	// twitch ..." and for where a message to everyone goes.
	std::vector<std::string> platforms;
	// Where a reply to each of `people` goes, by the same index: the chat that person last
	// spoke in (Chat::Chatter). The matcher reads names only; the registry addresses the
	// reply from this.
	struct ReplyRoute {
		std::string platform;
		std::string accountId;
		std::string profileUuid;
	};
	std::vector<ReplyRoute> replyRoutes;
};

enum class SlotKind { None, Scene, Source, AudioSource, Person, Platform };

struct CommandMatch {
	bool ok = false;
	bool ambiguous = false; // matched a command, but the slot was unclear
	bool needsConfirm = false;
	std::string commandId;
	SlotKind slot = SlotKind::None;
	std::string slotValue; // the resolved name, exactly as the studio spells it
	std::string summary;   // one line for the UI and the confirmation prompt
	std::string message;   // why, when ok is false
	// Copied from the matched row: the boolean parameter that tells show from hide and
	// mute from unmute, since those share one bridge method. nullptr for the rest.
	const char *flagKey = nullptr;
	bool flagValue = false;
	// A chat command's message: the free-text remainder, in the user's own spelling and
	// punctuation (Normalized::Original). Empty for every other command.
	std::string messageText;
	// Not a command, but so close to one that it was almost certainly meant as one: a
	// misheard phrase ("and stream", "go life", "switch 2 BRB") or a command word with
	// nothing after it ("mute"). ok is false and `message` says what was probably meant.
	// A near miss is never dictated to chat by the push-to-talk fallback: posting a
	// misheard "end stream" to every chat is the one outcome worse than doing nothing.
	bool nearMiss = false;
	// How sure the match is, 0 to 1, for the debug log (never the words): 1 for a phrase
	// heard exactly, the slot's similarity when a name was resolved, a near miss's own
	// closeness.
	double score = 0.0;
};

// Matches an utterance against the command table. Anchored: the phrase must start the
// utterance, so "I'll switch to BRB later" is conversation, not a command. When nothing
// matches, the utterance is weighed as a near miss of each command (see nearMiss). Pure:
// no libobs, no state.
CommandMatch MatchCommand(const Normalized &said, const CommandCandidates &candidates);

// Every phrase the matcher knows, for the whisper prompt bias.
std::vector<std::string> CommandPhrases();

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_COMMAND_MATCHER_HPP_
