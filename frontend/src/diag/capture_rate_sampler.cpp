#include "diag/capture_rate_sampler.hpp"

#include "multistream/MultistreamEngine.hpp"
#include "obs_bootstrap.hpp"

#include <util/platform.h>

#include <optional>

namespace CaptureRate {

namespace {

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

SourceTraits TraitsOf(obs_source_t *source, bool hasSize)
{
	SourceTraits traits;
	const char *id = obs_source_get_id(source);
	traits.id = id ? id : "";
	traits.async = (obs_source_get_output_flags(source) & OBS_SOURCE_ASYNC_VIDEO) == OBS_SOURCE_ASYNC_VIDEO;
	traits.hasSize = hasSize;
	if (traits.async) {
		traits.deinterlaced = obs_source_get_deinterlace_mode(source) != OBS_DEINTERLACE_MODE_DISABLE;
	} else if (traits.id == kWindowCaptureId) {
		OBSDataAutoRelease settings = obs_source_get_settings(source);
		traits.windowWgc = obs_data_get_int(settings, "method") == kWindowCaptureMethodWgc;
	}
	return traits;
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

nlohmann::json RowJson(const Row &r)
{
	nlohmann::json note = nullptr;
	if (r.lockedFraction) {
		// A rate, never a cause: the screen may simply update at that pace.
		note = std::string("delivering ") + r.lockedFraction + " of canvas rate";
	} else if (r.unmeasurableNote) {
		note = r.unmeasurableNote;
	}
	return nlohmann::json{
		{"uuid", r.uuid},
		{"name", r.name},
		{"kind", KindName(r.kind)},
		{"status", StatusName(r.status)},
		{"refFps", OptionalNumber(r.refFps)},
		{"rate", OptionalNumber(r.rate)},
		{"fraction", OptionalNumber(r.fraction)},
		{"inputFps", OptionalNumber(r.inputFps)},
		{"renderedFps", OptionalNumber(r.renderedFps)},
		{"copiesFps", OptionalNumber(r.copiesFps)},
		{"below", r.below},
		{"lockedFraction", r.lockedFraction ? nlohmann::json(r.lockedFraction) : nlohmann::json(nullptr)},
		{"note", note},
		{"inGrace", r.inGrace},
		{"sinceReset",
		 {{"liveSec", r.sinceReset.liveSec},
		  {"belowSec", r.sinceReset.belowSec},
		  {"lockedSec", r.sinceReset.lockedSec}}},
	};
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

	SampleInput in;
	in.mainFps = FpsOf(obs_get_video());
	in.dtSec = lastSampleNs_ && nowNs > lastSampleNs_ ? (nowNs - lastSampleNs_) / 1e9 : 0.0;

	std::map<std::string, Held> nextHeld;
	for (auto &[uuid, p] : pending) {
		// Size before kind: a capture sets its kind no later than it gains a size,
		// so a sized source read here is never mistaken for one with no counter.
		const bool hasSize = obs_source_get_base_width(p.source) > 0;
		struct obs_source_frame_counts raw = {};
		obs_source_get_frame_counts(p.source, &raw);
		const Kind kind = ToKind(raw.kind);
		std::optional<SourceTraits> traits;
		if (kind == Kind::None) {
			traits = TraitsOf(p.source, hasSize);
			if (!IsListed(*traits)) {
				continue;
			}
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
		src.frameSignal = !traits || HasFrameSignal(*traits);
		src.unmeasurableNote = traits ? UnmeasurableNote(*traits) : nullptr;
		src.counts =
			Counts{kind, raw.live_ticks, raw.new_frame_ticks, raw.frames_delivered, raw.frames_offered};
		src.reach = std::move(p.reach);
		in.sources.push_back(std::move(src));
		nextHeld.emplace(uuid, std::move(held));
	}
	held_.swap(nextHeld);

	tracker_.Sample(in);
	for (const std::string &line : tracker_.TakeWindowLines()) {
		blog(LOG_INFO, "%s", line.c_str());
	}
	lastSampleNs_ = nowNs;
}

nlohmann::json Sampler::RowsPayload(bool overlays) const
{
	nlohmann::json rows = nlohmann::json::array();
	for (const Row &r : tracker_.Rows()) {
		if ((r.kind == Kind::BrowserPaint) == overlays) {
			rows.push_back(RowJson(r));
		}
	}
	return rows;
}

nlohmann::json Sampler::Payload() const
{
	return RowsPayload(false);
}

nlohmann::json Sampler::OverlayPayload() const
{
	return RowsPayload(true);
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
