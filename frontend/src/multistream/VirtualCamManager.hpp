#pragma once

#include <obs.hpp>

#include <atomic>
#include <functional>
#include <string>

// Owns the OBS virtual-camera output ("virtualcam_output"), feeding it a chosen
// canvas's video mix. The output is created lazily on first Start() and released
// in Shutdown(). The target canvas uuid persists to virtualcam.json; an
// empty/unknown/Default uuid resolves to the global program video.
class VirtualCamManager {
public:
	VirtualCamManager() = default;
	~VirtualCamManager() = default;

	VirtualCamManager(const VirtualCamManager &) = delete;
	VirtualCamManager &operator=(const VirtualCamManager &) = delete;

	// Start the virtual camera, feeding it the target canvas's video (or the
	// global program video for the Default/unknown/inactive canvas). Idempotent:
	// returns true if already active. On failure fills `err` and returns false.
	bool Start(std::string &err);
	// Stop the virtual camera if active. The actual stop is async, and "stop" fires
	// while libobs is still draining; "deactivate" is the first signal that means
	// fully down, which is why the preview release keys off that one.
	void Stop();
	bool IsActive() const;

	// Set the target canvas uuid. MVP: if the camera is already active the new
	// feed applies on the NEXT Start (stock OBS restarts the output to swap its
	// source); we keep the current feed running until restarted.
	void SetTargetCanvas(const std::string &uuid);
	const std::string &TargetCanvas() const { return targetCanvas_; }

	// True while the vcam still holds a live mix ref on canvas `uuid`: its output's
	// video_t references that canvas's mix from Start until the async stop drains.
	// Reports heldCanvas_ (the canvas actually being fed), NOT targetCanvas_, which
	// may already point at a not-yet-applied next target. Lets the canvas.remove /
	// live-edit gate treat a vcam-fed canvas as live so its mix is never freed under
	// the running output (render-thread UAF).
	bool FeedsCanvas(const std::string &uuid) const;

	// Stop + disconnect the signals + release the output. Call during teardown
	// while libobs is still up, BEFORE the canvases/runtime it feeds are torn down.
	void Shutdown();

	// Persist/restore the target canvas to virtualcam.json (key "canvas").
	void Load();
	bool Save() const;

	// Fired (possibly off the libobs thread, via the output start/stop signals)
	// whenever the active state changes. The bridge routes it through its
	// thread-safe EmitEvent, so off-thread invocation is fine.
	std::function<void()> onChanged;

private:
	static void OnStart(void *data, calldata_t *cd);
	static void OnStop(void *data, calldata_t *cd);
	static void OnDeactivate(void *data, calldata_t *cd);
	void NotifyChanged();
	// Drop the preview refcount held on heldCanvas_ (see Start), but only once the
	// output is confirmed down -- its media still references the mix's video_t while
	// active, so freeing the mix under a live output would be a UAF. A still-active
	// (async-stopping) output defers; PostReleaseTargetPreview re-drives it from the
	// output's own stop/deactivate signals once the drain completes.
	void ReleaseTargetPreview();
	// Marshal ReleaseTargetPreview onto the UI thread. The output signals fire on
	// libobs threads, while CanvasRuntime (and the video gate behind it) is
	// unsynchronized UI-thread-only state.
	void PostReleaseTargetPreview();

	OBSOutputAutoRelease vcam_; // owned: created lazily, released in Shutdown
	std::string targetCanvas_;
	// The runtime canvas we activated (via CanvasRuntime::AddPreview) to guarantee a
	// mix while feeding the vcam. It is legitimately empty when the target is the
	// Default canvas addressed by the empty uuid, so emptiness cannot stand in for
	// "nothing held" -- previewRefHeld_ carries that instead.
	std::string heldCanvas_;
	bool previewRefHeld_ = false; // an AddPreview on heldCanvas_ is still outstanding
	// Set from the "deactivate" signal, which libobs raises once the output has
	// disconnected from its media -- the first moment the mix's video_t is provably
	// unreferenced. obs_output_active alone cannot stand in for it: libobs clears
	// that flag one store LATER, on the same thread, so a release marshaled off the
	// stop signal can observe a still-active output and defer forever. Written on a
	// libobs thread, read on the UI thread.
	std::atomic<bool> mediaDrained_{false};
	OBSSignal startSignal_;
	OBSSignal stopSignal_;
	OBSSignal deactivateSignal_;
};
