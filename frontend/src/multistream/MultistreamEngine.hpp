#pragma once

#include <obs.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class CanvasStore;
class StreamProfileStore;
class OutputBindingStore;
struct OutputBinding;

/* Encode-once, fan-out streaming engine, independent of the single-stream
 * BasicOutputHandler. For each ENABLED output binding it streams the binding's
 * canvas (encoded once per canvas) to the binding's stream profile. De-Qt'd port
 * of the legacy MultistreamOutput: it binds to the new (non-Qt) CanvasStore /
 * StreamProfileStore / OutputBindingStore and notifies via a plain callback the
 * bridge routes through its thread-safe EmitEvent (no QMetaObject). */
class MultistreamEngine {
public:
	enum class State { Idle, Connecting, Live, Error };

	/* Lowercase state name carried in the status JSON; the Svelte side maps it
	 * to a status-dot color. Single source of truth for the contract. */
	static const char *StateName(State state);

	struct OutputStatus {
		std::string bindingUuid;
		std::string canvasUuid;
		std::string profileLabel; // {platform} - {label}
		std::string canvasName;
		State state = State::Idle;
		std::string lastError;
	};

	/* Numeric performance snapshot for one enabled binding. Plain struct (no
	 * json/bridge dependency); the bridge shapes it into the stats payload and
	 * derives bitrate from the delta of totalBytes across polls. A non-live
	 * binding yields zeros + its current state. */
	struct OutputStats {
		std::string bindingUuid;
		std::string canvasUuid;
		std::string profileLabel; // {platform} - {label}
		std::string canvasName;
		State state = State::Idle;
		uint64_t totalBytes = 0;
		int droppedFrames = 0;
		int totalFrames = 0;
		double congestion = 0.0; // 0..1
		uint64_t uptimeMs = 0;
	};

	/* Resolver: video_t* for a canvas uuid (Default -> obs_get_video();
	 * additional -> nullptr until 4.4.5). Injected so the engine never reaches
	 * into bootstrap/canvas-binding internals. */
	using VideoResolver = std::function<video_t *(const std::string &canvasUuid)>;

	MultistreamEngine(CanvasStore &canvases, StreamProfileStore &profiles, OutputBindingStore &bindings,
			  VideoResolver resolver);
	~MultistreamEngine();

	/* Start/stop one binding by uuid (used by the bridge per-row toggle). The
	 * error-returning overload fills `error` with the reason a start refused/failed
	 * (empty profile, profile live elsewhere, encoder build, obs_output_start), so the
	 * caller can surface WHY a row won't go live instead of a bare false; the reason is
	 * also stashed on the binding's status (Statuses().lastError). */
	bool StartOutput(const std::string &bindingUuid);
	bool StartOutput(const std::string &bindingUuid, std::string &error);
	void StopOutput(const std::string &bindingUuid);
	/* Start every enabled binding; logs per-binding failures. */
	void StartAllEnabled();
	/* Stop everything. Used on teardown. */
	void StopAll();

	bool IsLive(const std::string &bindingUuid) const;
	/* True if any output is currently running on this canvas. While running, the
	 * canvas's encoders are bound to its video mix, so the mix must not be reset
	 * (obs_canvas_reset_video frees it -> UAF). */
	bool IsCanvasLive(const std::string &canvasUuid) const;
	/* True iff a streaming output on this canvas is still truly active per libobs
	 * (obs_output_active on the real handle), NOT merely per our live-list state. A
	 * user stop may have flipped the state while the RTMP output is still draining
	 * asynchronously, and its encoder keeps the canvas mix bound until the handle goes
	 * inactive. CanvasRuntime gates its mix-drop on this so the video mix is never
	 * freed while an encoder still pulls from it. */
	bool CanvasHasActiveOutput(const std::string &canvasUuid) const;
	bool AnyLive() const;
	/* True if some OTHER currently-live output uses this profile. */
	bool ProfileLiveElsewhere(const std::string &bindingUuid, const std::string &profileUuid) const;

	/* Drop the cached encoder pair for a canvas. Call when the canvas's video is
	 * reset or the canvas is removed: the cached encoder is bound to the old video
	 * mix, which obs_canvas_reset_video frees, so reusing it would be a UAF. */
	void InvalidateCanvasEncoders(const std::string &canvasUuid);

	std::vector<OutputStatus> Statuses() const;

	/* Numeric per-binding performance snapshot for the Stats dock. Mirrors how
	 * Statuses() enumerates enabled bindings, but reads live counters off each
	 * binding's obs_output_t (total bytes, dropped/total frames, congestion,
	 * connect time). Non-live bindings yield zeros + their current state. */
	std::vector<OutputStats> StatsSnapshot() const;

	/* Invoked (possibly off the libobs thread, via the output signal handlers)
	 * whenever any output's state changes. The bridge target routes through its
	 * thread-safe EmitEvent, so off-thread invocation is fine. */
	std::function<void()> onStatusChanged;

	/* Invoked (off the libobs thread, from an output's stop signal) with the canvas
	 * uuid once one of its outputs has fully stopped. The bootstrap marshals this to
	 * the UI thread and re-runs CanvasRuntime::Reconcile, so a canvas that went inert
	 * while an output was still async-stopping drops its mix once the stop completes. */
	std::function<void(const std::string &canvasUuid)> onOutputStopped;

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
		/* Set when the output signals start (goes Live); the stats snapshot derives
		 * elapsed uptime from it. obs_output_get_connect_time_ms is a one-time
		 * handshake latency, not a running counter, so it can't drive the badge. */
		uint64_t liveStartNs = 0;
	};

	/* Display strings shared by Statuses()/StatsSnapshot() for one binding: the
	 * profile's "{platform} - {label}" and the canvas's name. */
	struct BindingMeta {
		std::string profileLabel;
		std::string canvasName;
	};
	BindingMeta ResolveBindingMeta(const OutputBinding &b) const;

	/* Get-or-create the shared encoder pair for a canvas, bound to that
	 * canvas's video mix + the global audio. Returns nullptr on failure. */
	CanvasEncoders *EnsureCanvasEncoders(const std::string &canvasUuid);
	/* video_t for a canvas: delegates to the injected resolver (obs_get_video()
	 * for the Default, the additional canvas's mix otherwise, NULL if none). */
	video_t *VideoForCanvas(const std::string &canvasUuid);
	/* A binding "occupies" its canvas/profile only while its output is actively
	 * connecting or streaming. An Error (failed start or dropped mid-stream) or Idle
	 * entry is a dead output: it must NOT count as live (else IsCanvasLive stays true
	 * for a dead stream, blocking canvas edits) yet it lingers in `live` so its
	 * lastError still surfaces, until a retry reaps it or StopOutput/StopAll clears it. */
	static bool IsActiveState(State state) { return state == State::Connecting || state == State::Live; }

	/* FindLive/RemoveLive assume the caller already holds liveMutex. */
	LiveOutput *FindLive(const std::string &bindingUuid);
	void RemoveLive(const std::string &bindingUuid);
	void NotifyChanged();

	static void OnOutputStart(void *data, calldata_t *cd);
	static void OnOutputStop(void *data, calldata_t *cd);

	CanvasStore &canvases;
	StreamProfileStore &profiles;
	OutputBindingStore &bindings;
	VideoResolver resolver;
	/* Built once per canvas on first use and reused until InvalidateCanvasEncoders
	 * clears the entry; NOT rebuilt automatically when a canvas definition changes. */
	std::vector<CanvasEncoders> canvasEncoders;
	/* The off-thread output start/stop signal handlers read `live` while the UI
	 * thread inserts/erases it; liveMutex guards every access. It is never held
	 * across an obs_output_start/stop call (those can fire signals -> handler ->
	 * re-lock -> deadlock), only around the bare vector operations. */
	mutable std::mutex liveMutex;
	std::vector<std::unique_ptr<LiveOutput>> live;
};
