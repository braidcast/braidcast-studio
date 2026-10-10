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
	// Audio. The spec's table says "mute mic"; any global channel ("mute desktop audio")
	// is the P2 plan's own command table and case table, kept deliberately. The muted-mic
	// rule (CommandRegistry's IsAllowedWhileMuted) still lets only the mic back.
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
	//
	// The spec's own phrasings ("end stream", "stop streaming") are rows, and so is every
	// plain variant of them: StartsWith compares words exactly, and in push-to-talk an
	// utterance no row claims is dictated to chat. Starting is beyond the spec's table:
	// "go live" and the start rows are the P2 plan's, made confirm-gated by the plan
	// index's revision 4 (2026-09-20).
	{"start streaming", "streaming.start", SlotKind::None, true, "Start streaming", nullptr, false, false, nullptr,
	 false},
	{"start the stream", "streaming.start", SlotKind::None, true, "Start streaming", nullptr, false, false, nullptr,
	 false},
	{"start stream", "streaming.start", SlotKind::None, true, "Start streaming", nullptr, false, false, nullptr,
	 false},
	{"go live", "streaming.start", SlotKind::None, true, "Start streaming", nullptr, false, false, nullptr, false},
	{"stop streaming", "streaming.stop", SlotKind::None, true, "Stop streaming", nullptr, false, false, nullptr,
	 false},
	{"stop the stream", "streaming.stop", SlotKind::None, true, "Stop streaming", nullptr, false, false, nullptr,
	 false},
	{"stop stream", "streaming.stop", SlotKind::None, true, "Stop streaming", nullptr, false, false, nullptr,
	 false},
	{"end the stream", "streaming.stop", SlotKind::None, true, "Stop streaming", nullptr, false, false, nullptr,
	 false},
	{"end stream", "streaming.stop", SlotKind::None, true, "Stop streaming", nullptr, false, false, nullptr, false},
	{"end streaming", "streaming.stop", SlotKind::None, true, "Stop streaming", nullptr, false, false, nullptr,
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
	result.score = 1.0;
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
		result.score = slot.score;
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

// ---- Near misses ------------------------------------------------------------------
//
// In push-to-talk an utterance no row claims is dictated to every chat (CommandRegistry's
// fallback), so a command that was misheard -- "and stream", "go life", "switch 2 BRB" --
// would otherwise be posted to chat instead of done. A near miss is a miss with a reason
// ("Did you mean 'end stream'?") and never reaches chat. It must stay narrow, because
// ordinary chat ("nice stream", "I'll switch to BRB later") has to keep reaching chat:
//   - the first word must sound like the command's first word (commands are anchored at
//     the start, and a near miss of one is too);
//   - a command with no slot is weighed against the whole utterance, which may be at most
//     one word longer than the phrase;
//   - a command with a slot counts only when what follows the misheard phrase resolves
//     to one of the studio's own names (a scene, a source, a person, a platform).
// Chat rows without a slot are left out: a misheard "chat hello" reaches the same chats
// through the fallback anyway, with the countdown to stop it.

// How close a whole-utterance command must come. "nice stream" against "end stream"
// scores 0.5 and stays chat; "and stream" scores 0.83.
constexpr double kNearPhraseScore = 0.7;
// How close the phrase before a slot must come when the slot then resolves. Lower, since
// a resolved name is evidence of its own: "so webcam" (0.5) means "show webcam".
constexpr double kNearVerbScore = 0.5;
// How close a single word must be to count as a slip of that word rather than another
// word: "and" for "end" (0.67) counts, "nice" for "end" (0) does not.
constexpr double kNearWordScore = 0.5;
// A whole-utterance command is weighed only against an utterance at most this many
// words longer, so "go live soon" and "stop streaming for a second" stay conversation.
constexpr size_t kNearExtraWords = 1;

// Words whisper writes for one another that no spelling distance relates.
struct SoundAlike {
	const char *heard;
	const char *meant;
};

constexpr SoundAlike kSoundAlikes[] = {
	{"2", "to"},
	{"too", "to"},
	{"4", "for"},
};
constexpr double kSoundAlikeScore = 0.9;

size_t EditDistance(const std::string &a, const std::string &b)
{
	std::vector<size_t> row(b.size() + 1);
	for (size_t j = 0; j <= b.size(); ++j) {
		row[j] = j;
	}
	for (size_t i = 1; i <= a.size(); ++i) {
		size_t diagonal = row[0];
		row[0] = i;
		for (size_t j = 1; j <= b.size(); ++j) {
			const size_t above = row[j];
			row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
			diagonal = above;
		}
	}
	return row[b.size()];
}

// 0 to 1: how alike two spoken words are, by spelling distance.
double WordSimilarity(const std::string &heard, const std::string &meant)
{
	if (heard == meant) {
		return 1.0;
	}
	for (const SoundAlike &alike : kSoundAlikes) {
		if (heard == alike.heard && meant == alike.meant) {
			return kSoundAlikeScore;
		}
	}
	const size_t longest = std::max(heard.size(), meant.size());
	return longest == 0 ? 0.0
			    : 1.0 - static_cast<double>(EditDistance(heard, meant)) / static_cast<double>(longest);
}

// 0 to 1: how alike two word sequences are, as a word-level edit distance in which
// swapping a word for a near spelling of it costs less than a whole word. Joined
// spellings ("endstream") count as the phrase.
double PhraseSimilarity(const std::vector<std::string> &heard, const std::vector<std::string> &phrase)
{
	if (heard.empty() || phrase.empty()) {
		return 0.0;
	}
	std::string heardJoined;
	std::string phraseJoined;
	for (const std::string &word : heard) {
		heardJoined += word;
	}
	for (const std::string &word : phrase) {
		phraseJoined += word;
	}
	if (heardJoined == phraseJoined) {
		return heard.size() == phrase.size() ? 1.0 : 0.95;
	}
	std::vector<double> row(phrase.size() + 1);
	for (size_t j = 0; j <= phrase.size(); ++j) {
		row[j] = static_cast<double>(j);
	}
	for (size_t i = 1; i <= heard.size(); ++i) {
		double diagonal = row[0];
		row[0] = static_cast<double>(i);
		for (size_t j = 1; j <= phrase.size(); ++j) {
			const double above = row[j];
			const double alike = WordSimilarity(heard[i - 1], phrase[j - 1]);
			const double swap = alike >= kNearWordScore ? 1.0 - alike : 1.0;
			row[j] = std::min({row[j] + 1.0, row[j - 1] + 1.0, diagonal + swap});
			diagonal = above;
		}
	}
	const double longest = static_cast<double>(std::max(heard.size(), phrase.size()));
	return std::max(0.0, 1.0 - row[phrase.size()] / longest);
}

// 0 to 1: how alike two phrases of the same length are, word by word, or 0 when any one
// word is not a slip of its counterpart ("go live" is not "go to" misheard).
double WordForWord(const std::vector<std::string> &heard, const std::vector<std::string> &phrase)
{
	if (heard.size() != phrase.size() || phrase.empty()) {
		return 0.0;
	}
	double total = 0.0;
	for (size_t i = 0; i < phrase.size(); ++i) {
		const double alike = WordSimilarity(heard[i], phrase[i]);
		if (alike < kNearWordScore) {
			return 0.0;
		}
		total += alike;
	}
	return total / static_cast<double>(phrase.size());
}

// The words that carry meaning, fillers dropped.
std::vector<std::string> ContentWords(const std::vector<std::string> &tokens)
{
	std::vector<std::string> out;
	for (const std::string &token : tokens) {
		if (!IsFiller(token)) {
			out.push_back(token);
		}
	}
	return out;
}

std::string Joined(const std::vector<std::string> &words, size_t from, size_t count)
{
	std::string out;
	for (size_t i = from; i < from + count && i < words.size(); ++i) {
		if (!out.empty()) {
			out.push_back(' ');
		}
		out += words[i];
	}
	return out;
}

// The best near miss among the rows, or a result with nearMiss false.
CommandMatch NearMiss(const Normalized &said, const CommandCandidates &candidates)
{
	CommandMatch best;
	const std::vector<std::string> heard = ContentWords(said.tokens);
	if (heard.empty()) {
		return best;
	}
	for (const Pattern &pattern : kPatterns) {
		if (*pattern.prefix == '\0' || (pattern.takesMessage && pattern.slot == SlotKind::None)) {
			continue;
		}
		// Anchored: the first word is a slip of the command's first word, or runs it
		// together with the next ("endstream").
		const std::vector<std::string> &spokenPhrase = Normalize(pattern.prefix).tokens;
		if (spokenPhrase.empty() || (WordSimilarity(said.tokens[0], spokenPhrase[0]) < kNearWordScore &&
					     said.tokens[0].rfind(spokenPhrase[0], 0) != 0)) {
			continue;
		}
		const SlotInfo *info = FindSlot(pattern.slot);
		double score = 0.0;
		std::string meant;
		std::vector<std::string> rest;
		if (!info) {
			// Fillers aside on both sides: "end a stream" is "end the stream" misheard.
			const std::vector<std::string> phrase = ContentWords(spokenPhrase);
			if (heard.size() > phrase.size() + kNearExtraWords) {
				continue;
			}
			score = PhraseSimilarity(heard, phrase);
			if (score < kNearPhraseScore) {
				continue;
			}
			meant = Joined(phrase, 0, phrase.size());
		} else {
			// The phrase word for word ("to" is a filler only inside a name), then the
			// name.
			if (said.tokens.size() <= spokenPhrase.size()) {
				continue;
			}
			const std::vector<std::string> verb(said.tokens.begin(),
							    said.tokens.begin() +
								    static_cast<std::ptrdiff_t>(spokenPhrase.size()));
			score = WordForWord(verb, spokenPhrase);
			if (score < kNearVerbScore) {
				continue;
			}
			rest = ContentWords(std::vector<std::string>(
				said.tokens.begin() + static_cast<std::ptrdiff_t>(spokenPhrase.size()),
				said.tokens.end()));
			if (rest.empty()) {
				continue;
			}
			meant = pattern.prefix;
			// The slot must resolve, and clearly: an ambiguous name is no evidence.
			const std::vector<std::string> &names = candidates.*(info->list);
			std::vector<std::string> spokenNames;
			for (const std::string &name : names) {
				spokenNames.push_back(info->handles ? SpokenHandle(name) : name);
			}
			SlotMatch slot;
			if (pattern.takesMessage) {
				// A name of up to kMaxNameTokens words, and a message after it.
				for (size_t take = 1; take < rest.size() && take <= kMaxNameTokens; ++take) {
					const SlotMatch tried = BestMatch(Joined(rest, 0, take), spokenNames);
					if (tried.ok && tried.score > slot.score) {
						slot = tried;
					}
				}
			} else {
				slot = ResolveSlot(pattern.slot, Joined(rest, 0, rest.size()), spokenNames);
			}
			if (!slot.ok) {
				continue;
			}
			meant += " " + names[slot.index];
		}
		if (score > best.score) {
			best.nearMiss = true;
			best.score = score;
			best.message = "Did you mean '" + meant + "'?";
		}
	}
	return best;
}

} // namespace

CommandMatch MatchCommand(const Normalized &said, const CommandCandidates &candidates)
{
	CommandMatch result;
	if (said.tokens.empty()) {
		return result;
	}

	// A command word with nothing after it ("mute", "switch to the"): not a command, and
	// not chat either. Kept until every row has been tried, since another may match.
	CommandMatch bare;
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
				result = CommandMatch{};
				continue;
			}
			result.ok = true;
			result.score = 1.0;
			result.summary = pattern.summaryVerb;
			return result;
		}

		if (spoken.empty()) {
			// "switch to" with nothing after it is not a command, but it was meant as one.
			if (!bare.nearMiss) {
				bare.nearMiss = true;
				bare.score = 1.0;
				bare.message =
					std::string("Say which ") + info->noun + " after '" + pattern.prefix + "'.";
			}
			result = CommandMatch{};
			continue;
		}
		const std::vector<std::string> &names = candidates.*(info->list);
		const SlotMatch slot = ResolveSlot(pattern.slot, spoken, names);
		result.score = slot.score;
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
	if (bare.nearMiss) {
		return bare;
	}
	return NearMiss(said, candidates);
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
