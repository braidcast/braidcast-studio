#include "voice/Recognizer.hpp"

#include "log.hpp"
#include "util/string_util.hpp"
#include "voice/VoiceResampler.hpp"

#include <whisper.h>

#include <windows.h>

#include <algorithm>
#include <chrono>

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

void Recognizer::SetPrompt(std::string prompt)
{
	std::lock_guard<std::mutex> lock(mutex_);
	prompt_ = std::move(prompt);
}

void Recognizer::Start(const std::string &modelPath, int threads)
{
	Stop();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		quit_ = false;
		segment_ = Segment::Idle;
	}
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
	if (segment_ != Segment::Idle) {
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
	if (preRoll_.empty()) {
		preRoll_.assign(MsToSamples(kPreRollMs), 0.f);
	}
	float block[1024];
	for (;;) {
		const size_t got = ring_.Read(block, 1024);
		if (got == 0) {
			return;
		}
		for (size_t i = 0; i < got; ++i) {
			preRoll_[preRollPos_] = block[i];
			preRollPos_ = (preRollPos_ + 1) % preRoll_.size();
			if (preRollPos_ == 0) {
				preRollFull_ = true;
			}
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
	whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	params.n_threads = std::max(1, threads);
	params.language = "en";
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

void Recognizer::WorkerMain(std::string modelPath, int threads)
{
	// Voice must never cost the encoder a frame. This covers our own thread; whisper's
	// internal workers set their own priority (index correction 2), which is why the
	// thread count is capped by the P0 measurements rather than left to whisper.
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

	std::string error;
	const bool loaded = LoadModel(modelPath, error);
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
			DrainToPreRoll();
			continue;
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

	if (ctx_) {
		whisper_free(ctx_);
		ctx_ = nullptr;
	}
	ready_.store(false, std::memory_order_release);
}

} // namespace Voice
