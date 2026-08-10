#include "VirtualCamManager.hpp"

#include "CanvasRuntime.hpp"
#include "StorePaths.hpp"

#include "../obs_bootstrap.hpp"

#include "util/async_task.hpp"

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

	// Release a canvas still held from a prior async stop before acquiring the new
	// target: the output is now down (not active), so freeing that mix is safe, and
	// this prevents a target change across stop/start from leaking the old refcount.
	ReleaseTargetPreview();

	if (!vcam_) {
		vcam_ = OBSOutputAutoRelease(obs_output_create(kVirtualCamId, kVirtualCamId, nullptr, nullptr));
		if (!vcam_) {
			err = "failed to create virtual camera output (module not loaded)";
			return false;
		}
		signal_handler_t *sh = obs_output_get_signal_handler(vcam_);
		startSignal_.Connect(sh, "start", OnStart, this);
		stopSignal_.Connect(sh, "stop", OnStop, this);
		deactivateSignal_.Connect(sh, "deactivate", OnDeactivate, this);
	}

	// Activate the target canvas so its mix exists before binding the output to it:
	// in the lazy model an inert runtime canvas has no mix, so without this VideoFor
	// would return null and we would wrongly feed the global program video. The held
	// preview refcount also keeps the mix alive for the whole live duration, so a
	// ReconcileAll from an unrelated binding edit cannot free it under the output.
	// The Default/empty uuid has no mix to build (VideoFor then falls back); there the
	// ref keeps the main composite showing instead. An unknown uuid no-ops.
	ObsBootstrap::CanvasRuntime().AddPreview(targetCanvas_);
	previewRefHeld_ = true;
	heldCanvas_ = targetCanvas_;

	// Resolve the target canvas's mix; the Default/unknown/inactive canvas has no
	// runtime mix, so fall back to the global program video pipeline.
	video_t *video = ObsBootstrap::CanvasRuntime().VideoFor(targetCanvas_);
	if (!video) {
		video = obs_get_video();
	}
	if (!video) {
		err = "no video pipeline available for virtual camera";
		ReleaseTargetPreview(); // balance: output never went live (still inactive)
		return false;
	}

	obs_output_set_media(vcam_, video, obs_get_audio());
	// Armed before the start so the drain flag can only ever describe THIS run: a
	// "deactivate" raised by the run below must not be overwritten afterwards.
	mediaDrained_.store(false, std::memory_order_release);
	if (!obs_output_start(vcam_)) {
		const char *e = obs_output_get_last_error(vcam_);
		err = e ? e : "failed to start virtual camera";
		ReleaseTargetPreview(); // balance: start failed, output not active
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
	// Let the target canvas go inert now if the stop was synchronous; a still-active
	// (async-stopping) output defers (guarded inside) and the output's own
	// stop/deactivate signals re-drive the release once it has drained.
	ReleaseTargetPreview();
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

bool VirtualCamManager::FeedsCanvas(const std::string &uuid) const
{
	// heldCanvas_ names the canvas being fed from Start until ReleaseTargetPreview
	// confirms the output down, so it matches the live-mix-reference window. The
	// empty uuid is excluded deliberately: that is the Default canvas, which has no
	// runtime mix for anything to free under the output.
	return !heldCanvas_.empty() && heldCanvas_ == uuid;
}

void VirtualCamManager::Shutdown()
{
	// Disconnect FIRST: after this the static start/stop/deactivate callbacks can no
	// longer fire, so NotifyChanged (which reads onChanged) cannot run on a libobs
	// thread concurrently with teardown -- the owner needs no separate onChanged
	// nulling. It also means the obs_output_stop below raises no spurious teardown
	// event, and queues no release task that would outlive this call.
	startSignal_.Disconnect();
	stopSignal_.Disconnect();
	deactivateSignal_.Disconnect();
	if (IsActive()) {
		obs_output_stop(vcam_);
	}
	vcam_ = nullptr;
	// The output is released; the vcam_-null guard in ReleaseTargetPreview now passes,
	// so any canvas still held for the vcam is dropped before CanvasRuntime is torn
	// down (Shutdown runs before g_canvasRuntime->ClearAll in the bootstrap teardown).
	ReleaseTargetPreview();
}

void VirtualCamManager::ReleaseTargetPreview()
{
	if (!previewRefHeld_) {
		return;
	}
	if (vcam_ && obs_output_active(vcam_) && !mediaDrained_.load(std::memory_order_acquire)) {
		return; // output still pulling this mix's video; defer the free
	}
	ObsBootstrap::CanvasRuntime().RemovePreview(heldCanvas_);
	previewRefHeld_ = false;
	heldCanvas_.clear();
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

bool VirtualCamManager::Save() const
{
	OBSDataAutoRelease root = obs_data_create();
	obs_data_set_string(root, "canvas", targetCanvas_.c_str());

	const std::string path = MultistreamBasicPath("virtualcam.json");
	return ReportSaveResult(SaveJsonAtomic(root, path), path);
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
	auto *self = static_cast<VirtualCamManager *>(data);
	self->NotifyChanged();
	// Stop() saw the output still active and deferred the release; without this
	// re-drive nothing would ever take it, leaving the target canvas activated (and
	// the Default canvas ungatable) until the app is relaunched.
	self->PostReleaseTargetPreview();
}

void VirtualCamManager::OnDeactivate(void *data, calldata_t * /*cd*/)
{
	auto *self = static_cast<VirtualCamManager *>(data);
	self->mediaDrained_.store(true, std::memory_order_release);
	// libobs raises "stop" from the thread that spawns the drain and "deactivate"
	// from the drain itself, so the stop-signal re-drive above is the one that races
	// the active flag and this one is the one that always resolves it.
	self->PostReleaseTargetPreview();
}

void VirtualCamManager::PostReleaseTargetPreview()
{
	// `this` is the single process-lifetime instance the bootstrap owns, so it cannot
	// dangle; a task that lands after Shutdown finds previewRefHeld_ already false
	// and no-ops.
	AsyncTask::PostToUi([this] { ReleaseTargetPreview(); });
}
