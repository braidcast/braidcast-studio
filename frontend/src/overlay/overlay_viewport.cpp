// bridge.hpp pulls in the CEF headers, whose CefDOMNode declares methods like
// GetNextSibling that <windows.h> would otherwise macro-clobber. Include it (and
// thus CEF) before any Windows header so CEF parses clean.
#include "bridge.hpp"

#include "overlay_viewport.hpp"

#include "overlay_server.hpp"
#include "overlay_sources.hpp"
#include "overlay_store.hpp"

#include "obs_bootstrap.hpp"
#include "scene/scene_persistence.hpp"

#include "UndoManager.hpp"

#include <obs.h>
#include <obs.hpp>

#include <graphics/vec2.h>

#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "log.hpp"

namespace Overlay {

namespace {

// An item plus the scale its ancestors draw it at: canvas units per unit of the item's
// own parent-local coordinate space. (1, 1) for an item sitting directly in a scene.
using ItemFn = std::function<void(obs_sceneitem_t *, const vec2 &ancestorScale)>;

// libobs subtracts the crop from the source size before scaling, and clamps rather than
// underflowing when the crop swallows the whole source. This is calc_cx/calc_cy from
// obs-scene.c; bounds_crop is deliberately absent because update_item_transform zeroes it
// before every recompute, so it is always 0 at the moment cx is derived.
uint32_t CroppedExtent(uint32_t extent, uint32_t cropSum)
{
	return (cropSum > extent) ? 2 : (extent - cropSum);
}

// The scale libobs will actually draw `item`'s CONTENT at, per axis, as a magnitude.
// This is calculate_bounds_data's scale derivation (obs-scene.c:477-521) recomputed from
// UI-thread-owned state: libobs keeps the same number in item->output_scale, but the
// graphics thread writes it, so it is not ours to read.
//
// Needed for GROUP items and nothing else. A group child's transform lives in the group's
// own coordinate space, so its box has to be multiplied by the group's draw scale before
// it means anything in canvas pixels -- which is the unit the page size is in.
vec2 EffectiveScale(obs_sceneitem_t *item)
{
	obs_transform_info info;
	obs_sceneitem_get_info2(item, &info);
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);

	vec2 scale;
	vec2_set(&scale, std::fabs(info.scale.x), std::fabs(info.scale.y));

	obs_source_t *src = obs_sceneitem_get_source(item);
	const uint32_t cx = CroppedExtent(obs_source_get_width(src), uint32_t(crop.left) + uint32_t(crop.right));
	const uint32_t cy = CroppedExtent(obs_source_get_height(src), uint32_t(crop.top) + uint32_t(crop.bottom));

	const float boundsX = std::fabs(info.bounds.x);
	const float boundsY = std::fabs(info.bounds.y);
	const float width = float(cx) * scale.x;
	const float height = float(cy) * scale.y;
	if (info.bounds_type == OBS_BOUNDS_NONE || width <= 0.0f || height <= 0.0f || boundsX <= 0.0f ||
	    boundsY <= 0.0f) {
		return scale; // plain scale, or nothing sane to divide by
	}

	// MAX_ONLY only binds once the content overflows the box; below that it is NONE.
	obs_bounds_type effective = info.bounds_type;
	if (effective == OBS_BOUNDS_MAX_ONLY && (width > boundsX || height > boundsY)) {
		effective = OBS_BOUNDS_SCALE_INNER;
	}

	switch (effective) {
	case OBS_BOUNDS_STRETCH:
		vec2_set(&scale, boundsX / float(cx), boundsY / float(cy));
		break;
	case OBS_BOUNDS_SCALE_INNER:
	case OBS_BOUNDS_SCALE_OUTER: {
		// The aspect test picks the axis that fits; OUTER is the same test inverted.
		// Read off info.bounds_type, not `effective`, so a promoted MAX_ONLY keeps
		// INNER's sense (which is what libobs does).
		bool useWidth = (boundsX / boundsY) < (width / height);
		if (info.bounds_type == OBS_BOUNDS_SCALE_OUTER) {
			useWidth = !useWidth;
		}
		vec2_mulf(&scale, &scale, useWidth ? boundsX / width : boundsY / height);
		break;
	}
	case OBS_BOUNDS_SCALE_TO_WIDTH:
		vec2_mulf(&scale, &scale, boundsX / width);
		break;
	case OBS_BOUNDS_SCALE_TO_HEIGHT:
		vec2_mulf(&scale, &scale, boundsY / height);
		break;
	default:
		break;
	}
	return scale;
}

// One walk over every scene item drawing `src`, across the main canvas and every named
// canvas, descending into groups. The two enumerations are disjoint: obs_enum_scenes
// covers the main canvas only and obs_enum_canvases walks the named ones, so both are
// required and neither repeats the other. Group scenes are skipped where they surface as
// scenes of their own -- their items are reached through the group ITEM instead, so each
// item is visited exactly once, with the group's draw scale carried down to it.
//
// Items are collected under the scene locks and visited after they are released: `fn`
// mutates transforms, and doing that inside obs_scene_enum_items would run libobs's
// transform update (and its item_transform signal) with the scene held.
struct MeasuredItem {
	obs_sceneitem_t *item; // addref'd
	vec2 ancestorScale;
};

struct Collect {
	const obs_source_t *src;
	std::vector<MeasuredItem> *items;
	vec2 scale; // accumulated ancestor scale for the level being walked
};

bool CollectItemCb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	Collect *c = static_cast<Collect *>(param);
	if (obs_sceneitem_is_group(item)) {
		const vec2 groupScale = EffectiveScale(item);
		Collect inner{c->src, c->items, {{{c->scale.x * groupScale.x, c->scale.y * groupScale.y}}}};
		obs_sceneitem_group_enum_items(item, CollectItemCb, &inner);
		return true;
	}
	if (obs_sceneitem_get_source(item) == c->src) {
		obs_sceneitem_addref(item);
		c->items->push_back(MeasuredItem{item, c->scale});
	}
	return true;
}

bool CollectSceneCb(void *param, obs_source_t *sceneSource)
{
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (scene != nullptr && !obs_scene_is_group(scene)) {
		obs_scene_enum_items(scene, CollectItemCb, param);
	}
	return true;
}

bool CollectCanvasCb(void *param, obs_canvas_t *canvas)
{
	obs_canvas_enum_scenes(canvas, CollectSceneCb, param);
	return true;
}

void ForEachItemOfSource(obs_source_t *src, const ItemFn &fn)
{
	if (src == nullptr) {
		return;
	}
	std::vector<MeasuredItem> items;
	Collect ctx{src, &items, {{{1.0f, 1.0f}}}};
	obs_enum_scenes(CollectSceneCb, &ctx);
	obs_enum_canvases(CollectCanvasCb, &ctx);

	// Every collected item is addref'd, so the releases have to happen even if `fn`
	// leaves by an exception.
	struct ReleaseAll {
		std::vector<MeasuredItem> &items;
		~ReleaseAll()
		{
			for (const MeasuredItem &m : items) {
				obs_sceneitem_release(m.item);
			}
		}
	} releaseAll{items};

	for (const MeasuredItem &m : items) {
		fn(m.item, m.ancestorScale);
	}
}

// Our own viewport write changes obs_source_get_width(), which makes libobs re-run
// update_item_transform for every item on the source on the next graphics tick. Three
// things already terminate that: nothing here subscribes to item_transform, a pinned
// item's box no longer depends on the source size, and the target is a pure function of
// (bounds, crop) which the write does not touch. The guard is here so that property is
// local to this file rather than an argument spanning three subsystems.
bool g_inCommit = false;

struct ReentryGuard {
	const bool already;
	ReentryGuard() : already(g_inCommit) { g_inCommit = true; }
	~ReentryGuard()
	{
		if (!already) {
			g_inCommit = false;
		}
	}
	ReentryGuard(const ReentryGuard &) = delete;
	ReentryGuard &operator=(const ReentryGuard &) = delete;
};

// The viewport follow is a consequence of the user's edit, not an edit of its own: it
// must never land on the undo stack as a second entry the user has to step through
// (UndoManager has no grouping, so every AddAction is one discrete step).
struct UndoSuppression {
	UndoSuppression() { ObsBootstrap::Undo().PushDisabled(); }
	~UndoSuppression() { ObsBootstrap::Undo().PopDisabled(); }
	UndoSuppression(const UndoSuppression &) = delete;
	UndoSuppression &operator=(const UndoSuppression &) = delete;
};

// Pin + size one source. Returns true when anything actually changed, which is what
// decides whether the collection is persisted and the UI is told to re-read.
bool CommitOne(obs_source_t *src)
{
	// Bail before the pin, not just before the write: pinning items whose page can never
	// be resized to match would leave the boxes right and the pages permanently stale.
	if (!IsOverlayUrlResolvable(src)) {
		return false;
	}
	const bool pinned = PinAllItems(src);

	uint32_t width = 0;
	uint32_t height = 0;
	if (!ComputeViewport(src, width, height)) {
		return pinned;
	}
	return WriteViewport(src, width, height) || pinned;
}

bool CollectOverlayUuidCb(void *param, obs_source_t *source)
{
	if (IsOverlaySource(source)) {
		const char *uuid = obs_source_get_uuid(source);
		if (uuid != nullptr) {
			static_cast<std::vector<std::string> *>(param)->push_back(uuid);
		}
	}
	return true;
}

// Trailing-coalesce bookkeeping for CommitForSourceDebounced. UI thread only, so the map
// needs no lock. An entry holds the sequence number of the newest request for that uuid;
// a delayed task whose number no longer matches was superseded and drops itself.
std::unordered_map<std::string, uint64_t> g_pendingPublish;
uint64_t g_publishSeq = 0;

// The deferred half of the debounced commit: write the collection to disk and tell the UI
// to re-read. The pin and the page size itself are NOT deferred -- the caller's own
// undo AFTER-state is captured the moment it returns, so it has to see the final
// viewport. What a keystroke burst actually makes expensive is this half (a full
// scene-collection JSON write, plus a list refresh, per character), so this is the half
// that coalesces.
void FireCoalescedPublish(std::string uuid, uint64_t seq)
{
	CEF_REQUIRE_UI_THREAD();
	// Teardown runs the authoritative SceneCollection::Save(), THEN unbinds every scene,
	// and only then pumps CEF -- so a task armed 200 ms before quit would land after the
	// scenes were gone and save a gutted collection over the user's real one.
	// CancelPendingCommits() clears the map ahead of that save; this is the second half,
	// for a task that survives into the CefShutdown drain (mirrors SampleStatsTick).
	if (Bridge::IsShuttingDown()) {
		return;
	}
	auto it = g_pendingPublish.find(uuid);
	if (it == g_pendingPublish.end() || it->second != seq) {
		return; // superseded by a newer request in the same window
	}
	g_pendingPublish.erase(it);
	OBSSourceAutoRelease src = obs_get_source_by_uuid(uuid.c_str()); // addref'd; null once removed
	if (!src) {
		return;
	}
	SceneCollection::Save();
	Bridge::EmitSceneItemsChangedForSource(src);
}

} // namespace

bool PinItemToBounds(obs_sceneitem_t *item)
{
	CEF_REQUIRE_UI_THREAD();
	if (item == nullptr) {
		return false;
	}
	obs_transform_info info;
	obs_sceneitem_get_info2(item, &info); // absolute coordinates, symmetric with set_info2
	if (info.bounds_type != OBS_BOUNDS_NONE) {
		return false; // already source-size independent
	}

	obs_source_t *src = obs_sceneitem_get_source(item); // borrowed
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	const uint32_t cx = CroppedExtent(obs_source_get_width(src), uint32_t(crop.left) + uint32_t(crop.right));
	const uint32_t cy = CroppedExtent(obs_source_get_height(src), uint32_t(crop.top) + uint32_t(crop.bottom));

	vec2 scale;
	obs_sceneitem_get_scale(item, &scale);

	// OBS_BOUNDS_STRETCH over exactly the box the item occupies now is pixel-identical
	// to the OBS_BOUNDS_NONE it replaces: calculate_bounds_data derives scale =
	// bounds / cx, which leaves both size diffs at zero (so the bounds alignment
	// contributes nothing and the crop-to-bounds branch cannot trigger) and leaves cx/cy
	// at the same numbers they came from (so the item alignment resolves the same
	// origin). copysignf keeps a flipped item flipped, which is why the magnitude is
	// taken here. bounds_alignment is deliberately left as-is.
	info.bounds_type = OBS_BOUNDS_STRETCH;
	vec2_set(&info.bounds, std::fabs(scale.x) * float(cx), std::fabs(scale.y) * float(cy));
	obs_sceneitem_set_info2(item, &info);
	return true;
}

bool PinAllItems(obs_source_t *src)
{
	CEF_REQUIRE_UI_THREAD();
	bool pinned = false;
	// The pin is expressed in the item's OWN coordinate space, so the ancestor scale is
	// deliberately not applied here -- only ComputeViewport, which works in canvas
	// pixels, needs it.
	ForEachItemOfSource(src, [&pinned](obs_sceneitem_t *item, const vec2 &) {
		pinned = PinItemToBounds(item) || pinned;
	});
	return pinned;
}

bool ComputeViewport(obs_source_t *src, uint32_t &outWidth, uint32_t &outHeight)
{
	CEF_REQUIRE_UI_THREAD();
	double wantW = 0.0;
	double wantH = 0.0;
	bool any = false;

	ForEachItemOfSource(src, [&](obs_sceneitem_t *item, const vec2 &ancestorScale) {
		vec2 bounds;
		obs_sceneitem_get_bounds(item, &bounds);
		obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(item, &crop);

		// Two different units in one sum, and they are not interchangeable. The BOX is
		// in the item's parent-local space, so it takes the ancestor scale to become
		// canvas pixels -- a 100 px item inside a 2x group draws 200. The CROP is
		// already in page pixels: libobs takes it off the page before scaling what is
		// left into the box, so the page has to be bigger by exactly the raw crop.
		const double w =
			std::fabs(double(bounds.x)) * double(ancestorScale.x) + double(crop.left) + double(crop.right);
		const double h =
			std::fabs(double(bounds.y)) * double(ancestorScale.y) + double(crop.top) + double(crop.bottom);
		if (!std::isfinite(w) || !std::isfinite(h)) {
			return;
		}
		any = true;
		wantW = std::max(wantW, w);
		wantH = std::max(wantH, h);
	});

	if (!any) {
		return false;
	}
	const long long w = std::llround(wantW);
	const long long h = std::llround(wantH);
	if (w < static_cast<long long>(kMinViewportPx) || h < static_cast<long long>(kMinViewportPx)) {
		return false; // keep the last good page rather than wrecking the widget's DOM
	}
	outWidth = static_cast<uint32_t>(std::min<long long>(w, kMaxViewportPx));
	outHeight = static_cast<uint32_t>(std::min<long long>(h, kMaxViewportPx));
	return true;
}

bool WriteViewport(obs_source_t *src, uint32_t width, uint32_t height)
{
	CEF_REQUIRE_UI_THREAD();
	// An overlay re-resolves its URL on EVERY update, so a write while that resolve
	// would come back empty turns a resize into about:blank + DestroyBrowser -- a widget
	// that was live on air goes blank. Not just "is the server up": a widget deleted
	// from the store is still live on its last-served page until the refresh sweep runs,
	// and it fails the same way. IsOverlayUrlResolvable is the proc's own predicate.
	if (!IsOverlayUrlResolvable(src)) {
		return false;
	}

	const uint32_t w = std::clamp(width, uint32_t(1), kMaxViewportPx);
	const uint32_t h = std::clamp(height, uint32_t(1), kMaxViewportPx);

	OBSDataAutoRelease current = obs_source_get_settings(src); // addref'd
	if (current && obs_data_get_int(current, "width") == static_cast<long long>(w) &&
	    obs_data_get_int(current, "height") == static_cast<long long>(h)) {
		return false;
	}

	UndoSuppression undoOff;
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "width", w);
	obs_data_set_int(settings, "height", h);
	obs_source_update(src, settings);

	const char *name = obs_source_get_name(src);
	DBG(LogCat::Overlay, "viewport %ux%u for '%s'", w, h, name ? name : "?");
	return true;
}

void CancelPendingCommits()
{
	CEF_REQUIRE_UI_THREAD();
	g_pendingPublish.clear();
}

void CommitForSource(obs_source_t *src)
{
	CEF_REQUIRE_UI_THREAD();
	ReentryGuard guard;
	if (guard.already) {
		return;
	}
	UndoSuppression undoOff;
	if (!CommitOne(src)) {
		return;
	}
	SceneCollection::Save();
	Bridge::EmitSceneItemsChangedForSource(src);
}

void CommitForSourceDebounced(obs_source_t *src)
{
	CEF_REQUIRE_UI_THREAD();
	const char *uuid = IsOverlaySource(src) ? obs_source_get_uuid(src) : nullptr;
	if (uuid == nullptr) {
		return;
	}
	const std::string key = uuid;

	{
		ReentryGuard guard;
		if (guard.already) {
			return;
		}
		UndoSuppression undoOff;
		if (!CommitOne(src)) {
			return;
		}
	}

	const uint64_t seq = ++g_publishSeq;
	g_pendingPublish[key] = seq;
	CefPostDelayedTask(TID_UI, base::BindOnce(&FireCoalescedPublish, key, seq), kCommitCoalesceMs);
}

void CommitAll()
{
	CEF_REQUIRE_UI_THREAD();
	ReentryGuard guard;
	if (guard.already) {
		return;
	}
	UndoSuppression undoOff;

	std::vector<std::string> uuids;
	obs_enum_sources(CollectOverlayUuidCb, &uuids);

	bool changed = false;
	for (const std::string &uuid : uuids) {
		OBSSourceAutoRelease src = obs_get_source_by_uuid(uuid.c_str()); // addref'd
		if (src && CommitOne(src)) {
			changed = true;
			Bridge::EmitSceneItemsChangedForSource(src);
		}
	}
	if (changed) {
		SceneCollection::Save();
	}
}

} // namespace Overlay
