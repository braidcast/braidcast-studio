#pragma once

#include <QFrame>

#include <string>
#include <unordered_map>

class OBSBasic;
class QVBoxLayout;
class QShowEvent;
class QLabel;

namespace idian {
class ToggleSwitch;
}

/* Control panel for the fan-out streaming engine. Lists every output binding
 * grouped by canvas with live status, and exposes per-canvas cascade toggles and
 * per-binding enable toggles to switch streams on and off. It never edits
 * bindings or profiles — that is done in Settings -> Outputs. */
class MultistreamDock : public QFrame {
	Q_OBJECT

public:
	explicit MultistreamDock(OBSBasic *main, QWidget *parent = nullptr);
	~MultistreamDock() override;

	/* Rebuild the whole widget tree from the current bindings. Call when the
	 * binding set changes (add/remove/profile/enable). */
	void Refresh();

	/* Update only the live status of the existing rows (dot, text, toggles,
	 * cascade) without tearing down the tree. Call on engine status ticks, which
	 * never change the binding set. Both run on the Qt main thread. */
	void UpdateStatuses();

protected:
	void showEvent(QShowEvent *event) override;

private:
	struct RowWidgets {
		QLabel *dot = nullptr;
		QLabel *stateText = nullptr;
		idian::ToggleSwitch *toggle = nullptr;
	};

	OBSBasic *main = nullptr;
	QVBoxLayout *groupsLayout = nullptr; // inside the scroll area; one entry per canvas
	bool refreshing = false;             // guards programmatic setChecked from firing Start/Stop

	/* Live handles into the current tree, repopulated by Refresh and read by
	 * UpdateStatuses. Cleared before each rebuild so no entry outlives its
	 * widgets. */
	std::unordered_map<std::string, RowWidgets> rowWidgets;              // by binding uuid
	std::unordered_map<std::string, idian::ToggleSwitch *> cascadeBoxes; // by canvas uuid
};
