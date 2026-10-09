#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_ENGINE_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_ENGINE_HPP_

#include "voice/MicMuteGuard.hpp"
#include "voice/VoiceListener.hpp"
#include "voice/VoiceSettings.hpp"

#include <obs.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Voice {

// Runs a matched command. P1 never calls it (the P1 interpreter only shows text);
// P2's command registry supplies the real one. `done` reports back to the listener.
using ActionRunner =
	std::function<void(const PendingAction &action, std::function<void(bool ok, std::string message)> done)>;
// Plays a feedback sound (P2) and speaks a confirmation (P4).
using CueSink = std::function<void(Cue)>;
using SpeechSink = std::function<void(const std::string &)>;

// The composition root for voice control: it owns the ring, the capture, the recognizer,
// the listener and the mute guard, and it is the only place that knows which thread each
// of them belongs to.
//
//   libobs hotkey thread  OnPtt, OnCancelKey: open or close the recognizer segment,
//                         engage or release the mute guard, post the listener event.
//                         Nothing that can block.
//   libobs audio thread   the capture callback only (VoiceCapture)
//   recognizer worker     model load and whisper (Recognizer)
//   CEF UI thread         everything else: the listener, every effect, every event
//                         emission, start and stop.
//
// The ring, the capture and the recognizer live together in one Runtime that exists
// while voice is enabled. The hotkey thread takes a reference to it under
// runtimeMutex_, so a key press never waits on the UI and never outlives the objects it
// is using. Every event posted to the UI carries the generation it was posted under;
// turning voice off (or changing the model) starts a new generation, so whatever the old
// runtime still reports is dropped rather than acted on.
class VoiceEngine {
public:
	VoiceEngine();
	~VoiceEngine();
	VoiceEngine(const VoiceEngine &) = delete;
	VoiceEngine &operator=(const VoiceEngine &) = delete;

	// ObsBootstrap::Start, right after the audio monitor is up.
	void Start();
	// ObsBootstrap::Stop, right after the frontend hotkeys are unregistered and well
	// before obs_shutdown. This one blocks: the capture callback is removed first, then
	// the recognizer is joined (and any runtime an earlier disable handed to a worker is
	// waited for), so nothing of voice runs once it returns.
	void Stop();

	const VoiceSettings &Settings() const;
	// Persisting is the bridge's job; this applies the change to the runtime. Turning
	// voice off or switching the model never waits for the recognizer: its join can
	// take an encoder pass (Recognizer::Stop), so that happens on a worker.
	void ApplySettings(const VoiceSettings &next);

	// The mix format changed (bridge, after obs_reset_audio): rebind the capture so the
	// resampler is rebuilt for the new rate.
	void OnAudioReset();
	// A model finished downloading, so a selection that failed to load may now work.
	void NoteModelsChanged();

	// libobs hotkey thread.
	void OnPtt(bool down);
	void OnCancelKey();

	// False, with a reason, when voice cannot run at all (no AVX2-class CPU).
	bool CanEnable(std::string &reason) const;
	int ThreadCount() const;
	// The voice.state payload: the listener's status, the bound device, what is missing
	// (`ready`), the settings, and for a pending action what is left of its window.
	nlohmann::json StateJson() const;
	// Emits voice.state. UI thread.
	void PublishState();

	void SetInterpreter(Interpreter fn);
	void SetActionRunner(ActionRunner fn);
	void SetCueSink(CueSink fn);
	void SetSpeechSink(SpeechSink fn);

private:
	struct Runtime;
	enum class ModelState { Unloaded, Loading, Ready, Failed };

	std::shared_ptr<Runtime> CurrentRuntime() const;
	void StartRuntime();
	// `block` is for Stop alone: everywhere else the recognizer is stopped on a worker.
	void StopRuntime(bool block);
	void Retire(std::shared_ptr<Runtime> runtime);
	void WaitForRetired();
	void LoadModel(uint64_t generation, const std::string &path, int threads);
	void BindMic(Runtime &runtime);
	void ReconcileMic();
	static void OnChannelChange(void *param, calldata_t *data);
	void PostEvent(Event event, uint64_t generation); // any thread -> UI thread, in order
	void HandleOnUi(const Event &event, uint64_t generation);
	void Deliver(const std::vector<Effect> &effects, uint64_t generation);
	void UpdateArmed();

	mutable std::mutex runtimeMutex_; // guards runtime_ for the hotkey thread's reads
	std::shared_ptr<Runtime> runtime_;
	bool watchingChannels_ = false;

	MicMuteGuard micGuard_;
	VoiceSettings settings_;
	Interpreter interpreter_;
	std::unique_ptr<VoiceListener> listener_;
	ActionRunner runAction_;
	CueSink playCue_;
	SpeechSink speak_;
	std::string cpuReason_;
	ModelState modelState_ = ModelState::Unloaded;
	bool started_ = false;

	// Bumped whenever the runtime is torn down or rebuilt.
	std::atomic<uint64_t> generation_{0};
	// Read by the hotkey thread, written by the UI thread after every listener change:
	// true only while the listener could take a segment (enabled, model loaded, mic
	// bound). A press while it is false does nothing at all.
	std::atomic<bool> pttArmed_{false};
	// Likewise for the cancel key: true only while there is something to cancel (a
	// segment in flight or a pending command).
	std::atomic<bool> cancelArmed_{false};

	// Runtimes handed to a worker to stop; Stop waits for the count to reach zero.
	std::mutex retireMutex_;
	std::condition_variable retireCv_;
	int retiring_ = 0;
};

// The process-wide engine.
VoiceEngine &Engine();

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_ENGINE_HPP_
