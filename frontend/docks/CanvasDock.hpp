#pragma once

#include <docks/OBSDock.hpp>

#include <obs.hpp>

#include <string>
#include <vector>

class OBSBasic;
class OBSQTDisplay;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QSplitter;
class QStackedWidget;
class QResizeEvent;
class QTimer;

/* One dockable panel per additional canvas: a per-canvas scene list driven by
 * 2a's canvas scene model plus a live preview of that canvas's video mix. The
 * scene-list/preview split orientation follows the dock's shape so the preview
 * always gets usable area. */
class CanvasDock : public OBSDock {
	Q_OBJECT

public:
	CanvasDock(OBSBasic *main, OBSCanvas canvas, QWidget *parent = nullptr);
	~CanvasDock() override;

public slots:
	/* Repopulate the scene list from the canvas. Invoked (queued) from the
	 * canvas signal trampolines so external scene edits reflect. */
	void RefreshScenes();

	/* Repopulate the source list from the dock's current scene. Invoked
	 * (queued) from the current-scene's item signal trampolines. */
	void RefreshSources();

protected:
	void resizeEvent(QResizeEvent *event) override;

private:
	static void OBSRender(void *data, uint32_t cx, uint32_t cy);

	static void SceneListChanged(void *data, calldata_t *params);
	static void ChannelChanged(void *data, calldata_t *params);
	static void SourceListChanged(void *data, calldata_t *params);
	/* The 🔗 lit-state tracks the main PROGRAM scene, which changes outside this
	 * dock; refresh the scene rows when it does. */
	static void OnFrontendEvent(enum obs_frontend_event event, void *data);

	void ApplyOrientation();
	/* Swap the preview pane to a placeholder when this canvas has no enabled
	 * output binding (Settings > Outputs is the source of truth for "live"). */
	void UpdatePreviewGate();
	/* Open the canvas editor for this (non-default) canvas and apply the edit. */
	void EditCanvas();
	/* Color the footer status dot from this canvas's multistream state (polled). */
	void UpdateStatusDot();
	void OnSceneSelected();
	void AddScene();
	void RenameScene();
	void RemoveScene();

	obs_sceneitem_t *SelectedSceneItem();
	void AddSource();
	void AddExistingSource(OBSSource source);
	void RemoveSource();
	void OpenSourceProperties();
	void MoveSource(enum obs_order_movement movement);

	OBSBasic *main = nullptr;
	OBSCanvas canvas;
	std::string canvasUuid;

	OBSQTDisplay *preview = nullptr;
	QStackedWidget *previewStack = nullptr;
	QListWidget *sceneList = nullptr;
	QListWidget *sourceList = nullptr;
	QSplitter *splitter = nullptr;
	QLabel *statusDot = nullptr;
	QTimer *statusTimer = nullptr;

	std::vector<OBSSignal> sigs;

	/* Item signals subscribed on the dock's CURRENT scene source; torn down
	 * and rebuilt whenever the current scene changes (see RefreshSources). */
	std::vector<OBSSignal> sceneItemSigs;

	bool ready = false;        // guards the draw callback before the display is wired
	bool refreshing = false;   // guards programmatic list rebuild from firing selection
	bool previewGated = false; // true when no enabled output -> placeholder shown, render skipped
};
