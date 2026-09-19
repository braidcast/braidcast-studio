#include "scene_items.hpp"

#include <obs.hpp>

#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>

#include <algorithm>
#include <cmath>

namespace SceneItems {

std::vector<int64_t> IdsOf(const std::vector<SceneItemKey> &keys)
{
	std::vector<int64_t> ids;
	ids.reserve(keys.size());
	for (const SceneItemKey &key : keys) {
		ids.push_back(key.id);
	}
	return ids;
}

std::vector<int64_t> TopLevelIds(const std::vector<SceneItemKey> &keys)
{
	std::vector<int64_t> ids;
	for (const SceneItemKey &key : keys) {
		if (key.IsTopLevel()) {
			ids.push_back(key.id);
		}
	}
	return ids;
}

obs_source_t *GroupSourceOf(obs_sceneitem_t *item)
{
	obs_scene_t *ownerScene = item ? obs_sceneitem_get_scene(item) : nullptr;
	obs_source_t *ownerSource = ownerScene ? obs_scene_get_source(ownerScene) : nullptr;
	return obs_source_is_group(ownerSource) ? ownerSource : nullptr;
}

namespace {

bool IsGroupItemWithUuid(obs_sceneitem_t *item, const std::string &groupUuid)
{
	if (!obs_sceneitem_is_group(item)) {
		return false;
	}
	const char *uuid = obs_source_get_uuid(obs_sceneitem_get_source(item));
	return uuid && groupUuid == uuid;
}

} // namespace

obs_sceneitem_t *FindGroupItem(obs_scene_t *scene, const std::string &groupUuid)
{
	if (!scene || groupUuid.empty()) {
		return nullptr;
	}
	struct Ctx {
		const std::string &uuid;
		obs_sceneitem_t *found;
	} ctx{groupUuid, nullptr};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			auto *c = static_cast<Ctx *>(param);
			if (IsGroupItemWithUuid(item, c->uuid)) {
				c->found = item;
				return false;
			}
			return true;
		},
		&ctx);
	return ctx.found;
}

obs_sceneitem_t *GroupItemOf(obs_sceneitem_t *item, obs_scene_t *scene)
{
	obs_source_t *groupSource = GroupSourceOf(item);
	const char *groupUuid = groupSource ? obs_source_get_uuid(groupSource) : nullptr;
	if (!groupUuid) {
		return nullptr;
	}
	if (scene) {
		return obs_sceneitem_get_group(scene, item);
	}
	// A group with no canvas of its own is sized against the main canvas by libobs
	// (get_scene_dimensions), so that is where its scene is looked for too.
	OBSCanvasAutoRelease canvas = obs_source_get_canvas(groupSource); // addref'd
	if (!canvas) {
		canvas = obs_get_main_canvas(); // addref'd
	}
	if (!canvas) {
		return nullptr;
	}
	// Refs taken inside the enumeration and searched outside it, so no scene lock is taken
	// while the canvas holds its source-list mutex.
	std::vector<OBSSourceAutoRelease> scenes;
	obs_canvas_enum_scenes(
		canvas,
		[](void *param, obs_source_t *sceneSource) -> bool {
			if (!obs_source_is_group(sceneSource)) {
				static_cast<std::vector<OBSSourceAutoRelease> *>(param)->emplace_back(
					obs_source_get_ref(sceneSource));
			}
			return true;
		},
		&scenes);
	for (const OBSSourceAutoRelease &sceneSource : scenes) {
		obs_sceneitem_t *groupItem = FindGroupItem(obs_scene_from_source(sceneSource), groupUuid);
		if (groupItem && obs_sceneitem_get_source(groupItem) == groupSource) {
			return groupItem;
		}
	}
	return nullptr;
}

SceneItemKey KeyOf(obs_sceneitem_t *item)
{
	obs_source_t *groupSource = GroupSourceOf(item);
	const char *groupUuid = groupSource ? obs_source_get_uuid(groupSource) : nullptr;
	return SceneItemKey(item ? obs_sceneitem_get_id(item) : int64_t(-1), groupUuid ? groupUuid : "");
}

obs_sceneitem_t *FindItem(obs_scene_t *scene, const SceneItemKey &key)
{
	if (!scene || key.id < 0) {
		return nullptr;
	}
	if (key.IsTopLevel()) {
		return obs_scene_find_sceneitem_by_id(scene, key.id);
	}
	obs_sceneitem_t *groupItem = FindGroupItem(scene, key.groupUuid);
	return groupItem ? obs_scene_find_sceneitem_by_id(obs_sceneitem_group_get_scene(groupItem), key.id) : nullptr;
}

void HeldSceneItem::Release()
{
	obs_sceneitem_release(item);
	obs_sceneitem_release(group);
	item = nullptr;
	group = nullptr;
}

bool AcquireItem(obs_scene_t *scene, const SceneItemKey &key, HeldSceneItem &out)
{
	out.Release();
	if (!scene || key.id < 0) {
		return false;
	}
	// Matches by id, or by group uuid when `groupUuid` is set, and takes the reference while
	// the enumeration holds the list's lock.
	struct Ctx {
		int64_t id;
		const std::string *groupUuid;
		obs_sceneitem_t *found;
	};
	auto acquire = [](obs_scene_t *list, Ctx &ctx) {
		obs_scene_enum_items(
			list,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
				auto *c = static_cast<Ctx *>(param);
				const bool match = c->groupUuid ? IsGroupItemWithUuid(item, *c->groupUuid)
								: obs_sceneitem_get_id(item) == c->id;
				if (match) {
					obs_sceneitem_addref(item);
					c->found = item;
				}
				return !match;
			},
			&ctx);
		return ctx.found;
	};
	if (key.IsTopLevel()) {
		Ctx ctx{key.id, nullptr, nullptr};
		out.item = acquire(scene, ctx);
		return out.item != nullptr;
	}
	Ctx groupCtx{-1, &key.groupUuid, nullptr};
	out.group = acquire(scene, groupCtx);
	if (!out.group) {
		return false;
	}
	// Outside the scene's enumeration, so the two lists are never locked together; the group
	// item's reference keeps its scene alive in between.
	Ctx childCtx{key.id, nullptr, nullptr};
	out.item = acquire(obs_sceneitem_group_get_scene(out.group), childCtx);
	if (!out.item) {
		out.Release();
		return false;
	}
	return true;
}

bool GroupToCanvas(obs_sceneitem_t *groupItem, matrix4 &out)
{
	if (!groupItem) {
		matrix4_identity(&out);
		return true;
	}
	if (obs_sceneitem_get_bounds_type(groupItem) != OBS_BOUNDS_NONE && obs_sceneitem_get_bounds_crop(groupItem)) {
		return false;
	}
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(groupItem, &crop);
	matrix4 draw;
	obs_sceneitem_get_draw_transform(groupItem, &draw);
	matrix4 shift;
	matrix4_identity(&shift);
	shift.t.x = -float(crop.left);
	shift.t.y = -float(crop.top);
	// libobs transforms row vectors, so the shift applies first and the draw transform after.
	matrix4_mul(&out, &shift, &draw);
	return true;
}

bool ItemBoxThroughGroup(obs_sceneitem_t *item, obs_sceneitem_t *groupItem, matrix4 &out)
{
	if (!item) {
		return false;
	}
	matrix4 owner;
	if (!GroupToCanvas(groupItem, owner)) {
		return false;
	}
	obs_sceneitem_get_box_transform(item, &out);
	if (groupItem) {
		matrix4_mul(&out, &out, &owner);
	}
	return true;
}

bool ItemBoxToCanvas(obs_sceneitem_t *item, matrix4 &out, obs_scene_t *scene)
{
	if (!item) {
		return false;
	}
	const bool child = GroupSourceOf(item) != nullptr;
	obs_sceneitem_t *groupItem = child ? GroupItemOf(item, scene) : nullptr;
	if (child && !groupItem) {
		return false;
	}
	return ItemBoxThroughGroup(item, groupItem, out);
}

bool CanvasToGroup(obs_sceneitem_t *groupItem, matrix4 &out)
{
	matrix4 groupToCanvas;
	if (!GroupToCanvas(groupItem, groupToCanvas)) {
		return false;
	}
	if (!groupItem) {
		matrix4_identity(&out);
		return true;
	}
	// Inverted here rather than by matrix4_inv, which refuses any determinant under 0.0005
	// (libobs/graphics/matrix4.c:283) -- a general 4x4 guard that would turn away a group
	// scaled to 0.02 on both axes, which is tiny but perfectly workable in float. A group to
	// canvas map is a 2D affine, so its inverse is the 2x2 inverse plus the moved origin, and
	// sharing CanvasToOwnerVector's det == 0 rule is what keeps this and CanvasPlacementRefusal
	// answering alike: a group the refusal admits has to be one a gesture can open on.
	const matrix4 &m = groupToCanvas;
	const float det = m.x.x * m.y.y - m.y.x * m.x.y;
	if (det == 0.0f) {
		return false;
	}
	matrix4_identity(&out);
	out.x.x = m.y.y / det;
	out.x.y = -m.x.y / det;
	out.y.x = -m.y.x / det;
	out.y.y = m.x.x / det;
	out.t.x = -(m.t.x * out.x.x + m.t.y * out.y.x);
	out.t.y = -(m.t.x * out.x.y + m.t.y * out.y.y);
	return std::isfinite(out.x.x) && std::isfinite(out.x.y) && std::isfinite(out.y.x) && std::isfinite(out.y.y) &&
	       std::isfinite(out.t.x) && std::isfinite(out.t.y);
}

void BoxExtent(const matrix4 &boxTransform, vec3 &tl, vec3 &br)
{
	vec3_set(&tl, M_INFINITE, M_INFINITE, 0.0f);
	vec3_set(&br, -M_INFINITE, -M_INFINITE, 0.0f);
	for (float u : {0.0f, 1.0f}) {
		for (float v : {0.0f, 1.0f}) {
			vec3 corner;
			vec3_set(&corner, u, v, 0.0f);
			vec3_transform(&corner, &corner, &boxTransform);
			vec3_min(&tl, &tl, &corner);
			vec3_max(&br, &br, &corner);
		}
	}
}

bool CanvasToOwnerVector(const matrix4 &ownerToCanvas, const vec2 &canvas, vec2 &out)
{
	const matrix4 &m = ownerToCanvas;
	const float det = m.x.x * m.y.y - m.y.x * m.x.y;
	if (det == 0.0f) {
		return false;
	}
	const float gx = (m.y.y * canvas.x - m.y.x * canvas.y) / det;
	const float gy = (m.x.x * canvas.y - m.x.y * canvas.x) / det;
	if (!std::isfinite(gx) || !std::isfinite(gy)) {
		return false;
	}
	vec2_set(&out, gx, gy);
	return true;
}

bool OffsetThroughGroup(obs_sceneitem_t *groupItem, const vec2 &canvasOffset, vec2 &out)
{
	if (!groupItem) {
		out = canvasOffset;
		return true;
	}
	matrix4 drawTransform;
	obs_sceneitem_get_draw_transform(groupItem, &drawTransform);
	return CanvasToOwnerVector(drawTransform, canvasOffset, out);
}

const char *CanvasPlacementRefusal(obs_sceneitem_t *item, obs_sceneitem_t *groupItem)
{
	if (!GroupSourceOf(item)) {
		return nullptr;
	}
	if (!groupItem) {
		return "its group is not in a scene";
	}
	if (obs_sceneitem_get_bounds_type(groupItem) != OBS_BOUNDS_NONE) {
		return "its group has a bounding box, which rescales the group's content into it";
	}
	// The map a gesture actually opens on, not a stand-in for it: CanvasToGroup builds
	// GroupToCanvas on the way in and fails on exactly the determinant that stops the inverse,
	// so anything this admits is something BeginHandleGesture can then build.
	matrix4 canvasToGroup;
	if (!CanvasToGroup(groupItem, canvasToGroup)) {
		return "its group is scaled to nothing";
	}
	return nullptr;
}

void ApplyPendingChildUpdate(obs_sceneitem_t *item)
{
	if (GroupSourceOf(item)) {
		obs_sceneitem_force_update_transform(item);
	}
}

GroupResizeDeferral::GroupResizeDeferral(obs_sceneitem_t *groupItem) : groupItem_(groupItem)
{
	if (groupItem_) {
		obs_sceneitem_addref(groupItem_);
		obs_sceneitem_defer_group_resize_begin(groupItem_);
	}
}

void GroupResizeDeferral::End()
{
	if (obs_sceneitem_t *groupItem = std::exchange(groupItem_, nullptr)) {
		obs_sceneitem_defer_group_resize_end(groupItem);
		obs_sceneitem_release(groupItem);
	}
}

void HoldGroup(std::vector<GroupResizeDeferral> &holds, obs_sceneitem_t *groupItem)
{
	if (groupItem && std::none_of(holds.begin(), holds.end(),
				      [&](const GroupResizeDeferral &hold) { return hold.GroupItem() == groupItem; })) {
		holds.emplace_back(groupItem);
	}
}

void HoldGroupOf(std::vector<GroupResizeDeferral> &holds, obs_sceneitem_t *item, obs_scene_t *scene)
{
	HoldGroup(holds, GroupItemOf(item, scene));
}

} // namespace SceneItems
