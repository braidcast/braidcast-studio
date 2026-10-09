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
};

enum class SlotKind { None, Scene, Source, AudioSource };

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
};

// Matches an utterance against the command table. Anchored: the phrase must start the
// utterance, so "I'll switch to BRB later" is conversation, not a command. Pure: no
// libobs, no state.
CommandMatch MatchCommand(const Normalized &said, const CommandCandidates &candidates);

// Every phrase the matcher knows, for the whisper prompt bias.
std::vector<std::string> CommandPhrases();

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_COMMAND_MATCHER_HPP_
