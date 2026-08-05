#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace History {

class SessionRecorder;

// Sampled every 5 minutes rather than grabbed once at go-live: a single grab
// catches the "Starting soon" card, which is the one frame nobody wants as the
// image for their stream.
inline constexpr int64_t kThumbIntervalMs = 5 * 60 * 1000;

// A four-hour broadcast would otherwise leave 48 files on disk to pick one
// from.
inline constexpr int kMaxCandidates = 12;

// A stream that ends before the first 5-minute mark still needs an image.
inline constexpr int64_t kFirstGrabMs = 60 * 1000;

// Long edge of a stored candidate. Small enough that the downscale is worth
// doing on the GPU, large enough for a history card.
inline constexpr uint32_t kThumbLongEdge = 320;

// Mean luminance below this reads as a blank or near-blank frame -- a black
// screen between scenes, or a stream that ended on a fade.
inline constexpr double kBlankLumaThreshold = 8.0;

// Absolute path of the thumbnail directory, created on demand.
std::string ThumbnailDir();

bool IsBlank(const std::vector<uint8_t> &rgba);

// Candidate frames for the running session, in the config directory beside the
// database. Files rather than blobs: a blob would bloat every query that
// touches sessions.
//
// UI thread only -- the grab enters the obs graphics context.
class ThumbnailSampler {
public:
	// Called on each sampler tick. Grabs at most one frame, and only when due.
	void OnTick(int64_t sessionElapsedMs, const std::string &sessionId);

	// Choose the last non-blank candidate, stamp it on the session, and delete
	// the rest. Safe when there are no candidates.
	void Finalize(SessionRecorder &recorder);

	void Reset();

private:
	// Blankness is judged at grab time, while the decoded pixels are still in
	// hand. Deciding at Finalize instead would mean a WIC decode per candidate to
	// recover what we already had.
	struct Candidate {
		std::string name; // file name, relative to ThumbnailDir()
		bool blank = false;
	};

	std::vector<Candidate> candidates_;
	int64_t lastGrabMs_ = 0;
	bool haveFirst_ = false;
};

} // namespace History
