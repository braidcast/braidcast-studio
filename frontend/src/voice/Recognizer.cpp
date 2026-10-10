#include "voice/Recognizer.hpp"

#include "log.hpp"
#include "util/string_util.hpp"
#include "voice/VoiceResampler.hpp"
#include "voice/WakeGate.hpp"

#include <whisper.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <iterator>

namespace Voice {

namespace {

size_t MsToSamples(size_t ms)
{
	return ms * static_cast<size_t>(kVoiceSampleRate) / 1000;
}

// The stale deadline is a duration, so it runs on the steady clock: a wall-clock step
// must not abandon an inference early or let one run on.
int64_t SteadyMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

// whisper and ggml print to stdout by default, which would land in the user's log as
// unfiltered noise. Keep the warnings and errors, drop the rest. Neither prints
// decoded text at these levels with print_realtime/print_progress off (Risk R10).
void WhisperLog(enum ggml_log_level level, const char *text, void *)
{
	if (level != GGML_LOG_LEVEL_WARN && level != GGML_LOG_LEVEL_ERROR) {
		return;
	}
	std::string line = text ? text : "";
	while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
		line.pop_back();
	}
	if (!line.empty()) {
		HostLog(std::string("[voice] whisper: ") + line);
	}
}

// whisper_tokenize, sized so it never fails: its tokenizer emits at most one token per
// byte. Asking with a short buffer (or through whisper_token_count, which passes none)
// logs an ERROR line on every call.
int CountTokens(whisper_context *ctx, const std::string &text)
{
	std::vector<whisper_token> tokens(text.size() + 1);
	const int n = whisper_tokenize(ctx, text.c_str(), tokens.data(), static_cast<int>(tokens.size()));
	return n < 0 ? -n : n;
}

} // namespace

std::string CleanTranscript(const std::string &raw)
{
	std::string out;
	out.reserve(raw.size());
	int depth = 0;
	for (char c : raw) {
		if (c == '[' || c == '(') {
			++depth;
			continue;
		}
		if (c == ']' || c == ')') {
			depth = depth > 0 ? depth - 1 : 0;
			continue;
		}
		if (depth > 0) {
			continue;
		}
		if (c == '\n' || c == '\r' || c == '\t') {
			c = ' ';
		}
		if (c == ' ' && (out.empty() || out.back() == ' ')) {
			continue;
		}
		out.push_back(c);
	}
	return StringUtil::Trim(out);
}

std::string TrimPromptFront(const std::string &prompt, int budget,
			    const std::function<int(const std::string &)> &countTokens)
{
	if (prompt.empty() || budget <= 0) {
		return std::string();
	}
	if (countTokens(prompt) <= budget) {
		return prompt;
	}
	// Candidate cut points: the start of every word after the first.
	std::vector<size_t> starts;
	for (size_t i = 1; i < prompt.size(); ++i) {
		if (prompt[i - 1] == ' ' && prompt[i] != ' ') {
			starts.push_back(i);
		}
	}
	// Binary search for the first cut whose suffix fits: tokenizing costs a regex pass
	// over the prompt, so one word at a time would be quadratic in a long name list.
	// `hi` only ever holds a cut that was measured to fit, so the result always fits.
	size_t lo = 0;
	size_t hi = starts.size();
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (countTokens(prompt.substr(starts[mid])) <= budget) {
			hi = mid;
		} else {
			lo = mid + 1;
		}
	}
	return lo < starts.size() ? prompt.substr(starts[lo]) : std::string();
}

Recognizer::Recognizer(SpscRing &ring, std::function<bool()> takeMutedSeen)
	: ring_(ring),
	  takeMutedSeen_(std::move(takeMutedSeen))
{
}

Recognizer::~Recognizer()
{
	Stop();
}

void Recognizer::SetModelCallback(std::function<void(bool, const std::string &)> fn)
{
	onModel_ = std::move(fn);
}

void Recognizer::SetResultCallback(std::function<void(Result)> fn)
{
	onResult_ = std::move(fn);
}

void Recognizer::SetWakeCallback(std::function<void()> fn)
{
	onWake_ = std::move(fn);
}

void Recognizer::SetContinuous(Continuous config)
{
	continuous_ = std::move(config);
}

void Recognizer::SetPrompt(std::string prompt)
{
	std::lock_guard<std::mutex> lock(mutex_);
	prompt_ = std::move(prompt);
}

void Recognizer::SetLanguage(std::string language)
{
	std::lock_guard<std::mutex> lock(mutex_);
	language_ = language.empty() ? std::string("en") : std::move(language);
}

void Recognizer::Start(const std::string &modelPath, int threads)
{
	Stop();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		quit_ = false;
		segment_ = Segment::Idle;
		wakeBusy_ = false;
	}
	utterance_ = Utterance::None;
	stopping_.store(false, std::memory_order_release);
	worker_ = std::thread(&Recognizer::WorkerMain, this, modelPath, threads);
}

void Recognizer::Stop()
{
	if (!worker_.joinable()) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		quit_ = true;
	}
	// Make any inference that is running, or about to start, give up rather than
	// waiting out a 15 second segment during shutdown.
	stopping_.store(true, std::memory_order_release);
	cv_.notify_all();
	worker_.join();
	ready_.store(false, std::memory_order_release);
	continuousActive_.store(false, std::memory_order_release);
	deadlineMs_.store(0, std::memory_order_release);
}

bool Recognizer::ShouldAbort(void *self)
{
	const auto *rec = static_cast<const Recognizer *>(self);
	if (rec->stopping_.load(std::memory_order_acquire)) {
		return true;
	}
	const int64_t at = rec->deadlineMs_.load(std::memory_order_acquire);
	return at != 0 && SteadyMs() > at;
}

bool Recognizer::BeginSegment()
{
	if (!ready_.load(std::memory_order_acquire)) {
		return false;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	if (segment_ != Segment::Idle || wakeBusy_) {
		return false;
	}
	segment_ = Segment::Open;
	cv_.notify_all();
	return true;
}

void Recognizer::EndSegment()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (segment_ == Segment::Open) {
		segment_ = Segment::Closing;
		cv_.notify_all();
	}
}

bool Recognizer::LoadModel(const std::string &modelPath, std::string &error)
{
	whisper_log_set(&WhisperLog, nullptr);
	whisper_context_params cparams = whisper_context_default_params();
	cparams.use_gpu = false; // the CPU backend is the only one linked
	// UTF-8 as is: on MSVC whisper widens the path itself (whisper.cpp:3707-3711), so a
	// narrowed ANSI copy would break, or throw on, a non-ASCII profile directory.
	ctx_ = whisper_init_from_file_with_params(modelPath.c_str(), cparams);
	if (!ctx_) {
		error = "could not load the speech model";
		return false;
	}
	return true;
}

// Idle: keep the newest kPreRollMs of audio and throw the rest away.
void Recognizer::DrainToPreRoll()
{
	float block[1024];
	for (;;) {
		const size_t got = ring_.Read(block, 1024);
		if (got == 0) {
			return;
		}
		KeepPreRoll(block, got);
	}
}

void Recognizer::KeepPreRoll(const float *samples, size_t count)
{
	if (preRoll_.empty()) {
		preRoll_.assign(MsToSamples(kPreRollMs), 0.f);
	}
	for (size_t i = 0; i < count; ++i) {
		preRoll_[preRollPos_] = samples[i];
		preRollPos_ = (preRollPos_ + 1) % preRoll_.size();
		if (preRollPos_ == 0) {
			preRollFull_ = true;
		}
	}
}

// A segment starts with whatever pre-roll the idle drain kept, oldest sample first.
// The ring is NOT drained into the pre-roll here: everything still in it arrived after
// the last idle poll (at most kPollMs before the key) or after the key went down, and
// all of it belongs to the segment. Draining it here would keep only its last 300 ms.
void Recognizer::StartCollecting()
{
	audio_.clear();
	if (preRollFull_) {
		audio_.insert(audio_.end(), preRoll_.begin() + static_cast<long>(preRollPos_), preRoll_.end());
	}
	if (!preRoll_.empty()) {
		audio_.insert(audio_.end(), preRoll_.begin(), preRoll_.begin() + static_cast<long>(preRollPos_));
	}
	preRollTaken_ = audio_.size();
	preRollPos_ = 0;
	preRollFull_ = false;
}

void Recognizer::PullAudio()
{
	// One pull per wake-up, bounded by the segment cap.
	const size_t cap = MsToSamples(kMaxSegmentMs);
	float block[1024];
	while (audio_.size() < cap) {
		const size_t got = ring_.Read(block, std::min<size_t>(1024, cap - audio_.size()));
		if (got == 0) {
			break;
		}
		audio_.insert(audio_.end(), block, block + got);
	}
}

std::string Recognizer::BuildPrompt()
{
	std::string prompt;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		prompt = prompt_;
	}
	if (prompt.empty() || !ctx_) {
		return std::string();
	}
	// whisper keeps the last n_text_ctx/2 - 1 tokens of a long prompt (the take from
	// prompt_past1 in whisper_full_with_state); trimming to the same budget here keeps
	// what whisper would keep, on a word boundary, and keeps it visible.
	const int budget = whisper_n_text_ctx(ctx_) / 2 - 1;
	whisper_context *ctx = ctx_;
	return TrimPromptFront(prompt, budget, [ctx](const std::string &text) { return CountTokens(ctx, text); });
}

Recognizer::Result Recognizer::Transcribe(int threads)
{
	Result result;
	result.mutedSeen = takeMutedSeen_ ? takeMutedSeen_() : false;

	if (audio_.size() - preRollTaken_ < MsToSamples(kMinSegmentMs)) {
		result.error = "That was too short to hear.";
		return result;
	}

	const std::string prompt = BuildPrompt();
	std::string language;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		language = language_;
	}
	whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	params.n_threads = std::max(1, threads);
	// An English-only model ignores it; the multilingual one needs it, or it would
	// guess the language from every short command.
	params.language = language.c_str();
	params.translate = false;
	params.no_timestamps = true;
	params.single_segment = false;
	params.print_progress = false; // defaults to true, and would spam the log
	params.print_realtime = false;
	params.print_timestamps = false;
	params.print_special = false;
	params.suppress_blank = true;
	params.no_context = true; // each command stands alone
	params.temperature = 0.0f;
	params.initial_prompt = prompt.empty() ? nullptr : prompt.c_str();
	params.abort_callback = &Recognizer::ShouldAbort;
	params.abort_callback_user_data = this;

	const int64_t started = SteadyMs();
	deadlineMs_.store(started + kStaleMs, std::memory_order_release);
	const int rc = whisper_full(ctx_, params, audio_.data(), static_cast<int>(audio_.size()));
	result.inferenceMs = SteadyMs() - started;
	deadlineMs_.store(0, std::memory_order_release);

	if (rc != 0) {
		result.error = result.inferenceMs > kStaleMs ? "Recognition took too long." : "Recognition failed.";
		DBG(LogCat::Voice, "recognizer: whisper_full returned %d after %lld ms", rc,
		    static_cast<long long>(result.inferenceMs));
		return result;
	}
	std::string text;
	const int segments = whisper_full_n_segments(ctx_);
	for (int i = 0; i < segments; ++i) {
		const char *part = whisper_full_get_segment_text(ctx_, i);
		if (part) {
			text += part;
		}
	}
	result.text = CleanTranscript(text);
	result.ok = !result.text.empty();
	if (!result.ok) {
		result.error = "I did not hear anything.";
	}
	// Sizes and timings only: the transcript itself is never logged here.
	DBG(LogCat::Voice, "recognizer: %zu samples, %lld ms inference, %s", audio_.size(),
	    static_cast<long long>(result.inferenceMs), result.ok ? "text" : "empty");
	return result;
}

// Worker, after the speech model loaded: the voice activity detector and the tiny wake
// model. Either failing leaves push-to-talk working, which is what the user had before
// turning always-listen on, so it is logged rather than reported as a model failure.
bool Recognizer::StartContinuous()
{
	std::string error;
	if (!vad_.Start(continuous_.vadModelPath, 1, error)) {
		HostLog("[voice] " + error + "; always-listen is off, push-to-talk still works");
		return false;
	}
	whisper_context_params wakeParams = whisper_context_default_params();
	wakeParams.use_gpu = false;
	// UTF-8 as is, like the speech model (LoadModel).
	wakeCtx_ = whisper_init_from_file_with_params(continuous_.wakeModelPath.c_str(), wakeParams);
	if (!wakeCtx_) {
		HostLog("[voice] could not load the wake model; always-listen is off, push-to-talk still works");
		vad_.Stop();
		return false;
	}
	DBG(LogCat::Voice, "recognizer: always-listen up");
	return true;
}

// One pass of the always-listen loop: run what has arrived past the detector, a few
// whole detector windows at a time, and act on what it reports. Returns as soon as a
// key opens a segment (that segment owns the audio from then on) or Stop begins.
void Recognizer::ContinuousStep()
{
	constexpr size_t kWindow = VadEndpointer::kWindowSamples;
	float block[kWindow * 3]; // 96 ms
	for (;;) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (quit_ || segment_ != Segment::Idle) {
				return;
			}
		}
		// Whole windows only, so what the detector judged is exactly what was read.
		const size_t whole = ring_.Available() / kWindow * kWindow;
		if (whole == 0) {
			return;
		}
		const size_t got = ring_.Read(block, std::min(std::size(block), whole));
		ContinuousBlock(block, got, vad_.Push(block, got));
	}
}

void Recognizer::ContinuousBlock(const float *block, size_t count, VadEndpointer::State state)
{
	switch (utterance_) {
	case Utterance::None:
		if (state == VadEndpointer::State::Silence) {
			KeepPreRoll(block, count);
			return;
		}
		BeginUtterance(block, count);
		if (state == VadEndpointer::State::Ended) {
			ConcludeUtterance(); // all of it fit in one block
		}
		return;

	case Utterance::Collecting:
		audio_.insert(audio_.end(), block, block + count);
		KeepPreRoll(block, count);
		if (state == VadEndpointer::State::Ended) {
			ConcludeUtterance();
		} else if (audio_.size() >= MsToSamples(kWakeWindowMs)) {
			// Decide as soon as there is enough to decide on, so the indicator can
			// light up while the user is still speaking.
			DecideWake();
		}
		return;

	case Utterance::Dismissed:
		// Not addressed to us: the rest of it is ignored, so a long stretch of talk costs
		// one wake check, not one per two seconds.
		KeepPreRoll(block, count);
		if (state == VadEndpointer::State::Ended) {
			utterance_ = Utterance::None;
		}
		return;

	case Utterance::Woken:
		audio_.insert(audio_.end(), block, block + count);
		KeepPreRoll(block, count);
		if (state == VadEndpointer::State::Ended) {
			FinishWoken();
		}
		return;
	}
}

// The detector heard speech start in `block`. The utterance starts with the pre-roll
// before it (the detector needs a window or two to be sure, and the first syllable is
// in there), then the block itself.
void Recognizer::BeginUtterance(const float *block, size_t count)
{
	StartCollecting();
	audio_.insert(audio_.end(), block, block + count);
	// The pre-roll keeps rolling from here, so a key pressed mid-utterance still gets
	// the 300 ms before it.
	KeepPreRoll(block, count);
	utterance_ = Utterance::Collecting;
	// The mute flag is about this utterance only. Taken under the state mutex, and only
	// while no segment is open: once a key has opened one, the flag is that segment's.
	std::lock_guard<std::mutex> lock(mutex_);
	if (segment_ == Segment::Idle && takeMutedSeen_) {
		takeMutedSeen_();
	}
}

void Recognizer::DecideWake()
{
	if (!WakeCheck() || !ClaimWake()) {
		// Not addressed to us (or a key press got there first, and owns what follows).
		// No result, no cue, no transcript anywhere.
		audio_.clear();
		utterance_ = Utterance::Dismissed;
		return;
	}
	utterance_ = Utterance::Woken;
	if (onWake_ && !stopping_.load(std::memory_order_acquire)) {
		onWake_();
	}
}

// The detector ended an utterance that was still being collected: one shorter than the
// wake window. A blip shorter than kMinSegmentMs of sound (a cough, a click) is not
// worth a wake check at all; the silence that ended it does not count.
void Recognizer::ConcludeUtterance()
{
	if (vad_.SpeechMs() < kMinSegmentMs || !WakeCheck() || !ClaimWake()) {
		audio_.clear();
		utterance_ = Utterance::None;
		return;
	}
	utterance_ = Utterance::Woken;
	if (onWake_ && !stopping_.load(std::memory_order_acquire)) {
		onWake_();
	}
	FinishWoken();
}

// The tiny model over the opening of the utterance, then the wake gate. True when it was
// addressed to us. English always: the tiny model is English-only, and the wake phrase
// is a name rather than a sentence in the user's language.
bool Recognizer::WakeCheck()
{
	if (!wakeCtx_ || audio_.empty()) {
		return false;
	}
	const size_t window = std::min(audio_.size(), MsToSamples(kWakeWindowMs));
	whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	params.n_threads = 1; // the wake check is short and must not steal the encoder's cores
	params.language = "en";
	params.translate = false;
	params.no_timestamps = true;
	params.print_progress = false;
	params.print_realtime = false;
	params.print_timestamps = false;
	params.print_special = false;
	params.suppress_blank = true;
	params.no_context = true;
	params.temperature = 0.0f;
	// The phrase itself biases the tiny model toward spelling it as the user does (the
	// spec's decoding bias); the match below is still the judge.
	params.initial_prompt = continuous_.wakePhrase.empty() ? nullptr : continuous_.wakePhrase.c_str();
	params.abort_callback = &Recognizer::ShouldAbort;
	params.abort_callback_user_data = this;
	const int64_t started = SteadyMs();
	if (whisper_full(wakeCtx_, params, audio_.data(), static_cast<int>(window)) != 0) {
		return false;
	}
	std::string text;
	const int segments = whisper_full_n_segments(wakeCtx_);
	for (int i = 0; i < segments; ++i) {
		const char *part = whisper_full_get_segment_text(wakeCtx_, i);
		if (part) {
			text += part;
		}
	}
	const WakeResult wake = MatchWakePhrase(CleanTranscript(text), continuous_.wakePhrase);
	// Timings and the verdict only: what was said is never logged here.
	DBG(LogCat::Voice, "recognizer: wake check over %zu ms in %lld ms: %s (score %.2f)",
	    window * 1000 / static_cast<size_t>(kVoiceSampleRate), static_cast<long long>(SteadyMs() - started),
	    wake.matched ? "woken" : "not for us", wake.score);
	return wake.matched;
}

// The wake phrase was heard: the utterance owns the recognizer until its result, unless
// a key opened a segment in the meantime (the key wins; the utterance is dropped).
bool Recognizer::ClaimWake()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (quit_ || segment_ != Segment::Idle) {
		return false;
	}
	wakeBusy_ = true;
	return true;
}

// The woken utterance is over: the speech model transcribes all of it and the wake
// phrase comes off the front. A result always follows the wake callback, even an empty
// one, because the listener is waiting for it.
void Recognizer::FinishWoken()
{
	Result result = Transcribe(threads_);
	result.wake = true;
	if (result.ok) {
		// The tiny model only decided; the text comes from the speech model, which may
		// split or spell the phrase differently, so it is matched again here, and an
		// utterance it does not open is dropped unread (ConfirmWake).
		const WokenUtterance woken = ConfirmWake(result.text, continuous_.wakePhrase);
		result.ok = woken.ok;
		result.text = woken.ok ? woken.command : std::string();
		result.error = woken.reason;
		if (!woken.ok) {
			DBG(LogCat::Voice, "recognizer: woken utterance dropped: %s", woken.reason.c_str());
		}
	}
	audio_.clear();
	utterance_ = Utterance::None;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		wakeBusy_ = false;
	}
	if (onResult_ && !stopping_.load(std::memory_order_acquire)) {
		onResult_(std::move(result));
	}
}

// A key opened a segment: whatever always-listen was collecting is dropped (a woken
// utterance cannot be in progress, since BeginSegment refuses then), and the detector
// starts over when listening resumes, since the stream it saw has a gap in it.
void Recognizer::AbandonUtterance()
{
	if (utterance_ != Utterance::None) {
		DBG(LogCat::Voice, "recognizer: always-listen utterance set aside for push-to-talk");
	}
	audio_.clear();
	utterance_ = Utterance::None;
	vad_.Reset();
}

void Recognizer::WorkerMain(std::string modelPath, int threads)
{
	// Voice must never cost the encoder a frame. This covers our own thread; whisper's
	// internal workers set their own priority (index correction 2), which is why the
	// thread count is capped by the P0 measurements rather than left to whisper.
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

	threads_ = std::max(1, threads);
	std::string error;
	const bool loaded = LoadModel(modelPath, error);
	const bool continuous = loaded && continuous_.enabled && StartContinuous();
	continuousActive_.store(continuous, std::memory_order_release);
	ready_.store(loaded, std::memory_order_release);
	if (onModel_) {
		onModel_(loaded, error);
	}
	if (!loaded) {
		return;
	}

	// The loop polls every kPollMs rather than sleeping until an event: while idle it
	// still has work to do (keeping the pre-roll fresh), and BeginSegment on the
	// hotkey thread must not have to wait for anything.
	constexpr int kPollMs = 20;
	bool collecting = false;
	bool keyHeld = false; // always-listen set aside while a key's segment runs
	for (;;) {
		Segment state = Segment::Idle;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait_for(lock, std::chrono::milliseconds(kPollMs), [&] { return quit_; });
			if (quit_) {
				break;
			}
			state = segment_;
			if (state == Segment::Closing) {
				segment_ = Segment::Busy;
			}
		}

		if (state == Segment::Idle) {
			collecting = false;
			keyHeld = false;
			if (continuous) {
				ContinuousStep();
			} else {
				DrainToPreRoll();
			}
			continue;
		}
		if (continuous && !keyHeld) {
			// Before StartCollecting: the segment takes the pre-roll and audio_ over.
			AbandonUtterance();
			keyHeld = true;
		}
		if (state == Segment::Open) {
			if (!collecting) {
				StartCollecting();
				collecting = true;
			}
			PullAudio();
			continue;
		}
		if (state == Segment::Busy) {
			continue; // a result is already being delivered
		}

		// Closing: take whatever arrived between the key release and now, then run.
		if (!collecting) {
			StartCollecting();
		}
		collecting = false;
		PullAudio();
		Result result = Transcribe(threads);
		audio_.clear();
		{
			std::lock_guard<std::mutex> lock(mutex_);
			segment_ = Segment::Idle;
		}
		// Nobody is waiting for a result once Stop has begun.
		if (onResult_ && !stopping_.load(std::memory_order_acquire)) {
			onResult_(std::move(result));
		}
	}

	if (wakeCtx_) {
		whisper_free(wakeCtx_);
		wakeCtx_ = nullptr;
	}
	vad_.Stop();
	continuousActive_.store(false, std::memory_order_release);
	if (ctx_) {
		whisper_free(ctx_);
		ctx_ = nullptr;
	}
	ready_.store(false, std::memory_order_release);
}

} // namespace Voice
