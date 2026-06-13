#include "OBSBasicSettings.hpp"

#include <widgets/OBSBasic.hpp>
#include <utility/CanvasManager.hpp>
#include <dialogs/CanvasEditorDialog.hpp>

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

	auto encLine = [](const QString &label, const CanvasEncoderDef &enc) {
		QString id = enc.useDefault ? QTStr("Basic.Settings.Canvas.Card.UseDefault")
			     : enc.id.empty()  ? QTStr("Basic.Settings.Canvas.Card.Unset")
					       : QString::fromStdString(enc.id);
		return QString("%1: %2").arg(label).arg(id);
	};
	outer->addWidget(new QLabel(encLine(QTStr("Basic.Settings.Canvas.Card.Video"), def.video)));
	outer->addWidget(new QLabel(encLine(QTStr("Basic.Settings.Canvas.Card.Audio"), def.audio)));

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

void OBSBasicSettings::AddCanvasClicked()
{
	CanvasManager &mgr = main->GetCanvasManager();

	CanvasDefinition def;
	def.name = QT_TO_UTF8(QTStr("Basic.Settings.Canvas.NewName"));
	def.width = 1920;
	def.height = 1080;
	def.fpsNum = 60;
	def.fpsDen = 1;

	CanvasDefinition &added = mgr.Add(std::move(def));

	obs_video_info covi = {};
	added.ToVideoInfo(covi);
	main->AddCanvas(added.name, &covi, ACTIVATE | MIX_AUDIO | SCENE_REF, added.uuid.c_str());

	CanvasEditorDialog dlg(added, main, this);
	if (dlg.exec() == QDialog::Accepted) {
		ApplyCanvasEdit(added);
	}

	/* Add/remove are immediate management actions (like profiles/scene collections):
	 * commit to disk now rather than deferring to the Settings Apply button. */
	mgr.Save();
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

	mgr.Save();
	RebuildCanvasList();
}

void OBSBasicSettings::EditCanvasClicked(const std::string &uuid)
{
	CanvasManager &mgr = main->GetCanvasManager();
	CanvasDefinition *def = mgr.Find(uuid);
	if (!def) {
		return;
	}

	CanvasEditorDialog dlg(*def, main, this);
	if (dlg.exec() != QDialog::Accepted) {
		return;
	}

	ApplyCanvasEdit(*def);
	mgr.Save();
	RebuildCanvasList();
}

void OBSBasicSettings::ApplyCanvasEdit(const CanvasDefinition &def)
{
	if (def.isDefault) {
		obs_video_info ovi;
		if (obs_get_video_info(&ovi)) {
			ovi.base_width = def.width;
			ovi.base_height = def.height;
			ovi.output_width = def.width;
			ovi.output_height = def.height;
			ovi.fps_num = def.fpsNum;
			ovi.fps_den = def.fpsDen;
			if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS) {
				blog(LOG_WARNING, "Failed to apply Default canvas resolution %ux%u", def.width,
				     def.height);
			}
		}
		return;
	}

	for (const OBS::Canvas &canvas : main->GetCanvases()) {
		if (def.uuid == obs_canvas_get_uuid(canvas)) {
			obs_video_info ovi = {};
			def.ToVideoInfo(ovi);
			if (!obs_canvas_reset_video(static_cast<obs_canvas_t *>(canvas), &ovi)) {
				blog(LOG_WARNING, "Failed to apply canvas '%s' resolution %ux%u", def.name.c_str(),
				     def.width, def.height);
			}
			break;
		}
	}
}
