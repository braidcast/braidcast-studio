#include "VirtualCamManager.hpp"

#include "CanvasRuntime.hpp"
#include "StorePaths.hpp"

#include "../obs_bootstrap.hpp"

#include <util/platform.h>

// The output id registered by the win-dshow virtual-camera module (legacy
// VIRTUAL_CAM_ID). Create with this id + a fixed name; absent (module not built)
// obs_output_create returns null and Start() reports the failure.
static constexpr const char *kVirtualCamId = "virtualcam_output";

bool VirtualCamManager::Start(std::string &err)
{
	if (IsActive()) {
		return true;
	}

	if (!vcam_) {
		vcam_ = OBSOutputAutoRelease(obs_output_create(kVirtualCamId, kVirtualCamId, nullptr, nullptr));
		if (!vcam_) {
			err = "failed to create virtual camera output (module not loaded)";
			return false;
		}
		signal_handler_t *sh = obs_output_get_signal_handler(vcam_);
		startSignal_.Connect(sh, "start", OnStart, this);
		stopSignal_.Connect(sh, "stop", OnStop, this);
	}

	// Resolve the target canvas's mix; the Default/unknown/inactive canvas has no
	// runtime mix, so fall back to the global program video pipeline.
	video_t *video = ObsBootstrap::CanvasRuntime().VideoFor(targetCanvas_);
	if (!video) {
		video = obs_get_video();
	}
	if (!video) {
		err = "no video pipeline available for virtual camera";
		return false;
	}

	obs_output_set_media(vcam_, video, obs_get_audio());
	if (!obs_output_start(vcam_)) {
		const char *e = obs_output_get_last_error(vcam_);
		err = e ? e : "failed to start virtual camera";
		return false;
	}
	// onChanged fires from the "start" signal once the output is actually live.
	return true;
}

void VirtualCamManager::Stop()
{
	if (IsActive()) {
		obs_output_stop(vcam_);
	}
}

bool VirtualCamManager::IsActive() const
{
	return vcam_ && obs_output_active(vcam_);
}

void VirtualCamManager::SetTargetCanvas(const std::string &uuid)
{
	// MVP: a change while active applies on the next Start (the running feed is
	// left untouched until the output is restarted).
	targetCanvas_ = uuid;
}

void VirtualCamManager::Shutdown()
{
	// Disconnect FIRST: after this the static start/stop callbacks can no longer
	// fire, so NotifyChanged (which reads onChanged) cannot run on a libobs thread
	// concurrently with teardown -- the owner needs no separate onChanged nulling.
	// It also means the obs_output_stop below raises no spurious teardown event.
	startSignal_.Disconnect();
	stopSignal_.Disconnect();
	if (IsActive()) {
		obs_output_stop(vcam_);
	}
	vcam_ = nullptr;
}

void VirtualCamManager::Load()
{
	OBSDataAutoRelease root =
		obs_data_create_from_json_file_safe(MultistreamBasicPath("virtualcam.json").c_str(), "bak");
	if (root) {
		const char *canvas = obs_data_get_string(root, "canvas");
		targetCanvas_ = canvas ? canvas : "";
	}
}

void VirtualCamManager::Save() const
{
	OBSDataAutoRelease root = obs_data_create();
	obs_data_set_string(root, "canvas", targetCanvas_.c_str());

	SaveJsonAtomic(root, MultistreamBasicPath("virtualcam.json"));
}

void VirtualCamManager::NotifyChanged()
{
	if (onChanged) {
		onChanged();
	}
}

void VirtualCamManager::OnStart(void *data, calldata_t * /*cd*/)
{
	static_cast<VirtualCamManager *>(data)->NotifyChanged();
}

void VirtualCamManager::OnStop(void *data, calldata_t * /*cd*/)
{
	static_cast<VirtualCamManager *>(data)->NotifyChanged();
}
