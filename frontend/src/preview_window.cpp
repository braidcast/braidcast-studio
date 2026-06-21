// bridge.hpp pulls in the CEF headers, whose CefDOMNode declares methods like
// GetNextSibling that <windows.h> would otherwise macro-clobber. Include it (and
// thus CEF) before any Windows header so CEF parses clean.
#include "bridge.hpp"

#include "preview_window.hpp"

#include <obs.h>

#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <windowsx.h>

#include <cmath>
#include <mutex>
#include <string>

#include "log.hpp"

// Item-edit handle bit flags, mirroring the legacy preview so the resize math is
// identical. Rotation is deferred but its slot is kept so corner/edge handles
// keep the same bit layout.
#define ITEM_LEFT (1 << 0)
#define ITEM_RIGHT (1 << 1)
#define ITEM_TOP (1 << 2)
#define ITEM_BOTTOM (1 << 3)
#define ITEM_ROT (1 << 4)

namespace {

constexpr wchar_t kPreviewClassName[] = L"ObsMultiStreamPreview";

constexpr float kHandleRadius = 4.0f;        // handle half-size in screen px
constexpr float kHandleSelRadius = 6.0f;     // hit-test radius (kHandleRadius * 1.5)
constexpr float kBoxLineThickness = 2.0f;    // selection outline thickness in screen px

enum class ItemHandle : uint32_t {
	None = 0,
	TopLeft = ITEM_TOP | ITEM_LEFT,
	TopCenter = ITEM_TOP,
	TopRight = ITEM_TOP | ITEM_RIGHT,
	CenterLeft = ITEM_LEFT,
	CenterRight = ITEM_RIGHT,
	BottomLeft = ITEM_BOTTOM | ITEM_LEFT,
	BottomCenter = ITEM_BOTTOM,
	BottomRight = ITEM_BOTTOM | ITEM_RIGHT,
	Rot = ITEM_ROT,
};

obs_display_t *g_display = nullptr;
PreviewWindow *g_instance = nullptr;

// Unit-quad TRISTRIP vertbuffer for the selection handles, created lazily on the
// render thread and destroyed under a graphics context in Destroy().
gs_vertbuffer_t *g_boxBuffer = nullptr;

// Shared editor state. Mouse handling + selection mutation happen on the UI
// thread (WndProc); the draw callback (render thread) only reads g_selectedId and
// the letterbox transform. Guard both with one mutex; copy out under the lock and
// never hold it across libobs render calls.
std::mutex g_stateMutex;
int64_t g_selectedId = -1;

// The letterbox transform the draw callback computed last frame, in HWND device
// px. Mouse client px -> canvas: (px - drawOrigin) / scale.
struct PreviewTransform {
	float scale = 0.0f;
	int drawX = 0;
	int drawY = 0;
	float baseCX = 0.0f;
	float baseCY = 0.0f;
};
PreviewTransform g_transform;

// Per-drag state, all captured at mousedown on the UI thread and only touched on
// the UI thread, so a drag is atomic. We store the int64 id (re-resolved each
// message) and the box-transform-derived matrices, never an obs_sceneitem_t*.
enum class DragMode { None, Move, Resize };
struct DragState {
	DragMode mode = DragMode::None;
	int64_t id = -1;
	vec2 startCanvasPos = {};  // mouse canvas pos at mousedown
	vec2 startItemPos = {};    // item pos at mousedown (move)
	ItemHandle handle = ItemHandle::None;
	matrix4 itemToScreen = {};
	matrix4 screenToItem = {};
	vec2 stretchItemSize = {};
	obs_sceneitem_crop startCrop = {};
};
DragState g_drag;

// --- scene resolution -------------------------------------------------------

// The editor's authoritative scene is always output channel 0. Returns the scene
// source addref'd (caller releases) or null when nothing is bound.
obs_source_t *AcquireOutputSceneSource()
{
	return obs_get_output_source(0); // addref'd; null if unbound
}

// --- hit-testing (ported from legacy FindItemAtPos) -------------------------

bool SceneItemHasVideo(obs_sceneitem_t *item)
{
	const uint32_t flags = obs_source_get_output_flags(obs_sceneitem_get_source(item));
	return (flags & OBS_SOURCE_VIDEO) != 0;
}

bool CloseFloat(float a, float b, float epsilon = 0.01f)
{
	return std::fabs(a - b) <= epsilon;
}

struct HitFind {
	vec2 pos;
	obs_sceneitem_t *item;
};

// Topmost-wins: obs_scene_enum_items yields bottom-to-top, so the last match
// (overwriting `item`) is the topmost hit.
bool FindItemAtPos(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	HitFind *data = static_cast<HitFind *>(param);

	if (!SceneItemHasVideo(item) || obs_sceneitem_locked(item)) {
		return true;
	}

	matrix4 transform;
	matrix4 invTransform;
	vec3 transformedPos;
	vec3 pos3;
	vec3 pos3_;

	vec3_set(&pos3, data->pos.x, data->pos.y, 0.0f);
	obs_sceneitem_get_box_transform(item, &transform);
	matrix4_inv(&invTransform, &transform);
	vec3_transform(&transformedPos, &pos3, &invTransform);
	vec3_transform(&pos3_, &transformedPos, &transform);

	if (CloseFloat(pos3.x, pos3_.x) && CloseFloat(pos3.y, pos3_.y) && transformedPos.x >= 0.0f &&
	    transformedPos.x <= 1.0f && transformedPos.y >= 0.0f && transformedPos.y <= 1.0f) {
		data->item = item;
	}
	return true;
}

// Returns the topmost item id at a canvas-space point, or -1. Caller holds the
// scene alive (the returned id is re-resolved later, never a pointer).
int64_t HitTestItemId(obs_scene_t *scene, const vec2 &canvasPos)
{
	HitFind data{canvasPos, nullptr};
	obs_scene_enum_items(scene, FindItemAtPos, &data);
	return data.item ? obs_sceneitem_get_id(data.item) : int64_t(-1);
}

struct ItemFindById {
	int64_t id;
	obs_sceneitem_t *found;
};

bool FindItemByIdCb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *c = static_cast<ItemFindById *>(param);
	if (obs_sceneitem_get_id(item) == c->id) {
		c->found = item;
		return false;
	}
	return true;
}

// Resolve a scene-item by id within a scene. The returned pointer is owned by the
// scene and valid only while the scene source is held by the caller.
obs_sceneitem_t *FindItemById(obs_scene_t *scene, int64_t id)
{
	ItemFindById ctx{id, nullptr};
	obs_scene_enum_items(scene, FindItemByIdCb, &ctx);
	return ctx.found;
}

// --- handle hit-testing (ported from legacy FindHandleAtPos, no group/rot) ---

vec3 GetTransformedPos(float x, float y, const matrix4 &mat)
{
	vec3 result;
	vec3_set(&result, x, y, 0.0f);
	vec3_transform(&result, &result, &mat);
	return result;
}

// Test the 8 resize handles of `item` against a canvas-space point. `radius` is
// in canvas units (kHandleSelRadius / scale) so the on-screen proximity is fixed.
ItemHandle FindHandleAtPos(obs_sceneitem_t *item, const vec2 &canvasPos, float radius)
{
	matrix4 transform;
	obs_sceneitem_get_box_transform(item, &transform);

	vec3 pos3;
	vec3_set(&pos3, canvasPos.x, canvasPos.y, 0.0f);

	ItemHandle found = ItemHandle::None;
	float closest = radius;

	struct HandleCoord {
		float x, y;
		ItemHandle handle;
	};
	static const HandleCoord kHandles[] = {
		{0.0f, 0.0f, ItemHandle::TopLeft},     {0.5f, 0.0f, ItemHandle::TopCenter},
		{1.0f, 0.0f, ItemHandle::TopRight},    {0.0f, 0.5f, ItemHandle::CenterLeft},
		{1.0f, 0.5f, ItemHandle::CenterRight}, {0.0f, 1.0f, ItemHandle::BottomLeft},
		{0.5f, 1.0f, ItemHandle::BottomCenter}, {1.0f, 1.0f, ItemHandle::BottomRight},
	};
	for (const auto &h : kHandles) {
		vec3 handlePos = GetTransformedPos(h.x, h.y, transform);
		const float dist = vec3_dist(&handlePos, &pos3);
		if (dist < radius && dist < closest) {
			closest = dist;
			found = h.handle;
		}
	}
	return found;
}

// --- resize math (ported from legacy GetItemSize/StretchItem/ClampAspect) ----

vec2 GetItemSize(obs_sceneitem_t *item)
{
	vec2 size;
	if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
		obs_sceneitem_get_bounds(item, &size);
	} else {
		obs_source_t *source = obs_sceneitem_get_source(item);
		obs_sceneitem_crop crop;
		vec2 scale;
		obs_sceneitem_get_scale(item, &scale);
		obs_sceneitem_get_crop(item, &crop);
		size.x = fmaxf(float(int(obs_source_get_width(source)) - crop.left - crop.right), 0.0f);
		size.y = fmaxf(float(int(obs_source_get_height(source)) - crop.top - crop.bottom), 0.0f);
		vec2_mul(&size, &size, &scale);
	}
	return size;
}

vec3 CalculateStretchPos(obs_sceneitem_t *item, const vec3 &tl, const vec3 &br)
{
	const uint32_t alignment = obs_sceneitem_get_alignment(item);
	vec3 pos;
	vec3_zero(&pos);

	if (alignment & OBS_ALIGN_LEFT) {
		pos.x = tl.x;
	} else if (alignment & OBS_ALIGN_RIGHT) {
		pos.x = br.x;
	} else {
		pos.x = (br.x - tl.x) * 0.5f + tl.x;
	}

	if (alignment & OBS_ALIGN_TOP) {
		pos.y = tl.y;
	} else if (alignment & OBS_ALIGN_BOTTOM) {
		pos.y = br.y;
	} else {
		pos.y = (br.y - tl.y) * 0.5f + tl.y;
	}
	return pos;
}

void ClampAspect(ItemHandle handle, vec3 &tl, vec3 &br, vec2 &size, const vec2 &baseSize)
{
	const float baseAspect = baseSize.x / baseSize.y;
	const float aspect = size.x / size.y;
	const uint32_t flags = uint32_t(handle);

	if (handle == ItemHandle::TopLeft || handle == ItemHandle::TopRight || handle == ItemHandle::BottomLeft ||
	    handle == ItemHandle::BottomRight) {
		if (aspect < baseAspect) {
			if ((size.y >= 0.0f && size.x >= 0.0f) || (size.y <= 0.0f && size.x <= 0.0f)) {
				size.x = size.y * baseAspect;
			} else {
				size.x = size.y * baseAspect * -1.0f;
			}
		} else {
			if ((size.y >= 0.0f && size.x >= 0.0f) || (size.y <= 0.0f && size.x <= 0.0f)) {
				size.y = size.x / baseAspect;
			} else {
				size.y = size.x / baseAspect * -1.0f;
			}
		}
	} else if (handle == ItemHandle::TopCenter || handle == ItemHandle::BottomCenter) {
		if ((size.y >= 0.0f && size.x >= 0.0f) || (size.y <= 0.0f && size.x <= 0.0f)) {
			size.x = size.y * baseAspect;
		} else {
			size.x = size.y * baseAspect * -1.0f;
		}
	} else if (handle == ItemHandle::CenterLeft || handle == ItemHandle::CenterRight) {
		if ((size.y >= 0.0f && size.x >= 0.0f) || (size.y <= 0.0f && size.x <= 0.0f)) {
			size.y = size.x / baseAspect;
		} else {
			size.y = size.x / baseAspect * -1.0f;
		}
	}

	size.x = std::round(size.x);
	size.y = std::round(size.y);

	if (flags & ITEM_LEFT) {
		tl.x = br.x - size.x;
	} else if (flags & ITEM_RIGHT) {
		br.x = tl.x + size.x;
	}
	if (flags & ITEM_TOP) {
		tl.y = br.y - size.y;
	} else if (flags & ITEM_BOTTOM) {
		br.y = tl.y + size.y;
	}
}

// Resize the active drag item to the current mouse canvas pos. Single-select,
// OBS_BOUNDS_NONE (scale) and bounds paths; aspect is preserved on corner drags.
// TODO(deferred): Shift = free aspect / Ctrl = no snap, crop (Alt) drag.
void StretchItem(obs_sceneitem_t *item, const vec2 &canvasPos)
{
	const obs_bounds_type boundsType = obs_sceneitem_get_bounds_type(item);
	const uint32_t flags = uint32_t(g_drag.handle);

	vec3 tl, br, pos3;
	vec3_zero(&tl);
	vec3_set(&br, g_drag.stretchItemSize.x, g_drag.stretchItemSize.y, 0.0f);

	vec3_set(&pos3, canvasPos.x, canvasPos.y, 0.0f);
	vec3_transform(&pos3, &pos3, &g_drag.screenToItem);

	if (flags & ITEM_LEFT) {
		tl.x = pos3.x;
	} else if (flags & ITEM_RIGHT) {
		br.x = pos3.x;
	}
	if (flags & ITEM_TOP) {
		tl.y = pos3.y;
	} else if (flags & ITEM_BOTTOM) {
		br.y = pos3.y;
	}

	obs_source_t *source = obs_sceneitem_get_source(item);
	const uint32_t source_cx = obs_source_get_width(source);
	const uint32_t source_cy = obs_source_get_height(source);
	if (!source_cx || !source_cy) {
		return;
	}

	vec2 baseSize;
	vec2_set(&baseSize, float(source_cx), float(source_cy));
	vec2 size;
	vec2_set(&size, br.x - tl.x, br.y - tl.y);

	if (boundsType != OBS_BOUNDS_NONE) {
		if (tl.x > br.x) {
			std::swap(tl.x, br.x);
		}
		if (tl.y > br.y) {
			std::swap(tl.y, br.y);
		}
		vec2_abs(&size, &size);
		obs_sceneitem_set_bounds(item, &size);
	} else {
		obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(item, &crop);
		baseSize.x -= float(crop.left + crop.right);
		baseSize.y -= float(crop.top + crop.bottom);

		ClampAspect(g_drag.handle, tl, br, size, baseSize);

		vec2_div(&size, &size, &baseSize);
		obs_sceneitem_set_scale(item, &size);
	}

	pos3 = CalculateStretchPos(item, tl, br);
	vec3_transform(&pos3, &pos3, &g_drag.itemToScreen);
	vec2 newPos;
	vec2_set(&newPos, std::round(pos3.x), std::round(pos3.y));
	obs_sceneitem_set_pos(item, &newPos);
}

// Capture the matrices/sizes a resize drag needs (legacy GetStretchHandleData,
// no-group path) for the chosen item + handle.
void BeginResize(obs_sceneitem_t *item, ItemHandle handle, const vec2 &startCanvasPos)
{
	matrix4 boxTransform;
	vec3 itemUL;

	g_drag.mode = DragMode::Resize;
	g_drag.id = obs_sceneitem_get_id(item);
	g_drag.handle = handle;
	g_drag.startCanvasPos = startCanvasPos;
	g_drag.stretchItemSize = GetItemSize(item);

	obs_sceneitem_get_box_transform(item, &boxTransform);
	const float itemRot = obs_sceneitem_get_rot(item);
	vec3_from_vec4(&itemUL, &boxTransform.t);

	matrix4_identity(&g_drag.itemToScreen);
	matrix4_rotate_aa4f(&g_drag.itemToScreen, &g_drag.itemToScreen, 0.0f, 0.0f, 1.0f, RAD(itemRot));
	matrix4_translate3f(&g_drag.itemToScreen, &g_drag.itemToScreen, itemUL.x, itemUL.y, 0.0f);

	matrix4_identity(&g_drag.screenToItem);
	matrix4_translate3f(&g_drag.screenToItem, &g_drag.screenToItem, -itemUL.x, -itemUL.y, 0.0f);
	matrix4_rotate_aa4f(&g_drag.screenToItem, &g_drag.screenToItem, 0.0f, 0.0f, 1.0f, RAD(-itemRot));

	obs_sceneitem_get_crop(item, &g_drag.startCrop);
	obs_sceneitem_get_pos(item, &g_drag.startItemPos);
}

// --- selection -------------------------------------------------------------

struct SelectCtx {
	int64_t id;
};

bool SelectOnlyCb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	const SelectCtx *ctx = static_cast<SelectCtx *>(param);
	obs_sceneitem_select(item, obs_sceneitem_get_id(item) == ctx->id);
	return true;
}

// Select exactly `id` in the scene (deselect everything else). id == -1 clears.
void SelectOnly(obs_scene_t *scene, int64_t id)
{
	SelectCtx ctx{id};
	obs_scene_enum_items(scene, SelectOnlyCb, &ctx);
}

// Emit sceneItem.selected to JS. `id` < 0 -> {scene:null,id:null}. Posts to the
// UI thread internally so this is safe from WndProc.
void EmitSelection(int64_t id)
{
	std::string sceneName;
	if (id >= 0) {
		obs_source_t *sceneSource = AcquireOutputSceneSource();
		if (sceneSource) {
			const char *n = obs_source_get_name(sceneSource);
			if (n) {
				sceneName = n;
			}
			obs_source_release(sceneSource);
		}
	}
	using Bridge::json;
	json payload = json{
		{"scene", id >= 0 && !sceneName.empty() ? json(sceneName) : json(nullptr)},
		{"id", id >= 0 ? json(id) : json(nullptr)},
	};
	Bridge::EmitEvent("sceneItem.selected", payload);
}

// --- drawing (ported from legacy DrawLine/DrawSquareAtPos/DrawRect) ----------

void EnsureBoxBuffer()
{
	if (g_boxBuffer) {
		return;
	}
	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(1.0f, 1.0f);
	g_boxBuffer = gs_render_save();
}

// Draw a thin line (as a quad) in the current matrix space; thickness in canvas
// units, divided by the per-axis box scale so the on-screen width is constant.
void DrawLine(float x1, float y1, float x2, float y2, float thickness, const vec2 &boxScale)
{
	vec2 scale;
	vec2_abs(&scale, &boxScale);
	const float tx = thickness / scale.x;
	const float ty = thickness / scale.y;
	const bool xAxis = !CloseFloat(x1, x2, 0.0000001f) || CloseFloat(y1, y2, 0.0000001f);

	float cx, cy;
	if (xAxis) {
		cx = std::fabs(x2 - x1) + tx;
		cy = ty;
	} else {
		cy = std::fabs(y2 - y1) + ty;
		cx = tx;
	}
	x1 -= tx * 0.5f;
	y1 -= ty * 0.5f;

	gs_matrix_push();
	gs_matrix_translate3f(x1, y1, 0.0f);
	gs_draw_quadf(nullptr, 0, cx, cy);
	gs_matrix_pop();
}

// The 4 box edges in unit space, drawn inside the box-transform matrix so a
// rotated/scaled item still boxes correctly.
void DrawRect(float thickness, const vec2 &boxScale)
{
	DrawLine(0.0f, 0.0f, 0.0f, 1.0f, thickness, boxScale);
	DrawLine(0.0f, 0.0f, 1.0f, 0.0f, thickness, boxScale);
	DrawLine(1.0f, 0.0f, 1.0f, 1.0f, thickness, boxScale);
	DrawLine(0.0f, 1.0f, 1.0f, 1.0f, thickness, boxScale);
}

// Draw a fixed-screen-size filled square at a unit-space handle coord. Reads the
// current matrix (box transform), maps the point to canvas space, then draws an
// axis-aligned square there in canvas-ortho space (matches legacy DrawSquareAtPos).
void DrawSquareAtPos(float x, float y, float halfSize)
{
	vec3 pos;
	vec3_set(&pos, x, y, 0.0f);
	matrix4 matrix;
	gs_matrix_get(&matrix);
	vec3_transform(&pos, &pos, &matrix);

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate(&pos);
	gs_matrix_translate3f(-halfSize, -halfSize, 0.0f);
	gs_matrix_scale3f(halfSize * 2.0f, halfSize * 2.0f, 1.0f);
	gs_draw(GS_TRISTRIP, 0, 0);
	gs_matrix_pop();
}

// Draw the selection box + 8 handles for `item`. Runs inside the draw callback's
// canvas ortho/viewport, so canvas coords map to the screen. `scale` = letterbox
// screen-px-per-canvas-unit, used to keep outline/handles a constant pixel size.
void DrawSelection(obs_sceneitem_t *item, float scale)
{
	if (scale <= 0.0f) {
		return;
	}
	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colParam = gs_effect_get_param_by_name(solid, "color");

	vec4 green;
	vec4_set(&green, 0.0f, 1.0f, 0.235f, 1.0f); // OBS selection green

	// Box outline: drawn inside the box-transform matrix in unit space so the
	// outline follows rotation/scale. boxScale maps unit->canvas px-equivalent so
	// the line thickness stays ~constant on screen.
	vec2 boxScale;
	obs_sceneitem_get_box_scale(item, &boxScale);
	boxScale.x *= scale;
	boxScale.y *= scale;

	gs_matrix_push();
	gs_matrix_mul(&boxTransform);

	gs_effect_set_vec4(colParam, &green);
	while (gs_effect_loop(solid, "Solid")) {
		DrawRect(kBoxLineThickness, boxScale);
	}

	// 8 handles: filled squares sized in canvas units so they read ~kHandleRadius
	// px on screen, drawn via the unit-space box vertbuffer.
	const float halfSize = kHandleRadius / scale;
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_load_vertexbuffer(g_boxBuffer);
	gs_effect_set_vec4(colParam, &green);

	DrawSquareAtPos(0.0f, 0.0f, halfSize);
	DrawSquareAtPos(0.5f, 0.0f, halfSize);
	DrawSquareAtPos(1.0f, 0.0f, halfSize);
	DrawSquareAtPos(0.0f, 0.5f, halfSize);
	DrawSquareAtPos(1.0f, 0.5f, halfSize);
	DrawSquareAtPos(0.0f, 1.0f, halfSize);
	DrawSquareAtPos(0.5f, 1.0f, halfSize);
	DrawSquareAtPos(1.0f, 1.0f, halfSize);

	gs_matrix_pop();
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}

// Draw callback: fired by libobs once per frame on the render thread. cx/cy are
// the display (HWND) pixel size. Fit the base canvas into it with letterboxing so
// the composited scene keeps its aspect ratio, then overlay the selection box for
// the currently-selected item (re-resolved by id from output 0's scene).
void RenderPreview(void *, uint32_t cx, uint32_t cy)
{
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		return;
	}

	const float baseCX = float(ovi.base_width);
	const float baseCY = float(ovi.base_height);
	if (baseCX <= 0.0f || baseCY <= 0.0f || cx == 0 || cy == 0) {
		return;
	}

	const float scale = (float(cx) / baseCX < float(cy) / baseCY) ? float(cx) / baseCX : float(cy) / baseCY;
	const int drawCX = int(baseCX * scale);
	const int drawCY = int(baseCY * scale);
	const int drawX = (int(cx) - drawCX) / 2;
	const int drawY = (int(cy) - drawCY) / 2;

	{
		std::lock_guard<std::mutex> lock(g_stateMutex);
		g_transform.scale = scale;
		g_transform.drawX = drawX;
		g_transform.drawY = drawY;
		g_transform.baseCX = baseCX;
		g_transform.baseCY = baseCY;
	}

	gs_viewport_push();
	gs_projection_push();

	gs_ortho(0.0f, baseCX, 0.0f, baseCY, -100.0f, 100.0f);
	gs_set_viewport(drawX, drawY, drawCX, drawCY);

	obs_render_main_texture();

	int64_t selectedId;
	{
		std::lock_guard<std::mutex> lock(g_stateMutex);
		selectedId = g_selectedId;
	}

	if (selectedId >= 0) {
		obs_source_t *sceneSource = AcquireOutputSceneSource();
		if (sceneSource) {
			obs_scene_t *scene = obs_scene_from_source(sceneSource);
			obs_sceneitem_t *item = FindItemById(scene, selectedId);
			if (item && SceneItemHasVideo(item) && !obs_sceneitem_locked(item)) {
				EnsureBoxBuffer();
				DrawSelection(item, scale);
			}
			obs_source_release(sceneSource);
		}
	}

	gs_projection_pop();
	gs_viewport_pop();
}

// Client px (device) -> canvas coords using the last-rendered letterbox transform.
// Returns false when the transform is not yet known (no frame drawn).
bool ClientToCanvas(int mx, int my, vec2 &out)
{
	std::lock_guard<std::mutex> lock(g_stateMutex);
	if (g_transform.scale <= 0.0f) {
		return false;
	}
	out.x = (float(mx) - float(g_transform.drawX)) / g_transform.scale;
	out.y = (float(my) - float(g_transform.drawY)) / g_transform.scale;
	return true;
}

float CurrentScale()
{
	std::lock_guard<std::mutex> lock(g_stateMutex);
	return g_transform.scale;
}

// --- WndProc (mouse editing) ------------------------------------------------

void OnLeftDown(HWND hwnd, int mx, int my)
{
	SetCapture(hwnd);

	vec2 canvasPos;
	if (!ClientToCanvas(mx, my, canvasPos)) {
		return;
	}

	obs_source_t *sceneSource = AcquireOutputSceneSource();
	if (!sceneSource) {
		return;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);

	const float scale = CurrentScale();

	// If a handle of the currently-selected item is hit, begin a resize.
	int64_t selectedId;
	{
		std::lock_guard<std::mutex> lock(g_stateMutex);
		selectedId = g_selectedId;
	}
	if (selectedId >= 0 && scale > 0.0f) {
		obs_sceneitem_t *sel = FindItemById(scene, selectedId);
		if (sel && !obs_sceneitem_locked(sel)) {
			const ItemHandle handle = FindHandleAtPos(sel, canvasPos, kHandleSelRadius / scale);
			if (handle != ItemHandle::None) {
				BeginResize(sel, handle, canvasPos);
				HostLog("[preview] resize start id=" + std::to_string(selectedId) +
					" handle=" + std::to_string(uint32_t(handle)));
				obs_source_release(sceneSource);
				return;
			}
		}
	}

	// Otherwise hit-test items and select/move (or deselect on empty).
	const int64_t hitId = HitTestItemId(scene, canvasPos);
	HostLog("[preview] click canvas=(" + std::to_string(int(canvasPos.x)) + "," + std::to_string(int(canvasPos.y)) +
		") hit id=" + std::to_string(hitId));

	if (hitId >= 0) {
		SelectOnly(scene, hitId);

		obs_sceneitem_t *item = FindItemById(scene, hitId);

		{
			std::lock_guard<std::mutex> lock(g_stateMutex);
			g_selectedId = hitId;
		}
		g_drag.mode = DragMode::Move;
		g_drag.id = hitId;
		g_drag.startCanvasPos = canvasPos;
		if (item) {
			obs_sceneitem_get_pos(item, &g_drag.startItemPos);
		}
		EmitSelection(hitId);
	} else {
		SelectOnly(scene, -1);
		{
			std::lock_guard<std::mutex> lock(g_stateMutex);
			g_selectedId = -1;
		}
		g_drag.mode = DragMode::None;
		EmitSelection(-1);
	}

	obs_source_release(sceneSource);
}

void OnMouseMove(int mx, int my)
{
	if (g_drag.mode == DragMode::None) {
		return;
	}
	vec2 canvasPos;
	if (!ClientToCanvas(mx, my, canvasPos)) {
		return;
	}

	obs_source_t *sceneSource = AcquireOutputSceneSource();
	if (!sceneSource) {
		return;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);

	obs_sceneitem_t *item = FindItemById(scene, g_drag.id);
	if (item && !obs_sceneitem_locked(item)) {
		if (g_drag.mode == DragMode::Move) {
			vec2 newPos;
			newPos.x = std::round(g_drag.startItemPos.x + (canvasPos.x - g_drag.startCanvasPos.x));
			newPos.y = std::round(g_drag.startItemPos.y + (canvasPos.y - g_drag.startCanvasPos.y));
			obs_sceneitem_set_pos(item, &newPos);
		} else if (g_drag.mode == DragMode::Resize) {
			StretchItem(item, canvasPos);
		}
	}
	obs_source_release(sceneSource);
}

void OnLeftUp()
{
	ReleaseCapture();
	if (g_drag.mode != DragMode::None) {
		HostLog("[preview] drag end id=" + std::to_string(g_drag.id));
	}
	g_drag.mode = DragMode::None;
	g_drag.handle = ItemHandle::None;
}

LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg) {
	case WM_LBUTTONDOWN:
		OnLeftDown(hwnd, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
		return 0;
	case WM_MOUSEMOVE:
		OnMouseMove(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
		return 0;
	case WM_LBUTTONUP:
		OnLeftUp();
		return 0;
	case WM_CAPTURECHANGED:
		g_drag.mode = DragMode::None;
		g_drag.handle = ItemHandle::None;
		return 0;
	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// The overlay HWND uses a no-background class: the obs_display swapchain paints
// every pixel (the canvas plus its black letterbox bars), so a WM_ERASEBKGND
// fill would only flicker against it.
ATOM RegisterPreviewClass(HINSTANCE instance)
{
	static ATOM atom = 0;
	if (atom) {
		return atom;
	}
	WNDCLASSEXW wc = {0};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = PreviewWndProc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	wc.lpszClassName = kPreviewClassName;
	atom = RegisterClassExW(&wc);
	return atom;
}

} // namespace

PreviewWindow::PreviewWindow(HWND host, HINSTANCE instance) : host_(host), instance_(instance) {}

void PreviewWindow::EnsureCreated()
{
	if (hwnd_) {
		return;
	}

	RegisterPreviewClass(instance_);

	// Borderless child sibling of the CEF browser HWND. WS_CLIPSIBLINGS keeps the
	// browser from overdrawing into it (the browser HWND also sets it). Starts
	// hidden; SetRect shows it after positioning so no frame flashes at 0,0.
	hwnd_ = CreateWindowExW(0, kPreviewClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 16, 16, host_, nullptr,
				instance_, nullptr);
	if (!hwnd_) {
		HostLog("[obs] preview overlay HWND create FAILED");
		return;
	}
	HostLog("[obs] preview overlay HWND created");

	gs_init_data init = {};
	init.cx = 16;
	init.cy = 16;
	init.format = GS_BGRA;
	init.zsformat = GS_ZS_NONE;
	init.window.hwnd = hwnd_; // child HWND passthrough (gs_window.hwnd is void*)

	g_display = obs_display_create(&init, 0x000000);
	HostLog(std::string("[obs] obs_display_create -> ") + (g_display ? "OK" : "NULL"));
	if (g_display) {
		obs_display_add_draw_callback(g_display, RenderPreview, nullptr);
		HostLog("[obs] preview draw callback registered");
	}
}

void PreviewWindow::SetRect(int x, int y, int cx, int cy)
{
	if (cx <= 0 || cy <= 0) {
		Hide();
		return;
	}

	EnsureCreated();
	if (!hwnd_) {
		return;
	}

	// Position in host-client device pixels and keep above the CEF browser HWND
	// (HWND_TOP raises within the sibling z-order). SWP_SHOWWINDOW reveals it on
	// the first sized call.
	SetWindowPos(hwnd_, HWND_TOP, x, y, cx, cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);

	if (g_display) {
		obs_display_resize(g_display, uint32_t(cx), uint32_t(cy));
	}
}

void PreviewWindow::Hide()
{
	if (hwnd_) {
		ShowWindow(hwnd_, SW_HIDE);
	}
}

void PreviewWindow::Destroy()
{
	if (g_display) {
		obs_display_remove_draw_callback(g_display, RenderPreview, nullptr);
		obs_display_destroy(g_display);
		g_display = nullptr;
		HostLog("[obs] preview display destroyed");
	}
	if (g_boxBuffer) {
		obs_enter_graphics();
		gs_vertexbuffer_destroy(g_boxBuffer);
		obs_leave_graphics();
		g_boxBuffer = nullptr;
	}
	if (hwnd_) {
		DestroyWindow(hwnd_);
		hwnd_ = nullptr;
	}
}

namespace Preview {

void SetInstance(PreviewWindow *pw)
{
	g_instance = pw;
}

PreviewWindow *Instance()
{
	return g_instance;
}

bool SelectFromBridge(const std::string &scene, int64_t id, bool hasId)
{
	obs_source_t *sceneSource = AcquireOutputSceneSource();
	if (!sceneSource) {
		return false;
	}
	// Ignore a foreign scene name to keep "preview shows output 0" intact.
	if (!scene.empty()) {
		const char *n = obs_source_get_name(sceneSource);
		if (!n || scene != n) {
			obs_source_release(sceneSource);
			return false;
		}
	}
	obs_scene_t *sc = obs_scene_from_source(sceneSource);

	const int64_t newId = hasId ? id : int64_t(-1);
	SelectOnly(sc, newId);
	{
		std::lock_guard<std::mutex> lock(g_stateMutex);
		g_selectedId = newId;
	}
	obs_source_release(sceneSource);

	EmitSelection(newId);
	return true;
}

int64_t HitTestForTest(float canvasX, float canvasY)
{
	obs_source_t *sceneSource = AcquireOutputSceneSource();
	if (!sceneSource) {
		return -1;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	vec2 pos;
	vec2_set(&pos, canvasX, canvasY);
	const int64_t id = HitTestItemId(scene, pos);
	obs_source_release(sceneSource);
	return id;
}

bool OnVideoReset()
{
	if (!g_display) {
		// No display yet (UI never sized the preview). The next SetRect creates
		// it fresh against the new base resolution; nothing to re-validate.
		HostLog("[preview] OnVideoReset: no display yet (will create lazily)");
		return false;
	}

	// The base resolution changed, so the cached letterbox transform is stale.
	// Reset it; RenderPreview recomputes it from obs_get_video_info() on the next
	// frame. ClientToCanvas treats scale<=0 as "no frame yet" and ignores mouse
	// input until that recompute lands, avoiding a one-frame mis-mapped drag.
	{
		std::lock_guard<std::mutex> lock(g_stateMutex);
		g_transform = PreviewTransform{};
	}

	// Nudge a redraw at the current size so the new mix is presented promptly.
	obs_display_update_color_space(g_display);
	HostLog("[preview] OnVideoReset: display alive, letterbox transform invalidated");
	return true;
}

} // namespace Preview
