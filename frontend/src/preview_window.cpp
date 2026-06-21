// bridge.hpp pulls in the CEF headers, whose CefDOMNode declares methods like
// GetNextSibling that <windows.h> would otherwise macro-clobber. Include it (and
// thus CEF) before any Windows header so CEF parses clean.
#include "bridge.hpp"

#include "preview_window.hpp"

#include "multistream/CanvasRuntime.hpp"
#include "multistream/CanvasStore.hpp"
#include "obs_bootstrap.hpp"

#include <CanvasDefinition.hpp>

#include <obs.h>

#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <windowsx.h>

#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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

constexpr float kHandleRadius = 4.0f;     // handle half-size in screen px
constexpr float kHandleSelRadius = 6.0f;  // hit-test radius (kHandleRadius * 1.5)
constexpr float kBoxLineThickness = 2.0f; // selection outline thickness in screen px

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

PreviewManager *g_instance = nullptr;

// The Default canvas has no obs_canvas_t mix (it uses the global pipeline). The
// manager keys its Default surface under the empty string; a caller may also
// address it by the Default canvas's own uuid, which normalizes to "" here.
bool IsDefaultCanvasUuid(const std::string &uuid)
{
	return uuid.empty() || uuid == ObsBootstrap::Canvases().Default().uuid;
}

// The letterbox transform the draw callback computed last frame, in HWND device
// px. Mouse client px -> canvas: (px - drawOrigin) / scale.
struct PreviewTransform {
	float scale = 0.0f;
	int drawX = 0;
	int drawY = 0;
	float baseCX = 0.0f;
	float baseCY = 0.0f;
};

// Per-drag state, all captured at mousedown on the UI thread and only touched on
// the UI thread, so a drag is atomic. We store the int64 id (re-resolved each
// message) and the box-transform-derived matrices, never an obs_sceneitem_t*.
enum class DragMode { None, Move, Resize };
struct DragState {
	DragMode mode = DragMode::None;
	int64_t id = -1;
	vec2 startCanvasPos = {}; // mouse canvas pos at mousedown
	vec2 startItemPos = {};   // item pos at mousedown (move)
	ItemHandle handle = ItemHandle::None;
	matrix4 itemToScreen = {};
	matrix4 screenToItem = {};
	vec2 stretchItemSize = {};
	obs_sceneitem_crop startCrop = {};
};

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
		{0.0f, 0.0f, ItemHandle::TopLeft},      {0.5f, 0.0f, ItemHandle::TopCenter},
		{1.0f, 0.0f, ItemHandle::TopRight},     {0.0f, 0.5f, ItemHandle::CenterLeft},
		{1.0f, 0.5f, ItemHandle::CenterRight},  {0.0f, 1.0f, ItemHandle::BottomLeft},
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
void StretchItem(const DragState &drag, obs_sceneitem_t *item, const vec2 &canvasPos)
{
	const obs_bounds_type boundsType = obs_sceneitem_get_bounds_type(item);
	const uint32_t flags = uint32_t(drag.handle);

	vec3 tl, br, pos3;
	vec3_zero(&tl);
	vec3_set(&br, drag.stretchItemSize.x, drag.stretchItemSize.y, 0.0f);

	vec3_set(&pos3, canvasPos.x, canvasPos.y, 0.0f);
	vec3_transform(&pos3, &pos3, &drag.screenToItem);

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

		ClampAspect(drag.handle, tl, br, size, baseSize);

		vec2_div(&size, &size, &baseSize);
		obs_sceneitem_set_scale(item, &size);
	}

	pos3 = CalculateStretchPos(item, tl, br);
	vec3_transform(&pos3, &pos3, &drag.itemToScreen);
	vec2 newPos;
	vec2_set(&newPos, std::round(pos3.x), std::round(pos3.y));
	obs_sceneitem_set_pos(item, &newPos);
}

// Capture the matrices/sizes a resize drag needs (legacy GetStretchHandleData,
// no-group path) for the chosen item + handle.
void BeginResize(DragState &drag, obs_sceneitem_t *item, ItemHandle handle, const vec2 &startCanvasPos)
{
	matrix4 boxTransform;
	vec3 itemUL;

	drag.mode = DragMode::Resize;
	drag.id = obs_sceneitem_get_id(item);
	drag.handle = handle;
	drag.startCanvasPos = startCanvasPos;
	drag.stretchItemSize = GetItemSize(item);

	obs_sceneitem_get_box_transform(item, &boxTransform);
	const float itemRot = obs_sceneitem_get_rot(item);
	vec3_from_vec4(&itemUL, &boxTransform.t);

	matrix4_identity(&drag.itemToScreen);
	matrix4_rotate_aa4f(&drag.itemToScreen, &drag.itemToScreen, 0.0f, 0.0f, 1.0f, RAD(itemRot));
	matrix4_translate3f(&drag.itemToScreen, &drag.itemToScreen, itemUL.x, itemUL.y, 0.0f);

	matrix4_identity(&drag.screenToItem);
	matrix4_translate3f(&drag.screenToItem, &drag.screenToItem, -itemUL.x, -itemUL.y, 0.0f);
	matrix4_rotate_aa4f(&drag.screenToItem, &drag.screenToItem, 0.0f, 0.0f, 1.0f, RAD(-itemRot));

	obs_sceneitem_get_crop(item, &drag.startCrop);
	obs_sceneitem_get_pos(item, &drag.startItemPos);
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

// --- drawing (ported from legacy DrawLine/DrawSquareAtPos/DrawRect) ----------

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
// `boxBuffer` is the shared unit-quad TRISTRIP vertbuffer.
void DrawSelection(gs_vertbuffer_t *boxBuffer, obs_sceneitem_t *item, float scale)
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
	gs_load_vertexbuffer(boxBuffer);
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

// The overlay HWND uses a no-background class: the obs_display swapchain paints
// every pixel (the canvas plus its black letterbox bars), so a WM_ERASEBKGND
// fill would only flicker against it.
LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

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

// Per-surface state shared between the render thread (draw callback) and the UI
// thread (WndProc + bridge). One mutex guards the selection + letterbox transform;
// copy out under the lock and never hold it across a libobs render call. The drag
// state + box buffer are touched only on their owning thread (drag = UI thread,
// box buffer = render thread under a graphics context), but live here so they are
// per-surface, not process-global.
struct PreviewSurface::State {
	std::mutex stateMutex;
	int64_t selectedId = -1;
	PreviewTransform transform;

	DragState drag; // UI thread only

	// Unit-quad TRISTRIP vertbuffer for the selection handles, created lazily on
	// the render thread and destroyed under a graphics context in Destroy().
	gs_vertbuffer_t *boxBuffer = nullptr;

	obs_canvas_t *targetCanvas = nullptr; // mirror of the surface's binding for the callback
};

namespace {

// Resolve the scene a surface edits, addref'd (caller releases) or null. Null
// targetCanvas => output channel 0 (the global current scene). A non-null canvas
// => that canvas's current channel-0 scene via the runtime.
obs_source_t *AcquireSurfaceSceneSource(obs_canvas_t *targetCanvas)
{
	if (!targetCanvas) {
		return obs_get_output_source(0); // addref'd; null if unbound
	}
	const char *uuid = obs_canvas_get_uuid(targetCanvas);
	if (!uuid) {
		return nullptr;
	}
	return ObsBootstrap::CanvasRuntime().CurrentScene(uuid); // addref'd; null if unbound
}

// Build the surface's letterbox + base size from its mix. Null targetCanvas =>
// the global obs_get_video_info; otherwise the canvas's own video info. Returns
// false when no video info is available.
bool SurfaceVideoInfo(obs_canvas_t *targetCanvas, obs_video_info &ovi)
{
	if (!targetCanvas) {
		return obs_get_video_info(&ovi);
	}
	return obs_canvas_get_video_info(targetCanvas, &ovi);
}

void EnsureBoxBuffer(PreviewSurface::State *state)
{
	if (state->boxBuffer) {
		return;
	}
	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(1.0f, 1.0f);
	state->boxBuffer = gs_render_save();
}

// Emit sceneItem.selected to JS for the surface's scene. `id` < 0 ->
// {scene:null,id:null}. Posts to the UI thread internally so it is safe from
// WndProc.
void EmitSelection(obs_canvas_t *targetCanvas, int64_t id)
{
	std::string sceneName;
	if (id >= 0) {
		obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas);
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

// Draw callback: fired by libobs once per frame on the render thread. cx/cy are
// the display (HWND) pixel size. Fit the surface's base canvas into it with
// letterboxing so the composited scene keeps its aspect ratio, then overlay the
// selection box for the currently-selected item (re-resolved by id from the
// surface's scene). `data` is the PreviewSurface::State.
void RenderPreview(void *data, uint32_t cx, uint32_t cy)
{
	auto *state = static_cast<PreviewSurface::State *>(data);
	obs_canvas_t *targetCanvas = state->targetCanvas;

	obs_video_info ovi;
	if (!SurfaceVideoInfo(targetCanvas, ovi)) {
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
		std::lock_guard<std::mutex> lock(state->stateMutex);
		state->transform.scale = scale;
		state->transform.drawX = drawX;
		state->transform.drawY = drawY;
		state->transform.baseCX = baseCX;
		state->transform.baseCY = baseCY;
	}

	gs_viewport_push();
	gs_projection_push();

	gs_ortho(0.0f, baseCX, 0.0f, baseCY, -100.0f, 100.0f);
	gs_set_viewport(drawX, drawY, drawCX, drawCY);

	if (targetCanvas) {
		obs_canvas_render(targetCanvas);
	} else {
		obs_render_main_texture();
	}

	int64_t selectedId;
	{
		std::lock_guard<std::mutex> lock(state->stateMutex);
		selectedId = state->selectedId;
	}

	if (selectedId >= 0) {
		obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas);
		if (sceneSource) {
			obs_scene_t *scene = obs_scene_from_source(sceneSource);
			obs_sceneitem_t *item = FindItemById(scene, selectedId);
			if (item && SceneItemHasVideo(item) && !obs_sceneitem_locked(item)) {
				EnsureBoxBuffer(state);
				DrawSelection(state->boxBuffer, item, scale);
			}
			obs_source_release(sceneSource);
		}
	}

	gs_projection_pop();
	gs_viewport_pop();
}

} // namespace

PreviewSurface::PreviewSurface(HWND host, HINSTANCE instance, obs_canvas_t *targetCanvas)
	: state_(new State()), host_(host), instance_(instance), targetCanvas_(targetCanvas)
{
	state_->targetCanvas = targetCanvas;
}

PreviewSurface::~PreviewSurface()
{
	Destroy();
	delete state_;
}

// Client px (device) -> canvas coords using the last-rendered letterbox transform.
// Returns false when the transform is not yet known (no frame drawn).
namespace {
bool ClientToCanvas(PreviewSurface::State *state, int mx, int my, vec2 &out)
{
	std::lock_guard<std::mutex> lock(state->stateMutex);
	if (state->transform.scale <= 0.0f) {
		return false;
	}
	out.x = (float(mx) - float(state->transform.drawX)) / state->transform.scale;
	out.y = (float(my) - float(state->transform.drawY)) / state->transform.scale;
	return true;
}

float CurrentScale(PreviewSurface::State *state)
{
	std::lock_guard<std::mutex> lock(state->stateMutex);
	return state->transform.scale;
}
} // namespace

void PreviewSurface::OnLeftDown(int mx, int my)
{
	if (hwnd_) {
		SetCapture(hwnd_);
	}

	vec2 canvasPos;
	if (!ClientToCanvas(state_, mx, my, canvasPos)) {
		return;
	}

	obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas_);
	if (!sceneSource) {
		return;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);

	const float scale = CurrentScale(state_);

	// If a handle of the currently-selected item is hit, begin a resize.
	int64_t selectedId;
	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		selectedId = state_->selectedId;
	}
	if (selectedId >= 0 && scale > 0.0f) {
		obs_sceneitem_t *sel = FindItemById(scene, selectedId);
		if (sel && !obs_sceneitem_locked(sel)) {
			const ItemHandle handle = FindHandleAtPos(sel, canvasPos, kHandleSelRadius / scale);
			if (handle != ItemHandle::None) {
				BeginResize(state_->drag, sel, handle, canvasPos);
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
			std::lock_guard<std::mutex> lock(state_->stateMutex);
			state_->selectedId = hitId;
		}
		state_->drag.mode = DragMode::Move;
		state_->drag.id = hitId;
		state_->drag.startCanvasPos = canvasPos;
		if (item) {
			obs_sceneitem_get_pos(item, &state_->drag.startItemPos);
		}
		EmitSelection(targetCanvas_, hitId);
	} else {
		SelectOnly(scene, -1);
		{
			std::lock_guard<std::mutex> lock(state_->stateMutex);
			state_->selectedId = -1;
		}
		state_->drag.mode = DragMode::None;
		EmitSelection(targetCanvas_, -1);
	}

	obs_source_release(sceneSource);
}

void PreviewSurface::OnMouseMove(int mx, int my)
{
	if (state_->drag.mode == DragMode::None) {
		return;
	}
	vec2 canvasPos;
	if (!ClientToCanvas(state_, mx, my, canvasPos)) {
		return;
	}

	obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas_);
	if (!sceneSource) {
		return;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);

	obs_sceneitem_t *item = FindItemById(scene, state_->drag.id);
	if (item && !obs_sceneitem_locked(item)) {
		if (state_->drag.mode == DragMode::Move) {
			vec2 newPos;
			newPos.x = std::round(state_->drag.startItemPos.x +
					      (canvasPos.x - state_->drag.startCanvasPos.x));
			newPos.y = std::round(state_->drag.startItemPos.y +
					      (canvasPos.y - state_->drag.startCanvasPos.y));
			obs_sceneitem_set_pos(item, &newPos);
		} else if (state_->drag.mode == DragMode::Resize) {
			StretchItem(state_->drag, item, canvasPos);
		}
	}
	obs_source_release(sceneSource);
}

void PreviewSurface::OnLeftUp()
{
	ReleaseCapture();
	if (state_->drag.mode != DragMode::None) {
		HostLog("[preview] drag end id=" + std::to_string(state_->drag.id));
	}
	state_->drag.mode = DragMode::None;
	state_->drag.handle = ItemHandle::None;
}

void PreviewSurface::CancelDrag()
{
	state_->drag.mode = DragMode::None;
	state_->drag.handle = ItemHandle::None;
}

void PreviewSurface::EnsureCreated()
{
	if (hwnd_) {
		return;
	}

	RegisterPreviewClass(instance_);

	// Borderless child sibling of the CEF browser HWND. WS_CLIPSIBLINGS keeps the
	// browser from overdrawing into it (the browser HWND also sets it). Starts
	// hidden; SetRect shows it after positioning so no frame flashes at 0,0. The
	// State pointer is stashed in GWLP_USERDATA so the shared WndProc can map this
	// HWND back to its surface state without a global table.
	hwnd_ = CreateWindowExW(0, kPreviewClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 16, 16, host_, nullptr,
				instance_, nullptr);
	if (!hwnd_) {
		HostLog("[obs] preview overlay HWND create FAILED");
		return;
	}
	SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
	HostLog("[obs] preview overlay HWND created");

	gs_init_data init = {};
	init.cx = 16;
	init.cy = 16;
	init.format = GS_BGRA;
	init.zsformat = GS_ZS_NONE;
	init.window.hwnd = hwnd_; // child HWND passthrough (gs_window.hwnd is void*)

	obs_display_t *display = obs_display_create(&init, 0x000000);
	display_ = display;
	HostLog(std::string("[obs] obs_display_create -> ") + (display ? "OK" : "NULL"));
	if (display) {
		obs_display_add_draw_callback(display, RenderPreview, state_);
		HostLog("[obs] preview draw callback registered");
	}
}

void PreviewSurface::SetRect(int x, int y, int cx, int cy)
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

	if (display_) {
		obs_display_resize(static_cast<obs_display_t *>(display_), uint32_t(cx), uint32_t(cy));
	}
}

void PreviewSurface::Hide()
{
	if (hwnd_) {
		ShowWindow(hwnd_, SW_HIDE);
	}
}

void PreviewSurface::Destroy()
{
	if (display_) {
		obs_display_t *display = static_cast<obs_display_t *>(display_);
		obs_display_remove_draw_callback(display, RenderPreview, state_);
		obs_display_destroy(display);
		display_ = nullptr;
		HostLog("[obs] preview display destroyed");
	}
	if (state_->boxBuffer) {
		obs_enter_graphics();
		gs_vertexbuffer_destroy(state_->boxBuffer);
		obs_leave_graphics();
		state_->boxBuffer = nullptr;
	}
	if (hwnd_) {
		DestroyWindow(hwnd_);
		hwnd_ = nullptr;
	}
}

bool PreviewSurface::SelectFromBridge(const std::string &scene, int64_t id, bool hasId)
{
	obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas_);
	if (!sceneSource) {
		return false;
	}
	// Ignore a foreign scene name to keep "preview shows the surface's scene" intact.
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
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		state_->selectedId = newId;
	}
	obs_source_release(sceneSource);

	EmitSelection(targetCanvas_, newId);
	return true;
}

int64_t PreviewSurface::HitTestForTest(float canvasX, float canvasY)
{
	obs_source_t *sceneSource = AcquireSurfaceSceneSource(targetCanvas_);
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

int64_t PreviewSurface::SelectedIdForTest()
{
	std::lock_guard<std::mutex> lock(state_->stateMutex);
	return state_->selectedId;
}

bool PreviewSurface::OnVideoReset()
{
	if (!display_) {
		// No display yet (UI never sized this surface). The next SetRect creates it
		// fresh against the new base resolution; nothing to re-validate.
		HostLog("[preview] OnVideoReset: no display yet (will create lazily)");
		return false;
	}

	// The base resolution changed, so the cached letterbox transform is stale.
	// Reset it; RenderPreview recomputes it from the surface's video info on the
	// next frame. ClientToCanvas treats scale<=0 as "no frame yet" and ignores
	// mouse input until that recompute lands, avoiding a one-frame mis-mapped drag.
	{
		std::lock_guard<std::mutex> lock(state_->stateMutex);
		state_->transform = PreviewTransform{};
	}

	// Nudge a redraw at the current size so the new mix is presented promptly.
	obs_display_update_color_space(static_cast<obs_display_t *>(display_));
	HostLog("[preview] OnVideoReset: display alive, letterbox transform invalidated");
	return true;
}

namespace {

// Route a window message to the surface that owns the HWND (stashed in
// GWLP_USERDATA at creation). null before the first SetRect / for a foreign HWND.
PreviewSurface *SurfaceFromHwnd(HWND hwnd)
{
	return reinterpret_cast<PreviewSurface *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	PreviewSurface *surface = SurfaceFromHwnd(hwnd);
	if (!surface) {
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
	switch (msg) {
	case WM_LBUTTONDOWN:
		surface->OnLeftDown(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
		return 0;
	case WM_MOUSEMOVE:
		surface->OnMouseMove(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
		return 0;
	case WM_LBUTTONUP:
		surface->OnLeftUp();
		return 0;
	case WM_CAPTURECHANGED:
		surface->CancelDrag();
		return 0;
	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

// One managed surface: its canvas uuid key ("" for the Default surface) and the
// owned PreviewSurface. unique_ptr so the surface keeps a stable address while the
// list grows (the WndProc maps an HWND to a surface by pointer).
struct ManagedSurface {
	std::string uuid; // "" => Default surface
	std::unique_ptr<PreviewSurface> surface;
};

// pimpl body: the surface list, kept out of the header so it stays free of
// <vector>/<memory> and the PreviewSurface definition.
struct PreviewManager::Impl {
	std::vector<ManagedSurface> surfaces;
};

PreviewManager::PreviewManager(HWND host, HINSTANCE instance) : impl_(new Impl()), host_(host), instance_(instance) {}

PreviewManager::~PreviewManager()
{
	DestroyAll();
	delete impl_;
}

PreviewSurface *PreviewManager::SurfaceFor(const std::string &canvasUuid)
{
	const bool isDefault = IsDefaultCanvasUuid(canvasUuid);
	const std::string key = isDefault ? std::string() : canvasUuid;

	for (ManagedSurface &s : impl_->surfaces) {
		if (s.uuid == key) {
			return s.surface.get();
		}
	}

	// First use of this canvas: bind the surface to the right mix. Default => null
	// targetCanvas (global mix); otherwise resolve the live obs_canvas_t. An
	// unknown non-Default uuid (no live mix) gets no surface.
	obs_canvas_t *targetCanvas = nullptr;
	if (!isDefault) {
		targetCanvas = ObsBootstrap::CanvasRuntime().Find(canvasUuid);
		if (!targetCanvas) {
			return nullptr;
		}
	}

	impl_->surfaces.push_back(ManagedSurface{key, std::make_unique<PreviewSurface>(host_, instance_, targetCanvas)});
	return impl_->surfaces.back().surface.get();
}

void PreviewManager::SetRect(const std::string &canvasUuid, int x, int y, int cx, int cy)
{
	PreviewSurface *surface = SurfaceFor(canvasUuid);
	if (surface) {
		surface->SetRect(x, y, cx, cy);
	}
}

void PreviewManager::Hide(const std::string &canvasUuid)
{
	const std::string key = IsDefaultCanvasUuid(canvasUuid) ? std::string() : canvasUuid;
	for (ManagedSurface &s : impl_->surfaces) {
		if (s.uuid == key) {
			s.surface->Hide();
			return;
		}
	}
}

void PreviewManager::Destroy(const std::string &canvasUuid)
{
	const std::string key = IsDefaultCanvasUuid(canvasUuid) ? std::string() : canvasUuid;
	for (auto it = impl_->surfaces.begin(); it != impl_->surfaces.end(); ++it) {
		if (it->uuid == key) {
			it->surface->Destroy();
			impl_->surfaces.erase(it);
			return;
		}
	}
}

void PreviewManager::DestroyAll()
{
	for (ManagedSurface &s : impl_->surfaces) {
		s.surface->Destroy();
	}
	impl_->surfaces.clear();
}

void PreviewManager::OnVideoResetAll()
{
	for (ManagedSurface &s : impl_->surfaces) {
		s.surface->OnVideoReset();
	}
}

namespace Preview {

void SetInstance(PreviewManager *pm)
{
	g_instance = pm;
}

PreviewManager *Instance()
{
	return g_instance;
}

bool SelectFromBridge(const std::string &canvas, const std::string &scene, int64_t id, bool hasId)
{
	if (!g_instance) {
		return false;
	}
	PreviewSurface *surface = g_instance->SurfaceFor(canvas);
	if (!surface) {
		return false;
	}
	return surface->SelectFromBridge(scene, id, hasId);
}

int64_t HitTestForTest(const std::string &canvas, float canvasX, float canvasY)
{
	if (!g_instance) {
		return -1;
	}
	PreviewSurface *surface = g_instance->SurfaceFor(canvas);
	if (!surface) {
		return -1;
	}
	return surface->HitTestForTest(canvasX, canvasY);
}

void OnVideoReset()
{
	if (g_instance) {
		g_instance->OnVideoResetAll();
	}
}

} // namespace Preview
