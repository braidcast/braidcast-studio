#include "CanvasEditorDialog.hpp"

#include <widgets/OBSBasic.hpp>
#include <properties-view.hpp>
#include <qt-wrappers.hpp>

#include <Idian/Idian.hpp>

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

	if (!def.isDefault) {
		resUseDefault = new idian::ToggleSwitch(def.useDefaultResolution);
		QWidget *resUseDefaultRow = new QWidget();
		QHBoxLayout *resUseDefaultLayout = new QHBoxLayout(resUseDefaultRow);
		resUseDefaultLayout->setContentsMargins(0, 0, 0, 0);
		resUseDefaultLayout->addWidget(resUseDefault);
		resUseDefaultLayout->addStretch();
		form->addRow(QTStr("Basic.Settings.Canvas.Editor.UseDefaultRes"), resUseDefaultRow);
		auto applyResDefault = [this](bool on) {
			resCombo->setEnabled(!on);
			fpsNum->setEnabled(!on);
			fpsDen->setEnabled(!on);
		};
		applyResDefault(def.useDefaultResolution);
		connect(resUseDefault, &QAbstractButton::toggled, this, applyResDefault);
	}

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
	if (!def.isDefault) {
		videoUseDefault = new idian::ToggleSwitch(def.video.useDefault);
		QWidget *row = new QWidget();
		QHBoxLayout *rowLayout = new QHBoxLayout(row);
		rowLayout->setContentsMargins(0, 0, 0, 0);
		rowLayout->addWidget(new QLabel(QTStr("Basic.Settings.Canvas.Editor.UseDefaultVideo")));
		rowLayout->addWidget(videoUseDefault);
		rowLayout->addStretch();
		videoTabLayout->insertWidget(0, row);
		auto applyVideoDefault = [this](bool on) {
			videoEncoderCombo->setEnabled(!on);
			if (videoProps) {
				videoProps->SetDisabled(on);
			}
		};
		applyVideoDefault(def.video.useDefault);
		connect(videoUseDefault, &QAbstractButton::toggled, this, applyVideoDefault);
	}
	connect(videoEncoderCombo, &QComboBox::currentIndexChanged, this, [this](int) {
		RebuildEncoderProps(videoEncoderCombo, videoTabLayout, videoProps, def.video);
		if (videoProps && videoUseDefault) {
			videoProps->SetDisabled(videoUseDefault->isChecked());
		}
	});
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
	if (!def.isDefault) {
		audioUseDefault = new idian::ToggleSwitch(def.audio.useDefault);
		QWidget *row = new QWidget();
		QHBoxLayout *rowLayout = new QHBoxLayout(row);
		rowLayout->setContentsMargins(0, 0, 0, 0);
		rowLayout->addWidget(new QLabel(QTStr("Basic.Settings.Canvas.Editor.UseDefaultAudio")));
		rowLayout->addWidget(audioUseDefault);
		rowLayout->addStretch();
		audioTabLayout->insertWidget(0, row);
		auto applyAudioDefault = [this](bool on) {
			audioEncoderCombo->setEnabled(!on);
			if (audioProps) {
				audioProps->SetDisabled(on);
			}
		};
		applyAudioDefault(def.audio.useDefault);
		connect(audioUseDefault, &QAbstractButton::toggled, this, applyAudioDefault);
	}
	connect(audioEncoderCombo, &QComboBox::currentIndexChanged, this, [this](int) {
		RebuildEncoderProps(audioEncoderCombo, audioTabLayout, audioProps, def.audio);
		if (audioProps && audioUseDefault) {
			audioProps->SetDisabled(audioUseDefault->isChecked());
		}
	});
	tabs->addTab(audioTab, QTStr("Basic.Settings.Canvas.Editor.Tab.Audio"));

	QWidget *advTab = new QWidget();
	QVBoxLayout *advTabLayout = new QVBoxLayout(advTab);
	QFormLayout *advForm = new QFormLayout();

	colorFormat = new QComboBox();
	colorFormat->addItem(QTStr("Basic.Settings.Advanced.Video.ColorFormat.NV12"), "NV12");
	colorFormat->addItem(QTStr("Basic.Settings.Advanced.Video.ColorFormat.I420"), "I420");
	colorFormat->addItem(QTStr("Basic.Settings.Advanced.Video.ColorFormat.I444"), "I444");
	colorFormat->addItem(QTStr("Basic.Settings.Advanced.Video.ColorFormat.P010"), "P010");
	colorFormat->addItem(QTStr("Basic.Settings.Advanced.Video.ColorFormat.I010"), "I010");
	colorFormat->addItem(QTStr("Basic.Settings.Advanced.Video.ColorFormat.P216"), "P216");
	colorFormat->addItem(QTStr("Basic.Settings.Advanced.Video.ColorFormat.P416"), "P416");
	colorFormat->addItem(QTStr("Basic.Settings.Advanced.Video.ColorFormat.BGRA"), "RGB");
	advForm->addRow(QTStr("Basic.Settings.Advanced.Video.ColorFormat"), colorFormat);

	colorSpace = new QComboBox();
	colorSpace->addItem(QTStr("Basic.Settings.Advanced.Video.ColorSpace.sRGB"), "sRGB");
	colorSpace->addItem(QTStr("Basic.Settings.Advanced.Video.ColorSpace.709"), "709");
	colorSpace->addItem(QTStr("Basic.Settings.Advanced.Video.ColorSpace.601"), "601");
	colorSpace->addItem(QTStr("Basic.Settings.Advanced.Video.ColorSpace.2100PQ"), "2100PQ");
	colorSpace->addItem(QTStr("Basic.Settings.Advanced.Video.ColorSpace.2100HLG"), "2100HLG");
	advForm->addRow(QTStr("Basic.Settings.Advanced.Video.ColorSpace"), colorSpace);

	colorRange = new QComboBox();
	colorRange->addItem(QTStr("Basic.Settings.Advanced.Video.ColorRange.Partial"), "Partial");
	colorRange->addItem(QTStr("Basic.Settings.Advanced.Video.ColorRange.Full"), "Full");
	advForm->addRow(QTStr("Basic.Settings.Advanced.Video.ColorRange"), colorRange);

	auto selectData = [](QComboBox *combo, const std::string &value) {
		int i = combo->findData(QString::fromStdString(value));
		if (i >= 0) {
			combo->setCurrentIndex(i);
		}
	};
	selectData(colorFormat, def.color.format);
	selectData(colorSpace, def.color.space);
	selectData(colorRange, def.color.range);

	sdrWhiteLevel = new QSpinBox();
	sdrWhiteLevel->setRange(80, 480);
	sdrWhiteLevel->setSuffix(" nits");
	sdrWhiteLevel->setValue((int)def.color.sdrWhiteLevel);
	advForm->addRow(QTStr("Basic.Settings.Advanced.Video.SdrWhiteLevel"), sdrWhiteLevel);

	hdrNominalPeak = new QSpinBox();
	hdrNominalPeak->setRange(400, 10000);
	hdrNominalPeak->setSuffix(" nits");
	hdrNominalPeak->setValue((int)def.color.hdrNominalPeakLevel);
	advForm->addRow(QTStr("Basic.Settings.Advanced.Video.HdrNominalPeakLevel"), hdrNominalPeak);

	advTabLayout->addLayout(advForm);
	if (!def.isDefault) {
		colorUseDefault = new idian::ToggleSwitch(def.color.useDefault);
		QWidget *row = new QWidget();
		QHBoxLayout *rowLayout = new QHBoxLayout(row);
		rowLayout->setContentsMargins(0, 0, 0, 0);
		rowLayout->addWidget(new QLabel(QTStr("Basic.Settings.Canvas.Editor.UseDefaultAdvanced")));
		rowLayout->addWidget(colorUseDefault);
		rowLayout->addStretch();
		advTabLayout->insertWidget(0, row);
		auto applyColorDefault = [this](bool on) {
			colorFormat->setEnabled(!on);
			colorSpace->setEnabled(!on);
			colorRange->setEnabled(!on);
			sdrWhiteLevel->setEnabled(!on);
			hdrNominalPeak->setEnabled(!on);
		};
		applyColorDefault(def.color.useDefault);
		connect(colorUseDefault, &QAbstractButton::toggled, this, applyColorDefault);
	}

	tabs->addTab(advTab, QTStr("Basic.Settings.Canvas.Editor.Tab.Advanced"));

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

	def.color.format = QT_TO_UTF8(colorFormat->currentData().toString());
	def.color.space = QT_TO_UTF8(colorSpace->currentData().toString());
	def.color.range = QT_TO_UTF8(colorRange->currentData().toString());
	def.color.sdrWhiteLevel = (uint32_t)sdrWhiteLevel->value();
	def.color.hdrNominalPeakLevel = (uint32_t)hdrNominalPeak->value();

	if (!def.isDefault) {
		def.useDefaultResolution = resUseDefault->isChecked();
		def.video.useDefault = videoUseDefault->isChecked();
		def.audio.useDefault = audioUseDefault->isChecked();
		def.color.useDefault = colorUseDefault->isChecked();
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
