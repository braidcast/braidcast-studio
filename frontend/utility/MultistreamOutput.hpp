#pragma once

#include <obs.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class OBSBasic;

/* Encode-once, fan-out streaming engine, independent of the single-stream
 * BasicOutputHandler. For each ENABLED output binding it streams the binding's
 * canvas (encoded once per canvas) to the binding's stream profile. */
class MultistreamOutput {
public:
	enum class State { Idle, Connecting, Live, Error };

	struct OutputStatus {
		std::string bindingUuid;
		std::string canvasUuid;
		std::string profileLabel; // {platform} - {label}
		State state = State::Idle;
		std::string lastError;
	};

	explicit MultistreamOutput(OBSBasic *main);
	~MultistreamOutput();

	/* Start every enabled binding not already live. Honors the single-key
	 * guard (one live output per profile). Returns the number started. */
	int StartAll();
	/* Stop everything. */
	void StopAll();
	/* Start/stop one binding by uuid (used by the dock row + cascade). */
	bool StartOutput(const std::string &bindingUuid);
	void StopOutput(const std::string &bindingUuid);

	bool AnyActive() const;
	bool IsLive(const std::string &bindingUuid) const;
	/* True if some OTHER currently-live output uses this profile. */
	bool ProfileLiveElsewhere(const std::string &bindingUuid, const std::string &profileUuid) const;

	std::vector<OutputStatus> Statuses() const;

	/* Invoked (on the Qt main thread) whenever any output's state changes, so
	 * the dock can refresh. */
	std::function<void()> onStatusChanged;

private:
	struct CanvasEncoders {
		std::string canvasUuid;
		OBSEncoderAutoRelease video;
		OBSEncoderAutoRelease audio;
	};
	struct LiveOutput {
		std::string bindingUuid;
		std::string profileUuid;
		std::string canvasUuid;
		OBSServiceAutoRelease service;
		OBSOutputAutoRelease output;
		OBSSignal startSignal;
		OBSSignal stopSignal;
		State state = State::Connecting;
		std::string lastError;
	};

	/* Get-or-create the shared encoder pair for a canvas, bound to that
	 * canvas's video mix + the global audio. Returns nullptr on failure. */
	CanvasEncoders *EnsureCanvasEncoders(const std::string &canvasUuid);
	/* video_t for a canvas: obs_get_video() for the Default, else the
	 * additional canvas's mix. NULL if not found/no mix. */
	video_t *VideoForCanvas(const std::string &canvasUuid);
	/* FindLive/RemoveLive assume the caller already holds liveMutex. */
	LiveOutput *FindLive(const std::string &bindingUuid);
	void RemoveLive(const std::string &bindingUuid);
	void NotifyChanged();

	static void OnOutputStart(void *data, calldata_t *cd);
	static void OnOutputStop(void *data, calldata_t *cd);

	OBSBasic *main;
	std::vector<CanvasEncoders> canvasEncoders; // cached, rebuilt lazily
	/* The off-thread output start/stop signal handlers read `live` while the Qt
	 * thread inserts/erases it; liveMutex guards every access. It is never held
	 * across an obs_output_start/stop call (those can fire signals → handler →
	 * re-lock → deadlock), only around the bare vector operations. */
	mutable std::mutex liveMutex;
	std::vector<std::unique_ptr<LiveOutput>> live;
};
