#pragma once

#include <obs.hpp>
#include <string>

/* A persisted canvas *definition* — global, survives profile/scene-collection
 * switches. Resolution is a single value (edit-surface == encode size). Encoder
 * settings are stored as raw obs_data JSON blobs and only rehydrated into real
 * encoders in Sub-plan 4. */
struct CanvasEncoderDef {
	std::string id;              // e.g. "obs_x264", "ffmpeg_aac"
	OBSDataAutoRelease settings; // encoder settings blob (may be null -> defaults)
	bool useDefault = false;     // live-follow the Default canvas

	/* This encoder slot resolves to the Default canvas's encoder: either it is
	 * explicitly flagged use-default, or it was never configured (empty id, so
	 * there is nothing to build). Unlike resolution/color inheritance (a pure
	 * use-default flag), an encoder carries an id, so the empty-id case must
	 * inherit too rather than yield a broken encoder. */
	bool InheritsDefault() const { return useDefault || id.empty(); }
};

struct CanvasColorDef {
	std::string format = "NV12";   // ColorFormat
	std::string space = "709";     // ColorSpace
	std::string range = "Partial"; // ColorRange
	uint32_t sdrWhiteLevel = 300;
	uint32_t hdrNominalPeakLevel = 1000;
	bool useDefault = true; // inherit global/Default advanced color
};

struct CanvasDefinition {
	std::string uuid; // correlates with obs_canvas context uuid
	std::string name;
	bool isDefault = false; // the immutable base canvas

	uint32_t width = 1920;
	uint32_t height = 1080;
	uint32_t outputWidth = 0;  // scaled encode size; 0 = mirror base width
	uint32_t outputHeight = 0; // scaled encode size; 0 = mirror base height
	uint32_t fpsNum = 60;
	uint32_t fpsDen = 1;
	std::string scaleType = "bicubic"; // downscale filter token (see kScaleFilters)
	bool useDefaultResolution = false;

	CanvasEncoderDef video;
	CanvasEncoderDef audio; // Phase 1: single audio track (mixer 0)
	CanvasColorDef color;

	/* Serialize to a single obs_data object (one array element in canvases.json). */
	[[nodiscard]] OBSDataAutoRelease ToData() const;
	/* Build from one obs_data object; missing keys fall back to struct defaults. */
	static CanvasDefinition FromData(obs_data_t *data);

	/* Part of this canvas's effective pipeline config resolves from the Default
	 * canvas: an encoder slot (InheritsDefault), base/output resolution + fps
	 * (useDefaultResolution), or color (color.useDefault) -- see ToVideoInfo and
	 * MultistreamEngine::EnsureCanvasEncoders. Editing the Default therefore
	 * changes this canvas's effective config too. */
	bool InheritsAnyDefault() const
	{
		return video.InheritsDefault() || audio.InheritsDefault() || useDefaultResolution || color.useDefault;
	}

	/* Fill an obs_video_info from this definition's resolution/fps/color. */
	void ToVideoInfo(struct obs_video_info &ovi, const CanvasDefinition *inheritFrom = nullptr) const;
};
