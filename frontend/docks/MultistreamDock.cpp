#include "MultistreamDock.hpp"

#include <widgets/OBSBasic.hpp>
#include <utility/MultistreamOutput.hpp>
#include <utility/OutputBinding.hpp>
#include <utility/StreamProfile.hpp>
#include <utility/StreamProfileManager.hpp>
#include <utility/CanvasManager.hpp>
#include <utility/CanvasDefinition.hpp>

#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
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

/* Single source of truth for State -> (localized text, dot color). Indexed by
 * the State enum value so adding a state is a one-line table edit. */
StateStyle StyleFor(MultistreamOutput::State state)
{
	static const std::array<StateStyle, 4> table = {{
		{"Basic.Multistream.Status.Idle", "#888888"},
		{"Basic.Multistream.Status.Connecting", "#e0a000"},
		{"Basic.Multistream.Status.Live", "#2ecc40"},
		{"Basic.Multistream.Status.Error", "#ff4136"},
	}};
	size_t idx = static_cast<size_t>(state);
	if (idx >= table.size()) {
		idx = 0;
	}
	return table[idx];
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

	QHBoxLayout *footer = new QHBoxLayout();
	QPushButton *goLive = new QPushButton(QTStr("Basic.Multistream.GoLive"));
	QPushButton *stopAll = new QPushButton(QTStr("Basic.Multistream.StopAll"));
	connect(goLive, &QPushButton::clicked, this, [this]() {
		if (auto *e = main->GetMultistreamOutput()) {
			e->StartAll();
		}
		Refresh();
	});
	connect(stopAll, &QPushButton::clicked, this, [this]() {
		if (auto *e = main->GetMultistreamOutput()) {
			e->StopAll();
		}
		Refresh();
	});
	footer->addWidget(goLive);
	footer->addWidget(stopAll);
	footer->addStretch();
	outer->addLayout(footer);

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
		QCheckBox *cascade = new QCheckBox();
		cascade->setObjectName("multistreamCascade");
		cascade->setToolTip(QTStr("Basic.Multistream.Cascade.Tooltip"));
		cascade->setChecked(allLive);
		connect(cascade, &QCheckBox::toggled, this, [this, canvasUuid](bool on) {
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
			QString profileText = p ? QString::fromStdString(p->DisplayName())
						: QString::fromUtf8("\xE2\x80\x94");
			QLabel *profileLabel = new QLabel(profileText);
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

			/* Per-row enable toggle: set checked BEFORE connecting so the
			 * initial state never emits toggled. The refreshing guard is the
			 * belt-and-suspenders for any later programmatic change. */
			QCheckBox *toggle = new QCheckBox();
			toggle->setObjectName("multistreamRowToggle");
			toggle->setChecked(engine && engine->IsLive(bindingUuid));
			connect(toggle, &QCheckBox::toggled, this, [this, bindingUuid](bool on) {
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

			groupLayout->addWidget(row);
		}

		groupsLayout->addWidget(group);
		return true;
	};

	bool anyRow = false;

	/* Default canvas first (not part of the additional-canvases vector). */
	const CanvasDefinition &def = main->GetCanvasManager().Default();
	anyRow |= buildGroup(def.uuid, QString::fromStdString(def.name));

	for (const OBS::Canvas &canvas : main->GetCanvases()) {
		if (obs_canvas_get_flags(canvas) & EPHEMERAL) {
			continue;
		}
		const char *uuid = obs_canvas_get_uuid(canvas);
		const char *name = obs_canvas_get_name(canvas);
		anyRow |= buildGroup(std::string(uuid ? uuid : ""), QString::fromUtf8(name ? name : ""));
	}

	if (!anyRow) {
		QLabel *empty = new QLabel(QTStr("Basic.Multistream.Empty"));
		empty->setAlignment(Qt::AlignCenter);
		empty->setWordWrap(true);
		groupsLayout->addWidget(empty);
	}

	groupsLayout->addStretch();

	refreshing = false;
}
