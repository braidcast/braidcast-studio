#pragma once

#include "diag/capture_rate.hpp"
#include "multistream/VideoGate.hpp"

#include <obs.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace CaptureRate {

// Feeds the tracker from the live source graph once per stats tick, while any
// output is live or a viewer holds a lease (stats.watchCaptures), and holds the
// session the stop edge summarizes.
//
// Owned by ObsBootstrap rather than the bridge: the stop edge that ends a session
// also runs during shutdown, after the bridge is gone.
//
// Across ticks only weak refs are held. A strong ref kept past the walk that took
// it could make the sampler the last holder, and its release would then destroy
// the source here on the UI thread.
//
// UI thread only.
class Sampler {
public:
	// Samples the sources `roots` reach, if a live output or a lease wants it;
	// otherwise forgets the previous sample so the next one starts clean.
	void Tick(const std::vector<VideoGate::Root> &roots, uint64_t nowNs);

	// The last sample's rows, shaped for stats.get; an empty array while idle.
	nlohmann::json Payload() const;

	// stats.watchCaptures: sample for kLeaseNs from now even with nothing live.
	void Watch(uint64_t nowNs);

	// stats.reset: rebase the rows' "since reset" windows. Session sums stay.
	void ResetWindows();

	void SessionBegin();
	// Logs the session's one-line summary. A no-op without an open session.
	void SessionEnd();

	// Drop every weak ref and all state. Called from ObsBootstrap::Stop while
	// libobs is still up.
	void Clear();

	// Answers "is an output live on this canvas" in place of the engine, so the
	// capture-rate self-test can exercise the reference-fps rules without a real
	// broadcast. Pass nullptr to restore the engine.
	void SetCanvasLiveOverrideForTest(std::function<bool(const std::string &)> fn);

	static constexpr uint64_t kLeaseNs = 5'000'000'000ull;

private:
	struct Held {
		OBSWeakSource weak;
		uint64_t identity = 0;
	};

	bool CanvasLive(const std::string &canvasUuid) const;
	bool WantsSample(uint64_t nowNs) const;

	Tracker tracker_;
	std::map<std::string, Held> held_;
	uint64_t nextIdentity_ = 1;
	uint64_t lastSampleNs_ = 0;
	uint64_t leaseUntilNs_ = 0;
	std::function<bool(const std::string &)> liveOverride_;
};

} // namespace CaptureRate
