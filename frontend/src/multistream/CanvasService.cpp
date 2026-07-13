#include "CanvasService.hpp"

#include "CanvasRuntime.hpp"
#include "CanvasStore.hpp"
#include "MultistreamEngine.hpp"

#include <obs.h>
#include <obs.hpp>

#include <utility>

CanvasService::CanvasService(CanvasStore &canvases, CanvasRuntime &runtime, MultistreamEngine &engine,
			     GlobalVideoApplier applyGlobalVideo)
	: canvases(canvases),
	  runtime(runtime),
	  engine(engine),
	  applyGlobalVideo(std::move(applyGlobalVideo))
{
}

CanvasUpdateResult CanvasService::Update(const CanvasUpdateRequest &req)
{
	CanvasUpdateResult result;

	CanvasDefinition *def = canvases.Find(req.uuid);
	if (!def) {
		result.notFound = true;
		result.error = "no canvas with uuid '" + req.uuid + "'";
		return result;
	}

	// Name change is always allowed. Applied to the in-memory def before the
	// structural gate below: a refused structural edit still leaves the new name in
	// memory but never Save()s it, so canvases.json stays consistent with the live
	// pipeline.
	if (!req.name.empty()) {
		def->name = req.name;
	}

	const uint32_t newW = req.width, newH = req.height, newFpsN = req.fpsNum, newFpsD = req.fpsDen;
	const uint32_t newOutW = req.outputWidth, newOutH = req.outputHeight;
	const std::string &newScale = req.scaleType;
	const CanvasColorDef &newColor = req.color;
	const std::string &venc = req.videoEncoderId;
	const std::string &aenc = req.audioEncoderId;

	// Per-section inheritance toggles. useDefaultResolution swaps the effective
	// resolution/fps (Default vs this canvas) via ToVideoInfo, so it resizes the live
	// mix like a raw resolution change; video/audio useDefault swap the effective
	// encoder the engine builds, like an encoder-id change. The Default canvas has no
	// canvas to inherit from, so its resolution toggle is inert (BuildVideoInfo is
	// never called for it) and not treated as structural.
	const bool newUseDefRes = req.useDefaultResolution;
	const bool newVideoUseDefault = req.videoUseDefault;
	const bool newAudioUseDefault = req.audioUseDefault;
	const bool useDefResChanged = newUseDefRes != def->useDefaultResolution && !def->isDefault;
	const bool videoUseDefaultChanged = newVideoUseDefault != def->video.useDefault;
	const bool audioUseDefaultChanged = newAudioUseDefault != def->audio.useDefault;

	// Output res and the downscale filter resize/reconfigure the live mix just like
	// base res, so they are structural and gated by the same live refusal + reset.
	const bool resChanged = newW != def->width || newH != def->height || newFpsN != def->fpsNum ||
				newFpsD != def->fpsDen || newOutW != def->outputWidth || newOutH != def->outputHeight ||
				newScale != def->scaleType || useDefResChanged;
	// A color change rewrites the obs_video_info (output_format/colorspace/range), so
	// it resets the canvas video and rebuilds its encoders exactly like a resolution
	// change -- structural, gated by the same live refusal + reset. Only these three
	// fields reach the pipeline (via ToVideoInfo); the sdr/hdr nit levels and
	// useDefault are persisted below but never force a reset or block while-live.
	const bool colorChanged = newColor.format != def->color.format || newColor.space != def->color.space ||
				  newColor.range != def->color.range;
	const bool vencChanged = !venc.empty() && venc != def->video.id;
	const bool aencChanged = !aenc.empty() && aenc != def->audio.id;
	// Toggling an encoder's inheritance swaps the effective encoder just like an
	// id change, so it is gated by the same live refusal + encoder-cache invalidation.
	const bool encDefChanged = videoUseDefaultChanged || audioUseDefaultChanged;

	if ((resChanged || colorChanged || vencChanged || aencChanged || encDefChanged) &&
	    engine.IsCanvasLive(req.uuid)) {
		result.refusedLive = true;
		result.error = "cannot change resolution/fps/color/encoder while the canvas is live";
		return result;
	}

	// The Default canvas has no runtime mix -- a resolution/fps/color change drives
	// the global/main video pipeline instead. Apply it BEFORE committing any def
	// fields so a failed reset (rolled back inside the applier) leaves the def -- and
	// thus canvases.json -- untouched and consistent with the live pipeline.
	if ((resChanged || colorChanged) && def->isDefault) {
		CanvasDefinition scratch;
		scratch.width = newW;
		scratch.height = newH;
		scratch.outputWidth = newOutW;
		scratch.outputHeight = newOutH;
		scratch.fpsNum = newFpsN;
		scratch.fpsDen = newFpsD;
		scratch.scaleType = newScale;
		scratch.color = newColor;
		std::string e;
		if (!applyGlobalVideo(scratch, e)) {
			result.error = e;
			return result;
		}
	}

	def->width = newW;
	def->height = newH;
	def->outputWidth = newOutW;
	def->outputHeight = newOutH;
	def->scaleType = newScale;
	def->fpsNum = newFpsN;
	def->fpsDen = newFpsD;
	def->color = newColor;
	def->useDefaultResolution = newUseDefRes;
	def->video.useDefault = newVideoUseDefault;
	def->audio.useDefault = newAudioUseDefault;
	// Switching an encoder id replaces its stored settings with that type's
	// defaults (the prior blob belongs to a different encoder schema).
	if (vencChanged) {
		def->video.id = venc;
		def->video.settings = obs_encoder_defaults(venc.c_str());
	}
	if (aencChanged) {
		def->audio.id = aenc;
		def->audio.settings = obs_encoder_defaults(aenc.c_str());
	}

	// A structural change invalidates the engine's cached encoder pair (bound to
	// the old resolution/color/id), so a later restart rebuilds it against the new
	// mix. InvalidateCanvasEncoders is inheritor-aware: editing the Default drops
	// every inheriting canvas's cached pair too.
	if (resChanged || colorChanged || vencChanged || aencChanged || encDefChanged) {
		engine.InvalidateCanvasEncoders(req.uuid);
	}
	// Resolution/fps changes resize the live mix; the guard above already refused
	// while the canvas is live, so this only runs on an idle canvas. (Encoder-id
	// changes don't touch the mix.) The Default canvas was already handled above
	// (it drives global video, not a runtime mix); ResetVideo reads *def so it must
	// run after the commit.
	if ((resChanged || colorChanged) && !def->isDefault) {
		runtime.ResetVideo(*def);
	}

	canvases.Save();

	result.ok = true;
	result.def = def;
	return result;
}
