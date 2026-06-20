#pragma once

#include "OBSQTDisplay.hpp"

#include <graphics/matrix4.h>

#include <mutex>

#define ITEM_LEFT (1 << 0)
#define ITEM_RIGHT (1 << 1)
#define ITEM_TOP (1 << 2)
#define ITEM_BOTTOM (1 << 3)
#define ITEM_ROT (1 << 4)

#define MAX_SCALING_LEVEL 32
#define MAX_SCALING_AMOUNT 8.0f
#define ZOOM_SENSITIVITY pow(MAX_SCALING_AMOUNT, 1.0f / MAX_SCALING_LEVEL)

#define SPACER_LABEL_MARGIN 6.0f

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
	Rot = ITEM_ROT
};

class OBSBasicPreview : public OBSQTDisplay {
	Q_OBJECT

	friend class SourceTree;
	friend class SourceTreeItem;

private:
	obs_sceneitem_crop startCrop;
	vec2 startItemPos;
	vec2 cropSize;
	OBSSceneItem stretchGroup;
	OBSSceneItem stretchItem;
	ItemHandle stretchHandle = ItemHandle::None;
	float rotateAngle;
	vec2 rotatePoint;
	vec2 offsetPoint;
	vec2 stretchItemSize;
	matrix4 screenToItem;
	matrix4 itemToScreen;
	matrix4 invGroupTransform;

	gs_texture_t *overflow = nullptr;
	gs_vertbuffer_t *rectFill = nullptr;
	gs_vertbuffer_t *circleFill = nullptr;
	gs_effect_t *solidEffect = nullptr;
	gs_effect_t *stripedLineEffect = nullptr;

	vec2 startPos;
	vec2 mousePos;
	vec2 lastMoveOffset;
	vec2 scrollingFrom;
	vec2 scrollingOffset;
	bool mouseDown = false;
	bool mouseMoved = false;
	bool mouseOverItems = false;
	bool cropping = false;
	bool locked = false;
	bool scrollMode = false;
	bool fixedScaling = false;
	bool selectionBox = false;
	bool overflowHidden = false;
	bool overflowSelectionHidden = false;
	bool overflowAlwaysVisible = false;
	int32_t scalingLevel = 0;
	float scalingAmount = 1.0f;
	float groupRot = 0.0f;
	bool updatingXScrollBar = false;
	bool updatingYScrollBar = false;

	std::vector<obs_sceneitem_t *> hoveredPreviewItems;
	std::vector<obs_sceneitem_t *> selectedItems;
	std::mutex selectMutex;

	vec2 GetMouseEventPos(QMouseEvent *event);
	static bool FindSelected(obs_scene_t *scene, obs_sceneitem_t *item, void *param);
	static bool DrawSelectedOverflow(obs_scene_t *scene, obs_sceneitem_t *item, void *param);
	static bool DrawSelectedItem(obs_scene_t *scene, obs_sceneitem_t *item, void *param);
	static bool DrawSelectionBox(float x1, float y1, float x2, float y2, gs_vertbuffer_t *box);

	OBSSceneItem GetItemAtPos(const vec2 &pos, bool selectBelow);
	bool SelectedAtPos(const vec2 &pos);

	void DoSelect(const vec2 &pos);
	void DoCtrlSelect(const vec2 &pos);

	vec3 GetSnapOffset(const vec3 &tl, const vec3 &br);

	void GetStretchHandleData(const vec2 &pos, bool ignoreGroup);

	void UpdateCursor(uint32_t &flags);

	void SnapStretchingToScreen(vec3 &tl, vec3 &br);
	void ClampAspect(vec3 &tl, vec3 &br, vec2 &size, const vec2 &baseSize);
	vec3 CalculateStretchPos(const vec3 &tl, const vec3 &br);
	void CropItem(const vec2 &pos);
	void StretchItem(const vec2 &pos);
	void RotateItem(const vec2 &pos);

	void SnapItemMovement(vec2 &offset);
	void MoveItems(const vec2 &pos);
	void BoxItems(const vec2 &startPos, const vec2 &pos);

	void ProcessClick(const vec2 &pos);

	void DrawStripedLine(float x1, float y1, float x2, float y2, float thickness, vec2 scale);

	OBSDataAutoRelease wrapper = nullptr;
	bool changed;

	/* nullptr = drive the main/program scene (today's behavior, the central
	 * preview). Non-null = drive this canvas's current scene and base resolution
	 * (the per-canvas dock surface). */
	obs_canvas_t *targetCanvas = nullptr;

	/* This surface's own viewport mapping (px<->canvas). For the main preview these
	 * mirror OBSBasic::previewX/Y/CX/CY/Scale every frame (kept in lockstep by
	 * RenderMain/ResizePreview) so behavior is unchanged; for a canvas dock surface
	 * only these are written, by the widget's own draw/resize path. Paint and
	 * hit-test both read THESE, never OBSBasic's copy, so they always agree. */
	int surfaceX = 0, surfaceY = 0;
	int surfaceCX = 0, surfaceCY = 0;
	float surfaceScale = 0.0f;

	/* The scene this surface edits: main current scene when targetCanvas is null,
	 * else the target canvas's current scene. Every scene-touching helper routes
	 * through here so the null path stays identical to the main preview. */
	OBSScene TargetScene() const;

	/* The base/output resolution this surface maps against: obs_get_video_info
	 * when targetCanvas is null, else this canvas's video info. Returns false (and
	 * leaves ovi untouched) when no info is available, matching obs_get_video_info. */
	bool TargetVideoInfo(obs_video_info &ovi) const;

	void RenderSpacingHelper(int sourceIndex, vec3 &start, vec3 &end, vec3 &viewport, float pixelRatio);
	void SetLabelText(int sourceIndex, int px);

public:
	OBSBasicPreview(QWidget *parent, Qt::WindowFlags flags = Qt::WindowFlags());
	~OBSBasicPreview();

	void Init();

	static OBSBasicPreview *Get();

	virtual void keyPressEvent(QKeyEvent *event) override;
	virtual void keyReleaseEvent(QKeyEvent *event) override;

	virtual void wheelEvent(QWheelEvent *event) override;

	virtual void mousePressEvent(QMouseEvent *event) override;
	virtual void mouseReleaseEvent(QMouseEvent *event) override;
	virtual void mouseMoveEvent(QMouseEvent *event) override;
	virtual void leaveEvent(QEvent *event) override;

	void DrawOverflow();
	void DrawSceneEditing();

	inline void SetViewport(int x, int y, int cx, int cy, float scale)
	{
		surfaceX = x;
		surfaceY = y;
		surfaceCX = cx;
		surfaceCY = cy;
		surfaceScale = scale;
	}

	/* Bind this surface to a canvas (non-null = per-canvas dock surface) or back
	 * to the main scene (null). The dock passes its canvas at construction. */
	inline void SetTargetCanvas(obs_canvas_t *canvas) { targetCanvas = canvas; }
	inline obs_canvas_t *GetTargetCanvas() const { return targetCanvas; }

	/* Draw callback for a canvas-dock display: paints obs_canvas_render(targetCanvas)
	 * plus the editing overlays, and stores this surface's viewport so hit-test and
	 * paint agree. Install via obs_display_add_draw_callback(display, &CanvasPreviewRender, this). */
	static void CanvasPreviewRender(void *data, uint32_t cx, uint32_t cy);

	inline void SetLocked(bool newLockedVal) { locked = newLockedVal; }
	inline void ToggleLocked() { locked = !locked; }
	inline bool Locked() const { return locked; }

	inline void SetFixedScaling(bool newFixedScalingVal)
	{
		if (fixedScaling == newFixedScalingVal) {
			return;
		}

		fixedScaling = newFixedScalingVal;
		emit fixedScalingChanged(fixedScaling);
	}
	inline bool IsFixedScaling() const { return fixedScaling; }

	void SetScalingLevel(int32_t newScalingLevelVal);
	void SetScalingAmount(float newScalingAmountVal);
	void SetScalingLevelAndAmount(int32_t newScalingLevelVal, float newScalingAmountVal);
	void increaseScalingLevel();
	void decreaseScalingLevel();
	void resetScalingLevel();
	inline int32_t GetScalingLevel() const { return scalingLevel; }
	inline float GetScalingAmount() const { return scalingAmount; }

	void ResetScrollingOffset();
	inline void SetScrollingOffset(float x, float y) { vec2_set(&scrollingOffset, x, y); }
	inline float GetScrollX() const { return scrollingOffset.x; }
	inline float GetScrollY() const { return scrollingOffset.y; }

	void xScrollBarChanged(int value);
	void yScrollBarChanged(int value);

	inline void SetOverflowHidden(bool hidden) { overflowHidden = hidden; }
	inline void SetOverflowSelectionHidden(bool hidden) { overflowSelectionHidden = hidden; }
	inline void SetOverflowAlwaysVisible(bool visible) { overflowAlwaysVisible = visible; }

	inline bool GetOverflowSelectionHidden() const { return overflowSelectionHidden; }
	inline bool GetOverflowAlwaysVisible() const { return overflowAlwaysVisible; }

	/* use libobs allocator for alignment because the matrices itemToScreen
	 * and screenToItem may contain SSE data, which will cause SSE
	 * instructions to crash if the data is not aligned to at least a 16
	 * byte boundary. */
	static inline void *operator new(size_t size) { return bmalloc(size); }
	static inline void operator delete(void *ptr) { bfree(ptr); }

	OBSSourceAutoRelease spacerLabel[4];
	int spacerPx[4] = {0};

	void DrawSpacingHelpers();
	void ClampScrollingOffsets();
	void UpdateXScrollBar(float cx);
	void UpdateYScrollBar(float cy);

signals:
	void scalingChanged(float scalingAmount);
	void fixedScalingChanged(bool isFixed);
};
