#include "MultistreamEngine.hpp"

#include "settings/AdvancedSettings.hpp"
#include "log.hpp"
#include "obs_bootstrap.hpp"

#include "CanvasStore.hpp"
#include "OutputBindingStore.hpp"
#include "StreamProfileStore.hpp"

#include <CanvasDefinition.hpp>
#include <OutputBinding.hpp>
#include <StreamProfile.hpp>

#include <util/platform.h>

#include <array>
#include <cstring>

// File-local ports of BasicOutputHandler's stream-output-type resolution, so the
// engine has no dependency on the Qt output handler.
namespace {

bool can_use_output(const char *prot, const char *output, const char *prot_test1, const char *prot_test2 = nullptr)
{
	return (strcmp(prot, prot_test1) == 0 || (prot_test2 && strcmp(prot, prot_test2) == 0)) &&
	       (obs_get_output_flags(output) & OBS_OUTPUT_SERVICE) != 0;
}

bool return_first_id(void *data, const char *id)
{
	const char **output = (const char **)data;

	*output = id;
	return false;
}

const char *GetStreamOutputType(const obs_service_t *service)
{
	const char *protocol = obs_service_get_protocol(service);
	const char *output = nullptr;

	if (!protocol) {
		blog(LOG_WARNING, "The service '%s' has no protocol set", obs_service_get_id(service));
		return nullptr;
	}

	if (!obs_is_output_protocol_registered(protocol)) {
		blog(LOG_WARNING, "The protocol '%s' is not registered", protocol);
		return nullptr;
	}

	/* Check if the service has a preferred output type */
	output = obs_service_get_preferred_output_type(service);
	if (output) {
		if ((obs_get_output_flags(output) & OBS_OUTPUT_SERVICE) != 0) {
			return output;
		}

		blog(LOG_WARNING, "The output '%s' is not registered, fallback to another one", output);
	}

	/* Otherwise, prefer first-party output types */
	if (can_use_output(protocol, "rtmp_output", "RTMP", "RTMPS")) {
		return "rtmp_output";
	} else if (can_use_output(protocol, "ffmpeg_hls_muxer", "HLS")) {
		return "ffmpeg_hls_muxer";
	} else if (can_use_output(protocol, "ffmpeg_mpegts_muxer", "SRT", "RIST")) {
		return "ffmpeg_mpegts_muxer";
	}

	/* If third-party protocol, use the first enumerated type */
	obs_enum_output_types_with_protocol(protocol, &output, return_first_id);
	if (output) {
		return output;
	}

	blog(LOG_WARNING, "No output compatible with the service '%s' is registered", obs_service_get_id(service));

	return nullptr;
}

} // namespace

const char *MultistreamEngine::StateName(State state)
{
	static constexpr std::array<const char *, 5> kNames = {{"idle", "connecting", "live", "error", "reconnecting"}};
	size_t idx = static_cast<size_t>(state);
	return kNames[idx < kNames.size() ? idx : 0];
}

MultistreamEngine::MultistreamEngine(CanvasStore &canvases_, StreamProfileStore &profiles_,
				     OutputBindingStore &bindings_, VideoResolver resolver_)
	: canvases(canvases_),
	  profiles(profiles_),
	  bindings(bindings_),
	  resolver(std::move(resolver_)),
	  sleepInhibit(os_inhibit_sleep_create("Braidcast streaming"))
{
}

MultistreamEngine::~MultistreamEngine()
{
	/* Defensive: stop everything still live. Clear the callback first so the
	 * StopAll notification can't reach back into a now-dangling owner (e.g. the
	 * unique_ptr that holds us is mid-reset(), so its global accessor is already
	 * null). The owner runs StopAll explicitly before destroying us anyway. */
	onStatusChanged = nullptr;
	onOutputStopped = nullptr;
	StopAll();
	os_inhibit_sleep_destroy(sleepInhibit);
}

void MultistreamEngine::UpdateSleepInhibit()
{
	os_inhibit_sleep_set_active(sleepInhibit, AnyLive());
}

video_t *MultistreamEngine::VideoForCanvas(const std::string &canvasUuid)
{
	return resolver ? resolver(canvasUuid) : nullptr;
}

MultistreamEngine::CanvasEncoders *MultistreamEngine::EnsureCanvasEncoders(const std::string &canvasUuid)
{
	for (CanvasEncoders &ce : canvasEncoders) {
		if (ce.canvasUuid == canvasUuid && ce.video && ce.audio) {
			return &ce;
		}
	}

	const CanvasDefinition &def = canvases.Default();
	const CanvasDefinition *cdef = (canvasUuid == def.uuid) ? &def : canvases.Find(canvasUuid);
	if (!cdef) {
		return nullptr;
	}
	video_t *video = VideoForCanvas(canvasUuid);
	if (!video) {
		blog(LOG_WARNING, "Multistream: no video mix for canvas %s", canvasUuid.c_str());
		return nullptr;
	}

	/* Resolve encoder ids/settings, honoring 'use default' inheritance from the
	 * Default canvas. If the id is empty or marked use-default, fall back to the
	 * Default canvas encoder. */
	const CanvasEncoderDef &vdef = cdef->video.InheritsDefault() ? def.video : cdef->video;
	const CanvasEncoderDef &adef = cdef->audio.InheritsDefault() ? def.audio : cdef->audio;
	if (vdef.id.empty() || adef.id.empty()) {
		blog(LOG_WARNING, "Multistream: canvas %s has no usable encoders", canvasUuid.c_str());
		return nullptr;
	}

	CanvasEncoders ce;
	ce.canvasUuid = canvasUuid;
	std::string vname = "multistream_v_" + canvasUuid;
	std::string aname = "multistream_a_" + canvasUuid;
	ce.video =
		OBSEncoderAutoRelease(obs_video_encoder_create(vdef.id.c_str(), vname.c_str(), vdef.settings, nullptr));
	ce.audio = OBSEncoderAutoRelease(
		obs_audio_encoder_create(adef.id.c_str(), aname.c_str(), adef.settings, 0, nullptr));
	if (!ce.video || !ce.audio) {
		return nullptr;
	}
	obs_encoder_set_video(ce.video, video);
	obs_encoder_set_audio(ce.audio, obs_get_audio());

	canvasEncoders.push_back(std::move(ce));
	return &canvasEncoders.back();
}

void MultistreamEngine::InvalidateCanvasEncoders(const std::string &canvasUuid)
{
	/* Canvases with an empty/use-default encoder id inherit the Default canvas's
	 * resolved encoder (see EnsureCanvasEncoders), so editing the Default staleifies
	 * every inheritor's cached pair too -- drop those as well when the Default is the
	 * one invalidated. */
	const bool defaultInvalidated = (canvasUuid == canvases.Default().uuid);
	for (auto it = canvasEncoders.begin(); it != canvasEncoders.end();) {
		bool drop = (it->canvasUuid == canvasUuid);
		if (!drop && defaultInvalidated) {
			const CanvasDefinition *cdef = canvases.Find(it->canvasUuid);
			if (cdef && (cdef->video.InheritsDefault() || cdef->audio.InheritsDefault())) {
				drop = true;
			}
		}
		if (drop) {
			it = canvasEncoders.erase(it);
		} else {
			++it;
		}
	}
}

bool MultistreamEngine::ProfileLiveElsewhere(const std::string &bindingUuid, const std::string &profileUuid) const
{
	if (profileUuid.empty()) {
		return false;
	}
	std::lock_guard<std::mutex> lock(liveMutex);
	for (const auto &lo : live) {
		// Only an actively-connecting/streaming output reserves the profile; a dead
		// (Error/Idle) entry does not. A live output is inherently enabled, so pass
		// rowEnabled = true to the shared predicate (the config-layer guard supplies
		// the real enable flag).
		if (IsActiveState(lo->state) && BindingMatchesProfile(lo->bindingUuid, lo->profileUuid,
								      /*rowEnabled=*/true, bindingUuid, profileUuid)) {
			return true;
		}
	}
	return false;
}

bool MultistreamEngine::StartOutput(const std::string &bindingUuid)
{
	std::string error;
	return StartOutput(bindingUuid, error);
}

bool MultistreamEngine::StartOutput(const std::string &bindingUuid, std::string &error)
{
	OutputBinding *b = bindings.Bindings().Find(bindingUuid);

	// Record a failure reason on the binding's status so a bare start refusal still
	// tells the UI WHY the row won't go live. The Error entry lingers in `live` (it is
	// not "active", so it never blocks canvas edits) until a retry reaps it. On a
	// binding that vanished mid-call there is nothing to attach status to, so only the
	// returned error is set.
	auto fail = [&](const std::string &reason) -> bool {
		error = reason;
		DBG(LogCat::Stream, "start refused for binding %s: %s", bindingUuid.c_str(), reason.c_str());
		auto lo = std::make_unique<LiveOutput>();
		lo->bindingUuid = bindingUuid;
		lo->profileUuid = b ? b->profileUuid : std::string();
		lo->canvasUuid = b ? b->canvasUuid : std::string();
		lo->state = State::Error;
		lo->lastError = reason;
		/* A reaped prior entry destructs after the lock scope ends (see TakeLive). */
		std::unique_ptr<LiveOutput> reaped;
		{
			std::lock_guard<std::mutex> lock(liveMutex);
			reaped = TakeLive(bindingUuid);
			live.push_back(std::move(lo));
		}
		UpdateSleepInhibit();
		NotifyChanged();
		return false;
	};

	// An actively connecting/streaming output for this binding needs no action; a
	// lingering dead (Error/Idle) entry from a prior failure or mid-stream drop is
	// reaped here on the UI thread so this start actually restarts it (rather than the
	// old short-circuit that returned true on any existing entry, dead or not).
	{
		/* The reaped entry destructs after the lock scope ends (see TakeLive). */
		std::unique_ptr<LiveOutput> reaped;
		{
			std::lock_guard<std::mutex> lock(liveMutex);
			if (LiveOutput *existing = FindLive(bindingUuid)) {
				if (IsActiveState(existing->state)) {
					return true;
				}
				reaped = TakeLive(bindingUuid);
			}
		}
	}

	if (!b) {
		error = "no output binding with uuid '" + bindingUuid + "'";
		return false;
	}
	if (b->profileUuid.empty()) {
		return fail("no stream profile is set for this output");
	}
	if (ProfileLiveElsewhere(bindingUuid, b->profileUuid)) {
		return fail("that stream profile is already live on another canvas");
	}
	StreamProfile *p = profiles.Find(b->profileUuid);
	if (!p) {
		return fail("the bound stream profile no longer exists");
	}

	// Pre-flight the destination credentials for keyed RTMP services. rtmp_common's
	// can_try_to_connect needs BOTH a non-empty ingest server and a key; when either
	// is missing obs_output_start bails deep in libobs with an empty last_error, so
	// the UI only ever sees the generic "the output failed to start". Surface the real
	// reason here. (WHIP/custom carry their own URL, so this covers rtmp_common only --
	// the OAuth-provisioned platforms whose key/server come from the connect flow.)
	if (p->serviceId == "rtmp_common") {
		const std::string platform = p->PlatformName();
		const char *server = p->settings ? obs_data_get_string(p->settings, "server") : "";
		if (p->Key().empty()) {
			return fail(platform + " has no stream key yet -- reconnect " + platform +
				    " in Settings, or paste a key into its stream profile");
		}
		if (!server || !*server) {
			// The key is present (checked above) but the ingest server was never
			// seeded -- a profile linked before the connect flow started writing
			// server=auto. Seed it inline so rtmp_common resolves the platform ingest
			// from the service's recommended server, instead of failing the go-live.
			obs_data_set_string(p->settings, "server", "auto");
			profiles.Save();
		}
	}

	CanvasEncoders *ce = EnsureCanvasEncoders(b->canvasUuid);
	if (!ce) {
		return fail("could not build encoders for this canvas");
	}

	auto lo = std::make_unique<LiveOutput>();
	lo->bindingUuid = bindingUuid;
	lo->profileUuid = b->profileUuid;
	lo->canvasUuid = b->canvasUuid;

	std::string sname = "multistream_svc_" + bindingUuid;
	lo->service =
		OBSServiceAutoRelease(obs_service_create(p->serviceId.c_str(), sname.c_str(), p->settings, nullptr));
	if (!lo->service) {
		return fail("could not create the streaming service");
	}

	const char *type = GetStreamOutputType(lo->service);
	if (!type) {
		type = "rtmp_output";
	}
	std::string oname = "multistream_out_" + bindingUuid;
	lo->output = OBSOutputAutoRelease(obs_output_create(type, oname.c_str(), nullptr, nullptr));
	if (!lo->output) {
		return fail("could not create the streaming output");
	}

	lo->startSignal.Connect(obs_output_get_signal_handler(lo->output), "start", OnOutputStart, this);
	lo->stopSignal.Connect(obs_output_get_signal_handler(lo->output), "stop", OnOutputStop, this);
	/* On a reconnectable drop libobs SUPPRESSES "stop" (obs_output_signal_stop ->
	 * can_reconnect) and fires "reconnect" per retry instead, then
	 * "reconnect_success" (not "start") when the stream comes back; only exhausted
	 * retries or a user stop end in "stop". Without these two the row would show
	 * "live" through the whole retry window. */
	lo->reconnectSignal.Connect(obs_output_get_signal_handler(lo->output), "reconnect", OnOutputReconnect, this);
	lo->reconnectSuccessSignal.Connect(obs_output_get_signal_handler(lo->output), "reconnect_success",
					   OnOutputReconnectSuccess, this);
	DBG(LogCat::Stream, "output created for binding %s (canvas %s, type %s)", bindingUuid.c_str(),
	    b->canvasUuid.c_str(), type);

	// Apply the global Advanced settings to this output. These are output-level (not
	// encoder-level) options, so they are compatible with the encode-once model: the
	// shared per-canvas encoders are untouched; only the per-binding output wrapper
	// carries the delay/reconnect/network config. They apply to NEWLY started
	// outputs -- a live output picks up changes only on its next restart.
	const AdvancedSettings &adv = ObsBootstrap::Advanced();

	// Stream delay (set 0 to clear any prior delay on a reused output name).
	if (adv.streamDelayEnabled) {
		obs_output_set_delay(lo->output, adv.streamDelaySec,
				     adv.streamDelayPreserve ? OBS_OUTPUT_DELAY_PRESERVE : 0);
	} else {
		obs_output_set_delay(lo->output, 0, 0);
	}

	// Automatic reconnect (retry_count 0 disables reconnecting).
	if (adv.reconnectEnabled) {
		obs_output_set_reconnect_settings(lo->output, (int)adv.reconnectMaxRetries,
						  (int)adv.reconnectRetryDelaySec);
	} else {
		obs_output_set_reconnect_settings(lo->output, 0, (int)adv.reconnectRetryDelaySec);
	}

	// Network / dynamic-bitrate options consumed by the rtmp output (rtmp-stream.h:
	// bind_ip / new_socket_loop_enabled / low_latency_mode_enabled / dyn_bitrate).
	// "default"/empty bindIP means "do not bind a specific NIC" -- leave it unset.
	OBSDataAutoRelease osettings = obs_data_create();
	if (adv.bindIP != "default" && !adv.bindIP.empty()) {
		obs_data_set_string(osettings, "bind_ip", adv.bindIP.c_str());
	}
	obs_data_set_bool(osettings, "new_socket_loop_enabled", adv.newSocketLoop);
	obs_data_set_bool(osettings, "low_latency_mode_enabled", adv.lowLatencyMode);
	obs_data_set_bool(osettings, "dyn_bitrate", adv.dynamicBitrate);
	obs_output_update(lo->output, osettings);

	obs_output_set_video_encoder(lo->output, ce->video);
	obs_output_set_audio_encoder(lo->output, ce->audio, 0);
	obs_output_set_service(lo->output, lo->service);

	lo->state = State::Connecting;
	LiveOutput *raw = lo.get();
	{
		std::lock_guard<std::mutex> lock(liveMutex);
		live.push_back(std::move(lo));
	}
	UpdateSleepInhibit();

	/* obs_output_start is async; raw stays valid because only this (UI) thread
	 * erases `live`. The handler may flip raw->state, so guard the failure write. */
	DBG(LogCat::Stream, "starting output for binding %s", bindingUuid.c_str());
	if (!obs_output_start(raw->output)) {
		const char *err = obs_output_get_last_error(raw->output);
		const std::string reason = err && *err ? err : "the output failed to start";
		{
			std::lock_guard<std::mutex> lock(liveMutex);
			raw->lastError = reason;
			raw->state = State::Error;
		}
		UpdateSleepInhibit();
		error = reason;
		blog(LOG_WARNING, "Multistream: output '%s' failed to start: %s", oname.c_str(), reason.c_str());
		NotifyChanged();
		return false;
	}
	NotifyChanged();
	return true;
}

void MultistreamEngine::StopOutput(const std::string &bindingUuid)
{
	obs_output_t *out = nullptr;
	{
		std::lock_guard<std::mutex> lock(liveMutex);
		LiveOutput *lo = FindLive(bindingUuid);
		if (!lo) {
			return;
		}
		out = lo->output;
	}
	/* Stop outside the lock: obs_output_stop can fire "stop" synchronously,
	 * which re-enters OnOutputStop and would deadlock on liveMutex. */
	if (out) {
		DBG(LogCat::Stream, "stopping output for binding %s", bindingUuid.c_str());
		obs_output_stop(out);
	}
	/* Take the entry out under the lock but destroy it outside: ~LiveOutput blocks
	 * on libobs signal/thread machinery while the output thread's stop dispatch may
	 * be blocked on liveMutex (see TakeLive). */
	std::unique_ptr<LiveOutput> dead;
	{
		std::lock_guard<std::mutex> lock(liveMutex);
		dead = TakeLive(bindingUuid);
	}
	UpdateSleepInhibit();
	NotifyChanged();
}

void MultistreamEngine::StartAllEnabled()
{
	for (auto &b : bindings.Bindings().bindings) {
		if (b.enabled) {
			std::string err;
			if (!StartOutput(b.uuid, err)) {
				// The reason is also recorded on the row's status (Statuses().lastError)
				// so the UI can show WHY the row stayed down; log it for the session too.
				blog(LOG_WARNING, "Multistream: binding %s did not go live: %s", b.uuid.c_str(),
				     err.c_str());
			}
		}
	}
}

void MultistreamEngine::StopAll()
{
	/* Collect raw handles under the lock, then stop outside it (obs_output_stop
	 * may fire "stop" synchronously -> OnOutputStop -> re-lock -> deadlock). */
	std::vector<obs_output_t *> toStop;
	{
		std::lock_guard<std::mutex> lock(liveMutex);
		for (auto &lo : live) {
			if (lo->output) {
				toStop.push_back(lo->output);
			}
		}
	}
	for (obs_output_t *out : toStop) {
		obs_output_stop(out);
	}
	/* Swap the entries out under the lock but destroy them outside it: ~LiveOutput
	 * blocks on libobs signal/thread machinery while an output thread's stop
	 * dispatch may be blocked on liveMutex (see TakeLive). */
	std::vector<std::unique_ptr<LiveOutput>> dead;
	{
		std::lock_guard<std::mutex> lock(liveMutex);
		dead.swap(live);
	}
	UpdateSleepInhibit();
	/* Encoders are released when the handler is destroyed or rebuilt; keep the
	 * cache so a quick restart reuses them, but they hold no output refs now. */
	NotifyChanged();
}

bool MultistreamEngine::IsLive(const std::string &bindingUuid) const
{
	std::lock_guard<std::mutex> lock(liveMutex);
	for (const auto &lo : live) {
		if (lo->bindingUuid == bindingUuid && IsActiveState(lo->state)) {
			return true;
		}
	}
	return false;
}

bool MultistreamEngine::IsCanvasLive(const std::string &canvasUuid) const
{
	std::lock_guard<std::mutex> lock(liveMutex);
	for (const auto &lo : live) {
		if (lo->canvasUuid == canvasUuid && IsActiveState(lo->state)) {
			return true;
		}
	}
	return false;
}

bool MultistreamEngine::IsCanvasOrInheritorLive(const std::string &canvasUuid) const
{
	const bool isDefault = (canvasUuid == canvases.Default().uuid);
	std::lock_guard<std::mutex> lock(liveMutex);
	for (const auto &lo : live) {
		if (!IsActiveState(lo->state)) {
			continue;
		}
		if (lo->canvasUuid == canvasUuid) {
			return true;
		}
		if (!isDefault) {
			continue;
		}
		const CanvasDefinition *cdef = canvases.Find(lo->canvasUuid);
		if (cdef && cdef->InheritsAnyDefault()) {
			return true;
		}
	}
	return false;
}

bool MultistreamEngine::CanvasHasActiveOutput(const std::string &canvasUuid) const
{
	std::lock_guard<std::mutex> lock(liveMutex);
	for (const auto &lo : live) {
		// Query the real output handle, not lo->state: a user stop may have flipped the
		// state (or the entry may linger post-stop) while the RTMP output is still
		// draining, and its encoder keeps the canvas mix bound until obs_output_active
		// clears. IsActiveState too, so a lingering dead entry whose handle briefly
		// still reports active during the stop-signal/active-flag lag isn't counted.
		if (lo->canvasUuid == canvasUuid && lo->output && IsActiveState(lo->state) &&
		    obs_output_active(lo->output)) {
			return true;
		}
	}
	return false;
}

bool MultistreamEngine::AnyLive() const
{
	std::lock_guard<std::mutex> lock(liveMutex);
	for (const auto &lo : live) {
		if (IsActiveState(lo->state)) {
			return true;
		}
	}
	return false;
}

MultistreamEngine::LiveOutput *MultistreamEngine::FindLive(const std::string &bindingUuid)
{
	for (auto &lo : live) {
		if (lo->bindingUuid == bindingUuid) {
			return lo.get();
		}
	}
	return nullptr;
}

std::unique_ptr<MultistreamEngine::LiveOutput> MultistreamEngine::TakeLive(const std::string &bindingUuid)
{
	for (auto it = live.begin(); it != live.end(); ++it) {
		if ((*it)->bindingUuid == bindingUuid) {
			std::unique_ptr<LiveOutput> taken = std::move(*it);
			live.erase(it);
			return taken;
		}
	}
	return nullptr;
}

MultistreamEngine::BindingMeta MultistreamEngine::ResolveBindingMeta(const OutputBinding &b) const
{
	BindingMeta meta;
	if (!b.profileUuid.empty()) {
		if (StreamProfile *p = profiles.Find(b.profileUuid)) {
			meta.profileLabel = p->DisplayName();
		}
	}
	const CanvasDefinition &def = canvases.Default();
	if (b.canvasUuid == def.uuid) {
		meta.canvasName = def.name;
	} else if (const CanvasDefinition *cdef = canvases.Find(b.canvasUuid)) {
		meta.canvasName = cdef->name;
	}
	return meta;
}

std::vector<MultistreamEngine::OutputStatus> MultistreamEngine::Statuses() const
{
	std::lock_guard<std::mutex> lock(liveMutex);
	std::vector<OutputStatus> out;
	for (const OutputBinding &b : bindings.Bindings().bindings) {
		if (!b.enabled) {
			continue;
		}
		OutputStatus st;
		st.bindingUuid = b.uuid;
		st.canvasUuid = b.canvasUuid;
		BindingMeta meta = ResolveBindingMeta(b);
		st.profileLabel = std::move(meta.profileLabel);
		st.canvasName = std::move(meta.canvasName);
		for (const auto &lo : live) {
			if (lo->bindingUuid == b.uuid) {
				st.state = lo->state;
				st.lastError = lo->lastError;
				break;
			}
		}
		out.push_back(std::move(st));
	}
	return out;
}

std::vector<MultistreamEngine::OutputStats> MultistreamEngine::StatsSnapshot() const
{
	std::lock_guard<std::mutex> lock(liveMutex);
	std::vector<OutputStats> out;
	for (const OutputBinding &b : bindings.Bindings().bindings) {
		if (!b.enabled) {
			continue;
		}
		OutputStats st;
		st.bindingUuid = b.uuid;
		st.canvasUuid = b.canvasUuid;
		BindingMeta meta = ResolveBindingMeta(b);
		st.profileLabel = std::move(meta.profileLabel);
		st.canvasName = std::move(meta.canvasName);
		for (const auto &lo : live) {
			if (lo->bindingUuid != b.uuid) {
				continue;
			}
			st.state = lo->state;
			if (lo->output) {
				/* obs_output_get_congestion/_connect_time_ms take a non-const
				 * handle; the auto-release yields one regardless of our constness. */
				obs_output_t *o = lo->output;
				st.totalBytes = obs_output_get_total_bytes(o);
				st.droppedFrames = obs_output_get_frames_dropped(o);
				st.totalFrames = obs_output_get_total_frames(o);
				st.congestion = obs_output_get_congestion(o);
				st.uptimeMs = lo->liveStartNs ? (os_gettime_ns() - lo->liveStartNs) / 1000000ULL : 0;
			}
			break;
		}
		out.push_back(std::move(st));
	}
	return out;
}

void MultistreamEngine::NotifyChanged()
{
	if (onStatusChanged) {
		onStatusChanged();
	}
}

void MultistreamEngine::OnOutputStart(void *data, calldata_t *cd)
{
	auto self = static_cast<MultistreamEngine *>(data);
	obs_output_t *out = (obs_output_t *)calldata_ptr(cd, "output");
	{
		std::lock_guard<std::mutex> lock(self->liveMutex);
		for (auto &lo : self->live) {
			if (lo->output == out) {
				lo->state = State::Live;
				lo->liveStartNs = os_gettime_ns();
				DBG(LogCat::Net, "rtmp connected (binding %s, canvas %s)", lo->bindingUuid.c_str(),
				    lo->canvasUuid.c_str());
				break;
			}
		}
	}
	self->NotifyChanged();
}

void MultistreamEngine::OnOutputStop(void *data, calldata_t *cd)
{
	auto self = static_cast<MultistreamEngine *>(data);
	obs_output_t *out = (obs_output_t *)calldata_ptr(cd, "output");
	int code = (int)calldata_int(cd, "code");
	const char *lastError = calldata_string(cd, "last_error");
	std::string canvasUuid;
	{
		std::lock_guard<std::mutex> lock(self->liveMutex);
		for (auto &lo : self->live) {
			if (lo->output == out) {
				canvasUuid = lo->canvasUuid;
				/* A non-success code means the stream dropped or never
				 * connected (e.g. a bad key); surface it as Error. */
				if (code != OBS_OUTPUT_SUCCESS) {
					lo->state = State::Error;
					lo->lastError = lastError ? lastError : "";
					DBG(LogCat::Net, "rtmp stopped (binding %s, code %d): %s",
					    lo->bindingUuid.c_str(), code, lo->lastError.c_str());
				} else {
					lo->state = State::Idle;
					DBG(LogCat::Net, "rtmp stopped cleanly (binding %s)", lo->bindingUuid.c_str());
					/* libobs only logs a drop summary when frames were actually
					 * dropped, so a clean session is silent -- "no drops" then
					 * reads as absence, not confirmation. Emit an unconditional
					 * per-output summary so the log positively states the result. */
					const int totalFrames = obs_output_get_total_frames(out);
					const int droppedFrames = obs_output_get_frames_dropped(out);
					const double droppedPct =
						totalFrames > 0 ? (double)droppedFrames / (double)totalFrames * 100.0
								: 0.0;
					blog(LOG_INFO,
					     "Multistream: output stopped cleanly (binding %s, canvas %s) -- %d/%d frames dropped (%.2f%%)",
					     lo->bindingUuid.c_str(), lo->canvasUuid.c_str(), droppedFrames,
					     totalFrames, droppedPct);
				}
				break;
			}
		}
	}
	self->UpdateSleepInhibit();
	self->NotifyChanged();
	/* The output has fully stopped, so a canvas that went inert while it was still
	 * async-stopping can now safely drop its mix. The bootstrap marshals this to the
	 * UI thread (where every CanvasRuntime reconcile runs) and re-runs Reconcile. */
	if (self->onOutputStopped && !canvasUuid.empty()) {
		self->onOutputStopped(canvasUuid);
	}
}

/* Runs on the output thread, once per scheduled retry. Only flips state under
 * liveMutex -- never destroys a LiveOutput (see TakeLive). */
void MultistreamEngine::OnOutputReconnect(void *data, calldata_t *cd)
{
	auto self = static_cast<MultistreamEngine *>(data);
	obs_output_t *out = (obs_output_t *)calldata_ptr(cd, "output");
	{
		std::lock_guard<std::mutex> lock(self->liveMutex);
		for (auto &lo : self->live) {
			if (lo->output == out) {
				lo->state = State::Reconnecting;
				const char *err = obs_output_get_last_error(out);
				lo->lastError = err ? err : "";
				DBG(LogCat::Net, "rtmp dropped, reconnecting in %d s (binding %s): %s",
				    (int)calldata_int(cd, "timeout_sec"), lo->bindingUuid.c_str(),
				    lo->lastError.c_str());
				break;
			}
		}
	}
	self->NotifyChanged();
}

void MultistreamEngine::OnOutputReconnectSuccess(void *data, calldata_t *cd)
{
	auto self = static_cast<MultistreamEngine *>(data);
	obs_output_t *out = (obs_output_t *)calldata_ptr(cd, "output");
	{
		std::lock_guard<std::mutex> lock(self->liveMutex);
		for (auto &lo : self->live) {
			if (lo->output == out) {
				lo->state = State::Live;
				lo->lastError.clear();
				DBG(LogCat::Net, "rtmp reconnected (binding %s, canvas %s)", lo->bindingUuid.c_str(),
				    lo->canvasUuid.c_str());
				break;
			}
		}
	}
	self->NotifyChanged();
}
