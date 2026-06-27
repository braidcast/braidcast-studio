#pragma once

#include <obs.hpp>

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
	// Stop the virtual camera if active. The actual stop is async; the "stop"
	// signal fires onChanged when the output is fully down.
	void Stop();
	bool IsActive() const;

	// Set the target canvas uuid. MVP: if the camera is already active the new
	// feed applies on the NEXT Start (stock OBS restarts the output to swap its
	// source); we keep the current feed running until restarted.
	void SetTargetCanvas(const std::string &uuid);
	const std::string &TargetCanvas() const { return targetCanvas_; }

	// Stop + disconnect the signals + release the output. Call during teardown
	// while libobs is still up, BEFORE the canvases/runtime it feeds are torn down.
	void Shutdown();

	// Persist/restore the target canvas to virtualcam.json (key "canvas").
	void Load();
	void Save() const;

	// Fired (possibly off the libobs thread, via the output start/stop signals)
	// whenever the active state changes. The bridge routes it through its
	// thread-safe EmitEvent, so off-thread invocation is fine.
	std::function<void()> onChanged;

private:
	static void OnStart(void *data, calldata_t *cd);
	static void OnStop(void *data, calldata_t *cd);
	void NotifyChanged();

	OBSOutputAutoRelease vcam_; // owned: created lazily, released in Shutdown
	std::string targetCanvas_;
	OBSSignal startSignal_;
	OBSSignal stopSignal_;
};
