#include "voice/VoiceEngine.hpp"

#include "bridge.hpp"
#include "event_names.hpp"
#include "log.hpp"
#include "multistream/GlobalAudioChannels.hpp"
#include "util/async_task.hpp"
#include "util/time_util.hpp"
#include "voice/Recognizer.hpp"
#include "voice/Tts.hpp"
#include "voice/VoiceCapture.hpp"
#include "voice/VoiceCpu.hpp"
#include "voice/VoiceModels.hpp"
#include "voice/VoiceP0Defaults.hpp"
#include "voice/VoiceResampler.hpp"
#include "voice/VoiceRing.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

#include <windows.h>

namespace Voice {

namespace {

// The process that owns the foreground window, or 0 when no window is in front.
uint32_t ForegroundProcessId()
{
	HWND foreground = GetForegroundWindow();
	DWORD pid = 0;
	if (foreground) {
		GetWindowThreadProcessId(foreground, &pid);
	}
	return static_cast<uint32_t>(pid);
}

// What the ring has to hold is what arrives while the worker is busy in whisper. With
// push-to-talk that audio is not wanted anyway, but always-listen hears all of it: a
// wake check and a transcription take seconds, and a ring that overflowed meanwhile
// would splice the stream the voice activity detector is following. Ten seconds is
// 640 KB.
constexpr size_t kRingSamples = static_cast<size_t>(kVoiceSampleRate) * 10;

// The wake phrase needs a model of its own and the detector; without both, always-listen
// cannot run and push-to-talk carries on.
constexpr const char *kWakeModelsMissing =
	"Always-listen needs the Tiny (English) model too. Download it in Settings, Voice; push-to-talk works "
	"meanwhile.";
constexpr const char *kWakeDidNotStart = "Always-listen could not start; push-to-talk still works.";

// The interpreter until one is installed: show what was heard, act on nothing. The
// command registry (Voice::InstallCommands) replaces it at startup.
Interpretation ShowOnly(const std::string &text, const InterpretContext &)
{
	Interpretation out;
	out.kind = text.empty() ? Interpretation::Kind::Miss : Interpretation::Kind::Shown;
	out.message = text.empty() ? "I did not hear anything." : text;
	return out;
}

Event MakeEvent(EventType type)
{
	Event event;
	event.type = type;
	event.nowMs = TimeUtil::NowMs();
	return event;
}

} // namespace

// Everything that exists only while voice is enabled. Members are destroyed bottom-up:
// the recognizer (the ring's consumer) first, then the capture (its producer, unbound
// on the UI thread before the runtime is let go), then the ring itself.
struct VoiceEngine::Runtime {
	Runtime() : capture(ring), recognizer(ring, [this] { return TakeUserMutedSeen(); }) {}

	// Recognizer worker, once per segment as it closes (Recognizer::Transcribe takes
	// the flag before it delivers that segment's result), and as an always-listen
	// utterance starts. libobs reports our own push-to-talk mute as muted too, so the
	// capture's flag alone would mark every push-to-talk segment muted; only a mute the
	// guard did not make is the user's. The guard's flag is consumed here: it belongs to
	// the one segment it was set for, and an always-listen utterance after it must not
	// inherit it (that would hide the user's own mute from the muted-mic rule).
	bool TakeUserMutedSeen()
	{
		const bool seen = capture.TakeMutedSeen();
		segmentPttMuted = pttMutedSegment.exchange(false, std::memory_order_acq_rel);
		return seen && !segmentPttMuted;
	}

	SpscRing ring{kRingSamples};
	VoiceCapture capture;
	// Set by the hotkey thread as a segment opens: the guard muted the mic for it.
	std::atomic<bool> pttMutedSegment{false};
	// Recognizer worker only: pttMutedSegment as it was when the closing segment's flag
	// was taken, for that segment's result. Copied there because a new segment may open
	// before the result reaches the UI.
	bool segmentPttMuted = false;
	// Tells the model verification worker to stop hashing once this runtime is retired.
	std::shared_ptr<std::atomic<bool>> verifyCancel = std::make_shared<std::atomic<bool>>(false);
	Recognizer recognizer;
};

VoiceEngine::VoiceEngine()
	: interpreter_(&ShowOnly),
	  listener_(std::make_unique<VoiceListener>(interpreter_)),
	  playCue_([this](Cue cue) { feedback_.Play(cue, settings_.cueVolume); }),
	  speak_([this](const std::string &text) { SpeakBack(text); })
{
}

// The engine's own speech sink: synthesize off the UI thread (SAPI takes tens of
// milliseconds and is synchronous), then play on the cue source at the cue volume, so it
// reaches the monitoring device only. Whether to speak was decided earlier: a ReadBack
// effect exists only for an action interpreted while read-back was on, or for the user
// saying "read that back".
void VoiceEngine::SpeakBack(const std::string &text)
{
	const double volume = settings_.cueVolume;
	const uint64_t generation = generation_.load(std::memory_order_acquire);
	try {
		AsyncTask::RunAsync([this, text, volume, generation] {
			auto spoken = std::make_shared<WavData>();
			std::string error;
			if (!Synthesize(text, *spoken, error)) {
				HostLog("[voice] read-back failed: " + error); // the reason, never the words
				return;
			}
			AsyncTask::PostToUi([this, spoken, volume, generation] {
				// Voice turned off (or the runtime rebuilt) meanwhile: the source it
				// would play on is gone or someone else's.
				if (started_ && generation == generation_.load(std::memory_order_acquire)) {
					feedback_.PlaySamples(std::move(*spoken), volume);
				}
			});
		});
	} catch (...) {
		HostLog("[voice] read-back failed: no worker to synthesize on");
	}
}

VoiceEngine::~VoiceEngine()
{
	WaitForRetired();
}

void VoiceEngine::SetInterpreter(Interpreter fn)
{
	interpreter_ = fn ? std::move(fn) : Interpreter(&ShowOnly);
	listener_ = std::make_unique<VoiceListener>(interpreter_);
	// A new listener knows nothing the running runtime has reported (model, mic), so the
	// runtime starts over rather than leaving the listener NotReady for good.
	if (CurrentRuntime()) {
		StopRuntime(false);
		StartRuntime();
	}
}

void VoiceEngine::SetActionRunner(ActionRunner fn)
{
	runAction_ = std::move(fn);
}

void VoiceEngine::SetCueSink(CueSink fn)
{
	playCue_ = std::move(fn);
}

void VoiceEngine::SetSpeechSink(SpeechSink fn)
{
	speak_ = std::move(fn);
}

void VoiceEngine::SetPromptSource(PromptSource fn)
{
	prompt_ = std::move(fn);
}

// The prompt biases whisper toward this studio's own scene and source names, so it is
// rebuilt as each segment opens (scenes are renamed while the app runs) and once the
// model is up. On the UI thread, where the studio may be read, rather than on the
// hotkey thread that opens the segment: the recognizer reads the prompt only when the
// segment closes, at least kMinSegmentMs later, so the event that announced the
// segment has long been handled by then.
void VoiceEngine::RefreshPrompt()
{
	std::shared_ptr<Runtime> runtime = CurrentRuntime();
	if (runtime && prompt_) {
		runtime->recognizer.SetPrompt(prompt_());
	}
}

const VoiceSettings &VoiceEngine::Settings() const
{
	return settings_;
}

int VoiceEngine::ThreadCount() const
{
	const unsigned hw = std::thread::hardware_concurrency();
	const int half = static_cast<int>(hw > 0 ? hw / 2 : 1);
	return std::max(1, std::min(P0::kThreadCap, half));
}

bool VoiceEngine::CanEnable(std::string &reason) const
{
	reason = cpuReason_;
	return cpuReason_.empty();
}

std::shared_ptr<VoiceEngine::Runtime> VoiceEngine::CurrentRuntime() const
{
	std::lock_guard<std::mutex> lock(runtimeMutex_);
	return runtime_;
}

void VoiceEngine::Start()
{
	if (started_) {
		return;
	}
	started_ = true;
	CpuSupportsVoice(cpuReason_);
	settings_.Load();
	Downloads().SetLanguage(settings_.language);
	HostLog(std::string("[voice] engine start: ") + (settings_.enabled ? "enabled" : "disabled") + ", model " +
		settings_.model + (cpuReason_.empty() ? "" : ", unsupported CPU"));
	if (settings_.enabled && cpuReason_.empty()) {
		StartRuntime();
	}
	PublishState();
}

void VoiceEngine::Stop()
{
	if (!started_) {
		return;
	}
	// First, so nothing below emits through a bridge that is already shut down.
	started_ = false;
	// The hotkeys are unregistered by now, so nothing else can engage the guard: this is
	// the one Release from the UI thread its contract allows. A key held at quit gives
	// the mic back here.
	micGuard_.Release();
	StopRuntime(true);
	// Blocking is acceptable here and only here: this is app shutdown, and whisper's
	// threads must be gone before libobs and the process are. A runtime an earlier
	// disable handed to a worker is waited for too; each wait is at most one model load
	// or one encoder pass.
	WaitForRetired();
	HostLog("[voice] engine stopped");
}

void VoiceEngine::StartRuntime()
{
	const uint64_t generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
	modelState_ = ModelState::Loading;
	listener_->Handle(MakeEvent(EventType::Enable));

	auto runtime = std::make_shared<Runtime>();
	{
		std::lock_guard<std::mutex> lock(runtimeMutex_);
		runtime_ = runtime;
	}

	// Cues are feedback, not function: without them voice still works, so a failure is
	// logged and nothing more.
	std::string cueError;
	if (!feedback_.Start(cueError)) {
		HostLog("[voice] command cues unavailable: " + cueError);
	}

	// The capture follows its own channel's source; the engine follows the mic to
	// another channel (ReconcileMic). Both act on the UI thread, never on the signal's
	// stack, which holds libobs's channel mutex.
	runtime->capture.WatchChannelChanges([this, generation] {
		std::shared_ptr<Runtime> current = CurrentRuntime();
		if (current) {
			PostEvent(MakeEvent(current->capture.Bound() ? EventType::MicBound : EventType::MicLost),
				  generation);
		}
	});
	if (!watchingChannels_) {
		signal_handler_connect(obs_get_signal_handler(), "channel_change", &VoiceEngine::OnChannelChange, this);
		watchingChannels_ = true;
	}
	BindMic(*runtime);

	// Verifying and loading the models take seconds, so they happen off the UI thread;
	// the outcome comes back as a listener event. Always-listen's two models are verified
	// on the same worker: hashing a hand-copied one on the UI thread would freeze it.
	const std::string modelId = settings_.model;
	const int threads = ThreadCount();
	const bool wantWake = settings_.triggerMode == "wake";
	const std::string wakePhrase = settings_.wakePhrase;
	alwaysListen_ = false;
	wakeReason_.clear();
	std::shared_ptr<std::atomic<bool>> cancel = runtime->verifyCancel;
	try {
		AsyncTask::RunAsync([this, generation, modelId, threads, cancel, wantWake, wakePhrase] {
			const ModelInfo *info = FindModel(modelId);
			std::string error;
			if (!info) {
				error = "That speech model is no longer available.";
			} else if (!EnsureModelVerified(*info, error, cancel.get()) && error.empty()) {
				error = "The speech model is unavailable.";
			}
			if (!error.empty()) {
				Event failed = MakeEvent(EventType::ModelFailed);
				failed.text = error;
				PostEvent(failed, generation);
				return;
			}
			Recognizer::Continuous continuous;
			std::string wakeReason;
			if (wantWake) {
				const ModelInfo *vad = FindModel(kVadModelId);
				const ModelInfo *wake = FindModel(kWakeModelId);
				std::string why;
				if (vad && wake && EnsureModelVerified(*vad, why, cancel.get()) &&
				    EnsureModelVerified(*wake, why, cancel.get())) {
					continuous.enabled = true;
					continuous.wakePhrase = wakePhrase;
					continuous.vadModelPath = ModelPath(*vad);
					continuous.wakeModelPath = ModelPath(*wake);
				} else {
					// Missing pieces fall back to push-to-talk rather than silently
					// doing nothing; the Voice tab offers the download.
					HostLog("[voice] always-listen needs the voice activity and tiny models (" +
						why + "); staying in push-to-talk");
					wakeReason = kWakeModelsMissing;
				}
			}
			const std::string path = ModelPath(*info);
			AsyncTask::PostToUi([this, generation, path, threads, continuous, wakeReason] {
				LoadModel(generation, path, threads, continuous, wakeReason);
			});
		});
	} catch (...) {
		Event failed = MakeEvent(EventType::ModelFailed);
		failed.text = "Could not start loading the speech model.";
		PostEvent(failed, generation);
	}
	UpdateArmed();
}

void VoiceEngine::LoadModel(uint64_t generation, const std::string &path, int threads,
			    const Recognizer::Continuous &continuous, const std::string &wakeReason)
{
	if (generation != generation_.load(std::memory_order_acquire)) {
		return; // voice was turned off, or the model changed, while it was being verified
	}
	wakeReason_ = wakeReason;
	std::shared_ptr<Runtime> runtime = CurrentRuntime();
	if (!runtime) {
		return;
	}
	Runtime *raw = runtime.get(); // the callbacks live inside it, so they cannot outlive it
	runtime->recognizer.SetModelCallback([this, generation](bool ok, const std::string &why) {
		Event event = MakeEvent(ok ? EventType::ModelReady : EventType::ModelFailed);
		if (!ok) {
			event.text = why.empty() ? "The speech model could not be loaded." : why;
		}
		PostEvent(event, generation);
	});
	runtime->recognizer.SetWakeCallback([this, generation] {
		// The wake phrase opened an utterance: the listener starts a segment, so the
		// indicator lights while the user is still speaking.
		PostEvent(MakeEvent(EventType::WakeMatched), generation);
	});
	runtime->recognizer.SetResultCallback([this, generation, raw](Recognizer::Result result) {
		if (result.wake) {
			// The detector closed the utterance; the listener still thinks it is
			// recording. Posted first, so it arrives first.
			PostEvent(MakeEvent(EventType::SpeechEnded), generation);
		}
		Event event = MakeEvent(result.ok ? EventType::Transcript : EventType::TranscribeFailed);
		event.text = result.ok ? std::move(result.text) : std::move(result.error);
		event.mutedSeen = result.mutedSeen;
		event.pttMuted = raw->segmentPttMuted;
		PostEvent(event, generation);
	});
	runtime->recognizer.SetContinuous(continuous);
	runtime->recognizer.SetLanguage(settings_.language);
	runtime->recognizer.Start(path, threads);
}

void VoiceEngine::StopRuntime(bool block)
{
	generation_.fetch_add(1, std::memory_order_acq_rel);
	pttArmed_.store(false, std::memory_order_release);
	cancelArmed_.store(false, std::memory_order_release);
	// Deliberately no micGuard_.Release() here: a hold in progress belongs to the hotkey
	// thread, and the key coming up releases it even with the runtime gone (OnPtt).

	std::shared_ptr<Runtime> runtime;
	{
		std::lock_guard<std::mutex> lock(runtimeMutex_);
		runtime = std::move(runtime_);
	}
	if (watchingChannels_) {
		// Waits out an emission in progress on another thread, so none is in flight after.
		signal_handler_disconnect(obs_get_signal_handler(), "channel_change", &VoiceEngine::OnChannelChange,
					  this);
		watchingChannels_ = false;
	}
	if (runtime) {
		runtime->verifyCancel->store(true, std::memory_order_release);
		// The producer goes before the consumer: once Unbind returns no capture callback
		// is in flight, so nothing writes into a ring whose reader is stopping.
		runtime->capture.StopWatchingChannelChanges();
		runtime->capture.Unbind();
		if (block) {
			runtime->recognizer.Stop();
		} else {
			Retire(std::move(runtime));
		}
	}
	// Last, as the index's teardown order has it: the player is joined and the private
	// source released, here on the UI thread (a cue is at most one 20 ms block away from
	// noticing).
	feedback_.Stop();
	modelState_ = ModelState::Unloaded;
	alwaysListen_ = false;
	listener_->Handle(MakeEvent(EventType::Disable));
	UpdateArmed();
	PublishState();
}

// whisper.cpp v1.9.4 polls the abort callback only after the encoder pass, so
// Recognizer::Stop can wait out one (about 1.4 s for base-sized models on 2 threads),
// or a model still loading. Turning voice off from the Settings tab must not freeze the
// UI for that long, so the stop and the join run on a worker. Nothing of the retired
// runtime can reach the UI afterwards: its callbacks post under a generation that is
// already stale, and a key press still holding a reference only finds a closed segment.
// A re-enable in between builds a new runtime beside it.
void VoiceEngine::Retire(std::shared_ptr<Runtime> runtime)
{
	{
		std::lock_guard<std::mutex> lock(retireMutex_);
		++retiring_;
	}
	auto finish = [this] {
		{
			std::lock_guard<std::mutex> lock(retireMutex_);
			--retiring_;
		}
		retireCv_.notify_all();
	};
	try {
		AsyncTask::RunAsync([runtime, finish]() mutable {
			const auto started = std::chrono::steady_clock::now();
			runtime->recognizer.Stop();
			runtime.reset(); // usually the last reference; a key press may still hold one
			DBG(LogCat::Voice, "retired runtime stopped in %lld ms",
			    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
							   std::chrono::steady_clock::now() - started)
							   .count()));
			finish();
		});
	} catch (...) {
		// No thread to hand it to: stop it here rather than leak a running worker.
		runtime->recognizer.Stop();
		runtime.reset();
		finish();
	}
}

void VoiceEngine::WaitForRetired()
{
	std::unique_lock<std::mutex> lock(retireMutex_);
	retireCv_.wait(lock, [this] { return retiring_ == 0; });
}

void VoiceEngine::BindMic(Runtime &runtime)
{
	const bool bound = runtime.capture.Bind(GlobalAudio::PrimaryMicChannel());
	PostEvent(MakeEvent(bound ? EventType::MicBound : EventType::MicLost),
		  generation_.load(std::memory_order_acquire));
}

// The capture only watches its own channel. When a channel change moves "the mic"
// elsewhere (the user's mic was cleared from the first Mic/Aux slot while another slot
// holds one, or a mic appeared on a later slot while the first is empty), follow it.
void VoiceEngine::ReconcileMic()
{
	std::shared_ptr<Runtime> runtime = CurrentRuntime();
	if (!runtime) {
		return;
	}
	const uint32_t channel = GlobalAudio::PrimaryMicChannel();
	if (channel == runtime->capture.Channel()) {
		return; // a swap on the same channel is the capture's own rebind
	}
	DBG(LogCat::Voice, "mic moved from channel %u to %u", runtime->capture.Channel(), channel);
	BindMic(*runtime);
}

void VoiceEngine::OnChannelChange(void *param, calldata_t *)
{
	// libobs emits this while holding its channel mutex, possibly off the UI thread, so
	// only queue. The engine outlives every queued task (it is a process-lifetime
	// singleton), and ReconcileMic finds nothing to do once the runtime is gone.
	auto *self = static_cast<VoiceEngine *>(param);
	AsyncTask::QueueOnUi([self] { self->ReconcileMic(); });
}

void VoiceEngine::ApplySettings(const VoiceSettings &next)
{
	// The fields the runtime is built from. The detector and the wake model are loaded
	// on the worker with the speech model, so a change to any of these rebuilds it, as a
	// model change always has. The cue volume, the send mode and read-back apply live.
	const bool runtimeChanged = settings_.model != next.model || settings_.triggerMode != next.triggerMode ||
				    settings_.wakePhrase != next.wakePhrase || settings_.language != next.language;
	settings_ = next;
	// Which models the Voice tab offers follows the language.
	Downloads().SetLanguage(settings_.language);
	if (!started_) {
		return;
	}
	const bool running = CurrentRuntime() != nullptr;
	const bool enable = settings_.enabled && cpuReason_.empty();
	if (!enable) {
		if (running) {
			StopRuntime(false);
		}
		PublishState();
		return;
	}
	if (!running || runtimeChanged) {
		if (running) {
			StopRuntime(false);
		}
		StartRuntime();
	}
	PublishState();
}

void VoiceEngine::OnAudioReset()
{
	std::shared_ptr<Runtime> runtime = CurrentRuntime();
	if (runtime) {
		// The mix rate may have changed, so the capture's resampler has to be rebuilt.
		BindMic(*runtime);
	}
}

void VoiceEngine::NoteModelsChanged()
{
	// Only a runtime whose model failed (typically: not downloaded yet) starts over. One
	// that is loading or loaded is left alone, so an unrelated download never cuts off a
	// command being recognized.
	// Likewise a runtime that is up in push-to-talk only because always-listen's models
	// were missing, once nothing is in flight: the download may be the one it lacked.
	const bool wakeMissing = settings_.triggerMode == "wake" && modelState_ == ModelState::Ready &&
				 !alwaysListen_ && listener_->Current() == State::Idle;
	if (started_ && (modelState_ == ModelState::Failed || wakeMissing) && CurrentRuntime()) {
		StopRuntime(false);
		StartRuntime();
		PublishState();
	}
}

void VoiceEngine::OnPtt(bool down)
{
	// libobs hotkey thread. Everything here is an atomic, a short mutex, or a libobs call
	// that is safe off the UI thread; the listener is reached only by posting.
	const uint64_t generation = generation_.load(std::memory_order_acquire);
	std::shared_ptr<Runtime> runtime = CurrentRuntime();

	if (!down) {
		if (runtime) {
			runtime->recognizer.EndSegment();
		}
		// Always, even with the runtime gone: a hold that began before voice was turned
		// off still muted the mic, and the key coming up is what gives it back.
		micGuard_.Release();
		if (runtime) {
			PostEvent(MakeEvent(EventType::PttUp), generation);
		}
		return;
	}

	if (!runtime || !pttArmed_.load(std::memory_order_acquire)) {
		return; // voice is off or not ready: the key does nothing at all
	}
	Event event = MakeEvent(EventType::PttDown);
	if (!runtime->recognizer.BeginSegment()) {
		event.ok = false;
		event.text = runtime->recognizer.Ready() ? "Still working on the last command."
							 : "The speech model is not ready yet.";
		PostEvent(event, generation);
		return;
	}
	// Only what the mic reports from here on belongs to this segment. Taken after
	// BeginSegment succeeded: until then the previous segment may not have read it.
	runtime->capture.TakeMutedSeen();
	OBSSourceAutoRelease mic = obs_get_output_source(GlobalAudio::PrimaryMicChannel());
	runtime->pttMutedSegment.store(micGuard_.Engage(mic), std::memory_order_release);
	PostEvent(event, generation);
}

bool CancelKeyApplies(uint32_t foregroundPid, uint32_t ownPid)
{
	return foregroundPid != ownPid;
}

void VoiceEngine::OnCancelKey()
{
	// In the app, Escape is the web UI's (closing a dialog); see CancelKeyApplies. Nothing
	// at all happens then: the segment, the mute guard and a pending action are left
	// alone, and the push-to-talk key coming up still ends and releases them.
	if (!CancelKeyApplies(ForegroundProcessId(), static_cast<uint32_t>(GetCurrentProcessId()))) {
		return;
	}
	const uint64_t generation = generation_.load(std::memory_order_acquire);
	std::shared_ptr<Runtime> runtime = CurrentRuntime();
	if (runtime) {
		runtime->recognizer.EndSegment();
	}
	micGuard_.Release();
	// Escape is the default binding and fires globally (in a game, too), so a press that
	// cannot cancel anything posts nothing rather than clearing what the indicator shows.
	if (runtime && cancelArmed_.load(std::memory_order_acquire)) {
		PostEvent(MakeEvent(EventType::Cancel), generation);
	}
}

void VoiceEngine::PostEvent(Event event, uint64_t generation)
{
	// Always to the back of the UI queue, even from the UI thread, so events reach the
	// listener in the order they were posted whichever thread posted them.
	AsyncTask::QueueOnUi([this, event, generation] { HandleOnUi(event, generation); });
}

void VoiceEngine::HandleOnUi(const Event &event, uint64_t generation)
{
	if (!started_ || generation != generation_.load(std::memory_order_acquire)) {
		return; // from a runtime that has been torn down (an old model, voice turned off)
	}
	if (event.type == EventType::ModelReady) {
		modelState_ = ModelState::Ready;
		std::shared_ptr<Runtime> runtime = CurrentRuntime();
		alwaysListen_ = runtime && runtime->recognizer.ContinuousActive();
		if (settings_.triggerMode == "wake" && !alwaysListen_ && wakeReason_.empty()) {
			wakeReason_ = kWakeDidNotStart; // its models are there but did not load
		}
	} else if (event.type == EventType::ModelFailed) {
		modelState_ = ModelState::Failed;
	}
	if (settings_.logTranscripts && event.type == EventType::Transcript) {
		// Two opt-ins, both off by default: the voice debug category (DBG's own gate)
		// and the explicit transcript setting. Nothing else in the feature ever writes
		// recognized text.
		DBG(LogCat::Voice, "transcript: %s", event.text.c_str());
	}
	if (event.type == EventType::ModelReady || event.type == EventType::WakeMatched ||
	    (event.type == EventType::PttDown && event.ok)) {
		RefreshPrompt();
	}
	Deliver(listener_->Handle(event), generation);
	UpdateArmed();
	PublishState();
}

void VoiceEngine::Deliver(const std::vector<Effect> &effects, uint64_t generation)
{
	for (const Effect &effect : effects) {
		switch (effect.type) {
		case EffectType::PlayCue:
			DBG(LogCat::Voice, "cue %s", CueName(effect.cue));
			if (playCue_) {
				playCue_(effect.cue);
			}
			break;
		case EffectType::Run:
			DBG(LogCat::Voice, "run %s", effect.action.commandId.c_str());
			if (runAction_) {
				runAction_(effect.action, [this, generation](bool ok, std::string message) {
					Event done = MakeEvent(EventType::ActionResult);
					done.ok = ok;
					done.text = std::move(message);
					PostEvent(done, generation);
				});
			}
			break;
		case EffectType::ReadBack:
			if (speak_) {
				speak_(effect.text);
			}
			break;
		case EffectType::ScheduleTick: {
			const uint64_t seq = effect.seq;
			const int64_t remaining = effect.atMs - TimeUtil::NowMs();
			// A deadline already (mostly) in the past means the window was spent before the
			// timer was ever armed. For a chat draft that sends on timeout that would be a
			// send with no take-it-back window at all, so it is never scheduled to send:
			// the listener holds it for "send" instead (EventType::Lapsed), and the user
			// sees why. Logged whatever the debug components: it is the one line that tells
			// you an interpretation step got slow.
			const PendingAction &pending = listener_->Snapshot().pending;
			if (pending.runOnTimeout && VoiceListener::CountdownSpent(remaining, pending.timeoutMs)) {
				HostLog("[voice] a chat draft's countdown had " +
					std::to_string(std::max<int64_t>(0, remaining)) + " of " +
					std::to_string(pending.timeoutMs) +
					" ms left when it was scheduled; holding it for send instead");
				Event lapsed = MakeEvent(EventType::Lapsed);
				lapsed.seq = seq;
				PostEvent(lapsed, generation);
				break;
			}
			if (remaining < 0) {
				DBG(LogCat::Voice, "pending deadline was already %lld ms past when scheduled",
				    static_cast<long long>(-remaining));
			}
			AsyncTask::PostToUiDelayed(
				[this, seq, generation] {
					Event tick = MakeEvent(EventType::Tick);
					tick.seq = seq;
					HandleOnUi(tick, generation);
				},
				std::max<int64_t>(0, remaining));
			break;
		}
		}
	}
}

void VoiceEngine::UpdateArmed()
{
	const State state = listener_->Current();
	pttArmed_.store(state != State::Disabled && state != State::NotReady, std::memory_order_release);
	cancelArmed_.store(state == State::Listening || state == State::Thinking || state == State::Pending,
			   std::memory_order_release);
}

nlohmann::json VoiceEngine::StateJson() const
{
	nlohmann::json state = listener_->StatusJson();
	std::shared_ptr<Runtime> runtime = CurrentRuntime();
	const bool micBound = runtime && runtime->capture.Bound();
	state["device"] = micBound ? runtime->capture.DeviceName() : std::string();
	state["ready"] = {
		{"cpu", cpuReason_.empty()},
		{"cpuReason", cpuReason_},
		{"model", modelState_ == ModelState::Ready},
		{"mic", micBound},
		// Always-listen: whether it is running, and why not when it was asked for and is
		// not (push-to-talk still works then).
		{"wake", alwaysListen_},
		{"wakeReason", settings_.triggerMode == "wake" ? wakeReason_ : std::string()},
	};
	state["settings"] = SettingsFields::ToJson(VoiceSettingsTable(), settings_);
	// The listener reports the deadline; only here is there a clock to measure it
	// against. The UI gets the remainder at the moment it is asked, so a countdown that
	// has already been partly spent renders short rather than restarting at full --
	// the difference between showing a take-it-back window that exists and one that
	// does not.
	if (state["pending"].is_object()) {
		const int64_t deadline = state["pending"].value("deadlineMs", static_cast<int64_t>(0));
		state["pending"]["remainingMs"] = deadline > 0 ? std::max<int64_t>(0, deadline - TimeUtil::NowMs()) : 0;
	}
	return state;
}

bool VoiceEngine::ConfirmPending(uint64_t id, std::string &error)
{
	const PendingAction &pending = listener_->Snapshot().pending;
	if (!started_ || pending.commandId.empty()) {
		error = "there is no command waiting for confirmation";
		return false;
	}
	if (id == 0 || id != pending.id) {
		// A confirmation must say what it confirms: a click on draft A that lands after A
		// expired must not confirm what is pending now, which may be "Stop streaming?".
		error = id == 0 ? "voice.confirm needs the id of the pending action it confirms"
				: "that action is no longer the one waiting for confirmation";
		return false;
	}
	Event confirm = MakeEvent(EventType::Confirm);
	confirm.pendingId = id;
	HandleOnUi(confirm, generation_.load(std::memory_order_acquire));
	return true;
}

bool VoiceEngine::CancelPending(uint64_t id, std::string &error)
{
	const State state = listener_->Current();
	const PendingAction &pending = listener_->Snapshot().pending;
	if (!started_ || (pending.commandId.empty() && state != State::Listening && state != State::Thinking)) {
		// A stray click is not a cancel: the listener's Cancel would also clear what the
		// indicator shows.
		error = "there is nothing to cancel";
		return false;
	}
	if (id != 0 && id != pending.id) {
		error = "that action is no longer the one waiting";
		return false;
	}
	// The listener only: a segment in flight is abandoned there and its transcript, when
	// it comes, finds nobody waiting. The recognizer and the mute guard belong to the
	// hotkey thread, and the key coming up still ends and releases them.
	Event cancel = MakeEvent(EventType::Cancel);
	cancel.pendingId = id;
	HandleOnUi(cancel, generation_.load(std::memory_order_acquire));
	return true;
}

void VoiceEngine::PublishState()
{
	if (!started_) {
		return;
	}
	Bridge::EmitEvent(EventNames::kVoiceState, StateJson());
}

VoiceEngine &Engine()
{
	static VoiceEngine engine;
	return engine;
}

} // namespace Voice
