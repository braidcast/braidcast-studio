#include "voice/CommandMatcher.hpp"

#include "voice/FuzzyMatch.hpp"

#include <iterator>

namespace Voice {

namespace {

// One row per spoken phrase. Adding a command is a row here, not a branch anywhere.
// `prefix` is matched at the start of the utterance; what follows fills the slot.
// Filler words are dropped from the slot text before it is resolved.
struct Pattern {
	const char *prefix;
	// The BRIDGE METHOD NAME, exactly as g_methods spells it -- not a voice-private id.
	// One namespace, so a PendingAction is a literal {method, params} bridge envelope and
	// anything that classifies bridge methods (MCP's Capability gate) applies unchanged.
	const char *commandId;
	SlotKind slot;
	bool needsConfirm;
	const char *summaryVerb; // "Switch to", "Show", ...
	// Show/hide and mute/unmute are ONE bridge method each, told apart by a boolean
	// parameter rather than by the method name. nullptr when the command takes none.
	const char *flagKey;
	bool flagValue;
};

// Order matters where two prefixes share a first word: the longer one goes above, which
// is why "scene" sits below the three longer scene prefixes. A later "start" would have
// to go below "start streaming".
constexpr Pattern kPatterns[] = {
	// Scenes.
	{"switch to", "scenes.setCurrent", SlotKind::Scene, false, "Switch to", nullptr, false},
	{"go to", "scenes.setCurrent", SlotKind::Scene, false, "Switch to", nullptr, false},
	{"change to", "scenes.setCurrent", SlotKind::Scene, false, "Switch to", nullptr, false},
	{"scene", "scenes.setCurrent", SlotKind::Scene, false, "Switch to", nullptr, false},
	// Sources.
	{"show", "sceneItems.setVisible", SlotKind::Source, false, "Show", "visible", true},
	{"hide", "sceneItems.setVisible", SlotKind::Source, false, "Hide", "visible", false},
	// Audio.
	{"mute", "audio.setMuted", SlotKind::AudioSource, false, "Mute", "muted", true},
	{"unmute", "audio.setMuted", SlotKind::AudioSource, false, "Unmute", "muted", false},
	// Lifecycle. Both ask first. Stopping ends a broadcast, which is obvious; starting
	// one is outward-facing too -- it opens a live broadcast on every armed destination,
	// and an accidental go-live is not undoable by switching back. An earlier draft left
	// "go live" unconfirmed on the grounds that a human had just held the key and spoken.
	// That reasoning holds only for a human: the same Interpretation can be produced by a
	// wake-word false positive, and by a non-human interpreter later (see the fallthrough
	// note in RunCommand). MCP already gates these two behind a separate GoLive
	// capability that is off by default; this is the same judgement.
	{"start streaming", "streaming.start", SlotKind::None, true, "Start streaming", nullptr, false},
	{"start the stream", "streaming.start", SlotKind::None, true, "Start streaming", nullptr, false},
	{"go live", "streaming.start", SlotKind::None, true, "Start streaming", nullptr, false},
	{"stop streaming", "streaming.stop", SlotKind::None, true, "Stop streaming", nullptr, false},
	{"stop the stream", "streaming.stop", SlotKind::None, true, "Stop streaming", nullptr, false},
	{"end the stream", "streaming.stop", SlotKind::None, true, "Stop streaming", nullptr, false},
	// No recording commands: recording is dormant in this streaming-only fork (the
	// bridge has no recording method at all), and the spec excludes it deliberately. Do
	// not "fix" the omission.
};

// Words that carry no meaning inside a slot: "hide the alerts" is "hide alerts", and
// "unmute my mic" is "unmute mic".
constexpr const char *kFillers[] = {"the", "my", "a", "an", "to", "please", "now"};

// Spoken forms of names that no similarity score will ever reach. Tried only when what
// was heard matches nothing at all, so a source the user really named "Mic" or "Game"
// is still found by its own name; scoped to the slot they make sense for, and tried in
// order. "mic" is in both directions because studios name the channel either way
// ("Mic/Aux", "Microphone").
struct Alias {
	SlotKind slot;
	const char *heard;
	const char *meant;
};

constexpr Alias kAliases[] = {
	{SlotKind::AudioSource, "mike", "mic"},
	{SlotKind::AudioSource, "mike", "microphone"},
	{SlotKind::AudioSource, "mic", "microphone"},
	{SlotKind::AudioSource, "microphone", "mic"},
	{SlotKind::AudioSource, "desktop", "desktop audio"},
	{SlotKind::Source, "cam", "camera"},
	{SlotKind::Source, "cam", "webcam"},
	{SlotKind::Source, "game", "game capture"},
};

// What each slot resolves against, and what it is called in a message.
struct SlotInfo {
	SlotKind slot;
	const char *noun;
	std::vector<std::string> CommandCandidates::*list;
};

constexpr SlotInfo kSlots[] = {
	{SlotKind::Scene, "scene", &CommandCandidates::scenes},
	{SlotKind::Source, "source", &CommandCandidates::sources},
	{SlotKind::AudioSource, "audio source", &CommandCandidates::audioSources},
};

const SlotInfo *FindSlot(SlotKind slot)
{
	for (const SlotInfo &info : kSlots) {
		if (info.slot == slot) {
			return &info;
		}
	}
	return nullptr;
}

bool IsFiller(const std::string &token)
{
	for (const char *filler : kFillers) {
		if (token == filler) {
			return true;
		}
	}
	return false;
}

// True when `said` starts with the whole of `prefix`, on token boundaries; `used`
// receives how many tokens the prefix consumed.
bool StartsWith(const Normalized &said, const char *prefix, size_t &used)
{
	const Normalized pattern = Normalize(prefix);
	if (pattern.tokens.size() > said.tokens.size()) {
		return false;
	}
	for (size_t i = 0; i < pattern.tokens.size(); ++i) {
		if (said.tokens[i] != pattern.tokens[i]) {
			return false;
		}
	}
	used = pattern.tokens.size();
	return true;
}

std::string SlotText(const Normalized &said, size_t from)
{
	std::string out;
	for (size_t i = from; i < said.tokens.size(); ++i) {
		if (IsFiller(said.tokens[i])) {
			continue;
		}
		if (!out.empty()) {
			out.push_back(' ');
		}
		out += said.tokens[i];
	}
	return out;
}

// The spoken name first; only when it matches nothing at all, its aliases in order.
SlotMatch ResolveSlot(SlotKind slot, const std::string &spoken, const std::vector<std::string> &candidates)
{
	const SlotMatch heard = BestMatch(spoken, candidates);
	if (heard.ok || heard.ambiguous) {
		return heard;
	}
	for (const Alias &alias : kAliases) {
		if (alias.slot != slot || spoken != alias.heard) {
			continue;
		}
		const SlotMatch meant = BestMatch(alias.meant, candidates);
		if (meant.ok || meant.ambiguous) {
			return meant;
		}
	}
	return heard;
}

} // namespace

CommandMatch MatchCommand(const Normalized &said, const CommandCandidates &candidates)
{
	CommandMatch result;
	if (said.tokens.empty()) {
		return result;
	}

	for (const Pattern &pattern : kPatterns) {
		size_t used = 0;
		if (!StartsWith(said, pattern.prefix, used)) {
			continue;
		}
		result.commandId = pattern.commandId;
		result.needsConfirm = pattern.needsConfirm;
		result.slot = pattern.slot;
		result.flagKey = pattern.flagKey;
		result.flagValue = pattern.flagValue;

		const std::string spoken = SlotText(said, used);
		const SlotInfo *info = FindSlot(pattern.slot);
		if (!info) {
			// A no-slot command must be the whole utterance, minus fillers: "stop
			// streaming for a second" is conversation.
			if (!spoken.empty()) {
				continue;
			}
			result.ok = true;
			result.summary = pattern.summaryVerb;
			return result;
		}

		if (spoken.empty()) {
			continue; // "switch to" with nothing after it is not a command
		}
		const std::vector<std::string> &names = candidates.*(info->list);
		const SlotMatch slot = ResolveSlot(pattern.slot, spoken, names);
		if (slot.ambiguous) {
			result.ambiguous = true;
			result.message = std::string("More than one ") + info->noun + " matches '" + spoken + "' (" +
					 names[slot.index] + ", " + slot.runnerUp + ").";
			return result;
		}
		if (!slot.ok) {
			result.message = std::string("I could not find a ") + info->noun + " called '" + spoken + "'.";
			return result;
		}
		result.ok = true;
		result.slotValue = names[slot.index];
		result.summary = std::string(pattern.summaryVerb) + " " + result.slotValue;
		return result;
	}
	return CommandMatch{};
}

std::vector<std::string> CommandPhrases()
{
	std::vector<std::string> phrases;
	phrases.reserve(std::size(kPatterns));
	for (const Pattern &pattern : kPatterns) {
		phrases.emplace_back(pattern.prefix);
	}
	return phrases;
}

} // namespace Voice
