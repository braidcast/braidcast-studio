#include "OBSBasicSettings.hpp"

#include <widgets/OBSBasic.hpp>
#include <utility/CanvasManager.hpp>
#include <utility/OutputBinding.hpp>
#include <utility/StreamProfile.hpp>
#include <utility/StreamProfileManager.hpp>

#include <Idian/ToggleSwitch.hpp>

#include <QComboBox>
#include <QCompleter>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

QWidget *OBSBasicSettings::BuildOutputRow(OutputBinding &binding)
{
	const std::string bindingUuid = binding.uuid;

	QFrame *row = new QFrame();
	row->setObjectName("outputRow");
	row->setFrameShape(QFrame::StyledPanel);

	QHBoxLayout *layout = new QHBoxLayout(row);

	QComboBox *profileCombo = new QComboBox();
	profileCombo->setObjectName("outputProfileCombo");
	profileCombo->setEditable(true);
	profileCombo->setInsertPolicy(QComboBox::NoInsert);
	profileCombo->lineEdit()->setPlaceholderText(QTStr("Basic.Settings.Outputs.PickProfile"));

	for (const StreamProfile &profile : main->GetStreamProfileManager().Profiles()) {
		profileCombo->addItem(QString::fromStdString(profile.DisplayName()),
				      QString::fromStdString(profile.uuid));
	}

	QCompleter *completer = profileCombo->completer();
	if (completer) {
		completer->setCaseSensitivity(Qt::CaseInsensitive);
		completer->setCompletionMode(QCompleter::PopupCompletion);
		completer->setFilterMode(Qt::MatchContains);
	}

	int current = profileCombo->findData(QString::fromStdString(binding.profileUuid));
	profileCombo->setCurrentIndex(current);
	if (current < 0) {
		profileCombo->setCurrentText(QString());
	}

	connect(profileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this, bindingUuid, profileCombo](int) {
			if (loadingOutputs) {
				return;
			}
			OutputBinding *b = main->GetOutputBindings().Find(bindingUuid);
			if (!b) {
				return;
			}
			b->profileUuid = profileCombo->currentData().toString().toStdString();
			main->SaveProject();
			RebuildOutputList();
		});
	layout->addWidget(profileCombo, 1);

	const bool inUse = main->GetOutputBindings().ProfileEnabledElsewhere(binding.uuid, binding.profileUuid);
	if (inUse) {
		QLabel *badge = new QLabel(QTStr("Basic.Settings.Outputs.InUseBadge"));
		badge->setObjectName("outputInUseBadge");
		layout->addWidget(badge);
	}

	idian::ToggleSwitch *enable = new idian::ToggleSwitch(binding.enabled);
	enable->setObjectName("outputEnableToggle");
	enable->setEnabled(!inUse);
	connect(enable, &QAbstractButton::toggled, this, [this, bindingUuid, enable](bool checked) {
		if (loadingOutputs) {
			return;
		}
		OutputBinding *b = main->GetOutputBindings().Find(bindingUuid);
		if (!b) {
			return;
		}
		if (checked && main->GetOutputBindings().ProfileEnabledElsewhere(b->uuid, b->profileUuid)) {
			QMessageBox::warning(this, QTStr("Basic.Settings.Outputs.InUse.Title"),
					     QTStr("Basic.Settings.Outputs.InUse"));
			QSignalBlocker block(enable);
			enable->setChecked(false);
			b->enabled = false;
			return;
		}
		b->enabled = checked;
		main->SaveProject();
		RebuildOutputList();
	});
	layout->addWidget(enable);

	QPushButton *remove = new QPushButton();
	remove->setObjectName("outputRemoveButton");
	remove->setProperty("class", "icon-trash");
	remove->setFlat(true);
	remove->setToolTip(QTStr("Remove"));
	connect(remove, &QPushButton::clicked, this, [this, bindingUuid]() { RemoveOutputClicked(bindingUuid); });
	layout->addWidget(remove);

	return row;
}

QWidget *OBSBasicSettings::BuildCanvasOutputGroup(const char *canvasUuid, const QString &canvasTitle)
{
	const std::string uuid = canvasUuid ? canvasUuid : "";

	QFrame *group = new QFrame();
	group->setObjectName("outputGroup");
	group->setFrameShape(QFrame::StyledPanel);

	QVBoxLayout *outer = new QVBoxLayout(group);

	std::vector<OutputBinding *> canvasBindings = main->GetOutputBindings().ForCanvas(uuid);

	QWidget *headerRow = new QWidget();
	headerRow->setObjectName("outputGroupHeaderRow");
	QHBoxLayout *headerLayout = new QHBoxLayout(headerRow);
	headerLayout->setContentsMargins(0, 0, 0, 0);

	QLabel *header = new QLabel(canvasTitle);
	header->setObjectName("outputGroupHeader");
	headerLayout->addWidget(header);
	headerLayout->addStretch();

	/* Cascade toggle: ON only when every binding under this canvas is enabled;
	 * mixed/none read as OFF. Empty canvas has nothing to cascade, so disable. */
	bool allEnabled = !canvasBindings.empty();
	for (const OutputBinding *binding : canvasBindings) {
		if (!binding->enabled) {
			allEnabled = false;
			break;
		}
	}

	idian::ToggleSwitch *cascade = new idian::ToggleSwitch(allEnabled);
	cascade->setObjectName("outputGroupCascadeToggle");
	cascade->setEnabled(!canvasBindings.empty());
	cascade->setToolTip(QTStr("Basic.Settings.Outputs.CascadeToggle"));
	connect(cascade, &QAbstractButton::toggled, this,
		[this, uuid](bool checked) { CascadeCanvasOutputs(uuid, checked); });
	headerLayout->addWidget(cascade);

	outer->addWidget(headerRow);

	for (OutputBinding *binding : canvasBindings) {
		outer->addWidget(BuildOutputRow(*binding));
	}

	QPushButton *add = new QPushButton(QTStr("Basic.Settings.Outputs.AddOutput"));
	add->setObjectName("outputAddTile");
	connect(add, &QPushButton::clicked, this, [this, uuid]() { AddOutputClicked(uuid); });
	outer->addWidget(add);

	return group;
}

void OBSBasicSettings::RebuildOutputList()
{
	QVBoxLayout *layout = ui->outputsPageLayout;

	loadingOutputs = true;

	QLayoutItem *item;
	while ((item = layout->takeAt(0)) != nullptr) {
		if (item->widget()) {
			item->widget()->deleteLater();
		}
		delete item;
	}

	auto canvasTitle = [](const QString &name, uint32_t width, uint32_t height) {
		return QString("%1  ·  %2x%3").arg(name).arg(width).arg(height);
	};

	/* The Default canvas drives the main pipeline and is not part of the
	 * additional-canvases vector, so render it explicitly first. */
	const CanvasDefinition &def = main->GetCanvasManager().Default();
	layout->addWidget(BuildCanvasOutputGroup(def.uuid.c_str(),
						 canvasTitle(QString::fromStdString(def.name), def.width, def.height)));

	for (const OBS::Canvas &canvas : main->GetCanvases()) {
		if (obs_canvas_get_flags(canvas) & EPHEMERAL) {
			continue;
		}
		const char *uuid = obs_canvas_get_uuid(canvas);
		const char *name = obs_canvas_get_name(canvas);
		obs_video_info ovi = {};
		uint32_t width = 0;
		uint32_t height = 0;
		if (obs_canvas_get_video_info(canvas, &ovi)) {
			width = ovi.base_width;
			height = ovi.base_height;
		}
		layout->addWidget(
			BuildCanvasOutputGroup(uuid, canvasTitle(QString::fromUtf8(name ? name : ""), width, height)));
	}

	layout->addStretch();

	loadingOutputs = false;
}

void OBSBasicSettings::LoadOutputSettings()
{
	RebuildOutputList();
}

void OBSBasicSettings::AddOutputClicked(const std::string &canvasUuid)
{
	/* Add() returns a reference invalidated by the next Add/Remove and the
	 * upcoming RebuildOutputList, so do not hold it. */
	main->GetOutputBindings().Add(canvasUuid);
	main->SaveProject();
	RebuildOutputList();
}

void OBSBasicSettings::RemoveOutputClicked(const std::string &bindingUuid)
{
	main->GetOutputBindings().Remove(bindingUuid);
	main->SaveProject();
	RebuildOutputList();
}

void OBSBasicSettings::CascadeCanvasOutputs(const std::string &canvasUuid, bool enabled)
{
	if (loadingOutputs) {
		return;
	}

	OutputBindings &bindings = main->GetOutputBindings();
	for (OutputBinding *binding : bindings.ForCanvas(canvasUuid)) {
		if (enabled) {
			/* Don't force a profile live twice (one RTMP key = one
			 * stream); leave guarded bindings untouched. */
			if (binding->profileUuid.empty() ||
			    bindings.ProfileEnabledElsewhere(binding->uuid, binding->profileUuid)) {
				continue;
			}
			binding->enabled = true;
		} else {
			binding->enabled = false;
		}
	}

	main->SaveProject();
	RebuildOutputList();
}
