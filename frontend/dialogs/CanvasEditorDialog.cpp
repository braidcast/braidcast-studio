#include "CanvasEditorDialog.hpp"

#include <widgets/OBSBasic.hpp>
#include <properties-view.hpp>
#include <qt-wrappers.hpp>

#include <cstdio>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include "moc_CanvasEditorDialog.cpp"

namespace {
static constexpr uint32_t ENCODER_HIDE_FLAGS = OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL;
}

CanvasEditorDialog::CanvasEditorDialog(CanvasDefinition &def_, OBSBasic *main_, QWidget *parent)
	: QDialog(parent),
	  def(def_),
	  main(main_)
{
	setModal(true);
	setWindowTitle(def.isDefault ? QTStr("Basic.Settings.Canvas.Editor.TitleDefault")
				     : QTStr("Basic.Settings.Canvas.Editor.Title"));
	BuildUI();
}

void CanvasEditorDialog::BuildUI()
{
	QVBoxLayout *root = new QVBoxLayout(this);

	QFormLayout *form = new QFormLayout();

	nameEdit = new QLineEdit(QString::fromStdString(def.name));
	nameEdit->setEnabled(!def.isDefault);
	form->addRow(QTStr("Basic.Settings.Canvas.Editor.Name"), nameEdit);

	resCombo = new QComboBox();
	resCombo->setEditable(true);
	resCombo->addItem("1920x1080");
	resCombo->addItem("1280x720");
	resCombo->addItem("1080x1920");
	resCombo->addItem("720x1280");
	resCombo->setCurrentText(QString("%1x%2").arg(def.width).arg(def.height));
	form->addRow(QTStr("Basic.Settings.Canvas.Editor.Resolution"), resCombo);

	QWidget *fpsWidget = new QWidget();
	QHBoxLayout *fpsLayout = new QHBoxLayout(fpsWidget);
	fpsLayout->setContentsMargins(0, 0, 0, 0);

	fpsNum = new QSpinBox();
	fpsNum->setRange(1, 1000000);
	fpsNum->setValue((int)def.fpsNum);

	fpsDen = new QSpinBox();
	fpsDen->setRange(1, 1000000);
	fpsDen->setValue((int)def.fpsDen);

	fpsLayout->addWidget(fpsNum);
	fpsLayout->addWidget(new QLabel("/"));
	fpsLayout->addWidget(fpsDen);
	fpsLayout->addStretch();
	form->addRow(QTStr("Basic.Settings.Canvas.Editor.FPS"), fpsWidget);

	root->addLayout(form);

	tabs = new QTabWidget();

	QWidget *videoTab = new QWidget();
	videoTabLayout = new QVBoxLayout(videoTab);
	videoEncoderCombo = new QComboBox();
	PopulateEncoderCombo(videoEncoderCombo, OBS_ENCODER_VIDEO);
	{
		int i = videoEncoderCombo->findData(QString::fromStdString(def.video.id));
		if (i >= 0) {
			videoEncoderCombo->setCurrentIndex(i);
		}
	}
	videoTabLayout->addWidget(videoEncoderCombo);
	RebuildEncoderProps(videoEncoderCombo, videoTabLayout, videoProps, def.video);
	connect(videoEncoderCombo, &QComboBox::currentIndexChanged, this,
		[this](int) { RebuildEncoderProps(videoEncoderCombo, videoTabLayout, videoProps, def.video); });
	tabs->addTab(videoTab, QTStr("Basic.Settings.Canvas.Editor.Tab.Video"));

	QWidget *audioTab = new QWidget();
	audioTabLayout = new QVBoxLayout(audioTab);
	audioEncoderCombo = new QComboBox();
	PopulateEncoderCombo(audioEncoderCombo, OBS_ENCODER_AUDIO);
	{
		int i = audioEncoderCombo->findData(QString::fromStdString(def.audio.id));
		if (i >= 0) {
			audioEncoderCombo->setCurrentIndex(i);
		}
	}
	audioTabLayout->addWidget(audioEncoderCombo);
	RebuildEncoderProps(audioEncoderCombo, audioTabLayout, audioProps, def.audio);
	connect(audioEncoderCombo, &QComboBox::currentIndexChanged, this,
		[this](int) { RebuildEncoderProps(audioEncoderCombo, audioTabLayout, audioProps, def.audio); });
	tabs->addTab(audioTab, QTStr("Basic.Settings.Canvas.Editor.Tab.Audio"));

	root->addWidget(tabs);

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
		ReadBack();
		accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);
}

void CanvasEditorDialog::ReadBack()
{
	if (!def.isDefault) {
		def.name = QT_TO_UTF8(nameEdit->text());
	}

	std::string res = QT_TO_UTF8(resCombo->currentText());
	unsigned cx = 0, cy = 0;
	if (sscanf(res.c_str(), "%ux%u", &cx, &cy) == 2 && cx > 0 && cy > 0) {
		def.width = cx;
		def.height = cy;
	}

	def.fpsNum = (uint32_t)fpsNum->value();
	def.fpsDen = (uint32_t)fpsDen->value();

	def.video.id = QT_TO_UTF8(videoEncoderCombo->currentData().toString());
	if (videoProps) {
		def.video.settings = obs_data_create();
		obs_data_apply(def.video.settings, videoProps->GetSettings());
	}

	def.audio.id = QT_TO_UTF8(audioEncoderCombo->currentData().toString());
	if (audioProps) {
		def.audio.settings = obs_data_create();
		obs_data_apply(def.audio.settings, audioProps->GetSettings());
	}
}

void CanvasEditorDialog::PopulateEncoderCombo(QComboBox *combo, obs_encoder_type want)
{
	const char *type;
	size_t idx = 0;
	while (obs_enum_encoder_types(idx++, &type)) {
		if (obs_get_encoder_type(type) != want) {
			continue;
		}
		if (want == OBS_ENCODER_VIDEO && (obs_get_encoder_caps(type) & ENCODER_HIDE_FLAGS)) {
			continue;
		}
		combo->addItem(QT_UTF8(obs_encoder_get_display_name(type)), QT_UTF8(type));
	}
	combo->model()->sort(0);
}

void CanvasEditorDialog::RebuildEncoderProps(QComboBox *combo, QVBoxLayout *layout, OBSPropertiesView *&view,
					     CanvasEncoderDef &enc)
{
	if (view) {
		layout->removeWidget(view);
		view->deleteLater();
		view = nullptr;
	}
	std::string id = QT_TO_UTF8(combo->currentData().toString());
	if (id.empty()) {
		return;
	}
	OBSDataAutoRelease settings = obs_encoder_defaults(id.c_str());
	if (enc.id == id && enc.settings) {
		obs_data_apply(settings, enc.settings);
	}
	view = new OBSPropertiesView(settings.Get(), id.c_str(), (PropertiesReloadCallback)obs_get_encoder_properties,
				    170);
	view->setFrameShape(QFrame::NoFrame);
	layout->addWidget(view);
}
