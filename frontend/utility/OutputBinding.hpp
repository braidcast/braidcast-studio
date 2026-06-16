#pragma once

#include <obs.hpp>
#include <string>
#include <vector>

/* One routing edge: a Stream profile (2c, by uuid) bound to a canvas (by uuid)
 * with an enable flag. Per scene-collection. Pure data — no libobs/Qt side
 * effects; the streaming engine (2e) is what acts on enabled bindings. */
struct OutputBinding {
	std::string uuid;        // identity of this binding row
	std::string profileUuid; // -> StreamProfile::uuid (may be empty = unset)
	std::string canvasUuid;  // -> obs_canvas uuid
	bool enabled = false;
};

/* Collection of bindings, serialized inside the scene-collection JSON as an
 * "output_bindings" array. Mirrors CanvasSceneLink. */
struct OutputBindings {
	std::vector<OutputBinding> bindings;

	OutputBinding &
	Add(const std::string &canvasUuid); // assigns a uuid, returns ref (invalidated by next Add/Remove)
	void Remove(const std::string &uuid);
	OutputBinding *Find(const std::string &uuid);
	/* All bindings for one canvas, in insertion order. */
	std::vector<OutputBinding *> ForCanvas(const std::string &canvasUuid);
	/* True if some binding for this canvas is enabled. Gates the canvas preview
	 * (Outputs are the source of truth for whether a canvas renders live). */
	bool AnyEnabledForCanvas(const std::string &canvasUuid) const;
	/* True if some OTHER binding with the same non-empty profile is already
	 * enabled (single RTMP key = one live stream). Used for the "in use" guard. */
	bool ProfileEnabledElsewhere(const std::string &bindingUuid, const std::string &profileUuid) const;

	[[nodiscard]] OBSDataArrayAutoRelease ToDataArray() const;
	static OutputBindings FromDataArray(obs_data_array_t *arr);
};
