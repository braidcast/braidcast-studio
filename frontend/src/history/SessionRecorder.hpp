#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Schema.hpp"

namespace History {

// One destination as it was at go-live, with the metadata actually sent rather
// than what the profile says today.
struct DestinationRecord {
	std::string bindingUuid;
	std::string profileId;
	std::string platform;
	std::string accountLabel;
	std::string title;
	std::string category;
	std::vector<std::string> tags;
};

struct SessionStart {
	int64_t startedAtMs = 0;
	std::string title;
	std::string scheduleId; // empty in Phase 1; the column exists for Phase 2
	std::vector<std::string> canvasUuids;
	std::vector<DestinationRecord> destinations;
};

// One tick off the host sampler. The frame counters are cumulative-since-
// baseline, exactly as the snapshot reports them -- the recorder differences
// them itself.
struct HealthSample {
	int64_t tMs = 0;
	int64_t bitrateKbps = 0;
	int64_t cumulativeDroppedFrames = 0;
	int64_t cumulativeEncodeSkipped = 0;
	double congestionPct = 0;
	double cpuPct = 0;
};

// Interval between persisted health rows. One second of resolution for a
// four-hour broadcast would be 14400 rows to answer a question nobody asks;
// ten seconds still resolves "skips spiked at 21:40".
inline constexpr int64_t kHealthIntervalMs = 10'000;

// The only writer during a broadcast. Clock-free: every timestamp arrives as an
// argument, so the whole write path is testable without a broadcast or a real
// second passing.
//
// UI thread only.
class SessionRecorder {
public:
	SessionRecorder() = default;
	~SessionRecorder() = default;
	SessionRecorder(const SessionRecorder &) = delete;
	SessionRecorder &operator=(const SessionRecorder &) = delete;

	bool Attach(const std::string &path);
	void Detach();
	bool IsAttached() const { return storage_ != nullptr; }

	bool IsRecording() const { return !currentId_.empty(); }
	const std::string &CurrentId() const { return currentId_; }
	const std::string &LastError() const { return lastError_; }

	// Open a session row with ended_at null plus one destination row each.
	// Returns the new session id, or empty on failure.
	std::string Begin(const SessionStart &start);

	// Feed one sampler tick. A no-op when not recording. Persists a row only
	// when kHealthIntervalMs has elapsed since the last persisted one.
	void OnSample(const HealthSample &sample);

	// Record how a single destination finished. Safe to call more than once
	// for the same binding -- the engine's ended callback is documented as
	// firing twice for a deliberate stop whose stop signal also fires.
	void OnDestinationEnded(const std::string &bindingUuid, const std::string &finalState,
				const std::string &error);

	// Close the session. `reason` is one of "ended", "crashed", "failed".
	void End(int64_t endedAtMs, const std::string &reason);

	// Attach a chosen thumbnail to the running session.
	void SetThumbnail(const std::string &relPath);

private:
	std::unique_ptr<Storage> storage_;
	std::string currentId_;
	std::unordered_map<std::string, std::string> bindings_; // bindingUuid -> destination row id
	// The last sample actually written, which is what the next delta is taken
	// against -- not the last sample seen, since the ticks in between were
	// dropped by the downsample and their frames belong to this interval.
	HealthSample lastPersisted_;
	bool havePersisted_ = false;
	std::string lastError_;
};

} // namespace History
