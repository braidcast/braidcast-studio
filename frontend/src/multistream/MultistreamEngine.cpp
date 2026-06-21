#include "MultistreamEngine.hpp"

#include "CanvasStore.hpp"
#include "OutputBindingStore.hpp"
#include "StreamProfileStore.hpp"

#include <CanvasDefinition.hpp>
#include <OutputBinding.hpp>
#include <StreamProfile.hpp>

#include <array>
#include <cstring>

// File-local ports of BasicOutputHandler's stream-output-type resolution, so the
// engine has no dependency on the Qt output handler. Verbatim from
// frontend_old/utility/BasicOutputHandler.{cpp,hpp}.
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
	static constexpr std::array<const char *, 4> kNames = {{"idle", "connecting", "live", "error"}};
	size_t idx = static_cast<size_t>(state);
	return kNames[idx < kNames.size() ? idx : 0];
}

MultistreamEngine::MultistreamEngine(CanvasStore &canvases_, StreamProfileStore &profiles_,
				     OutputBindingStore &bindings_, VideoResolver resolver_)
	: canvases(canvases_),
	  profiles(profiles_),
	  bindings(bindings_),
	  resolver(std::move(resolver_))
{
}

MultistreamEngine::~MultistreamEngine()
{
	/* Defensive: stop everything still live. Clear the callback first so the
	 * StopAll notification can't reach back into a now-dangling owner (e.g. the
	 * unique_ptr that holds us is mid-reset(), so its global accessor is already
	 * null). The owner runs StopAll explicitly before destroying us anyway. */
	onStatusChanged = nullptr;
	StopAll();
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
	const CanvasEncoderDef &vdef = (cdef->video.useDefault || cdef->video.id.empty()) ? def.video : cdef->video;
	const CanvasEncoderDef &adef = (cdef->audio.useDefault || cdef->audio.id.empty()) ? def.audio : cdef->audio;
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
	for (auto it = canvasEncoders.begin(); it != canvasEncoders.end(); ++it) {
		if (it->canvasUuid == canvasUuid) {
			canvasEncoders.erase(it);
			return;
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
		if (lo->bindingUuid != bindingUuid && lo->profileUuid == profileUuid) {
			return true;
		}
	}
	return false;
}

bool MultistreamEngine::StartOutput(const std::string &bindingUuid)
{
	if (IsLive(bindingUuid)) {
		return true;
	}
	OutputBinding *b = bindings.Bindings().Find(bindingUuid);
	if (!b || b->profileUuid.empty()) {
		return false;
	}
	if (ProfileLiveElsewhere(bindingUuid, b->profileUuid)) {
		blog(LOG_WARNING, "Multistream: profile already live; refusing second output");
		return false;
	}
	StreamProfile *p = profiles.Find(b->profileUuid);
	if (!p) {
		return false;
	}
	CanvasEncoders *ce = EnsureCanvasEncoders(b->canvasUuid);
	if (!ce) {
		return false;
	}

	auto lo = std::make_unique<LiveOutput>();
	lo->bindingUuid = bindingUuid;
	lo->profileUuid = b->profileUuid;
	lo->canvasUuid = b->canvasUuid;

	std::string sname = "multistream_svc_" + bindingUuid;
	lo->service =
		OBSServiceAutoRelease(obs_service_create(p->serviceId.c_str(), sname.c_str(), p->settings, nullptr));
	if (!lo->service) {
		return false;
	}

	const char *type = GetStreamOutputType(lo->service);
	if (!type) {
		type = "rtmp_output";
	}
	std::string oname = "multistream_out_" + bindingUuid;
	lo->output = OBSOutputAutoRelease(obs_output_create(type, oname.c_str(), nullptr, nullptr));
	if (!lo->output) {
		return false;
	}

	lo->startSignal.Connect(obs_output_get_signal_handler(lo->output), "start", OnOutputStart, this);
	lo->stopSignal.Connect(obs_output_get_signal_handler(lo->output), "stop", OnOutputStop, this);

	obs_output_set_video_encoder(lo->output, ce->video);
	obs_output_set_audio_encoder(lo->output, ce->audio, 0);
	obs_output_set_service(lo->output, lo->service);

	lo->state = State::Connecting;
	LiveOutput *raw = lo.get();
	{
		std::lock_guard<std::mutex> lock(liveMutex);
		live.push_back(std::move(lo));
	}

	/* obs_output_start is async; raw stays valid because only this (UI) thread
	 * erases `live`. The handler may flip raw->state, so guard the failure write. */
	if (!obs_output_start(raw->output)) {
		const char *err = obs_output_get_last_error(raw->output);
		{
			std::lock_guard<std::mutex> lock(liveMutex);
			raw->lastError = err ? err : "";
			raw->state = State::Error;
		}
		blog(LOG_WARNING, "Multistream: output '%s' failed to start: %s", oname.c_str(), err ? err : "");
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
		obs_output_stop(out);
	}
	{
		std::lock_guard<std::mutex> lock(liveMutex);
		RemoveLive(bindingUuid);
	}
	NotifyChanged();
}

void MultistreamEngine::StartAllEnabled()
{
	for (auto &b : bindings.Bindings().bindings) {
		if (b.enabled) {
			StartOutput(b.uuid);
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
	{
		std::lock_guard<std::mutex> lock(liveMutex);
		live.clear();
	}
	/* Encoders are released when the handler is destroyed or rebuilt; keep the
	 * cache so a quick restart reuses them, but they hold no output refs now. */
	NotifyChanged();
}

bool MultistreamEngine::IsLive(const std::string &bindingUuid) const
{
	std::lock_guard<std::mutex> lock(liveMutex);
	for (const auto &lo : live) {
		if (lo->bindingUuid == bindingUuid) {
			return true;
		}
	}
	return false;
}

bool MultistreamEngine::IsCanvasLive(const std::string &canvasUuid) const
{
	std::lock_guard<std::mutex> lock(liveMutex);
	for (const auto &lo : live) {
		if (lo->canvasUuid == canvasUuid) {
			return true;
		}
	}
	return false;
}

bool MultistreamEngine::AnyLive() const
{
	std::lock_guard<std::mutex> lock(liveMutex);
	return !live.empty();
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

void MultistreamEngine::RemoveLive(const std::string &bindingUuid)
{
	for (auto it = live.begin(); it != live.end(); ++it) {
		if ((*it)->bindingUuid == bindingUuid) {
			live.erase(it);
			return;
		}
	}
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
		if (!b.profileUuid.empty()) {
			if (StreamProfile *p = profiles.Find(b.profileUuid)) {
				st.profileLabel = p->DisplayName();
			}
		}
		const CanvasDefinition &def = canvases.Default();
		if (b.canvasUuid == def.uuid) {
			st.canvasName = def.name;
		} else if (const CanvasDefinition *cdef = canvases.Find(b.canvasUuid)) {
			st.canvasName = cdef->name;
		}
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
	{
		std::lock_guard<std::mutex> lock(self->liveMutex);
		for (auto &lo : self->live) {
			if (lo->output == out) {
				/* A non-success code means the stream dropped or never
				 * connected (e.g. a bad key); surface it as Error. */
				if (code != OBS_OUTPUT_SUCCESS) {
					lo->state = State::Error;
					lo->lastError = lastError ? lastError : "";
				} else {
					lo->state = State::Idle;
				}
				break;
			}
		}
	}
	self->NotifyChanged();
}
