#ifndef OBS_MULTISTREAM_FRONTEND_SCENE_ITEMS_HPP_
#define OBS_MULTISTREAM_FRONTEND_SCENE_ITEMS_HPP_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Declared rather than included: <obs.h> brings libobs's nameless-struct math types, which
// every file including this header (the preview's included) would then have to allow.
typedef struct obs_scene obs_scene_t;
typedef struct obs_scene_item obs_sceneitem_t;
typedef struct obs_source obs_source_t;
struct matrix4;
struct vec3;

// One scene item named the way a scene lists it: its id, and the uuid of the group source
// whose own item list holds it, empty for an item of the scene itself. Ids are unique only
// within one owner, so a child can share its id with a top-level item, and the pair is what
// tells them apart. Groups cannot nest, so a child's group always sits in the scene.
struct SceneItemKey {
	SceneItemKey() = default;
	explicit SceneItemKey(int64_t itemId, std::string group = std::string())
		: id(itemId),
		  groupUuid(std::move(group))
	{
	}

	int64_t id = -1;
	std::string groupUuid;

	bool IsTopLevel() const { return groupUuid.empty(); }
	bool operator==(const SceneItemKey &other) const { return id == other.id && groupUuid == other.groupUuid; }
	bool operator!=(const SceneItemKey &other) const { return !(*this == other); }
};

// Scene-item addressing and geometry shared by the bridge and the preview.
namespace SceneItems {

// The ids of `keys`, in order. A child and a top-level item can share an id, so this can
// repeat one and is not a way back to the keys.
std::vector<int64_t> IdsOf(const std::vector<SceneItemKey> &keys);

// The ids of the top-level members of `keys`, in order.
std::vector<int64_t> TopLevelIds(const std::vector<SceneItemKey> &keys);

// The group source whose own list holds `item`, borrowed, or null for a top-level item.
obs_source_t *GroupSourceOf(obs_sceneitem_t *item);

// The group item with source uuid `groupUuid` in `scene`'s own list, borrowed, or null.
obs_sceneitem_t *FindGroupItem(obs_scene_t *scene, const std::string &groupUuid);

// The group item that draws `item`'s group, borrowed, or null when `item` is not a group's
// child. With `scene`, only that scene is searched. Without it the group is looked up among
// the scenes of the group's canvas, since libobs keeps no link from a group's own scene back
// to the item drawing it; that walk takes each scene's lock, so a caller that already knows
// the scene passes it. Valid only for immediate use, while that scene still holds the group.
obs_sceneitem_t *GroupItemOf(obs_sceneitem_t *item, obs_scene_t *scene = nullptr);

// `item`'s key: its id and its group's uuid.
SceneItemKey KeyOf(obs_sceneitem_t *item);

// The item `key` names in `scene`, borrowed, or null when it does not resolve there. For
// UI-thread callers, and only for immediate use: the graphics tick also releases pruned
// items. The render thread uses AcquireItem.
obs_sceneitem_t *FindItem(obs_scene_t *scene, const SceneItemKey &key);

// An item and, for a child, the group item drawing it, each with its own reference, so a
// remove on another thread cannot free either while they are held. The group item keeps its
// source, and with it the group's own scene, alive too.
struct HeldSceneItem {
	HeldSceneItem() = default;
	HeldSceneItem(const HeldSceneItem &) = delete;
	HeldSceneItem &operator=(const HeldSceneItem &) = delete;
	~HeldSceneItem() { Release(); }

	// Drops both references.
	void Release();

	obs_sceneitem_t *item = nullptr;
	obs_sceneitem_t *group = nullptr; // null for a top-level item
};

// Resolve `key` in `scene` into `out`, taking each reference inside the enumeration of the
// list that holds the item, while that list's lock is held. False when it does not resolve.
bool AcquireItem(obs_scene_t *scene, const SceneItemKey &key, HeldSceneItem &out);

// The map from a group's own space, the one its children's positions are written in, to the
// canvas: the group's draw transform after its crop. A cropped group renders its children
// into a texture shifted by the crop (render_item in obs-scene.c), so a child at (x, y)
// draws where the draw transform puts (x - crop.left, y - crop.top). A group that crops to
// its bounds shifts by a bounds crop too, which libobs does not expose, so for that group
// this is false rather than a map that could be off by it. A null group item maps
// identically.
bool GroupToCanvas(obs_sceneitem_t *groupItem, matrix4 &out);

// `item`'s box transform in canvas space, the unit square mapped onto the box as the canvas
// shows it, carried through `groupItem`, the group item drawing it (null for a top-level
// item). Exact whatever the group's rotation, scale, mirroring or crop; false as
// GroupToCanvas is.
bool ItemBoxThroughGroup(obs_sceneitem_t *item, obs_sceneitem_t *groupItem, matrix4 &out);

// ItemBoxThroughGroup with the group item looked up. False when a child's group item is not
// found (see GroupItemOf for `scene`) or as ItemBoxThroughGroup is.
bool ItemBoxToCanvas(obs_sceneitem_t *item, matrix4 &out, obs_scene_t *scene = nullptr);

// The axis-aligned extent of the unit square `boxTransform` maps.
void BoxExtent(const matrix4 &boxTransform, vec3 &tl, vec3 &br);

} // namespace SceneItems

#endif // OBS_MULTISTREAM_FRONTEND_SCENE_ITEMS_HPP_
