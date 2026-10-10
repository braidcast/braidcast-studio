#include "voice/VoiceListener.hpp"

#include <cstddef>
#include <utility>

namespace Voice {

namespace {

constexpr const char *kCueNames[] = {"start", "accept", "reject", "pending", "cancel"};

// Why a pending action went away when its window closed.
constexpr const char *kTimedOut = "The command timed out.";
constexpr const char *kDraftExpired = "The draft expired and was not sent; its text is kept.";
constexpr const char *kStateNames[] = {"disabled", "notReady", "idle", "listening", "thinking", "pending"};

Effect CueEffect(Cue cue)
{
	Effect e;
	e.type = EffectType::PlayCue;
	e.cue = cue;
	return e;
}

Effect ReadBackEffect(std::string text)
{
	Effect e;
	e.type = EffectType::ReadBack;
	e.text = std::move(text);
	return e;
}

// A pending action's summary asks ("Stop streaming?"); once it has been confirmed, the
// spoken confirmation says it ("Stop streaming").
std::string Said(const std::string &summary)
{
	return !summary.empty() && summary.back() == '?' ? summary.substr(0, summary.size() - 1) : summary;
}

// A pending action's "text" parameter (a chat draft's message), or "".
std::string TextParam(const nlohmann::json &params)
{
	const auto it = params.is_object() ? params.find("text") : params.end();
	return it != params.end() && it->is_string() ? it->get<std::string>() : std::string();
}

} // namespace

const char *CueName(Cue cue)
{
	return kCueNames[static_cast<size_t>(cue)];
}

const char *StateName(State state)
{
	return kStateNames[static_cast<size_t>(state)];
}

VoiceListener::VoiceListener(Interpreter interpret) : interpret_(std::move(interpret)) {}

bool VoiceListener::Ready() const
{
	return enabled_ && modelReady_ && micReady_;
}

void VoiceListener::ClearPending()
{
	status_.pending = PendingAction{};
}

std::vector<Effect> VoiceListener::BeginSegment(int64_t, Trigger trigger)
{
	trigger_ = trigger;
	status_.state = State::Listening;
	status_.transcript.clear();
	status_.keptDraft.clear();
	return {CueEffect(Cue::Start)};
}

std::vector<Effect> VoiceListener::EndSegment()
{
	status_.state = State::Thinking;
	return {};
}

std::vector<Effect> VoiceListener::ApplyInterpretation(const Interpretation &interpretation, int64_t nowMs)
{
	std::vector<Effect> effects;
	switch (interpretation.kind) {
	case Interpretation::Kind::Ignored:
		status_.state = status_.pending.commandId.empty() ? State::Idle : State::Pending;
		return effects;

	case Interpretation::Kind::Miss:
	case Interpretation::Kind::Shown:
		status_.message = interpretation.message;
		status_.keptDraft = interpretation.keptDraft;
		status_.state = status_.pending.commandId.empty() ? State::Idle : State::Pending;
		if (interpretation.kind == Interpretation::Kind::Miss) {
			effects.push_back(CueEffect(Cue::Reject));
		}
		return effects;

	case Interpretation::Kind::Instant: {
		ClearPending();
		status_.message = interpretation.action.summary;
		status_.state = State::Idle;
		Effect run;
		run.type = EffectType::Run;
		run.action = interpretation.action;
		effects.push_back(run);
		effects.push_back(CueEffect(Cue::Accept));
		if (interpretation.action.readBack) {
			effects.push_back(ReadBackEffect(interpretation.action.summary));
		}
		return effects;
	}

	case Interpretation::Kind::Pending: {
		status_.pending = interpretation.action;
		status_.pending.id = ++pendingSeq_;
		status_.message = interpretation.action.summary;
		status_.state = State::Pending;
		effects.push_back(CueEffect(Cue::Pending));
		Effect tick;
		tick.type = EffectType::ScheduleTick;
		tick.atMs = nowMs +
			    (interpretation.action.timeoutMs > 0 ? interpretation.action.timeoutMs : kPendingTimeoutMs);
		tick.seq = ++tickSeq_;
		// One instant, written down once: the tick fires at it and the UI counts down to
		// it. Two independent notions of "when this expires" is how a countdown and a
		// timer drift apart.
		status_.pending.deadlineMs = tick.atMs;
		effects.push_back(tick);
		return effects;
	}

	case Interpretation::Kind::Control:
		if (interpretation.control == Interpretation::Control::ReadBackPending) {
			// Asked for, so spoken whatever the read-back setting; the command keeps
			// waiting, on the same deadline. Nothing pending: nothing to read.
			if (status_.pending.commandId.empty()) {
				status_.state = State::Idle;
				effects.push_back(CueEffect(Cue::Reject));
				return effects;
			}
			status_.state = State::Pending;
			effects.push_back(ReadBackEffect(status_.pending.summary));
			return effects;
		}
		if (interpretation.control == Interpretation::Control::ConfirmPending &&
		    !status_.pending.commandId.empty()) {
			Effect run;
			run.type = EffectType::Run;
			run.action = status_.pending;
			const bool readBack = status_.pending.readBack;
			const std::string summary = status_.pending.summary;
			ClearPending();
			status_.message = summary;
			status_.state = State::Idle;
			effects.push_back(run);
			effects.push_back(CueEffect(Cue::Accept));
			if (readBack) {
				effects.push_back(ReadBackEffect(Said(summary)));
			}
			return effects;
		}
		// Cancel, and a confirmation with nothing pending, both end up here.
		ClearPending();
		status_.message.clear();
		status_.state = State::Idle;
		effects.push_back(CueEffect(Cue::Cancel));
		return effects;
	}
	return effects;
}

std::vector<Effect> VoiceListener::Handle(const Event &event)
{
	std::vector<Effect> effects;

	switch (event.type) {
	case EventType::Enable:
		enabled_ = true;
		status_.state = Ready() ? State::Idle : State::NotReady;
		return effects;

	case EventType::Disable:
		// Disabling tears the engine's runtime down: the model is unloaded and the mic
		// unbound, so a later Enable waits for both to be reported again rather than
		// claiming Idle on the old runtime's word.
		enabled_ = false;
		modelReady_ = false;
		micReady_ = false;
		ClearPending();
		status_.state = State::Disabled;
		status_.transcript.clear();
		status_.keptDraft.clear();
		return effects;

	case EventType::ModelReady:
		modelReady_ = true;
		if (enabled_ && status_.state == State::NotReady) {
			status_.state = Ready() ? State::Idle : State::NotReady;
			status_.message.clear();
		}
		return effects;

	case EventType::ModelFailed:
		modelReady_ = false;
		status_.message = event.text;
		if (enabled_) {
			status_.state = State::NotReady;
		}
		return effects;

	case EventType::MicBound:
		micReady_ = true;
		if (enabled_ && status_.state == State::NotReady) {
			status_.state = Ready() ? State::Idle : State::NotReady;
		}
		return effects;

	case EventType::MicLost:
		micReady_ = false;
		if (!enabled_) {
			return effects;
		}
		if (status_.state == State::Listening) {
			effects.push_back(CueEffect(Cue::Reject));
		}
		ClearPending();
		status_.state = State::NotReady;
		return effects;

	case EventType::PttDown:
	case EventType::WakeMatched:
		if (!Ready()) {
			return effects;
		}
		if (!event.ok) {
			// A press the engine refused because a segment is still in flight (the
			// recognizer is busy) leaves that segment alone: moving to Idle here would
			// make its transcript arrive to a listener no longer waiting for it.
			if (status_.state == State::Listening || status_.state == State::Thinking) {
				return effects;
			}
			status_.message = event.text;
			status_.state = status_.pending.commandId.empty() ? State::Idle : State::Pending;
			effects.push_back(CueEffect(Cue::Reject));
			return effects;
		}
		// Only Idle and Pending accept a new segment: a press while Listening or
		// Thinking is refused rather than queued, so one key press is one command.
		if (status_.state != State::Idle && status_.state != State::Pending) {
			return effects;
		}
		return BeginSegment(event.nowMs, event.type == EventType::PttDown ? Trigger::Ptt : Trigger::Wake);

	case EventType::PttUp:
	case EventType::SpeechEnded:
		if (status_.state != State::Listening) {
			return effects;
		}
		return EndSegment();

	case EventType::Transcript: {
		if (status_.state != State::Thinking) {
			return effects;
		}
		status_.transcript = event.text;
		InterpretContext ctx;
		ctx.pending = status_.pending.commandId.empty() ? nullptr : &status_.pending;
		ctx.trigger = trigger_;
		ctx.mutedSeen = event.mutedSeen;
		ctx.pttMuted = event.pttMuted;
		return ApplyInterpretation(interpret_(event.text, ctx), event.nowMs);
	}

	case EventType::TranscribeFailed:
		if (status_.state != State::Thinking) {
			return effects;
		}
		status_.message = event.text;
		status_.state = status_.pending.commandId.empty() ? State::Idle : State::Pending;
		effects.push_back(CueEffect(Cue::Reject));
		return effects;

	case EventType::ActionResult:
		status_.message = event.text;
		if (!event.ok) {
			effects.push_back(CueEffect(Cue::Reject));
		}
		return effects;

	case EventType::Confirm: {
		// The UI's Confirm button: the same action as a spoken yes, so it goes through the
		// same code. Nothing pending, or another action pending than the one the click was
		// for (it expired and something else is waiting now), nothing to do.
		if (status_.pending.commandId.empty() || event.pendingId != status_.pending.id) {
			return effects;
		}
		Interpretation confirm;
		confirm.kind = Interpretation::Kind::Control;
		confirm.control = Interpretation::Control::ConfirmPending;
		const State segment = status_.state;
		effects = ApplyInterpretation(confirm, event.nowMs);
		// A click while the key is held (or while the answer is being recognized) leaves
		// that segment running, as a timeout does: its transcript then finds nothing
		// pending.
		if (segment == State::Listening || segment == State::Thinking) {
			status_.state = segment;
		}
		return effects;
	}

	case EventType::Cancel:
		// The cancel hotkey: abandon a segment being recorded, a pending command, or
		// both. Silent when there was nothing to cancel, so a stray press is not noise.
		// A UI Cancel naming a pending action that is no longer the one waiting does
		// nothing: the click was for something already gone.
		if (event.pendingId != 0 && event.pendingId != status_.pending.id) {
			return effects;
		}
		if (status_.state == State::Listening || status_.state == State::Thinking ||
		    !status_.pending.commandId.empty()) {
			effects.push_back(CueEffect(Cue::Cancel));
		}
		ClearPending();
		status_.transcript.clear();
		status_.message.clear();
		status_.keptDraft.clear();
		if (status_.state != State::Disabled && status_.state != State::NotReady) {
			status_.state = State::Idle;
		}
		return effects;

	case EventType::Tick:
		// Only the newest scheduled timeout counts: an older one refers to a command
		// that was already confirmed, cancelled or replaced.
		if (status_.pending.commandId.empty() || event.seq != tickSeq_) {
			return effects;
		}
		// The deadline holds even mid-segment (the user pressed the key to answer just as
		// it ran out). Ignoring the tick there would leave the command pending with no
		// timer at all once that segment missed; instead it expires now, the segment
		// carries on, and its answer finds nothing pending.
		if (status_.pending.runOnTimeout) {
			// A chat draft: the window closing is the send, not the cancel. Exactly what
			// confirming does, segment kept as the UI's Confirm keeps it; the Escape key
			// and Cancel are the way to stop it before then.
			Interpretation send;
			send.kind = Interpretation::Kind::Control;
			send.control = Interpretation::Control::ConfirmPending;
			const State segment = status_.state;
			effects = ApplyInterpretation(send, event.nowMs);
			if (segment == State::Listening || segment == State::Thinking) {
				status_.state = segment;
			}
			return effects;
		}
		{
			// A chat draft waiting for "send" (the say send mode) is not lost when its window
			// closes, and not sent either: the text is kept for the composer, as a message
			// refused as too long is, and the reason says so. Silently dropping it is what
			// the "say send" wording, which promises the message is held, cannot survive.
			const std::string draft = TextParam(status_.pending.params);
			ClearPending();
			status_.message = draft.empty() ? kTimedOut : kDraftExpired;
			if (!draft.empty()) {
				status_.keptDraft = draft;
			}
		}
		if (status_.state == State::Pending) {
			status_.state = State::Idle;
		}
		effects.push_back(CueEffect(Cue::Cancel));
		return effects;
	}
	return effects;
}

nlohmann::json VoiceListener::StatusJson() const
{
	nlohmann::json j = {
		{"state", StateName(status_.state)}, {"message", status_.message},
		{"transcript", status_.transcript},  {"device", status_.device},
		{"keptDraft", status_.keptDraft},
	};
	if (!status_.pending.commandId.empty()) {
		j["pending"] = {
			{"id", status_.pending.id},
			{"commandId", status_.pending.commandId},
			{"summary", status_.pending.summary},
			{"needsConfirmWord", status_.pending.needsConfirmWord},
			// The instant, not a duration. VoiceEngine::PublishState turns it into the
			// remainingMs the UI actually renders -- this class has no clock, by design.
			{"deadlineMs", status_.pending.deadlineMs},
			// A chat draft: whether the window closing sends it, how long the window is,
			// and the message as it will be posted, for the Multichat composer. Shown,
			// never logged.
			{"runOnTimeout", status_.pending.runOnTimeout},
			// The window as it applies, the default one included, so the UI can show a time
			// limit for every pending action, not only for a countdown.
			{"timeoutMs", status_.pending.timeoutMs > 0 ? status_.pending.timeoutMs : kPendingTimeoutMs},
			{"text", TextParam(status_.pending.params)},
		};
	} else {
		j["pending"] = nullptr;
	}
	return j;
}

} // namespace Voice
