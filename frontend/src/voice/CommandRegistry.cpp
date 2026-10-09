#include "voice/CommandRegistry.hpp"

#include "bridge.hpp"
#include "log.hpp"
#include "multistream/GlobalAudioChannels.hpp"
#include "scene/transitions.hpp"
#include "voice/TextNormalize.hpp"
#include "voice/VoiceEngine.hpp"

#include <obs.hpp>

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
	return candidates;
}

std::string PromptBiasFor(const CommandCandidates &candidates)
{
	// Least important first. A name that appears twice keeps its later (more important)
	// place.
	std::vector<std::string> parts = CommandPhrases();
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

Interpretation Interpret(const std::string &text, const InterpretContext &ctx, const CommandCandidates &candidates)
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
		return Miss("Say yes to " + Done(ctx.pending->summary) + ", or cancel.");
	}

	if (!match.ok) {
		DBG(LogCat::Voice, "no command matched%s", match.ambiguous ? " (ambiguous slot)" : "");
		return Miss(match.message.empty() ? "I did not catch a command." : match.message);
	}
	DBG(LogCat::Voice, "matched %s", match.commandId.c_str());

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
	return Interpret(text, ctx, CurrentCandidates());
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
