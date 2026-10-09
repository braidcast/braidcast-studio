#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_LISTENER_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_LISTENER_HPP_

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Voice {

enum class State {
	Disabled,  // the feature is off
	NotReady,  // on, but the model or the microphone is missing
	Idle,      // ready and waiting for the key
	Listening, // the key is held (or the wake word fired) and audio is being captured
	Thinking,  // the segment is closed and whisper is running
	Pending,   // a command is waiting for confirmation
};

enum class Cue { Start, Accept, Reject, Pending, Cancel };
const char *CueName(Cue cue);
const char *StateName(State state);

// A command the interpreter matched, ready to run.
struct PendingAction {
	// A BRIDGE METHOD NAME, exactly as g_methods spells it -- "scenes.setCurrent", not a
	// voice-private "scene.switch". With params below already in the bridge's own key
	// names, a PendingAction IS a {method, params} envelope, which is what lets
	// RunCommand fall through to Bridge::Dispatch and reach the whole registry. Keep it
	// that way: a second id namespace means a translation table nobody maintains.
	std::string commandId;
	std::string summary; // one line for the UI and the confirmation prompt
	nlohmann::json params = nlohmann::json::object();
	bool readBack = false;        // speak the summary back (P4)
	bool needsConfirmWord = true; // requires a spoken yes rather than a second key press
	// ABSOLUTE host wall clock, not a duration. The window must be measured from a fixed
	// instant: anything that delays the pending state after the transcript was stamped
	// (today a slow UI-thread hop, later an interpreter that has to ask something) eats
	// into the window, and a duration restarted on arrival would hide exactly that. The
	// UI is told what is LEFT, never how long the window was. 0 when nothing is pending.
	int64_t deadlineMs = 0;
	// True for a chat draft in the countdown send mode: when its window closes it is SENT,
	// where an ordinary pending command is dropped. This is the only difference between
	// the two, so a draft needs no second timer and no second pending slot.
	bool runOnTimeout = false;
	// How long the window is; 0 uses VoiceListener::kPendingTimeoutMs. The deadline above
	// is computed from it once, when the action becomes pending.
	int64_t timeoutMs = 0;
};

// What a transcript meant. P1 always answers Shown; P2 supplies the real interpreter.
struct Interpretation {
	enum class Kind {
		Miss,    // nothing matched: say so
		Shown,   // display the text, take no action (P1, and dictation preview)
		Instant, // run it now
		Pending, // ask for confirmation first
		Control, // yes / cancel, acting on the pending command
		Ignored, // deliberately silent (background speech, self-talk)
	};
	enum class Control { None, ConfirmPending, CancelPending };

	Kind kind = Kind::Miss;
	Control control = Control::None;
	std::string message; // shown to the user when Miss or Shown
	PendingAction action;
	// Miss only: a chat message refused before it became a draft because it was too long.
	// It is not sent and not lost: the Multichat composer takes it over to edit (the spec's
	// "too long" row, draft kept).
	std::string keptDraft;
};

enum class Trigger { Ptt, Wake };

struct InterpretContext {
	const PendingAction *pending = nullptr; // non-null while a command awaits confirmation
	Trigger trigger = Trigger::Ptt;
	// The mic was muted by the USER during the segment. Never our own push-to-talk mute:
	// libobs reports that as muted too, so the engine leaves it out (see pttMuted).
	bool mutedSeen = false;
	// Our push-to-talk mute (MicMuteGuard) held the mic for this segment. A mute the user
	// made during the hold cannot be seen under it, so mutedSeen is then false.
	bool pttMuted = false;
};

// std::function, not a raw function pointer, and deliberately: ActionRunner beside it is
// already one, and a resolver that owns anything (a client, a cache, a key) cannot be a
// capturing lambda otherwise -- it would need a file-scope singleton instead. One
// allocation, once, in VoiceEngine::SetInterpreter.
using Interpreter = std::function<Interpretation(const std::string &text, const InterpretContext &ctx)>;

enum class EventType {
	Enable,
	Disable,
	ModelReady,
	ModelFailed,
	MicBound,
	MicLost,
	PttDown,
	PttUp,
	WakeMatched,
	SpeechEnded,
	Transcript,
	TranscribeFailed,
	ActionResult,
	Confirm, // the UI's Confirm button: the same as a spoken yes
	Cancel,  // the cancel hotkey or the UI's Cancel button: drop whatever is in flight
	Tick,
};

struct Event {
	EventType type = EventType::Tick;
	std::string text;       // transcript, or the reason for a failure
	bool ok = true;         // PttDown accepted / ActionResult succeeded
	bool mutedSeen = false; // Transcript: as InterpretContext::mutedSeen
	bool pttMuted = false;  // Transcript: as InterpretContext::pttMuted
	int64_t nowMs = 0;
	uint64_t seq = 0; // Tick only: which scheduled tick this is
};

enum class EffectType { PlayCue, Run, ReadBack, ScheduleTick };

struct Effect {
	EffectType type = EffectType::PlayCue;
	Cue cue = Cue::Start;
	PendingAction action; // Run
	std::string text;     // ReadBack
	int64_t atMs = 0;     // ScheduleTick: the absolute time to fire
	uint64_t seq = 0;     // ScheduleTick: the sequence to send back
};

struct Status {
	State state = State::Disabled;
	std::string message;    // the last thing worth showing the user
	std::string transcript; // the last recognized text (never logged)
	PendingAction pending;  // commandId empty when nothing is pending
	std::string device;     // set by the engine, not the listener
	// The last chat message refused as too long (Interpretation::keptDraft), until the
	// next segment, a cancel or turning voice off. Shown, never logged.
	std::string keptDraft;
};

// Pure: no clock, no threads, no libobs, no I/O. Every input is an Event carrying its
// own timestamp, and every output is an Effect for the caller to perform. This is what
// makes the whole interaction testable in a headless smoke run.
class VoiceListener {
public:
	// How long a command waits for a spoken confirmation before it is dropped, unless the
	// action names its own window (PendingAction::timeoutMs).
	static constexpr int64_t kPendingTimeoutMs = 8000;

	explicit VoiceListener(Interpreter interpret);

	std::vector<Effect> Handle(const Event &event);

	State Current() const { return status_.state; }
	const Status &Snapshot() const { return status_; }
	nlohmann::json StatusJson() const;

private:
	void ClearPending();
	std::vector<Effect> BeginSegment(int64_t nowMs, Trigger trigger);
	std::vector<Effect> EndSegment();
	std::vector<Effect> ApplyInterpretation(const Interpretation &interpretation, int64_t nowMs);
	bool Ready() const;

	Interpreter interpret_;
	Status status_;
	bool enabled_ = false;
	bool modelReady_ = false;
	bool micReady_ = false;
	Trigger trigger_ = Trigger::Ptt;
	uint64_t tickSeq_ = 0; // incremented per scheduled timeout; stale ticks are ignored
};

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_LISTENER_HPP_
