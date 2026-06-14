/******************************************************************************
    Copyright (C) 2025 by Dennis Sädtler <saedtler@twitch.tv>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

void OBSBasic::CanvasRemoved(void *data, calldata_t *params)
{
	obs_canvas_t *canvas = static_cast<obs_canvas_t *>(calldata_ptr(params, "canvas"));
	QMetaObject::invokeMethod(static_cast<OBSBasic *>(data), "RemoveCanvas", Q_ARG(OBSCanvas, OBSCanvas(canvas)));
}

const OBS::Canvas &OBSBasic::AddCanvas(const std::string &name, obs_video_info *ovi, int flags, const char *uuid)
{
	obs_canvas_t *raw;
	if (uuid && *uuid) {
		/* Preserve the definition's uuid so per-collection scene bindings survive
		 * restarts; obs_canvas_create would mint a fresh uuid each launch. */
		OBSDataAutoRelease data = obs_data_create();
		obs_data_set_string(data, "name", name.c_str());
		obs_data_set_string(data, "uuid", uuid);
		obs_data_set_int(data, "flags", flags);
		raw = obs_load_canvas(data);
		if (raw && ovi && !obs_canvas_reset_video(raw, ovi)) {
			blog(LOG_WARNING, "Canvas '%s' (uuid %s) created without a video mix; reset_video failed",
			     name.c_str(), uuid);
		}
	} else {
		raw = obs_canvas_create(name.c_str(), ovi, flags);
	}
	OBSCanvas canvas = raw;
	auto &it = canvases.emplace_back(canvas);
	EnsureCanvasHasScene(raw);
	OnEvent(OBS_FRONTEND_EVENT_CANVAS_ADDED);
	return it;
}

void OBSBasic::EnsureCanvasHasScene(obs_canvas_t *canvas)
{
	if (!canvas) {
		return;
	}

	struct Ctx {
		obs_source_t *first = nullptr;
	} ctx;
	obs_canvas_enum_scenes(
		canvas,
		[](void *param, obs_source_t *scene) {
			static_cast<Ctx *>(param)->first = scene;
			return false; // first scene is enough
		},
		&ctx);

	obs_source_t *sceneSource = ctx.first;
	if (!ctx.first) {
		OBSSceneAutoRelease scene = obs_canvas_scene_create(canvas, Str("Basic.Scene"));
		sceneSource = obs_scene_get_source(scene);
	}

	OBSSourceAutoRelease cur = obs_canvas_get_channel(canvas, 0);
	if (!cur && sceneSource) {
		obs_canvas_set_channel(canvas, 0, sceneSource);
	}
}

OBSSource OBSBasic::GetCanvasCurrentScene(obs_canvas_t *canvas)
{
	OBSSourceAutoRelease cur = canvas ? obs_canvas_get_channel(canvas, 0) : nullptr;
	return cur.Get(); // OBSSource takes its own ref; the AutoRelease drops the owned one
}

void OBSBasic::SetCanvasCurrentScene(obs_canvas_t *canvas, obs_source_t *sceneSource)
{
	if (canvas && sceneSource) {
		obs_canvas_set_channel(canvas, 0, sceneSource);
	}
}

obs_source_t *OBSBasic::FindCanvasSceneByUuid(obs_canvas_t *canvas, const std::string &uuid)
{
	struct Ctx {
		std::string want;
		obs_source_t *found = nullptr;
	} ctx{uuid, nullptr};
	obs_canvas_enum_scenes(
		canvas,
		[](void *param, obs_source_t *scene) {
			auto *c = static_cast<Ctx *>(param);
			const char *u = obs_source_get_uuid(scene);
			if (u && c->want == u) {
				c->found = scene;
				return false; // stop
			}
			return true;
		},
		&ctx);
	return ctx.found;
}

void OBSBasic::ApplyCanvasSceneLinks()
{
	OBSSource mainScene = GetCurrentSceneSource();
	const char *mainUuid = mainScene ? obs_source_get_uuid(mainScene) : nullptr;
	if (!mainUuid) {
		return;
	}

	for (const OBS::Canvas &canvas : canvases) {
		obs_canvas_t *c = canvas;
		const char *canvasUuid = obs_canvas_get_uuid(c);
		if (!canvasUuid) {
			continue;
		}
		std::string target = canvasSceneLink.Resolve(mainUuid, canvasUuid);
		if (target.empty()) {
			continue; // unlinked: canvas keeps its own current scene
		}
		obs_source_t *match = FindCanvasSceneByUuid(c, target);
		if (match) {
			SetCanvasCurrentScene(c, match);
		}
	}
}

bool OBSBasic::RemoveCanvas(OBSCanvas canvas)
{
	bool removed = false;
	if (!canvas) {
		return removed;
	}

	auto canvas_it = std::find(std::begin(canvases), std::end(canvases), canvas);
	if (canvas_it != std::end(canvases)) {
		// Move canvas to a temporary object to delay removal of the canvas and calls to its signal handlers
		// until after erase() completes. This is to avoid issues with recursion coming from the
		// CanvasRemoved() signal handler.
		OBS::Canvas tmp = std::move(*canvas_it);
		canvases.erase(canvas_it);
		removed = true;
	}

	if (removed) {
		OnEvent(OBS_FRONTEND_EVENT_CANVAS_REMOVED);
	}

	return removed;
}

void OBSBasic::ClearCanvases()
{
	// Delete canvases one-by-one to ensure OBS_FRONTEND_EVENT_CANVAS_REMOVED is sent for each
	while (!canvases.empty()) {
		RemoveCanvas(OBSCanvas(canvases.back()));
	}
}
