#include "OBSBasicSettings.hpp"

#include <widgets/OBSBasic.hpp>
#include <utility/CanvasManager.hpp>

#include <utility>

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

QWidget *OBSBasicSettings::BuildCanvasCard(const CanvasDefinition &def)
{
	QFrame *card = new QFrame();
	card->setObjectName(def.isDefault ? "canvasCardDefault" : "canvasCard");
	card->setFrameShape(QFrame::StyledPanel);

	QVBoxLayout *outer = new QVBoxLayout(card);

	QHBoxLayout *header = new QHBoxLayout();
	QLabel *name = new QLabel(QString::fromStdString(def.name));
	name->setObjectName("canvasCardName");
	header->addWidget(name);

	if (def.isDefault) {
		QLabel *badge = new QLabel(QTStr("Basic.Settings.Canvas.BaseBadge"));
		badge->setObjectName("canvasCardBadge");
		header->addWidget(badge);
	}
	header->addStretch();

	QPushButton *edit = new QPushButton(QTStr("Edit"));
	connect(edit, &QPushButton::clicked, this, [this, uuid = def.uuid]() { EditCanvasClicked(uuid); });
	header->addWidget(edit);

	if (!def.isDefault) {
		QPushButton *remove = new QPushButton(QTStr("Remove"));
		connect(remove, &QPushButton::clicked, this, [this, uuid = def.uuid]() { RemoveCanvasClicked(uuid); });
		header->addWidget(remove);
	}
	outer->addLayout(header);

	QLabel *spec = new QLabel(QString("%1x%2  ·  %3 fps")
					  .arg(def.width)
					  .arg(def.height)
					  .arg(def.fpsDen ? (double)def.fpsNum / def.fpsDen : 0.0, 0, 'g', 4));
	spec->setObjectName("canvasCardSpec");
	outer->addWidget(spec);

	auto encLine = [](const char *label, const CanvasEncoderDef &enc) {
		QString id = enc.useDefault ? QString("Use Default")
					    : QString::fromStdString(enc.id.empty() ? "(unset)" : enc.id);
		return QString("%1: %2").arg(label).arg(id);
	};
	outer->addWidget(new QLabel(encLine("Video", def.video)));
	outer->addWidget(new QLabel(encLine("Audio", def.audio)));

	return card;
}

void OBSBasicSettings::RebuildCanvasList()
{
	QVBoxLayout *layout = ui->canvasPageLayout;

	QLayoutItem *item;
	while ((item = layout->takeAt(0)) != nullptr) {
		if (item->widget()) {
			item->widget()->deleteLater();
		}
		delete item;
	}

	CanvasManager &mgr = main->GetCanvasManager();

	layout->addWidget(BuildCanvasCard(mgr.Default()));

	QFrame *divider = new QFrame();
	divider->setFrameShape(QFrame::HLine);
	divider->setObjectName("canvasDivider");
	layout->addWidget(divider);

	for (const CanvasDefinition &def : mgr.Definitions()) {
		if (def.isDefault) {
			continue;
		}
		layout->addWidget(BuildCanvasCard(def));
	}

	QPushButton *add = new QPushButton(QTStr("Basic.Settings.Canvas.AddCanvas"));
	add->setObjectName("canvasAddTile");
	connect(add, &QPushButton::clicked, this, &OBSBasicSettings::AddCanvasClicked);
	layout->addWidget(add);

	layout->addStretch();
}

void OBSBasicSettings::LoadCanvasSettings()
{
	RebuildCanvasList();
}

void OBSBasicSettings::SaveCanvasSettings() {}

void OBSBasicSettings::AddCanvasClicked()
{
	CanvasManager &mgr = main->GetCanvasManager();

	CanvasDefinition def;
	def.name = QT_TO_UTF8(QTStr("Basic.Settings.Canvas.NewName"));
	def.width = 1920;
	def.height = 1080;
	def.fpsNum = 60;
	def.fpsDen = 1;

	const CanvasDefinition &added = mgr.Add(std::move(def));

	obs_video_info covi = {};
	added.ToVideoInfo(covi);
	main->AddCanvas(added.name, &covi, ACTIVATE | MIX_AUDIO | SCENE_REF, added.uuid.c_str());

	canvasChanged = true;
	EnableApplyButton(true);
	RebuildCanvasList();
}

void OBSBasicSettings::RemoveCanvasClicked(const std::string &uuid)
{
	CanvasManager &mgr = main->GetCanvasManager();
	CanvasDefinition *def = mgr.Find(uuid);
	if (!def || def->isDefault) {
		return;
	}

	QMessageBox::StandardButton confirm = QMessageBox::question(
		this, QTStr("Basic.Settings.Canvas.RemoveConfirm.Title"),
		QTStr("Basic.Settings.Canvas.RemoveConfirm.Text").arg(QString::fromStdString(def->name)));
	if (confirm != QMessageBox::Yes) {
		return;
	}

	for (const OBS::Canvas &canvas : main->GetCanvases()) {
		if (uuid == obs_canvas_get_uuid(canvas)) {
			main->RemoveCanvas(static_cast<obs_canvas_t *>(canvas));
			break;
		}
	}
	mgr.Remove(uuid);

	canvasChanged = true;
	EnableApplyButton(true);
	RebuildCanvasList();
}

void OBSBasicSettings::EditCanvasClicked(const std::string &uuid)
{
	(void)uuid;
	QMessageBox::information(this, QTStr("Basic.Settings.Canvas"),
				QTStr("Basic.Settings.Canvas.EditComingSoon"));
}
