#include "perf_repro_selftest.hpp"

#include <obs.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <util/platform.h>

#include "bridge.hpp"
#include "event_names.hpp"
#include "log.hpp"
#include "obs_bootstrap.hpp"
#include "scene_collections.hpp"
#include "session_log.hpp"

#include "chat/chat_hub.hpp"
#include "chat/viewer_poller.hpp"

#include "multistream/MultistreamEngine.hpp"
#include "multistream/OutputBindingStore.hpp"
#include "multistream/StreamProfileStore.hpp"

#include "oauth/account_store.hpp"
#include "oauth/provider.hpp"
#include "oauth/registry.hpp"

namespace {

// State-machine phases, advanced one step per WM_TIMER tick (see
// ObsBootstrap::RunPerfReproSelfTest). BootSettle/PollLive/Measuring are the only
// phases that legitimately span multiple ticks; every other phase runs to
// completion within the tick it is entered on (see the `continue`-driven loop).
enum class Phase {
	Idle,
	BootSettle,
	CheckConnected,
	EnableDebug,
	SwitchCollection,
	ForcePrivacy,
	Starting,
	PollLive,
	Measuring,
	Stopping,
	WriteSummary,
	Finished,
};

// One enabled output binding routed to a connected YouTube account -- the set
// ForcePrivacy applies broadcast-private to. Other enabled bindings (Twitch/Kick/
// custom RTMP) are left untouched and still go live via StartAllEnabled, matching
// the "drive the REAL setup" scope decision.
struct MatchedBinding {
	std::string bindingUuid;
	std::string profileUuid;
	std::string accountId;
};

// Worst-case (per mix) composite timing parsed out of the [render-debug] log
// lines emitted during the measurement window.
struct MixLoad {
	std::string name;
	double cpuPct = 0.0;
	double gpuPct = 0.0;
};

struct State {
	Phase phase = Phase::Idle;
	int exitCode = -1; // -1 = not yet decided; see PerfReproSelfTestExitCode()
	std::string failReason;

	std::chrono::steady_clock::time_point phaseStart;
	std::chrono::steady_clock::time_point pollLiveStart;
	std::chrono::steady_clock::time_point measureStart;
	std::chrono::steady_clock::time_point lastSampleAt;

	int durationSec = 90;
	double maxLagPct = 2.0;

	std::vector<MatchedBinding> targets;

	bool debugWasSet = false;
	bool originalDebugEnabled = false;

	bool measured = false;
	std::streamoff logOffsetAtMeasureStart = 0;
	double worstRenderLagPct = 0.0;
	double worstEncodeSkipPct = 0.0;
	std::unordered_map<std::string, double> worstDropPctByBinding;
	std::unordered_map<std::string, std::string> bindingLabel;
};

State g_state;

constexpr std::chrono::milliseconds kBootSettle{3000};
constexpr std::chrono::seconds kPollLiveTimeout{30};
constexpr std::chrono::seconds kSampleInterval{1};

int EnvInt(const char *name, int fallback)
{
	const char *v = getenv(name);
	if (!v || !*v) {
		return fallback;
	}
	const int n = atoi(v);
	return n > 0 ? n : fallback;
}

double EnvDouble(const char *name, double fallback)
{
	const char *v = getenv(name);
	if (!v || !*v) {
		return fallback;
	}
	char *end = nullptr;
	const double d = strtod(v, &end);
	return (end && end != v) ? d : fallback;
}

// <config>/braidcast/selftest/<file>, mirroring the shape of MultistreamBasicPath
// (StorePaths.hpp) / SessionLog's "braidcast/logs" -- a fresh subdir, so the same
// one-line os_get_config_path idiom, not a shared helper (there is nothing to
// factor: each caller resolves a different, genuinely distinct subdir).
std::string SelfTestConfigPath(const std::string &file)
{
	char base[512];
	if (os_get_config_path(base, sizeof(base), "braidcast/selftest") <= 0) {
		return std::string();
	}
	os_mkdirs(base);
	std::string path = base;
	path += "/";
	path += file;
	return path;
}

// Reuse SessionLog's own per-session timestamp (its filename is already
// "YYYY-MM-DD HH-MM-SS.txt") instead of a fresh time() call, per the "reuse the
// session log's timestamp" instruction.
std::string TimestampFromSessionLog()
{
	const std::string logPath = SessionLog::CurrentPath();
	if (logPath.empty()) {
		return "unknown";
	}
	const size_t slash = logPath.find_last_of("/\\");
	const std::string base = slash == std::string::npos ? logPath : logPath.substr(slash + 1);
	const size_t dot = base.rfind(".txt");
	return dot == std::string::npos ? base : base.substr(0, dot);
}

std::streamoff CurrentLogSize()
{
	const std::string logPath = SessionLog::CurrentPath();
	if (logPath.empty()) {
		return 0;
	}
	std::ifstream in(logPath, std::ios::binary | std::ios::ate);
	if (!in) {
		return 0;
	}
	return in.tellg();
}

// Parse every "[render-debug] mix '<name>': composite CPU .. (<cpu>% frame), GPU
// .. (<gpu>% frame)" line (libobs/obs-video.c debug_emit_composite_stats) written
// to the session log at or after `fromOffset`, track each mix's worst (max) CPU%/
// GPU% sample, and return them ranked by (cpuPct + gpuPct) descending -- the
// dominant-canvas ranking the summary reports. render-debug is log-only (no
// getter), so the session log is the only channel to read it back from.
std::vector<MixLoad> RankRenderDebugMixes(std::streamoff fromOffset)
{
	std::unordered_map<std::string, MixLoad> worst;
	const std::string logPath = SessionLog::CurrentPath();
	if (logPath.empty()) {
		return {};
	}
	std::ifstream in(logPath, std::ios::binary);
	if (!in) {
		return {};
	}
	in.seekg(fromOffset, std::ios::beg);

	const std::string marker = "[render-debug] mix '";
	std::string line;
	while (std::getline(in, line)) {
		const size_t p = line.find(marker);
		if (p == std::string::npos) {
			continue;
		}
		const size_t nameStart = p + marker.size();
		const size_t nameEnd = line.find('\'', nameStart);
		if (nameEnd == std::string::npos) {
			continue;
		}
		const size_t cpuOpen = line.find('(', nameEnd);
		const size_t cpuPctEnd = cpuOpen == std::string::npos ? std::string::npos : line.find('%', cpuOpen);
		if (cpuPctEnd == std::string::npos) {
			continue;
		}
		const size_t gpuOpen = line.find('(', cpuPctEnd);
		const size_t gpuPctEnd = gpuOpen == std::string::npos ? std::string::npos : line.find('%', gpuOpen);
		if (gpuPctEnd == std::string::npos) {
			continue;
		}

		double cpuPct = 0.0;
		double gpuPct = 0.0;
		try {
			cpuPct = std::stod(line.substr(cpuOpen + 1, cpuPctEnd - cpuOpen - 1));
			gpuPct = std::stod(line.substr(gpuOpen + 1, gpuPctEnd - gpuOpen - 1));
		} catch (const std::exception &) {
			continue;
		}

		const std::string name = line.substr(nameStart, nameEnd - nameStart);
		MixLoad &slot = worst[name];
		slot.name = name;
		slot.cpuPct = std::max(slot.cpuPct, cpuPct);
		slot.gpuPct = std::max(slot.gpuPct, gpuPct);
	}

	std::vector<MixLoad> ranked;
	ranked.reserve(worst.size());
	for (auto &entry : worst) {
		ranked.push_back(entry.second);
	}
	std::sort(ranked.begin(), ranked.end(), [](const MixLoad &a, const MixLoad &b) {
		return (a.cpuPct + a.gpuPct) > (b.cpuPct + b.gpuPct);
	});
	return ranked;
}

// Enabled output bindings whose stream profile links a currently-connected
// YouTube account. Empty covers both "YouTube not connected" and "connected but
// no enabled binding routes to it" -- both are a clean skip, nothing to repro.
std::vector<MatchedBinding> CollectYoutubeTargets()
{
	std::vector<std::string> connectedYoutubeAccounts;
	for (const auto &entry : OAuth::Accounts().All()) {
		const OAuth::OAuthAccount &acct = entry.second;
		if (acct.providerId == "youtube" && OAuth::IsAccountConnected(acct)) {
			connectedYoutubeAccounts.push_back(OAuth::AccountId(acct));
		}
	}
	if (connectedYoutubeAccounts.empty()) {
		return {};
	}

	std::vector<MatchedBinding> out;
	for (const OutputBinding &b : ObsBootstrap::OutputBindings().Bindings().bindings) {
		if (!b.enabled || b.profileUuid.empty()) {
			continue;
		}
		StreamProfile *profile = ObsBootstrap::StreamProfiles().Find(b.profileUuid);
		if (!profile || profile->accountId.empty()) {
			continue;
		}
		if (std::find(connectedYoutubeAccounts.begin(), connectedYoutubeAccounts.end(), profile->accountId) ==
		    connectedYoutubeAccounts.end()) {
			continue;
		}
		out.push_back(MatchedBinding{b.uuid, b.profileUuid, profile->accountId});
	}
	return out;
}

// Force broadcast-private on every matched binding via the SAME provider call
// streamMeta.set drives (bridge.cpp's MethodStreamMetaSet is anonymous-namespace
// internal-linkage, so it cannot be called directly; this mirrors its
// not-connected/provider-lookup contract and calls the real, public
// StreamProvider::applyMetadata -- the actual network path -- rather than
// reimplementing it). Synchronous/blocking by design (see the header comment):
// applyMetadata's own HTTP calls (SendAuthed) are already blocking, streamMeta.set
// only offloads them to a worker thread to keep the CEF UI thread responsive,
// which does not matter for this timer-driven, non-interactive self-test.
//
// Verified: privacy/title are NOT persisted anywhere -- YouTubeProvider::
// applyMetadata (youtube_provider.cpp) sends them only in the ephemeral
// liveBroadcasts.insert body for a fresh create-per-go-live broadcast. The one
// persisted side effect is WriteIngestToProfile writing the new broadcast's RTMP
// ingest server/key into the profile (streams.json) -- but that is the SAME
// mutation every real go-live already performs (the next real go-live overwrites
// it again with its own fresh broadcast), so there is nothing self-test-specific
// to snapshot/restore here.
bool ForcePrivacyOnTargets(const std::vector<MatchedBinding> &targets, std::string &error)
{
	for (const MatchedBinding &t : targets) {
		std::optional<OAuth::OAuthAccount> stored = OAuth::Accounts().Get(t.accountId);
		if (!stored) {
			error = "account " + t.accountId + " vanished before go-live";
			return false;
		}
		OAuth::StreamProvider *provider = OAuth::Registry().Get(stored->providerId);
		if (!provider) {
			error = "no provider registered for " + stored->providerId;
			return false;
		}
		OAuth::OAuthAccount acct = *stored;
		const Bridge::json fields{
			{"privacy", "private"},
			{"title", "Braidcast perf-repro selftest"},
		};
		std::string err;
		bool ok = false;
		try {
			ok = provider->applyMetadata(acct, t.profileUuid, fields, err);
		} catch (const std::exception &e) {
			error = std::string("applyMetadata threw: ") + e.what();
			return false;
		}
		if (!ok) {
			error = "force-private failed for binding " + t.bindingUuid + ": " + err;
			return false;
		}
		Bridge::EmitEvent(EventNames::kStreamMetaChanged, Bridge::json{{"profileUuid", t.profileUuid}});
	}
	return true;
}

// True iff every ENABLED binding (not just the YouTube targets) is State::Live.
// Waiting on the whole set, not just the YouTube targets, matches "drive the REAL
// setup": a Twitch/Kick sibling binding still exercises the same fan-out encoder
// path the compositor-starvation incident hit.
bool AllEnabledLive(std::string &notLiveSummary)
{
	bool allLive = true;
	std::ostringstream oss;
	for (const MultistreamEngine::OutputStatus &s : ObsBootstrap::Multistream().Statuses()) {
		if (s.state != MultistreamEngine::State::Live) {
			allLive = false;
			oss << " [" << s.profileLabel << "/" << s.canvasName << ": " << MultistreamEngine::StateName(s.state);
			if (!s.lastError.empty()) {
				oss << " (" << s.lastError << ")";
			}
			oss << "]";
		}
	}
	notLiveSummary = oss.str();
	return allLive;
}

// One measurement sample via "stats.get" -- the SAME synchronous bridge method
// (bridge.cpp:MethodStatsGet) the Stats dock polls, so renderLagPct/encodeSkipPct/
// dropPct are computed by the real stats source (obs_get_lagged_frames() /
// obs_get_total_frames() / video_output_get_skipped_frames / per-output
// obs_output_get_frames_dropped), not re-derived here. Unlike streamMeta.set,
// "stats.get" IS registered on the synchronous g_methods table, so Bridge::
// Dispatch is the real, non-reimplemented path.
void SampleStats(State &st)
{
	Bridge::json result;
	std::string err;
	if (!Bridge::Dispatch("stats.get", Bridge::json(nullptr), result, err)) {
		return;
	}
	if (result.contains("general") && result["general"].is_object()) {
		const Bridge::json &g = result["general"];
		st.worstRenderLagPct = std::max(st.worstRenderLagPct, g.value("renderLagPct", 0.0));
		st.worstEncodeSkipPct = std::max(st.worstEncodeSkipPct, g.value("encodeSkipPct", 0.0));
	}
	if (result.contains("outputs") && result["outputs"].is_array()) {
		for (const Bridge::json &o : result["outputs"]) {
			const std::string bindingUuid = o.value("bindingUuid", std::string());
			if (bindingUuid.empty()) {
				continue;
			}
			double &worst = st.worstDropPctByBinding[bindingUuid];
			worst = std::max(worst, o.value("dropPct", 0.0));
			st.bindingLabel[bindingUuid] =
				o.value("profileLabel", std::string("?")) + " / " + o.value("canvasName", std::string("?"));
		}
	}
}

// Mirrors bridge.cpp's MethodStreamingStop body (also anonymous-namespace
// internal-linkage, so it cannot be called directly): stop the live-only chat/
// viewer pollers, clear each connected account's active-broadcast target (a
// no-op except on YouTube), then StopAll -- the SAME "streaming.stop" sequence a
// real stop drives.
void StopStreamingLikeBridge()
{
	Chat::Viewers().Stop();
	Chat::Hub().Stop();
	for (const auto &entry : OAuth::Accounts().All()) {
		OAuth::StreamProvider *provider = OAuth::Registry().Get(entry.second.providerId);
		if (provider) {
			provider->clearActiveBroadcast(entry.first);
		}
	}
	ObsBootstrap::Multistream().StopAll();
	Bridge::EmitEvent(EventNames::kStreamingChanged, Bridge::json{{"active", false}});
}

void WriteSummary(State &st)
{
	if (st.exitCode < 0) {
		st.exitCode = st.worstRenderLagPct > st.maxLagPct ? 1 : 0;
	}
	const char *resultName = st.exitCode == 0   ? "PASS"
				  : st.exitCode == 1 ? "FAIL"
				  : st.exitCode == 2 ? "SKIP"
						     : "ERROR";

	const std::vector<MixLoad> ranked = st.measured ? RankRenderDebugMixes(st.logOffsetAtMeasureStart)
							 : std::vector<MixLoad>();

	Bridge::json ranksJson = Bridge::json::array();
	for (const MixLoad &m : ranked) {
		ranksJson.push_back(
			Bridge::json{{"mix", m.name}, {"compositeCpuPct", m.cpuPct}, {"compositeGpuPct", m.gpuPct}});
	}

	Bridge::json outputsJson = Bridge::json::array();
	for (const auto &entry : st.worstDropPctByBinding) {
		const auto labelIt = st.bindingLabel.find(entry.first);
		outputsJson.push_back(Bridge::json{
			{"bindingUuid", entry.first},
			{"label", labelIt != st.bindingLabel.end() ? labelIt->second : std::string()},
			{"worstDropPct", entry.second},
		});
	}

	const auto *active = ObsBootstrap::SceneCollections().Active();
	const std::string collectionName = active ? active->name : std::string("(none)");

	const Bridge::json summary{
		{"result", resultName},
		{"exitCode", st.exitCode},
		{"failReason", st.failReason},
		{"durationSec", st.durationSec},
		{"maxLagPctThreshold", st.maxLagPct},
		{"worstRenderLagPct", st.worstRenderLagPct},
		{"worstEncodeSkipPct", st.worstEncodeSkipPct},
		{"perOutputWorstDropPct", outputsJson},
		{"rankedMixes", ranksJson},
		{"collection", collectionName},
	};

	const std::string path = SelfTestConfigPath("perf-repro-" + TimestampFromSessionLog() + ".txt");
	if (!path.empty()) {
		std::ofstream out(path, std::ios::out | std::ios::trunc);
		if (out) {
			out << summary.dump(2);
		}
	}

	HostLog(std::string("[selftest-stream] perf-repro ") + resultName +
		" worstRenderLagPct=" + std::to_string(st.worstRenderLagPct) +
		" worstEncodeSkipPct=" + std::to_string(st.worstEncodeSkipPct) +
		" threshold=" + std::to_string(st.maxLagPct) + " collection='" + collectionName +
		"' summary=" + (path.empty() ? "(unwritten)" : path));
}

} // namespace

void ObsBootstrap::ArmPerfReproSelfTest()
{
	g_state = State{};
	g_state.phase = Phase::BootSettle;
	g_state.phaseStart = std::chrono::steady_clock::now();
	g_state.durationSec = EnvInt("BRAIDCAST_SELFTEST_DURATION", 90);
	g_state.maxLagPct = EnvDouble("BRAIDCAST_SELFTEST_MAXLAG", 2.0);
	HostLog("[selftest-stream] armed: duration=" + std::to_string(g_state.durationSec) +
		"s maxLagPct=" + std::to_string(g_state.maxLagPct));
}

bool ObsBootstrap::RunPerfReproSelfTest()
{
	State &st = g_state;

	for (;;) {
		switch (st.phase) {
		case Phase::Idle:
			return true; // never armed

		case Phase::BootSettle: {
			if (std::chrono::steady_clock::now() - st.phaseStart < kBootSettle) {
				return false;
			}
			st.phase = Phase::CheckConnected;
			continue;
		}

		case Phase::CheckConnected: {
			st.targets = CollectYoutubeTargets();
			if (st.targets.empty()) {
				HostLog("[selftest-stream] YouTube not connected; skipping");
				st.exitCode = 2;
				st.phase = Phase::WriteSummary;
				continue;
			}
			st.phase = Phase::EnableDebug;
			continue;
		}

		case Phase::EnableDebug: {
			st.originalDebugEnabled = Log::DebugEnabled();
			st.debugWasSet = true;
			Log::SetDebug(true);
			HostLog("[selftest-stream] render-debug enabled");
			st.phase = Phase::SwitchCollection;
			continue;
		}

		case Phase::SwitchCollection: {
			const char *target = getenv("BRAIDCAST_SELFTEST_COLLECTION");
			if (target && *target) {
				std::string id;
				for (const SceneCollectionRecord &rec : ObsBootstrap::SceneCollections().List()) {
					if (rec.name == target) {
						id = rec.id;
						break;
					}
				}
				if (id.empty()) {
					st.failReason = std::string("BRAIDCAST_SELFTEST_COLLECTION '") + target +
							"' not found";
					HostLog("[selftest-stream] " + st.failReason);
					st.exitCode = 3;
					st.phase = Phase::WriteSummary;
					continue;
				}
				std::string err;
				if (!ObsBootstrap::SceneCollections().Switch(id, err)) {
					st.failReason = "collection switch failed: " + err;
					HostLog("[selftest-stream] " + st.failReason);
					st.exitCode = 3;
					st.phase = Phase::WriteSummary;
					continue;
				}
				HostLog("[selftest-stream] switched to collection '" + std::string(target) + "'");
				// The switch reloaded the scene world; re-resolve the enabled-binding
				// targets against the now-active collection (bindings are per-collection).
				st.targets = CollectYoutubeTargets();
				if (st.targets.empty()) {
					HostLog("[selftest-stream] no enabled YouTube output binding in this "
						"collection; skipping");
					st.exitCode = 2;
					st.phase = Phase::WriteSummary;
					continue;
				}
			}
			st.phase = Phase::ForcePrivacy;
			continue;
		}

		case Phase::ForcePrivacy: {
			std::string err;
			if (!ForcePrivacyOnTargets(st.targets, err)) {
				st.failReason = err;
				HostLog("[selftest-stream] " + err);
				st.exitCode = 3;
				st.phase = Phase::WriteSummary;
				continue;
			}
			HostLog("[selftest-stream] forced privacy=private on " + std::to_string(st.targets.size()) +
				" YouTube binding(s)");
			st.phase = Phase::Starting;
			continue;
		}

		case Phase::Starting: {
			ObsBootstrap::Multistream().StartAllEnabled();
			HostLog("[selftest-stream] StartAllEnabled issued");
			st.pollLiveStart = std::chrono::steady_clock::now();
			st.phase = Phase::PollLive;
			return false;
		}

		case Phase::PollLive: {
			std::string notLive;
			if (AllEnabledLive(notLive)) {
				HostLog("[selftest-stream] all enabled outputs live");
				st.measureStart = std::chrono::steady_clock::now();
				st.lastSampleAt = st.measureStart;
				st.logOffsetAtMeasureStart = CurrentLogSize();
				st.measured = true;
				st.phase = Phase::Measuring;
				continue;
			}
			if (std::chrono::steady_clock::now() - st.pollLiveStart >= kPollLiveTimeout) {
				st.failReason = "did not go live within 30s:" + notLive;
				HostLog("[selftest-stream] " + st.failReason);
				st.exitCode = 3;
				st.phase = Phase::Stopping;
				continue;
			}
			return false;
		}

		case Phase::Measuring: {
			const auto now = std::chrono::steady_clock::now();
			if (now - st.lastSampleAt >= kSampleInterval) {
				SampleStats(st);
				st.lastSampleAt = now;
			}
			if (now - st.measureStart >= std::chrono::seconds(st.durationSec)) {
				HostLog("[selftest-stream] measurement window complete");
				st.phase = Phase::Stopping;
				continue;
			}
			return false;
		}

		case Phase::Stopping: {
			StopStreamingLikeBridge();
			HostLog("[selftest-stream] stopped");
			st.phase = Phase::WriteSummary;
			continue;
		}

		case Phase::WriteSummary: {
			WriteSummary(st);
			if (st.debugWasSet) {
				Log::SetDebug(st.originalDebugEnabled);
			}
			st.phase = Phase::Finished;
			continue;
		}

		case Phase::Finished:
			return true;
		}
	}
}

int ObsBootstrap::PerfReproSelfTestExitCode()
{
	return g_state.exitCode < 0 ? 0 : g_state.exitCode;
}
