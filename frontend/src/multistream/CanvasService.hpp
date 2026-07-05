#pragma once

#include "CanvasDefinition.hpp"

#include <cstdint>
#include <functional>
#include <string>

class CanvasStore;
class CanvasRuntime;
class MultistreamEngine;

// The canvas update/reconciliation domain: applies a desired canvas definition to
// the model + the live video pipeline, owning the rules that make this the most
// dangerous edit in the app. Extracted verbatim from the bridge's canvas.update
// handler so the JSON adapter stays thin. Owns, in order:
//   - the structural-field diff (which fields force a video reset),
//   - the live-refusal gate (structural edits are refused while an output is live,
//     because a live encoder is bound to the mix a reset would free -> UAF),
//   - the Default-canvas -> global/main video coupling (the Default canvas has no
//     runtime mix, so its resolution/color drives obs_reset_video instead),
//   - the commit-then-reset ordering (global video applied BEFORE committing the
//     def so a failed reset leaves the def -- and canvases.json -- untouched),
//   - encoder-cache invalidation (inheritor-aware: editing the Default staleifies
//     every inheriting canvas's cached encoder pair too).
//
// It performs NO JSON parsing and builds NO JSON response -- the bridge resolves +
// range-validates the request and shapes the result. Bootstrap-owned; constructed
// with the single shared CanvasStore / CanvasRuntime / MultistreamEngine instances
// so it never reconstructs or copies their state.

// The resolved intent of a canvas.update, already parsed + range-validated by the
// bridge. Scalar fields carry the desired post-edit values (the bridge seeds them
// from the current def for absent JSON keys, so an unchanged field compares equal).
// An empty name means "leave the name unchanged"; an empty encoder id means "leave
// that encoder unchanged".
struct CanvasUpdateRequest {
	std::string uuid;
	std::string name; // empty -> no change

	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	uint32_t fpsNum = 0;
	uint32_t fpsDen = 0;
	std::string scaleType;
	CanvasColorDef color;

	bool useDefaultResolution = false;
	bool videoUseDefault = false;
	bool audioUseDefault = false;

	std::string videoEncoderId; // empty -> keep current
	std::string audioEncoderId; // empty -> keep current
};

struct CanvasUpdateResult {
	bool ok = false;
	bool refusedLive = false; // a structural edit was refused because the canvas is live
	bool notFound = false;    // no canvas with the request's uuid
	std::string error;
	// The committed definition on success (owned by the CanvasStore; valid until the
	// next store mutation). Null on failure. The bridge serializes it into the
	// canvas.update response.
	const CanvasDefinition *def = nullptr;
};

class CanvasService {
public:
	// Applies the Default canvas's resolution/color to the global/main video
	// pipeline (obs_reset_video), returning false + setting `error` on a failed
	// reset. Injected so the service stays free of the bridge's preview/transition
	// side-effects (mirrors MultistreamEngine's VideoResolver): the bridge owns the
	// pipeline reset, the service owns WHEN + in what order it runs.
	using GlobalVideoApplier = std::function<bool(const CanvasDefinition &desired, std::string &error)>;

	CanvasService(CanvasStore &canvases, CanvasRuntime &runtime, MultistreamEngine &engine,
		      GlobalVideoApplier applyGlobalVideo);

	CanvasUpdateResult Update(const CanvasUpdateRequest &req);

private:
	CanvasStore &canvases;
	CanvasRuntime &runtime;
	MultistreamEngine &engine;
	GlobalVideoApplier applyGlobalVideo;
};
