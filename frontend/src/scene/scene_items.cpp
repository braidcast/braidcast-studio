#include "scene_items.hpp"

#include <obs.hpp>

#include <graphics/matrix4.h>
#include <graphics/vec3.h>

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

} // namespace SceneItems
