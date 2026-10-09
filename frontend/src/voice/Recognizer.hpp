#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_RECOGNIZER_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_RECOGNIZER_HPP_

#include "voice/VoiceRing.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct whisper_context;

namespace Voice {

// Owns whisper.cpp and the one thread it runs on. The push-to-talk hotkey thread calls
// BeginSegment and EndSegment directly (not through the UI thread) so the first
// syllable is never lost to UI latency; everything else happens on the worker.
//
// Callbacks are invoked from the worker thread. The engine marshals them to the UI
// thread; the recognizer itself knows nothing about CEF. Set them before Start.
class Recognizer {
public:
	struct Result {
		bool ok = false;
		std::string text;  // cleaned transcript, never logged here
		std::string error; // why, when ok is false
		bool mutedSeen = false;
		int64_t inferenceMs = 0;
	};

	// The pre-roll kept from before the key went down. Push-to-talk users start
	// speaking as they press.
	static constexpr size_t kPreRollMs = 300;
	// A segment whose key was held shorter than this was a slip of the key. The pre-roll
	// does not count toward it: it is always there, so counting it would let every tap
	// through to whisper.
	static constexpr size_t kMinSegmentMs = 250;
	// The longest segment whisper is asked to transcribe.
	static constexpr size_t kMaxSegmentMs = 15000;
	// A running inference older than this is abandoned: nobody is waiting for it. It
	// lands at the first abort poll after the deadline (see Stop for where those are).
	static constexpr int64_t kStaleMs = 5000;

	Recognizer(SpscRing &ring, std::function<bool()> takeMutedSeen);
	~Recognizer();
	Recognizer(const Recognizer &) = delete;
	Recognizer &operator=(const Recognizer &) = delete;

	// Starts the worker, which loads `modelPath` (UTF-8) and then reports through the
	// model callback. Returns immediately.
	void Start(const std::string &modelPath, int threads);
	// Aborts any running inference, joins the worker and frees the model. Idempotent.
	// Two waits it cannot cut short: a model still loading (whisper's loader has no
	// abort), and the encoder pass in flight, because whisper.cpp v1.9.4 polls the abort
	// callback only after the encoder and between decoder steps (src/whisper.cpp:2420-
	// 2504). That pass costs the same for any segment length, since whisper always
	// encodes a 30 s window: about 1.4 s for base-sized dims on 2 threads, measured on
	// a Linux host.
	void Stop();

	bool Ready() const { return ready_.load(std::memory_order_acquire); }

	// Hotkey thread. False when a segment is already open or the model is not ready.
	bool BeginSegment();
	void EndSegment();

	// Bias whisper toward the phrases that matter (command names, scene names, chat
	// handles), least important first. Trimmed from the front to the model's prompt
	// budget, which mirrors whisper's own behaviour: whisper_full keeps the LAST tokens
	// of a long prompt (src/whisper.cpp:7046 and the prompt_past1 take below it).
	void SetPrompt(std::string prompt);

	// (bool ok, why) once the model has loaded or failed.
	void SetModelCallback(std::function<void(bool, const std::string &)> fn);
	void SetResultCallback(std::function<void(Result)> fn);

private:
	enum class Segment { Idle, Open, Closing, Busy };

	static bool ShouldAbort(void *self); // whisper's abort_callback
	void WorkerMain(std::string modelPath, int threads);
	bool LoadModel(const std::string &modelPath, std::string &error);
	void DrainToPreRoll();
	void StartCollecting();
	void PullAudio();
	Result Transcribe(int threads);
	std::string BuildPrompt();

	SpscRing &ring_;
	std::function<bool()> takeMutedSeen_;
	std::function<void(bool, const std::string &)> onModel_;
	std::function<void(Result)> onResult_;

	std::thread worker_;
	std::mutex mutex_;
	std::condition_variable cv_;
	Segment segment_ = Segment::Idle;
	bool quit_ = false;
	std::string prompt_;
	std::atomic<bool> ready_{false};
	// Read by the abort callback on whisper's threads. Stop sets stopping_; the worker
	// sets the deadline (steady-clock ms) for each inference and clears it after.
	std::atomic<bool> stopping_{false};
	std::atomic<int64_t> deadlineMs_{0};
	// Worker thread only. While idle the worker keeps draining the capture ring into
	// preRoll_, a circular buffer of the last kPreRollMs, overwriting the oldest
	// samples; a new segment starts from its contents. Without that drain the ring
	// would fill with the oldest audio since capture began, because the writer (the
	// audio thread) may never move the reader's index.
	std::vector<float> preRoll_;
	size_t preRollPos_ = 0;
	bool preRollFull_ = false;
	std::vector<float> audio_; // the segment being collected
	size_t preRollTaken_ = 0;  // how much of audio_ is pre-roll
	whisper_context *ctx_ = nullptr;
};

// Strips whisper's non-speech markers ("[BLANK_AUDIO]", "(wind blowing)"), collapses
// whitespace and trims. Exposed for the self-test.
std::string CleanTranscript(const std::string &raw);

// The longest whole-word suffix of `prompt` that `countTokens` puts at or under
// `budget`, or "" when not even its last word fits. Words are dropped from the front,
// so the end of the prompt (the most important phrases) survives, as whisper itself
// would keep it. Exposed for the self-test; the recognizer counts with whisper's own
// tokenizer.
std::string TrimPromptFront(const std::string &prompt, int budget,
			    const std::function<int(const std::string &)> &countTokens);

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_RECOGNIZER_HPP_
