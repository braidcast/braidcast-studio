#include "voice/CommandRegistry.hpp"

#include "bridge.hpp"
#include "chat/chat_hub.hpp"
#include "chat/chat_limits.hpp"
#include "chat/recent_chatters.hpp"
#include "log.hpp"
#include "multistream/GlobalAudioChannels.hpp"
#include "scene/transitions.hpp"
#include "util/time_util.hpp"
#include "voice/TextNormalize.hpp"
#include "voice/VoiceEngine.hpp"

#include <obs.hpp>

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>
#include <vector>

namespace Voice {

namespace {

// Spoken answers to a confirmation prompt. They only mean something while a command is
// waiting; and while one is, nothing else does (see Interpret).
constexpr const char *kConfirmWords[] = {"yes", "yeah", "yep", "confirm", "send", "do it", "go ahead"};
constexpr const char *kCancelWords[] = {"no", "nope", "cancel", "never mind", "stop", "forget it"};

// "Unmute" is the one command that must still work while the mic is muted: it is the
// way back out. It shares audio.setMuted with "mute" now that command ids are bridge
// method names, so the id alone no longer identifies it -- the muted:false flag does,
// and the spec allows only the MIC back ("unmute mic"), not any source.
constexpr const char *kAllowedWhileMutedMethod = "audio.setMuted";
constexpr const char *kAllowedWhileMutedFlag = "muted";
constexpr bool kAllowedWhileMutedValue = false;

// The bridge method a chat message goes out through. A reply is the same method addressed
// to one destination (accountId + profileUuid), exactly as the Multichat composer sends one.
constexpr const char *kChatMethod = "chat.send";

// How a platform id reads in a sentence.
struct PlatformLabel {
	const char *id;
	const char *label;
};

constexpr PlatformLabel kPlatformLabels[] = {
	{"twitch", "Twitch"},
	{"youtube", "YouTube"},
	{"kick", "Kick"},
	{"facebook", "Facebook"},
};

// The bridge's own key for each slot, so params needs no translation on the way out:
// scenes.setCurrent takes "name", audio.setMuted resolves "source" by name through
// ResolveAudioSource, and sceneItems.setVisible gets "source" here plus the scene-item
// id only RunCommand can resolve (see the note there).
struct SlotParam {
	SlotKind slot;
	const char *key;
};

constexpr SlotParam kSlotParams[] = {
	{SlotKind::Scene, "name"},
	{SlotKind::Source, "source"},
	{SlotKind::AudioSource, "source"},
};

bool IsAllowedWhileMuted(const CommandMatch &match, const CommandCandidates &candidates)
{
	return match.commandId == kAllowedWhileMutedMethod && match.flagKey != nullptr &&
	       std::string(match.flagKey) == kAllowedWhileMutedFlag && match.flagValue == kAllowedWhileMutedValue &&
	       !candidates.micSource.empty() && match.slotValue == candidates.micSource;
}

template<size_t N> bool Matches(const std::string &text, const char *const (&words)[N])
{
	for (const char *word : words) {
		if (text == word) {
			return true;
		}
	}
	return false;
}

Interpretation Miss(std::string message)
{
	Interpretation out;
	out.kind = Interpretation::Kind::Miss;
	out.message = std::move(message);
	return out;
}

std::string LabelOf(const std::string &platform)
{
	for (const PlatformLabel &entry : kPlatformLabels) {
		if (platform == entry.id) {
			return entry.label;
		}
	}
	return platform;
}

bool IsChatDraft(const PendingAction &action)
{
	return action.commandId == kChatMethod;
}

// Where a reply to `name` (as listed in candidates.people) goes, or null.
const CommandCandidates::ReplyRoute *RouteTo(const std::string &name, const CommandCandidates &candidates)
{
	for (size_t i = 0; i < candidates.people.size() && i < candidates.replyRoutes.size(); ++i) {
		if (candidates.people[i] == name) {
			return &candidates.replyRoutes[i];
		}
	}
	return nullptr;
}

// A chat message turned into a chat.send envelope, checked against every platform it will
// reach, and given the send mode's shape. The params are exactly chat.send's: {text,
// platforms} for a message to chats, {text, accountId, profileUuid} for a reply to ONE
// chat (index revision 6: a reply never goes to the platform), plus replyTo, the name,
// for what is said back.
Interpretation ChatDraft(const CommandMatch &match, const CommandCandidates &candidates, const SendPolicy &send)
{
	Interpretation out;
	out.action.commandId = kChatMethod;
	std::string text = match.messageText;
	std::vector<std::string> targets;
	if (match.slot == SlotKind::Person) {
		// The person was in the ring when the matcher found them; their chat is what the
		// reply is addressed to. Without it there is no honest place to send it.
		const CommandCandidates::ReplyRoute *route = RouteTo(match.slotValue, candidates);
		if (!route || route->accountId.empty()) {
			return Miss("I have lost track of where " + match.slotValue + " said that.");
		}
		text = "@" + match.slotValue + " " + text;
		targets = {route->platform};
		out.action.params = {{"text", text},
				     {"accountId", route->accountId},
				     {"profileUuid", route->profileUuid},
				     {"replyTo", match.slotValue}};
		out.action.summary = "Reply to " + match.slotValue + ": " + match.messageText;
	} else {
		if (match.slot == SlotKind::Platform) {
			targets = {match.slotValue};
			out.action.summary = "Send to " + LabelOf(match.slotValue) + ": " + text;
		} else {
			targets = candidates.platforms;
			out.action.summary = "Send to chat: " + text;
		}
		if (targets.empty()) {
			return Miss("No chat is live, so there is nowhere to send that.");
		}
		out.action.params = {{"text", text}, {"platforms", targets}};
	}

	// Refused whole rather than cut: a platform that truncates leaves the user believing
	// they said something they did not. The text is kept for the composer to edit.
	std::string offender;
	if (!Chat::FitsEverywhere(text, targets, offender)) {
		if (offender.empty()) {
			return Miss("There was no message to send.");
		}
		Interpretation tooLong = Miss("That message is too long for " + LabelOf(offender) + ".");
		tooLong.keptDraft = text;
		return tooLong;
	}

	if (send.mode == "instant") {
		out.kind = Interpretation::Kind::Instant;
		return out;
	}
	out.kind = Interpretation::Kind::Pending;
	if (send.mode == "say") {
		// Held until "send" (or yes). The window is the ordinary 8 s, not the countdown:
		// it is the time to press the key and say the word.
		out.action.needsConfirmWord = true;
		return out;
	}
	// The countdown: shown for countdownSec and then sent, unless cancelled.
	out.action.needsConfirmWord = false;
	out.action.runOnTimeout = true;
	out.action.timeoutMs = static_cast<int64_t>(send.countdownSec * 1000.0);
	return out;
}

Interpretation Ignored()
{
	Interpretation out;
	out.kind = Interpretation::Kind::Ignored;
	return out;
}

Interpretation ControlOf(Interpretation::Control control)
{
	Interpretation out;
	out.kind = Interpretation::Kind::Control;
	out.control = control;
	return out;
}

std::string Bound(obs_source_t *source)
{
	const char *name = source ? obs_source_get_name(source) : nullptr;
	return name ? std::string(name) : std::string();
}

// The scene on program, addref'd or null: "show webcam" means the webcam in the scene
// the viewers are looking at.
obs_source_t *ProgramScene()
{
	return Transitions::GetProgramScene();
}

// The items of the program scene, top level only (an item inside a group is not
// listed, and obs_scene_find_source would not find it either).
std::vector<std::string> ProgramSceneItems()
{
	std::vector<std::string> names;
	OBSSourceAutoRelease program = ProgramScene();
	obs_scene_t *scene = program ? obs_scene_from_source(program) : nullptr;
	if (!scene) {
		return names;
	}
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *out = static_cast<std::vector<std::string> *>(param);
			const std::string name = Bound(obs_sceneitem_get_source(item));
			if (!name.empty()) {
				out->push_back(name);
			}
			return true;
		},
		&names);
	return names;
}

// The source bound to one of the global audio channels under exactly `name`, addref'd
// or null. The channels are what the matcher offered, so they are what it resolves in.
obs_source_t *ChannelSourceNamed(const std::string &name)
{
	for (const GlobalAudioChannels::Slot &slot : GlobalAudioChannels::Slots()) {
		obs_source_t *bound = obs_get_output_source(static_cast<uint32_t>(slot.channel)); // addref'd
		if (bound && Bound(bound) == name) {
			return bound;
		}
		obs_source_release(bound);
	}
	return nullptr;
}

// A pending action's summary ends in a question mark ("Stop streaming?"); the report of
// having done it does not.
std::string Done(const std::string &summary)
{
	if (!summary.empty() && summary.back() == '?') {
		return summary.substr(0, summary.size() - 1);
	}
	return summary;
}

// Posts a chat.send envelope. A reply goes to its one destination or fails out loud: the
// chat it was addressed to may have ended since it was drafted, and falling back to the
// platform would post it into every broadcast on it (index revision 6). A message to
// chats goes only if one of them is still live.
void RunChat(const PendingAction &action, const std::function<void(bool, std::string)> &finish)
{
	const nlohmann::json &params = action.params;
	const std::string text = params.value("text", std::string());
	if (text.empty()) {
		finish(false, "There was nothing to send.");
		return;
	}
	const std::string replyTo = params.value("replyTo", std::string());
	const std::string accountId = params.value("accountId", std::string());
	if (!accountId.empty()) {
		const OAuth::DestinationId dest{accountId, params.value("profileUuid", std::string())};
		// False when no live transport holds that exact destination: the broadcast
		// ended or its chat dropped. Nothing was sent.
		if (!Chat::Hub().SendToDestination(dest, text)) {
			finish(false, replyTo.empty() ? "That chat is no longer live, so nothing was sent."
						      : "The chat " + replyTo +
								" spoke in is no longer live, so the "
								"reply was not sent.");
			return;
		}
		finish(true, replyTo.empty() ? "Sent" : "Replied to " + replyTo);
		return;
	}
	if (!replyTo.empty()) {
		// A reply with no address: never widen it to the platform.
		finish(false, "I have lost track of where " + replyTo + " said that.");
		return;
	}

	std::vector<std::string> platforms;
	const auto listed = params.find("platforms");
	if (listed != params.end() && listed->is_array()) {
		for (const nlohmann::json &entry : *listed) {
			if (entry.is_string()) {
				platforms.push_back(entry.get<std::string>());
			}
		}
	}
	bool live = false;
	for (const nlohmann::json &row : Chat::Hub().State()) {
		const std::string platform = row.value("platform", std::string());
		live = live || platforms.empty() ||
		       std::find(platforms.begin(), platforms.end(), platform) != platforms.end();
	}
	if (!live) {
		finish(false, "No chat is live any more, so nothing was sent.");
		return;
	}
	// Fire and forget: "Sent" here means handed to each chat's transport, not delivered.
	// A send that fails later reports on that chat in the Multichat dock (chat.state),
	// which is where the user would look for it.
	Chat::Hub().SendToPlatforms(platforms, text);
	finish(true, "Sent to chat");
}

} // namespace

CommandCandidates CurrentCandidates()
{
	CommandCandidates candidates;

	// obs_enum_scenes yields the main canvas's scenes only, which is what a spoken scene
	// switch means: scenes.setCurrent without a canvas switches the Default program.
	obs_enum_scenes(
		[](void *param, obs_source_t *source) {
			auto *out = static_cast<std::vector<std::string> *>(param);
			const std::string name = Bound(source);
			if (!name.empty()) {
				out->push_back(name);
			}
			return true;
		},
		&candidates.scenes);

	candidates.sources = ProgramSceneItems();

	for (const GlobalAudioChannels::Slot &slot : GlobalAudioChannels::Slots()) {
		OBSSourceAutoRelease bound = obs_get_output_source(static_cast<uint32_t>(slot.channel));
		const std::string name = Bound(bound);
		if (!name.empty()) {
			candidates.audioSources.push_back(name);
		}
	}
	OBSSourceAutoRelease mic = obs_get_output_source(GlobalAudio::PrimaryMicChannel());
	candidates.micSource = Bound(mic);

	// The people who have spoken lately, for "reply to ...", one per name (the latest
	// speaker under it), with the chat each last spoke in.
	for (const Chat::Chatter &chatter : Chat::DistinctByName(Chat::Chatters().Recent(TimeUtil::NowMs()))) {
		candidates.people.push_back(chatter.displayName);
		candidates.replyRoutes.push_back({chatter.platform, chatter.accountId, chatter.profileUuid});
	}
	// The platforms with a connected chat, for "send to twitch ..." and for where a
	// message to everyone goes. ChatHub::State is one row per live destination.
	for (const nlohmann::json &row : Chat::Hub().State()) {
		const std::string platform = row.value("platform", std::string());
		if (!platform.empty() && row.value("connected", false) &&
		    std::find(candidates.platforms.begin(), candidates.platforms.end(), platform) ==
			    candidates.platforms.end()) {
			candidates.platforms.push_back(platform);
		}
	}
	return candidates;
}

std::string PromptBiasFor(const CommandCandidates &candidates)
{
	// Least important first. A name that appears twice keeps its later (more important)
	// place.
	std::vector<std::string> parts = CommandPhrases();
	// The most recent chatters, so an odd handle comes back spelled the way chat spells
	// it; oldest of them first, so the newest outlast a trim. Capped, so a busy chat
	// cannot push the command phrases out.
	const size_t people = std::min(kPromptPeople, candidates.people.size());
	parts.insert(parts.end(), candidates.people.rend() - static_cast<std::ptrdiff_t>(people),
		     candidates.people.rend());
	for (const std::vector<std::string> *list :
	     {&candidates.audioSources, &candidates.sources, &candidates.scenes}) {
		parts.insert(parts.end(), list->begin(), list->end());
	}

	// Walk from the most important end and stop at the first part that no longer fits,
	// so what is dropped is always a prefix: the same cut whisper itself would make.
	std::vector<const std::string *> kept;
	std::set<std::string> seen;
	size_t size = 0;
	for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
		if (it->empty() || !seen.insert(*it).second) {
			continue;
		}
		const size_t added = it->size() + (kept.empty() ? 0 : 2);
		if (size + added > kMaxPromptChars) {
			break;
		}
		size += added;
		kept.push_back(&*it);
	}

	std::string prompt;
	prompt.reserve(size);
	for (auto it = kept.rbegin(); it != kept.rend(); ++it) {
		if (!prompt.empty()) {
			prompt += ". ";
		}
		prompt += **it;
	}
	return prompt;
}

std::string PromptBias()
{
	return PromptBiasFor(CurrentCandidates());
}

Interpretation Interpret(const std::string &text, const InterpretContext &ctx, const CommandCandidates &candidates,
			 const SendPolicy &send)
{
	const Normalized said = Normalize(text);

	// A mic the user muted means "not now", and in always-listen mode that is honoured:
	// nothing they say is acted on, or even answered with a cue, except unmuting the mic.
	// In push-to-talk the key press is the user deciding to be heard, so a mic they had
	// muted is listened to normally (the spec: "nothing changes"). pttMuted -- our own
	// push-to-talk mute -- never blocks anything.
	const bool muted = ctx.mutedSeen && ctx.trigger == Trigger::Wake;
	const CommandMatch match = MatchCommand(said, candidates);
	if (muted && !(match.ok && IsAllowedWhileMuted(match, candidates))) {
		DBG(LogCat::Voice, "ignored %s spoken while the mic was muted",
		    match.ok ? match.commandId.c_str() : "an utterance");
		return Ignored();
	}

	if (said.tokens.empty()) {
		return Miss("I did not hear anything.");
	}

	// One pending action at a time, and while one waits a new utterance can only answer
	// it: anything else is a miss that leaves it pending (the spec's state machine). The
	// window is short, and the user can still cancel and say the other command.
	if (ctx.pending) {
		if (Matches(said.text, kConfirmWords)) {
			return ControlOf(Interpretation::Control::ConfirmPending);
		}
		if (Matches(said.text, kCancelWords)) {
			return ControlOf(Interpretation::Control::CancelPending);
		}
		return Miss(IsChatDraft(*ctx.pending) ? "Say send to post it, or cancel."
						      : "Say yes to " + Done(ctx.pending->summary) + ", or cancel.");
	}

	if (!match.ok) {
		// The spec's fallback, push-to-talk only: an utterance no command claimed is a
		// message to every live chat (hold the key, speak, done). Not in always-listen,
		// where ambient speech must never reach chat; not a lone yes or cancel with
		// nothing waiting; and with no chat live it is the ordinary miss, so a studio with
		// no chat hears "I did not catch a command" rather than a lecture about chat.
		const bool controlWord = Matches(said.text, kConfirmWords) || Matches(said.text, kCancelWords);
		if (match.commandId.empty() && ctx.trigger == Trigger::Ptt && !controlWord &&
		    !candidates.platforms.empty()) {
			DBG(LogCat::Voice, "no command matched; dictating to chat");
			CommandMatch dictated;
			dictated.ok = true;
			dictated.commandId = kChatMethod;
			dictated.messageText = said.Original(0, said.tokens.size() - 1);
			return ChatDraft(dictated, candidates, send);
		}
		DBG(LogCat::Voice, "no command matched%s", match.ambiguous ? " (ambiguous slot)" : "");
		return Miss(match.message.empty() ? "I did not catch a command." : match.message);
	}
	DBG(LogCat::Voice, "matched %s", match.commandId.c_str());
	if (match.commandId == kChatMethod) {
		return ChatDraft(match, candidates, send);
	}

	Interpretation out;
	out.action.commandId = match.commandId;
	out.action.summary = match.summary;
	out.action.needsConfirmWord = match.needsConfirm;
	for (const SlotParam &param : kSlotParams) {
		if (param.slot == match.slot) {
			out.action.params[param.key] = match.slotValue;
		}
	}
	if (match.flagKey) {
		out.action.params[match.flagKey] = match.flagValue;
	}
	if (match.needsConfirm) {
		out.kind = Interpretation::Kind::Pending;
		out.action.summary = match.summary + "?";
	} else {
		out.kind = Interpretation::Kind::Instant;
	}
	return out;
}

Interpretation InterpretTranscript(const std::string &text, const InterpretContext &ctx)
{
	const VoiceSettings &settings = Engine().Settings();
	return Interpret(text, ctx, CurrentCandidates(), SendPolicy{settings.sendMode, settings.countdownSec});
}

void RunCommand(const PendingAction &action, const std::function<void(bool ok, std::string message)> &done)
{
	auto finish = [&done](bool ok, std::string message) {
		if (done) {
			done(ok, std::move(message));
		}
	};
	const std::string &id = action.commandId;
	std::string error;
	nlohmann::json result;

	// The typed branches below survive only because they say something better than a
	// generic error does -- "There is no scene called BRB." beats "no scene named
	// 'BRB'" -- and because setVisible needs a name-to-id resolution. Adding a command
	// is a row in kPatterns; it is not a branch here.
	if (id == "scenes.setCurrent") {
		const std::string name = action.params.value("name", "");
		if (!Bridge::Dispatch(id, action.params, result, error)) {
			finish(false, error.rfind("no scene named", 0) == 0 ? "There is no scene called " + name + "."
									    : error);
			return;
		}
		finish(true, "Switched to " + name);
		return;
	}

	// sceneItems.setVisible is the one command whose params the matcher cannot finish:
	// the bridge addresses a scene ITEM by numeric id (ResolveParamsItem ->
	// ItemIdFromParams), and only a libobs lookup turns the spoken source name into
	// one. So this branch is the resolver, not just a caller. Any other front end
	// producing this action -- a future non-human interpreter included -- must either
	// supply an "id" itself (it then goes through Dispatch below) or come through here.
	if (id == "sceneItems.setVisible" && !action.params.contains("id")) {
		const std::string name = action.params.value("source", "");
		const bool visible = action.params.value("visible", false);
		OBSSourceAutoRelease program = ProgramScene();
		obs_scene_t *scene = program ? obs_scene_from_source(program) : nullptr;
		obs_sceneitem_t *item = scene ? obs_scene_find_source(scene, name.c_str()) : nullptr;
		if (!item) {
			finish(false, "There is no source called " + name + " in this scene.");
			return;
		}
		const nlohmann::json itemParams = {{"scene", Bound(program)}, {"id", obs_sceneitem_get_id(item)}};
		if (!Bridge::SetSceneItemVisible(itemParams, visible, error)) {
			finish(false, error);
			return;
		}
		finish(true, (visible ? "Showing " : "Hiding ") + name);
		return;
	}

	if (id == "audio.setMuted") {
		const std::string name = action.params.value("source", "");
		OBSSourceAutoRelease target = ChannelSourceNamed(name);
		if (!target) {
			finish(false, "There is no audio source called " + name + ".");
			return;
		}
		const bool mute = action.params.value("muted", false);
		if (!Bridge::SetSourceMuted(target, mute, error)) {
			finish(false, error);
			return;
		}
		finish(true, (mute ? "Muted " : "Unmuted ") + name);
		return;
	}

	// chat.send is async-only in the bridge (g_asyncMethods), so Dispatch cannot reach it;
	// this is the same routing MethodChatSend does, with voice's own wording.
	if (id == kChatMethod) {
		RunChat(action, finish);
		return;
	}

	// Everything else, streaming.start and streaming.stop included: dispatch by name.
	// commandId IS the bridge method name and params is already the bridge envelope, so
	// this reaches the whole g_methods registry rather than the handful spelled out
	// above. The two lifecycle commands deliberately come this way rather than through
	// Bridge::StartStreamingAllAdoptingSchedule / StopStreamingAll: those return void,
	// so a refusal (a go-live already starting) would be reported as success, and these
	// are the two confirm-gated commands, where a silent "Started" after nothing started
	// is worst. If a typed branch is ever added for them, it must report the failure.
	if (!Bridge::Dispatch(id, action.params, result, error)) {
		finish(false, error.empty() ? "I could not do that." : error);
		return;
	}
	finish(true, Done(action.summary));
}

void InstallCommands()
{
	Engine().SetInterpreter(&InterpretTranscript);
	Engine().SetActionRunner([](const PendingAction &action, std::function<void(bool, std::string)> done) {
		RunCommand(action, done);
	});
	Engine().SetPromptSource(&PromptBias);
}

} // namespace Voice
