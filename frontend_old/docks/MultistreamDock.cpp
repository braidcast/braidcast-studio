#include "MultistreamDock.hpp"

#include <widgets/OBSBasic.hpp>
#include <utility/MultistreamOutput.hpp>
#include <utility/OutputBinding.hpp>
#include <utility/StreamProfile.hpp>
#include <utility/StreamProfileManager.hpp>

#include <qt-wrappers.hpp>

#include <Idian/ToggleSwitch.hpp>

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QShowEvent>
#include <QVBoxLayout>

#include <array>
#include <unordered_map>

namespace {

struct StateStyle {
	const char *textKey;
	const char *color;
};

/* Localized status text per State. The dot color comes from
 * MultistreamOutput::StateColor, the shared palette also used by the canvas dock.
 * Indexed by the State enum value so adding a state is a one-line table edit. */
StateStyle StyleFor(MultistreamOutput::State state)
{
	static const std::array<const char *, 4> textKeys = {{
		"Basic.Multistream.Status.Idle",
		"Basic.Multistream.Status.Connecting",
		"Basic.Multistream.Status.Live",
		"Basic.Multistream.Status.Error",
	}};
	size_t idx = static_cast<size_t>(state);
	if (idx >= textKeys.size()) {
		idx = 0;
	}
	return {textKeys[idx], MultistreamOutput::StateColor(state)};
}

} // namespace

MultistreamDock::MultistreamDock(OBSBasic *main_, QWidget *parent) : QFrame(parent), main(main_)
{
	QVBoxLayout *outer = new QVBoxLayout(this);

	QScrollArea *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);

	QWidget *inner = new QWidget();
	groupsLayout = new QVBoxLayout(inner);
	scroll->setWidget(inner);
	/* setWidget force-enables the viewport's autoFillBackground, which paints it
	 * in the base palette role and makes the scrolled content read as a
	 * different theme than the dock frame; clear it so it inherits. */
	scroll->viewport()->setAutoFillBackground(false);
	inner->setAutoFillBackground(false);
	outer->addWidget(scroll, 1);

	Refresh();
}

MultistreamDock::~MultistreamDock()
{
	/* The engine may outlive teardown ordering and could fire onStatusChanged
	 * into a destroyed dock. Drop the callback so it cannot reach us. */
	if (auto *e = main->GetMultistreamOutput()) {
		e->onStatusChanged = nullptr;
	}
}

void MultistreamDock::showEvent(QShowEvent *event)
{
	Refresh();
	QFrame::showEvent(event);
}

void MultistreamDock::Refresh()
{
	refreshing = true;

	/* Drop handles into the about-to-be-destroyed tree before tearing it down. */
	rowWidgets.clear();
	cascadeBoxes.clear();

	/* Clear the existing groups. deleteLater (not delete) so any in-flight Qt
	 * events targeting these widgets unwind safely. */
	QLayoutItem *item;
	while ((item = groupsLayout->takeAt(0)) != nullptr) {
		if (item->widget()) {
			item->widget()->deleteLater();
		}
		delete item;
	}

	MultistreamOutput *engine = main->GetMultistreamOutput();

	/* Snapshot statuses once; a binding absent from the snapshot is Idle. */
	std::unordered_map<std::string, MultistreamOutput::OutputStatus> byBinding;
	if (engine) {
		for (const MultistreamOutput::OutputStatus &s : engine->Statuses()) {
			byBinding[s.bindingUuid] = s;
		}
	}

	auto buildGroup = [&](const std::string &canvasUuid, const QString &canvasName) -> bool {
		std::vector<OutputBinding *> bindings = main->GetOutputBindings().ForCanvas(canvasUuid);
		if (bindings.empty()) {
			return false;
		}

		QFrame *group = new QFrame();
		group->setObjectName("multistreamGroup");
		group->setFrameShape(QFrame::StyledPanel);
		QVBoxLayout *groupLayout = new QVBoxLayout(group);

		QHBoxLayout *headerRow = new QHBoxLayout();
		QLabel *title = new QLabel(canvasName);
		QFont f = title->font();
		f.setBold(true);
		title->setFont(f);
		headerRow->addWidget(title);
		headerRow->addStretch();

		/* Cascade: checked iff EVERY binding in this canvas is live. */
		bool allLive = true;
		for (OutputBinding *b : bindings) {
			if (!engine || !engine->IsLive(b->uuid)) {
				allLive = false;
				break;
			}
		}
		idian::ToggleSwitch *cascade = new idian::ToggleSwitch(allLive);
		cascade->setObjectName("multistreamCascade");
		cascade->setToolTip(QTStr("Basic.Multistream.Cascade.Tooltip"));
		cascadeBoxes[canvasUuid] = cascade;
		connect(cascade, &QAbstractButton::toggled, this, [this, canvasUuid](bool on) {
			if (refreshing) {
				return;
			}
			if (auto *e = main->GetMultistreamOutput()) {
				for (OutputBinding *b : main->GetOutputBindings().ForCanvas(canvasUuid)) {
					if (on) {
						e->StartOutput(b->uuid);
					} else {
						e->StopOutput(b->uuid);
					}
				}
			}
			Refresh();
		});
		headerRow->addWidget(cascade);
		groupLayout->addLayout(headerRow);

		for (OutputBinding *binding : bindings) {
			const std::string bindingUuid = binding->uuid;

			QFrame *row = new QFrame();
			row->setObjectName("multistreamRow");
			row->setFrameShape(QFrame::StyledPanel);
			QHBoxLayout *rowLayout = new QHBoxLayout(row);

			StreamProfile *p = main->GetStreamProfileManager().Find(binding->profileUuid);
			const bool profileMissing = !p && !binding->profileUuid.empty();
			QString profileText = p                ? QString::fromStdString(p->DisplayName())
					      : profileMissing ? QTStr("Basic.Settings.Outputs.ProfileMissing")
							       : QString::fromUtf8("\xE2\x80\x94");
			QLabel *profileLabel = new QLabel(profileText);
			if (profileMissing) {
				profileLabel->setToolTip(QTStr("Basic.Settings.Outputs.ProfileMissing.Tooltip"));
			}
			rowLayout->addWidget(profileLabel);

			QLabel *canvasLabel = new QLabel(canvasName);
			rowLayout->addWidget(canvasLabel);
			rowLayout->addStretch();

			MultistreamOutput::State state = MultistreamOutput::State::Idle;
			QString lastError;
			auto it = byBinding.find(bindingUuid);
			if (it != byBinding.end()) {
				state = it->second.state;
				lastError = QString::fromStdString(it->second.lastError);
			}
			StateStyle style = StyleFor(state);

			QLabel *dot = new QLabel();
			dot->setFixedSize(12, 12);
			dot->setStyleSheet(QString("border-radius:6px; background:%1;").arg(style.color));
			QLabel *stateText = new QLabel(QTStr(style.textKey));
			if (state == MultistreamOutput::State::Error && !lastError.isEmpty()) {
				dot->setToolTip(lastError);
				stateText->setToolTip(lastError);
			}
			rowLayout->addWidget(dot);
			rowLayout->addWidget(stateText);

			/* Per-row enable toggle: the constructor sets the initial state
			 * before any connection, so it never emits toggled. The refreshing
			 * guard is the belt-and-suspenders for any later programmatic change. */
			idian::ToggleSwitch *toggle = new idian::ToggleSwitch(engine && engine->IsLive(bindingUuid));
			toggle->setObjectName("multistreamRowToggle");
			connect(toggle, &QAbstractButton::toggled, this, [this, bindingUuid](bool on) {
				if (refreshing) {
					return;
				}
				if (auto *e = main->GetMultistreamOutput()) {
					if (on) {
						e->StartOutput(bindingUuid);
					} else {
						e->StopOutput(bindingUuid);
					}
				}
				Refresh();
			});
			rowLayout->addWidget(toggle);

			rowWidgets[bindingUuid] = {dot, stateText, toggle};

			groupLayout->addWidget(row);
		}

		groupsLayout->addWidget(group);
		return true;
	};

	bool anyRow = false;

	main->ForEachStreamableCanvas([&](const std::string &uuid, const std::string &name, uint32_t, uint32_t) {
		anyRow |= buildGroup(uuid, QString::fromStdString(name));
	});

	if (!anyRow) {
		QLabel *empty = new QLabel(QTStr("Basic.Multistream.Empty"));
		empty->setAlignment(Qt::AlignCenter);
		empty->setWordWrap(true);
		groupsLayout->addWidget(empty);
	}

	groupsLayout->addStretch();

	refreshing = false;
}

void MultistreamDock::UpdateStatuses()
{
	MultistreamOutput *engine = main->GetMultistreamOutput();

	/* A ToggleSwitch only repositions its handle on show or click, never on a
	 * programmatic setChecked, so an engine-driven state flip (a stream erroring
	 * out, an async connect completing) can't be reflected in place. Escalate any
	 * such divergence to a full Refresh, whose freshly-built switches lay out
	 * correctly on showEvent; the in-place dot/text path below covers the common
	 * tick where nothing toggled. */
	for (const auto &[bindingUuid, w] : rowWidgets) {
		if (w.toggle->isChecked() != (engine && engine->IsLive(bindingUuid))) {
			Refresh();
			return;
		}
	}
	for (const auto &[canvasUuid, cascade] : cascadeBoxes) {
		std::vector<OutputBinding *> bindings = main->GetOutputBindings().ForCanvas(canvasUuid);
		bool allLive = !bindings.empty();
		for (OutputBinding *b : bindings) {
			if (!engine || !engine->IsLive(b->uuid)) {
				allLive = false;
				break;
			}
		}
		if (cascade->isChecked() != allLive) {
			Refresh();
			return;
		}
	}

	std::unordered_map<std::string, MultistreamOutput::OutputStatus> byBinding;
	if (engine) {
		for (const MultistreamOutput::OutputStatus &s : engine->Statuses()) {
			byBinding[s.bindingUuid] = s;
		}
	}

	for (const auto &[bindingUuid, w] : rowWidgets) {
		MultistreamOutput::State state = MultistreamOutput::State::Idle;
		QString lastError;
		auto it = byBinding.find(bindingUuid);
		if (it != byBinding.end()) {
			state = it->second.state;
			lastError = QString::fromStdString(it->second.lastError);
		}
		StateStyle style = StyleFor(state);

		w.dot->setStyleSheet(QString("border-radius:6px; background:%1;").arg(style.color));
		w.stateText->setText(QTStr(style.textKey));

		const bool showError = state == MultistreamOutput::State::Error && !lastError.isEmpty();
		w.dot->setToolTip(showError ? lastError : QString());
		w.stateText->setToolTip(showError ? lastError : QString());
	}
}
