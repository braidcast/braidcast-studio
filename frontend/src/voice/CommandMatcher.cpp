#include "voice/CommandMatcher.hpp"

#include "voice/FuzzyMatch.hpp"

#include <algorithm>
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
	// True when what follows the prefix (and the slot, if there is one) is a message to
	// keep rather than a name to resolve: the chat rows. A message is never fuzzy-matched.
	bool takesMessage;
	// A word that must follow the slot before the message starts ("twitch ONLY ..."), or
	// nullptr.
	const char *slotThen;
	// True when a slot that resolves to nothing means the row does not apply, rather than
	// a miss with a reason. For the forms ordinary speech also takes: "tell them I'm back"
	// is not a platform called "them", it is something to say.
	bool soft;
};

// Order matters where two prefixes share a first word: the longer one goes above, which
// is why "scene" sits below the three longer scene prefixes. A later "start" would have
// to go below "start streaming".
constexpr Pattern kPatterns[] = {
	// Scenes.
	{"switch to", "scenes.setCurrent", SlotKind::Scene, false, "Switch to", nullptr, false, false, nullptr, false},
	{"go to", "scenes.setCurrent", SlotKind::Scene, false, "Switch to", nullptr, false, false, nullptr, false},
	{"change to", "scenes.setCurrent", SlotKind::Scene, false, "Switch to", nullptr, false, false, nullptr, false},
	{"scene", "scenes.setCurrent", SlotKind::Scene, false, "Switch to", nullptr, false, false, nullptr, false},
	// Sources.
	{"show", "sceneItems.setVisible", SlotKind::Source, false, "Show", "visible", true, false, nullptr, false},
	{"hide", "sceneItems.setVisible", SlotKind::Source, false, "Hide", "visible", false, false, nullptr, false},
	// Audio.
	{"mute", "audio.setMuted", SlotKind::AudioSource, false, "Mute", "muted", true, false, nullptr, false},
	{"unmute", "audio.setMuted", SlotKind::AudioSource, false, "Unmute", "muted", false, false, nullptr, false},
	// Lifecycle. Both ask first. Stopping ends a broadcast, which is obvious; starting
	// one is outward-facing too -- it opens a live broadcast on every armed destination,
	// and an accidental go-live is not undoable by switching back. An earlier draft left
	// "go live" unconfirmed on the grounds that a human had just held the key and spoken.
	// That reasoning holds only for a human: the same Interpretation can be produced by a
	// wake-word false positive, and by a non-human interpreter later (see the fallthrough
	// note in RunCommand). MCP already gates these two behind a separate GoLive
	// capability that is off by default; this is the same judgement.
	{"start streaming", "streaming.start", SlotKind::None, true, "Start streaming", nullptr, false, false, nullptr,
	 false},
	{"start the stream", "streaming.start", SlotKind::None, true, "Start streaming", nullptr, false, false, nullptr,
	 false},
	{"go live", "streaming.start", SlotKind::None, true, "Start streaming", nullptr, false, false, nullptr, false},
	{"stop streaming", "streaming.stop", SlotKind::None, true, "Stop streaming", nullptr, false, false, nullptr,
	 false},
	{"stop the stream", "streaming.stop", SlotKind::None, true, "Stop streaming", nullptr, false, false, nullptr,
	 false},
	{"end the stream", "streaming.stop", SlotKind::None, true, "Stop streaming", nullptr, false, false, nullptr,
	 false},
	// No recording commands: recording is dormant in this streaming-only fork (the
	// bridge has no recording method at all), and the spec excludes it deliberately. Do
	// not "fix" the omission.
	//
	// Chat, below every studio command (the spec's match order: commands, then chat forms,
	// then the push-to-talk fallback in CommandRegistry). All of it is chat.send, the
	// bridge method that posts a message, and a reply is chat.send addressed to one
	// destination. None asks at match time: the send mode (Settings -> Voice) decides
	// whether a message waits. "send to chat" must stay above "send to", or every message
	// would be matched as a platform called "chat".
	{"send to chat", "chat.send", SlotKind::None, false, "Send to chat", nullptr, false, true, nullptr, false},
	{"say in chat", "chat.send", SlotKind::None, false, "Send to chat", nullptr, false, true, nullptr, false},
	{"chat", "chat.send", SlotKind::None, false, "Send to chat", nullptr, false, true, nullptr, false},
	{"send to", "chat.send", SlotKind::Platform, false, "Send to", nullptr, false, true, nullptr, false},
	{"tell", "chat.send", SlotKind::Platform, false, "Send to", nullptr, false, true, nullptr, true},
	{"reply to", "chat.send", SlotKind::Person, false, "Reply to", nullptr, false, true, nullptr, false},
	// "{platform} only {message}": no prefix at all, so it may only claim an utterance
	// whose first words really are a platform and "only".
	{"", "chat.send", SlotKind::Platform, false, "Send to", nullptr, false, true, "only", true},
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
	// Candidates are typed chat handles, matched as they are said (SpokenHandle):
	// "DaveTheStreamer" is "Dave" out loud.
	bool handles;
};

constexpr SlotInfo kSlots[] = {
	{SlotKind::Scene, "scene", &CommandCandidates::scenes, false},
	{SlotKind::Source, "source", &CommandCandidates::sources, false},
	{SlotKind::AudioSource, "audio source", &CommandCandidates::audioSources, false},
	{SlotKind::Person, "person", &CommandCandidates::people, true},
	{SlotKind::Platform, "chat platform", &CommandCandidates::platforms, false},
};

// The longest name a chat slot is tried at, in spoken words.
constexpr size_t kMaxNameTokens = 3;

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

// The runner-up by its own name. BestMatch names it as it was matched, which for a chat
// handle is the spoken form.
std::string RunnerUp(const SlotMatch &slot, const std::vector<std::string> &names,
		     const std::vector<std::string> &matched)
{
	for (size_t i = 0; i < matched.size(); ++i) {
		if (i != slot.index && matched[i] == slot.runnerUp) {
			return names[i];
		}
	}
	return slot.runnerUp;
}

// The spoken words [from, from + count) joined by single spaces.
std::string Tokens(const Normalized &said, size_t from, size_t count)
{
	std::string out;
	for (size_t i = from; i < from + count && i < said.tokens.size(); ++i) {
		if (!out.empty()) {
			out.push_back(' ');
		}
		out += said.tokens[i];
	}
	return out;
}

// True when `word` (normalized) is the whole of tokens [at, ...); `used` gets its length.
bool WordsAt(const Normalized &said, size_t at, const char *word, size_t &used)
{
	const Normalized want = Normalize(word);
	if (want.tokens.empty() || at + want.tokens.size() > said.tokens.size()) {
		return false;
	}
	for (size_t i = 0; i < want.tokens.size(); ++i) {
		if (said.tokens[at + i] != want.tokens[i]) {
			return false;
		}
	}
	used = want.tokens.size();
	return true;
}

// The outcome of trying one chat row.
enum class ChatRow { Matched, Failed, NotThisRow };

// A chat row: the slot (a person or a platform, the name tried at up to kMaxNameTokens
// words, so "Dave The Streamer" is one name), then the message. `result`
// is filled for Matched and Failed.
ChatRow MatchChat(const Pattern &pattern, const Normalized &said, size_t used, const CommandCandidates &candidates,
		  CommandMatch &result)
{
	size_t messageFrom = used;
	const SlotInfo *info = FindSlot(pattern.slot);
	if (info) {
		const std::vector<std::string> &names = candidates.*(info->list);
		std::vector<std::string> spokenNames;
		if (info->handles) {
			spokenNames.reserve(names.size());
			for (const std::string &name : names) {
				spokenNames.push_back(SpokenHandle(name));
			}
		}
		const std::vector<std::string> &matchAgainst = info->handles ? spokenNames : names;
		// Every length is tried and the best-scoring one wins, so a name is never
		// extended into the message ("sarah 92 hi" scores lower against Sarah_92 than
		// "sarah 92" does); at equal scores the longer reading wins.
		SlotMatch slot;
		size_t slotTokens = 0;
		const size_t most = std::min(kMaxNameTokens, said.tokens.size() - used);
		for (size_t take = 1; take <= most; ++take) {
			const SlotMatch tried = BestMatch(Tokens(said, used, take), matchAgainst);
			if ((tried.ok || tried.ambiguous) && tried.score >= slot.score) {
				slot = tried;
				slotTokens = take;
			}
		}
		if (!slot.ok && pattern.soft) {
			return ChatRow::NotThisRow;
		}
		if (slot.ambiguous) {
			result.ambiguous = true;
			result.message = std::string("More than one ") + info->noun + " matches '" +
					 said.Original(used, used + slotTokens - 1) + "' (" + names[slot.index] + ", " +
					 RunnerUp(slot, names, matchAgainst) + ").";
			return ChatRow::Failed;
		}
		if (!slot.ok) {
			result.message = most == 0 ? std::string("Say who or where after '") + pattern.prefix + "'."
						   : std::string("I could not find a ") + info->noun + " called '" +
							     said.Original(used, used) + "'.";
			return ChatRow::Failed;
		}
		result.slotValue = names[slot.index];
		messageFrom = used + slotTokens;
		size_t thenTokens = 0;
		if (pattern.slotThen) {
			if (!WordsAt(said, messageFrom, pattern.slotThen, thenTokens)) {
				return ChatRow::NotThisRow;
			}
			messageFrom += thenTokens;
		}
	}
	if (messageFrom >= said.tokens.size()) {
		if (pattern.soft) {
			return ChatRow::NotThisRow;
		}
		// A chat phrase with nothing after it: the user meant to say something and did not.
		result.message = "There was no message to send.";
		return ChatRow::Failed;
	}
	result.messageText = said.Original(messageFrom, said.tokens.size() - 1);
	result.summary = std::string(pattern.summaryVerb) + (result.slotValue.empty() ? "" : " " + result.slotValue) +
			 ": " + result.messageText;
	return ChatRow::Matched;
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

		if (pattern.takesMessage) {
			const ChatRow row = MatchChat(pattern, said, used, candidates, result);
			if (row == ChatRow::NotThisRow) {
				result = CommandMatch{};
				continue;
			}
			result.ok = row == ChatRow::Matched;
			return result;
		}

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
		if (*pattern.prefix != '\0') {
			phrases.emplace_back(pattern.prefix);
		}
	}
	return phrases;
}

} // namespace Voice
