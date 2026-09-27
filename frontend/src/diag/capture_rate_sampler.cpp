#include "diag/capture_rate_sampler.hpp"

#include "multistream/MultistreamEngine.hpp"
#include "obs_bootstrap.hpp"

#include <util/platform.h>

#include <cstring>
#include <set>

namespace CaptureRate {

namespace {

// Capture sources that may count nothing right now: a failed duplicator, a BitBlt
// window capture. They get a row rather than none, so a missing number is visible
// as such. Sources that report a kind need no listing.
constexpr const char *kWindowCaptureId = "window_capture";
const std::set<std::string> kDisplayCaptureIds = {"monitor_capture", kWindowCaptureId};

// window-capture.c's METHOD_WGC; auto and BitBlt settle on BitBlt for most windows.
constexpr long long kWindowCaptureMethodWgc = 2;

Kind ToKind(enum obs_frame_count_kind kind)
{
	switch (kind) {
	case OBS_FRAME_COUNT_WGC:
		return Kind::Wgc;
	case OBS_FRAME_COUNT_DXGI:
		return Kind::Dxgi;
	case OBS_FRAME_COUNT_ASYNC:
		return Kind::Async;
	case OBS_FRAME_COUNT_BROWSER_PAINT:
		return Kind::BrowserPaint;
	case OBS_FRAME_COUNT_GAME_HOOK:
		return Kind::GameHook;
	case OBS_FRAME_COUNT_NONE:
		break;
	}
	return Kind::None;
}

bool IsCandidate(obs_source_t *source, Kind kind)
{
	if (kind != Kind::None) {
		return true;
	}
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_ASYNC_VIDEO) == OBS_SOURCE_ASYNC_VIDEO) {
		return true; // deinterlaced, or not delivering right now
	}
	const char *id = obs_source_get_id(source);
	return id && kDisplayCaptureIds.count(id) != 0;
}

// Whether the source's capture method counts its frames at all, for a source that
// reports no kind. Deinterlacing hides an async source's frames from the fold, and
// BitBlt has no signal; anything else counting nothing is not capturing now.
bool HasFrameSignal(obs_source_t *source)
{
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_ASYNC_VIDEO) == OBS_SOURCE_ASYNC_VIDEO) {
		return obs_source_get_deinterlace_mode(source) == OBS_DEINTERLACE_MODE_DISABLE;
	}
	const char *id = obs_source_get_id(source);
	if (id && strcmp(id, kWindowCaptureId) == 0) {
		OBSDataAutoRelease settings = obs_source_get_settings(source);
		return obs_data_get_int(settings, "method") == kWindowCaptureMethodWgc;
	}
	return true;
}

double FpsOf(video_t *video)
{
	const struct video_output_info *info = video ? video_output_get_info(video) : nullptr;
	return info && info->fps_den ? static_cast<double>(info->fps_num) / info->fps_den : 0.0;
}

nlohmann::json OptionalNumber(const std::optional<double> &v)
{
	return v ? nlohmann::json(*v) : nlohmann::json(nullptr);
}

} // namespace

bool HoldsSource(obs_weak_source_t *weak, obs_source_t *source)
{
	return !obs_weak_source_expired(weak) && obs_weak_source_references_source(weak, source);
}

bool Sampler::CanvasLive(const std::string &canvasUuid) const
{
	if (liveOverride_) {
		return liveOverride_(canvasUuid);
	}
	return ObsBootstrap::MultistreamAlive() && ObsBootstrap::Multistream().IsCanvasLive(canvasUuid);
}

bool Sampler::WantsSample(uint64_t nowNs) const
{
	return nowNs < leaseUntilNs_ || (ObsBootstrap::MultistreamAlive() && ObsBootstrap::Multistream().AnyLive());
}

void Sampler::Idle()
{
	if (lastSampleNs_) {
		tracker_.Pause();
		held_.clear();
		lastSampleNs_ = 0;
	}
}

void Sampler::Sample(const std::vector<VideoGate::Root> &roots, uint64_t nowNs)
{
	struct Pending {
		OBSSource source;
		std::vector<Reach> reach;
	};
	std::map<std::string, Pending> pending;
	for (const VideoGate::Root &root : roots) {
		// A showing root (projector, thumbnail) renders for no canvas: its sources
		// get a row but it sets no rule.
		std::optional<Reach> reach;
		if (root.kind != VideoGate::RootKind::ShowingRoot) {
			const bool live = CanvasLive(root.canvasUuid);
			const double fps = live && ObsBootstrap::MultistreamAlive()
						   ? FpsOf(ObsBootstrap::Multistream().VideoForCanvas(root.canvasUuid))
						   : 0.0;
			reach = Reach{fps, live};
		}
		for (const auto &[uuid, source] : root.sources) {
			Pending &p = pending[uuid];
			p.source = source;
			if (reach) {
				p.reach.push_back(*reach);
			}
		}
	}

	obs_video_info ovi = {};
	SampleInput in;
	in.mainFps = obs_get_video_info(&ovi) && ovi.fps_den ? static_cast<double>(ovi.fps_num) / ovi.fps_den : 0.0;
	in.dtSec = lastSampleNs_ && nowNs > lastSampleNs_ ? (nowNs - lastSampleNs_) / 1e9 : 0.0;

	std::map<std::string, Held> nextHeld;
	for (auto &[uuid, p] : pending) {
		struct obs_source_frame_counts raw = {};
		obs_source_get_frame_counts(p.source, &raw);
		const Kind kind = ToKind(raw.kind);
		if (!IsCandidate(p.source, kind)) {
			continue;
		}

		// uuids survive recreation, so the uuid alone cannot say these are the same
		// counters as last time; the weak ref can.
		Held held;
		auto it = held_.find(uuid);
		if (it != held_.end() && HoldsSource(it->second.weak, p.source)) {
			held = it->second;
		} else {
			held.weak = OBSGetWeakRef(p.source);
			held.identity = nextIdentity_++;
		}

		SourceInput src;
		src.uuid = uuid;
		const char *name = obs_source_get_name(p.source);
		src.name = name ? name : "";
		src.identity = held.identity;
		src.showing = obs_source_showing(p.source);
		src.frameSignal = kind != Kind::None || HasFrameSignal(p.source);
		src.counts = Counts{kind, raw.live_ticks, raw.new_frame_ticks, raw.frames_delivered};
		src.reach = std::move(p.reach);
		in.sources.push_back(std::move(src));
		nextHeld.emplace(uuid, std::move(held));
	}
	held_.swap(nextHeld);

	tracker_.Sample(in);
	lastSampleNs_ = nowNs;
}

nlohmann::json Sampler::Payload() const
{
	nlohmann::json rows = nlohmann::json::array();
	for (const Row &r : tracker_.Rows()) {
		nlohmann::json note = nullptr;
		if (r.lockedFraction) {
			// A rate, never a cause: the screen may simply update at that pace.
			note = std::string("delivering ") + r.lockedFraction + " of canvas rate";
		}
		rows.push_back(nlohmann::json{
			{"uuid", r.uuid},
			{"name", r.name},
			{"kind", KindName(r.kind)},
			{"status", StatusName(r.status)},
			{"refFps", OptionalNumber(r.refFps)},
			{"rate", OptionalNumber(r.rate)},
			{"fraction", OptionalNumber(r.fraction)},
			{"inputFps", OptionalNumber(r.inputFps)},
			{"renderedFps", OptionalNumber(r.renderedFps)},
			{"below", r.below},
			{"lockedFraction",
			 r.lockedFraction ? nlohmann::json(r.lockedFraction) : nlohmann::json(nullptr)},
			{"note", note},
			{"inGrace", r.inGrace},
			{"sinceReset",
			 {{"liveSec", r.sinceReset.liveSec},
			  {"belowSec", r.sinceReset.belowSec},
			  {"lockedSec", r.sinceReset.lockedSec}}},
		});
	}
	return rows;
}

void Sampler::Watch(uint64_t nowNs)
{
	leaseUntilNs_ = nowNs + kLeaseNs;
}

void Sampler::ResetWindows()
{
	tracker_.ResetWindows();
}

void Sampler::SessionBegin()
{
	tracker_.SessionBegin(os_gettime_ns());
}

std::string Sampler::SessionEnd()
{
	const std::string line = tracker_.SessionEnd(os_gettime_ns());
	if (!line.empty()) {
		blog(LOG_INFO, "%s", line.c_str());
	}
	return line;
}

void Sampler::Clear()
{
	tracker_.Clear();
	held_.clear();
	lastSampleNs_ = 0;
	leaseUntilNs_ = 0;
	liveOverride_ = nullptr;
}

void Sampler::SetCanvasLiveOverrideForTest(std::function<bool(const std::string &)> fn)
{
	liveOverride_ = std::move(fn);
}

} // namespace CaptureRate
