/******************************************************************************
 Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
 Philippe Groarke <philippe.groarke@gmail.com>
 
 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 2 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************/

#include "OBSBasicSettings.hpp"
#include "OBSHotkeyLabel.hpp"
#include "OBSHotkeyWidget.hpp"

#include <components/Multiview.hpp>
#include <components/OBSSourceLabel.hpp>
#include <components/SilentUpdateCheckBox.hpp>
#include <components/SilentUpdateSpinBox.hpp>
#ifdef YOUTUBE_ENABLED
#include <docks/YouTubeAppDock.hpp>
#endif
#include <utility/audio-encoders.hpp>
#include <utility/BaseLexer.hpp>
#include <utility/FFmpegCodec.hpp>
#include <utility/FFmpegFormat.hpp>
#include <utility/SettingsEventFilter.hpp>
#ifdef YOUTUBE_ENABLED
#include <utility/YoutubeApiWrappers.hpp>
#endif
#include <widgets/OBSBasic.hpp>
#include <widgets/OBSProjector.hpp>

#include <properties-view.hpp>
#include <qt-wrappers.hpp>

#include <QCompleter>
#include <QStandardItemModel>

#include <sstream>

#include "moc_OBSBasicSettings.cpp"

using namespace std;

extern const char *get_simple_output_encoder(const char *encoder);

extern bool restart;
extern bool opt_allow_opengl;
extern bool cef_js_avail;

static inline bool ResTooHigh(uint32_t cx, uint32_t cy)
{
	return cx > 16384 || cy > 16384;
}

static inline bool ResTooLow(uint32_t cx, uint32_t cy)
{
	return cx < 32 || cy < 32;
}

/* parses "[width]x[height]", string, i.e. 1024x768 */
static bool ConvertResText(const char *res, uint32_t &cx, uint32_t &cy)
{
	BaseLexer lex;
	base_token token;

	lexer_start(lex, res);

	/* parse width */
	if (!lexer_getbasetoken(lex, &token, IGNORE_WHITESPACE)) {
		return false;
	}
	if (token.type != BASETOKEN_DIGIT) {
		return false;
	}

	cx = std::stoul(token.text.array);

	/* parse 'x' */
	if (!lexer_getbasetoken(lex, &token, IGNORE_WHITESPACE)) {
		return false;
	}
	if (strref_cmpi(&token.text, "x") != 0) {
		return false;
	}

	/* parse height */
	if (!lexer_getbasetoken(lex, &token, IGNORE_WHITESPACE)) {
		return false;
	}
	if (token.type != BASETOKEN_DIGIT) {
		return false;
	}

	cy = std::stoul(token.text.array);

	/* shouldn't be any more tokens after this */
	if (lexer_getbasetoken(lex, &token, IGNORE_WHITESPACE)) {
		return false;
	}

	if (ResTooHigh(cx, cy) || ResTooLow(cx, cy)) {
		cx = cy = 0;
		return false;
	}

	return true;
}

static inline bool WidgetChanged(QWidget *widget)
{
	return widget->property("changed").toBool();
}

static inline void SetComboByName(QComboBox *combo, const QString &name)
{
	int idx = combo->findText(name);
	if (idx != -1) {
		combo->setCurrentIndex(idx);
	}
}

static inline bool SetComboByValue(QComboBox *combo, const QString &name)
{
	int idx = combo->findData(name);
	if (idx != -1) {
		combo->setCurrentIndex(idx);
		return true;
	}

	return false;
}

static inline bool SetInvalidValue(QComboBox *combo, const QString &name, const QVariant &data)
{
	combo->insertItem(0, name, data);

	QStandardItemModel *model = dynamic_cast<QStandardItemModel *>(combo->model());
	if (!model) {
		return false;
	}

	QStandardItem *item = model->item(0);
	item->setFlags(Qt::NoItemFlags);

	combo->setCurrentIndex(0);
	return true;
}

static inline QString GetComboData(QComboBox *combo)
{
	int idx = combo->currentIndex();
	if (idx == -1) {
		return QString();
	}

	return combo->itemData(idx).toString();
}

static int FindEncoder(QComboBox *combo, const char *name, int id)
{
	FFmpegCodec codec{name, id};

	for (int i = 0; i < combo->count(); i++) {
		QVariant v = combo->itemData(i);
		if (!v.isNull()) {
			if (codec == v.value<FFmpegCodec>()) {
				return i;
			}
		}
	}
	return -1;
}

static inline void HighlightGroupBoxLabel(QGroupBox *gb, QWidget *widget, QString objectName)
{
	QFormLayout *layout = qobject_cast<QFormLayout *>(gb->layout());

	if (!layout) {
		return;
	}

	QLabel *label = qobject_cast<QLabel *>(layout->labelForField(widget));

	if (label) {
		label->setObjectName(objectName);

		label->style()->unpolish(label);
		label->style()->polish(label);
	}
}

/* clang-format off */
#define COMBO_CHANGED   &QComboBox::currentIndexChanged
#define EDIT_CHANGED    &QLineEdit::textChanged
#define CBEDIT_CHANGED  &QComboBox::editTextChanged
#define CHECK_CHANGED   &QCheckBox::toggled
#define GROUP_CHANGED   &QGroupBox::toggled
#define SCROLL_CHANGED  &QSpinBox::valueChanged
#define DSCROLL_CHANGED &QDoubleSpinBox::valueChanged
#define TEXT_CHANGED    &QPlainTextEdit::textChanged
#define SLIDER_CHANGED  &QSlider::valueChanged

#define GENERAL_CHANGED &OBSBasicSettings::GeneralChanged
#define STREAM1_CHANGED &OBSBasicSettings::Stream1Changed
#define AUDIO_RESTART   &OBSBasicSettings::AudioChangedRestart
#define AUDIO_CHANGED   &OBSBasicSettings::AudioChanged
#define A11Y_CHANGED    &OBSBasicSettings::A11yChanged
#define APPEAR_CHANGED  &OBSBasicSettings::AppearanceChanged
#define ADV_CHANGED     &OBSBasicSettings::AdvancedChanged
#define ADV_RESTART     &OBSBasicSettings::AdvancedChangedRestart
/* clang-format on */

OBSBasicSettings::OBSBasicSettings(QWidget *parent)
	: QDialog(parent),
	  main(qobject_cast<OBSBasic *>(parent)),
	  ui(new Ui::OBSBasicSettings)
{
	string path;

	EnableThreadedMessageBoxes(true);

	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	ui->setupUi(this);

	main->EnableOutputs(false);

	ui->listWidget->setAttribute(Qt::WA_MacShowFocusRect, false);

	/* clang-format off */
	HookWidget(ui->language,             COMBO_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->updateChannelBox,     COMBO_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->enableAutoUpdates,    CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->openStatsOnStartup,   CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->hideOBSFromCapture,   CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->warnBeforeStreamStart,CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->warnBeforeStreamStop, CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->warnBeforeRecordStop, CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->hideProjectorCursor,  CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->projectorAlwaysOnTop, CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->recordWhenStreaming,  CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->keepRecordStreamStops,CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->replayWhileStreaming, CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->keepReplayStreamStops,CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->systemTrayEnabled,    CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->systemTrayWhenStarted,CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->systemTrayAlways,     CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->saveProjectors,       CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->closeProjectors,      CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->snappingEnabled,      CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->screenSnapping,       CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->centerSnapping,       CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->sourceSnapping,       CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->snapDistance,         DSCROLL_CHANGED,GENERAL_CHANGED);
	HookWidget(ui->overflowHide,         CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->overflowAlwaysVisible,CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->overflowSelectionHide,CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->previewSafeAreas,     CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->automaticSearch,      CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->previewSpacingHelpers,CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->doubleClickSwitch,    CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->studioPortraitLayout, CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->prevProgLabelToggle,  CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->multiviewMouseSwitch, CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->multiviewDrawNames,   CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->multiviewDrawAreas,   CHECK_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->multiviewLayout,      COMBO_CHANGED,  GENERAL_CHANGED);
	HookWidget(ui->theme, 		     COMBO_CHANGED,  APPEAR_CHANGED);
	HookWidget(ui->themeVariant,	     COMBO_CHANGED,  APPEAR_CHANGED);
	HookWidget(ui->appearanceFontScale,  SLIDER_CHANGED, APPEAR_CHANGED);
	HookWidget(ui->appearanceDensity1,   CHECK_CHANGED,  APPEAR_CHANGED);
	HookWidget(ui->appearanceDensity2,   CHECK_CHANGED,  APPEAR_CHANGED);
	HookWidget(ui->appearanceDensity3,   CHECK_CHANGED,  APPEAR_CHANGED);
	HookWidget(ui->appearanceDensity4,   CHECK_CHANGED,  APPEAR_CHANGED);
	HookWidget(ui->service,              COMBO_CHANGED,  STREAM1_CHANGED);
	HookWidget(ui->server,               COMBO_CHANGED,  STREAM1_CHANGED);
	HookWidget(ui->customServer,         EDIT_CHANGED,   STREAM1_CHANGED);
	HookWidget(ui->serviceCustomServer,  EDIT_CHANGED,   STREAM1_CHANGED);
	HookWidget(ui->key,                  EDIT_CHANGED,   STREAM1_CHANGED);
	HookWidget(ui->bandwidthTestEnable,  CHECK_CHANGED,  STREAM1_CHANGED);
	HookWidget(ui->twitchAddonDropdown,  COMBO_CHANGED,  STREAM1_CHANGED);
	HookWidget(ui->useAuth,              CHECK_CHANGED,  STREAM1_CHANGED);
	HookWidget(ui->authUsername,         EDIT_CHANGED,   STREAM1_CHANGED);
	HookWidget(ui->authPw,               EDIT_CHANGED,   STREAM1_CHANGED);
	HookWidget(ui->ignoreRecommended,    CHECK_CHANGED,  STREAM1_CHANGED);
	HookWidget(ui->whipSimulcastTotalLayers, SCROLL_CHANGED, STREAM1_CHANGED);
	HookWidget(ui->enableMultitrackVideo,      CHECK_CHANGED,  STREAM1_CHANGED);
	HookWidget(ui->multitrackVideoMaximumAggregateBitrateAuto, CHECK_CHANGED,  STREAM1_CHANGED);
	HookWidget(ui->multitrackVideoMaximumAggregateBitrate,     SCROLL_CHANGED, STREAM1_CHANGED);
	HookWidget(ui->multitrackVideoMaximumVideoTracksAuto, CHECK_CHANGED,  STREAM1_CHANGED);
	HookWidget(ui->multitrackVideoMaximumVideoTracks,     SCROLL_CHANGED, STREAM1_CHANGED);
	HookWidget(ui->multitrackVideoStreamDumpEnable,            CHECK_CHANGED,  STREAM1_CHANGED);
	HookWidget(ui->multitrackVideoConfigOverrideEnable,        CHECK_CHANGED,  STREAM1_CHANGED);
	HookWidget(ui->multitrackVideoConfigOverride,              TEXT_CHANGED,   STREAM1_CHANGED);
	HookWidget(ui->multitrackVideoAdditionalCanvas,            COMBO_CHANGED,   STREAM1_CHANGED);
	HookWidget(ui->channelSetup,         COMBO_CHANGED,  AUDIO_RESTART);
	HookWidget(ui->sampleRate,           COMBO_CHANGED,  AUDIO_RESTART);
	HookWidget(ui->meterDecayRate,       COMBO_CHANGED,  AUDIO_CHANGED);
	HookWidget(ui->peakMeterType,        COMBO_CHANGED,  AUDIO_CHANGED);
	HookWidget(ui->desktopAudioDevice1,  COMBO_CHANGED,  AUDIO_CHANGED);
	HookWidget(ui->desktopAudioDevice2,  COMBO_CHANGED,  AUDIO_CHANGED);
	HookWidget(ui->auxAudioDevice1,      COMBO_CHANGED,  AUDIO_CHANGED);
	HookWidget(ui->auxAudioDevice2,      COMBO_CHANGED,  AUDIO_CHANGED);
	HookWidget(ui->auxAudioDevice3,      COMBO_CHANGED,  AUDIO_CHANGED);
	HookWidget(ui->auxAudioDevice4,      COMBO_CHANGED,  AUDIO_CHANGED);
	HookWidget(ui->colorsGroupBox,       GROUP_CHANGED,  A11Y_CHANGED);
	HookWidget(ui->colorPreset,          COMBO_CHANGED,  A11Y_CHANGED);
	HookWidget(ui->renderer,             COMBO_CHANGED,  ADV_RESTART);
	HookWidget(ui->adapter,              COMBO_CHANGED,  ADV_RESTART);
	HookWidget(ui->disableOSXVSync,      CHECK_CHANGED,  ADV_CHANGED);
	HookWidget(ui->resetOSXVSync,        CHECK_CHANGED,  ADV_CHANGED);
	if (obs_audio_monitoring_available())
		HookWidget(ui->monitoringDevice,     COMBO_CHANGED,  ADV_CHANGED);
#ifdef _WIN32
	HookWidget(ui->disableAudioDucking,  CHECK_CHANGED,  ADV_CHANGED);
#endif
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
	HookWidget(ui->browserHWAccel,       CHECK_CHANGED,  ADV_RESTART);
#endif
	HookWidget(ui->filenameFormatting,   EDIT_CHANGED,   ADV_CHANGED);
	HookWidget(ui->overwriteIfExists,    CHECK_CHANGED,  ADV_CHANGED);
	HookWidget(ui->simpleRBPrefix,       EDIT_CHANGED,   ADV_CHANGED);
	HookWidget(ui->simpleRBSuffix,       EDIT_CHANGED,   ADV_CHANGED);
	HookWidget(ui->streamDelayEnable,    CHECK_CHANGED,  ADV_CHANGED);
	HookWidget(ui->streamDelaySec,       SCROLL_CHANGED, ADV_CHANGED);
	HookWidget(ui->streamDelayPreserve,  CHECK_CHANGED,  ADV_CHANGED);
	HookWidget(ui->reconnectEnable,      CHECK_CHANGED,  ADV_CHANGED);
	HookWidget(ui->reconnectRetryDelay,  SCROLL_CHANGED, ADV_CHANGED);
	HookWidget(ui->reconnectMaxRetries,  SCROLL_CHANGED, ADV_CHANGED);
	HookWidget(ui->processPriority,      COMBO_CHANGED,  ADV_CHANGED);
	HookWidget(ui->confirmOnExit,        CHECK_CHANGED,  ADV_CHANGED);
	HookWidget(ui->bindToIP,             COMBO_CHANGED,  ADV_CHANGED);
	HookWidget(ui->ipFamily,             COMBO_CHANGED,  ADV_CHANGED);
	HookWidget(ui->enableNewSocketLoop,  CHECK_CHANGED,  ADV_CHANGED);
	HookWidget(ui->enableLowLatencyMode, CHECK_CHANGED,  ADV_CHANGED);
	HookWidget(ui->hotkeyFocusType,      COMBO_CHANGED,  ADV_CHANGED);
	HookWidget(ui->autoRemux,            CHECK_CHANGED,  ADV_CHANGED);
	HookWidget(ui->dynBitrate,           CHECK_CHANGED,  ADV_CHANGED);
	/* clang-format on */

#define ADD_HOTKEY_FOCUS_TYPE(s) ui->hotkeyFocusType->addItem(QTStr("Basic.Settings.Advanced.Hotkeys." s), s)

	ADD_HOTKEY_FOCUS_TYPE("NeverDisableHotkeys");
	ADD_HOTKEY_FOCUS_TYPE("DisableHotkeysInFocus");
	ADD_HOTKEY_FOCUS_TYPE("DisableHotkeysOutOfFocus");

#undef ADD_HOTKEY_FOCUS_TYPE

#if !defined(_WIN32) && !defined(ENABLE_SPARKLE_UPDATER)
	delete ui->updateSettingsGroupBox;
	ui->updateSettingsGroupBox = nullptr;
	ui->updateChannelLabel = nullptr;
	ui->updateChannelBox = nullptr;
	ui->enableAutoUpdates = nullptr;
#else
	// Hide update section if disabled
	if (App()->IsUpdaterDisabled()) {
		ui->updateSettingsGroupBox->hide();
	}
#endif

	// Remove the Advanced Audio section if monitoring is not supported, as the monitoring device selection is the only item in the group box.
	if (!obs_audio_monitoring_available()) {
		delete ui->monitoringDeviceLabel;
		ui->monitoringDeviceLabel = nullptr;
		delete ui->monitoringDevice;
		ui->monitoringDevice = nullptr;
	}

#ifdef _WIN32
	if (!SetDisplayAffinitySupported()) {
		delete ui->hideOBSFromCapture;
		ui->hideOBSFromCapture = nullptr;
	}

	static struct ProcessPriority {
		const char *name;
		const char *val;
	} processPriorities[] = {
		{"Basic.Settings.Advanced.General.ProcessPriority.High", "High"},
		{"Basic.Settings.Advanced.General.ProcessPriority.AboveNormal", "AboveNormal"},
		{"Basic.Settings.Advanced.General.ProcessPriority.Normal", "Normal"},
		{"Basic.Settings.Advanced.General.ProcessPriority.BelowNormal", "BelowNormal"},
		{"Basic.Settings.Advanced.General.ProcessPriority.Idle", "Idle"},
	};

	for (ProcessPriority pri : processPriorities) {
		ui->processPriority->addItem(QTStr(pri.name), pri.val);
	}

#else
#if defined(__APPLE__) && defined(__aarch64__)
	delete ui->adapterLabel;
	delete ui->adapter;

	ui->adapterLabel = nullptr;
	ui->adapter = nullptr;
#else
	delete ui->rendererLabel;
	delete ui->renderer;
	delete ui->adapterLabel;
	delete ui->adapter;

	ui->rendererLabel = nullptr;
	ui->renderer = nullptr;
	ui->adapterLabel = nullptr;
	ui->adapter = nullptr;
#endif
	delete ui->processPriorityLabel;
	delete ui->processPriority;
	delete ui->enableNewSocketLoop;
	delete ui->enableLowLatencyMode;
	delete ui->hideOBSFromCapture;
#if !defined(__APPLE__) && !defined(__linux__)
	delete ui->browserHWAccel;
	delete ui->sourcesGroup;
#endif
	delete ui->disableAudioDucking;

	ui->processPriorityLabel = nullptr;
	ui->processPriority = nullptr;
	ui->enableNewSocketLoop = nullptr;
	ui->enableLowLatencyMode = nullptr;
	ui->hideOBSFromCapture = nullptr;
#if !defined(__APPLE__) && !defined(__linux__)
	ui->browserHWAccel = nullptr;
	ui->sourcesGroup = nullptr;
#endif
	ui->disableAudioDucking = nullptr;
#endif

#ifndef __APPLE__
	delete ui->disableOSXVSync;
	delete ui->resetOSXVSync;
	ui->disableOSXVSync = nullptr;
	ui->resetOSXVSync = nullptr;
#endif

	//Apply button disabled until change.
	EnableApplyButton(false);

	installEventFilter(new SettingsEventFilter());

	auto ReloadAudioSources = [](void *data, calldata_t *param) {
		auto settings = static_cast<OBSBasicSettings *>(data);
		auto source = static_cast<obs_source_t *>(calldata_ptr(param, "source"));

		if (!source) {
			return;
		}

		if (!(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)) {
			return;
		}

		QMetaObject::invokeMethod(settings, "ReloadAudioSources", Qt::QueuedConnection);
	};
	sourceCreated.Connect(obs_get_signal_handler(), "source_create", ReloadAudioSources, this);
	channelChanged.Connect(obs_get_signal_handler(), "channel_change", ReloadAudioSources, this);

	hotkeyConflictIcon = QIcon::fromTheme("obs", QIcon(":/res/images/warning.svg"));

	auto ReloadHotkeys = [](void *data, calldata_t *) {
		auto settings = static_cast<OBSBasicSettings *>(data);
		QMetaObject::invokeMethod(settings, "ReloadHotkeys");
	};
	hotkeyRegistered.Connect(obs_get_signal_handler(), "hotkey_register", ReloadHotkeys, this);

	auto ReloadHotkeysIgnore = [](void *data, calldata_t *param) {
		auto settings = static_cast<OBSBasicSettings *>(data);
		auto key = static_cast<obs_hotkey_t *>(calldata_ptr(param, "key"));
		QMetaObject::invokeMethod(settings, "ReloadHotkeys", Q_ARG(obs_hotkey_id, obs_hotkey_get_id(key)));
	};
	hotkeyUnregistered.Connect(obs_get_signal_handler(), "hotkey_unregister", ReloadHotkeysIgnore, this);

	if (obs_audio_monitoring_available()) {
		FillAudioMonitoringDevices();
	}

	connect(ui->channelSetup, &QComboBox::currentIndexChanged, this, &OBSBasicSettings::SurroundWarning);
	connect(ui->channelSetup, &QComboBox::currentIndexChanged, this, &OBSBasicSettings::SpeakerLayoutChanged);
	connect(ui->lowLatencyBuffering, &QCheckBox::clicked, this, &OBSBasicSettings::LowLatencyBufferingChanged);

	// Get Bind to IP Addresses
	OBSProperties ppts = obs_get_output_properties("rtmp_output");
	obs_property_t *p = obs_properties_get(ppts, "bind_ip");

	size_t count = obs_property_list_item_count(p);
	for (size_t i = 0; i < count; i++) {
		const char *name = obs_property_list_item_name(p, i);
		const char *val = obs_property_list_item_string(p, i);

		ui->bindToIP->addItem(QT_UTF8(name), val);
	}

	// Add IP Family options
	p = obs_properties_get(ppts, "ip_family");

	count = obs_property_list_item_count(p);
	for (size_t i = 0; i < count; i++) {
		const char *name = obs_property_list_item_name(p, i);
		const char *val = obs_property_list_item_string(p, i);

		ui->ipFamily->addItem(QT_UTF8(name), val);
	}

	InitStreamPage();
	InitAppearancePage();
	LoadSettings(false);

	ui->snappingEnabled->setAccessibleName(QTStr("Basic.Settings.General.Snapping"));
	ui->systemTrayEnabled->setAccessibleName(QTStr("Basic.Settings.General.SysTray"));
	ui->streamDelayEnable->setAccessibleName(QTStr("Basic.Settings.Advanced.StreamDelay"));
	ui->reconnectEnable->setAccessibleName(QTStr("Basic.Settings.Output.Reconnect"));

	App()->DisableHotkeys();

	channelIndex = ui->channelSetup->currentIndex();
	sampleRateIndex = ui->sampleRate->currentIndex();
	llBufferingEnabled = ui->lowLatencyBuffering->isChecked();

	connect(ui->useStreamKeyAdv, &QCheckBox::clicked, this, &OBSBasicSettings::UseStreamKeyAdvClicked);

	UpdateAudioWarnings();
	UpdateAdvNetworkGroup();

	ui->audioMsg->setVisible(false);
	ui->advancedMsg->setVisible(false);
	ui->advancedMsg2->setVisible(false);
}

OBSBasicSettings::~OBSBasicSettings()
{
	delete ui->filenameFormatting->completer();
	main->EnableOutputs(true);

	App()->UpdateHotkeyFocusSetting();

	EnableThreadedMessageBoxes(false);
}

bool EncoderAvailable(const char *encoder)
{
	const char *val;
	int i = 0;

	while (obs_enum_encoder_types(i++, &val)) {
		if (strcmp(val, encoder) == 0) {
			return true;
		}
	}

	return false;
}

void OBSBasicSettings::SaveCombo(QComboBox *widget, const char *section, const char *value)
{
	if (WidgetChanged(widget)) {
		config_set_string(main->Config(), section, value, QT_TO_UTF8(widget->currentText()));
	}
}

void OBSBasicSettings::SaveComboData(QComboBox *widget, const char *section, const char *value)
{
	if (WidgetChanged(widget)) {
		QString str = GetComboData(widget);
		config_set_string(main->Config(), section, value, QT_TO_UTF8(str));
	}
}

void OBSBasicSettings::SaveCheckBox(QAbstractButton *widget, const char *section, const char *value, bool invert)
{
	if (WidgetChanged(widget)) {
		bool checked = widget->isChecked();
		if (invert) {
			checked = !checked;
		}

		config_set_bool(main->Config(), section, value, checked);
	}
}

void OBSBasicSettings::SaveEdit(QLineEdit *widget, const char *section, const char *value)
{
	if (WidgetChanged(widget)) {
		config_set_string(main->Config(), section, value, QT_TO_UTF8(widget->text()));
	}
}

void OBSBasicSettings::SaveSpinBox(QSpinBox *widget, const char *section, const char *value)
{
	if (WidgetChanged(widget)) {
		config_set_int(main->Config(), section, value, widget->value());
	}
}

void OBSBasicSettings::SaveText(QPlainTextEdit *widget, const char *section, const char *value)
{
	if (!WidgetChanged(widget)) {
		return;
	}

	auto utf8 = widget->toPlainText().toUtf8();

	OBSDataAutoRelease safe_text = obs_data_create();
	obs_data_set_string(safe_text, "text", utf8.constData());

	config_set_string(main->Config(), section, value, obs_data_get_json(safe_text));
}

std::string DeserializeConfigText(const char *value)
{
	OBSDataAutoRelease data = obs_data_create_from_json(value);
	return obs_data_get_string(data, "text");
}

void OBSBasicSettings::SaveGroupBox(QGroupBox *widget, const char *section, const char *value)
{
	if (WidgetChanged(widget)) {
		config_set_bool(main->Config(), section, value, widget->isChecked());
	}
}

#define CS_PARTIAL_STR QTStr("Basic.Settings.Advanced.Video.ColorRange.Partial")
#define CS_FULL_STR QTStr("Basic.Settings.Advanced.Video.ColorRange.Full")

void OBSBasicSettings::LoadLanguageList()
{
	const char *currentLang = App()->GetLocale();

	ui->language->clear();

	for (const auto &locale : GetLocaleNames()) {
		int idx = ui->language->count();

		ui->language->addItem(QT_UTF8(locale.second.c_str()), QT_UTF8(locale.first.c_str()));

		if (locale.first == currentLang) {
			ui->language->setCurrentIndex(idx);
		}
	}

	ui->language->model()->sort(0);
}

#if defined(_WIN32) || defined(ENABLE_SPARKLE_UPDATER)
void TranslateBranchInfo(const QString &name, QString &displayName, QString &description)
{
	QString translatedName = QTStr(QT_TO_UTF8(("Basic.Settings.General.ChannelName." + name)));
	QString translatedDesc = QTStr(QT_TO_UTF8(("Basic.Settings.General.ChannelDescription." + name)));

	if (!translatedName.startsWith("Basic.Settings.")) {
		displayName = translatedName;
	}
	if (!translatedDesc.startsWith("Basic.Settings.")) {
		description = translatedDesc;
	}
}
#endif

void OBSBasicSettings::LoadBranchesList()
{
#if defined(_WIN32) || defined(ENABLE_SPARKLE_UPDATER)
	bool configBranchRemoved = true;
	QString configBranch = config_get_string(App()->GetAppConfig(), "General", "UpdateBranch");

	for (const UpdateBranch &branch : App()->GetBranches()) {
		if (branch.name == configBranch) {
			configBranchRemoved = false;
		}
		if (!branch.is_visible && branch.name != configBranch) {
			continue;
		}

		QString displayName = branch.display_name;
		QString description = branch.description;

		TranslateBranchInfo(branch.name, displayName, description);
		QString itemDesc = displayName + " - " + description;

		if (!branch.is_enabled) {
			itemDesc.prepend(" ");
			itemDesc.prepend(QTStr("Basic.Settings.General.UpdateChannelDisabled"));
		} else if (branch.name == "stable") {
			itemDesc.append(" ");
			itemDesc.append(QTStr("Basic.Settings.General.UpdateChannelDefault"));
		}

		ui->updateChannelBox->addItem(itemDesc, branch.name);

		// Disable item if branch is disabled
		if (!branch.is_enabled) {
			QStandardItemModel *model = dynamic_cast<QStandardItemModel *>(ui->updateChannelBox->model());
			QStandardItem *item = model->item(ui->updateChannelBox->count() - 1);
			item->setFlags(Qt::NoItemFlags);
		}
	}

	// Fall back to default if not yet set or user-selected branch has been removed
	if (configBranch.isEmpty() || configBranchRemoved) {
		configBranch = "stable";
	}

	int idx = ui->updateChannelBox->findData(configBranch);
	ui->updateChannelBox->setCurrentIndex(idx);
#endif
}

void OBSBasicSettings::LoadGeneralSettings()
{
	loading = true;

	LoadLanguageList();

#if defined(_WIN32) || defined(ENABLE_SPARKLE_UPDATER)
	bool enableAutoUpdates = config_get_bool(App()->GetAppConfig(), "General", "EnableAutoUpdates");
	ui->enableAutoUpdates->setChecked(enableAutoUpdates);

	LoadBranchesList();
#endif
	bool openStatsOnStartup = config_get_bool(main->Config(), "General", "OpenStatsOnStartup");
	ui->openStatsOnStartup->setChecked(openStatsOnStartup);

#if defined(_WIN32)
	if (ui->hideOBSFromCapture) {
		bool hideWindowFromCapture =
			config_get_bool(App()->GetUserConfig(), "BasicWindow", "HideOBSWindowsFromCapture");
		ui->hideOBSFromCapture->setChecked(hideWindowFromCapture);

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
		connect(ui->hideOBSFromCapture, &QCheckBox::checkStateChanged, this,
			&OBSBasicSettings::HideOBSWindowWarning);
#else
		connect(ui->hideOBSFromCapture, &QCheckBox::stateChanged, this,
			&OBSBasicSettings::HideOBSWindowWarning);
#endif
	}
#endif

	bool recordWhenStreaming = config_get_bool(App()->GetUserConfig(), "BasicWindow", "RecordWhenStreaming");
	ui->recordWhenStreaming->setChecked(recordWhenStreaming);

	bool keepRecordStreamStops =
		config_get_bool(App()->GetUserConfig(), "BasicWindow", "KeepRecordingWhenStreamStops");
	ui->keepRecordStreamStops->setChecked(keepRecordStreamStops);

	bool replayWhileStreaming =
		config_get_bool(App()->GetUserConfig(), "BasicWindow", "ReplayBufferWhileStreaming");
	ui->replayWhileStreaming->setChecked(replayWhileStreaming);

	bool keepReplayStreamStops =
		config_get_bool(App()->GetUserConfig(), "BasicWindow", "KeepReplayBufferStreamStops");
	ui->keepReplayStreamStops->setChecked(keepReplayStreamStops);

	/* Phase 1: recording/replay hidden and inert; hide the while-streaming rows so they can't be re-enabled. */
	ui->recordWhenStreaming->setVisible(false);
	ui->keepRecordStreamStops->setVisible(false);
	ui->replayWhileStreaming->setVisible(false);
	ui->keepReplayStreamStops->setVisible(false);

	bool systemTrayEnabled = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SysTrayEnabled");
	ui->systemTrayEnabled->setChecked(systemTrayEnabled);

	bool systemTrayWhenStarted = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SysTrayWhenStarted");
	ui->systemTrayWhenStarted->setChecked(systemTrayWhenStarted);

	bool systemTrayAlways = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SysTrayMinimizeToTray");
	ui->systemTrayAlways->setChecked(systemTrayAlways);

	bool saveProjectors = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SaveProjectors");
	ui->saveProjectors->setChecked(saveProjectors);

	bool closeProjectors = config_get_bool(App()->GetUserConfig(), "BasicWindow", "CloseExistingProjectors");
	ui->closeProjectors->setChecked(closeProjectors);

	bool snappingEnabled = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SnappingEnabled");
	ui->snappingEnabled->setChecked(snappingEnabled);

	bool screenSnapping = config_get_bool(App()->GetUserConfig(), "BasicWindow", "ScreenSnapping");
	ui->screenSnapping->setChecked(screenSnapping);

	bool centerSnapping = config_get_bool(App()->GetUserConfig(), "BasicWindow", "CenterSnapping");
	ui->centerSnapping->setChecked(centerSnapping);

	bool sourceSnapping = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SourceSnapping");
	ui->sourceSnapping->setChecked(sourceSnapping);

	double snapDistance = config_get_double(App()->GetUserConfig(), "BasicWindow", "SnapDistance");
	ui->snapDistance->setValue(snapDistance);

	bool warnBeforeStreamStart = config_get_bool(App()->GetUserConfig(), "BasicWindow", "WarnBeforeStartingStream");
	ui->warnBeforeStreamStart->setChecked(warnBeforeStreamStart);

	bool spacingHelpersEnabled = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SpacingHelpersEnabled");
	ui->previewSpacingHelpers->setChecked(spacingHelpersEnabled);

	bool warnBeforeStreamStop = config_get_bool(App()->GetUserConfig(), "BasicWindow", "WarnBeforeStoppingStream");
	ui->warnBeforeStreamStop->setChecked(warnBeforeStreamStop);

	bool warnBeforeRecordStop = config_get_bool(App()->GetUserConfig(), "BasicWindow", "WarnBeforeStoppingRecord");
	ui->warnBeforeRecordStop->setChecked(warnBeforeRecordStop);

	bool hideProjectorCursor = config_get_bool(App()->GetUserConfig(), "BasicWindow", "HideProjectorCursor");
	ui->hideProjectorCursor->setChecked(hideProjectorCursor);

	bool projectorAlwaysOnTop = config_get_bool(App()->GetUserConfig(), "BasicWindow", "ProjectorAlwaysOnTop");
	ui->projectorAlwaysOnTop->setChecked(projectorAlwaysOnTop);

	bool overflowHide = config_get_bool(App()->GetUserConfig(), "BasicWindow", "OverflowHidden");
	ui->overflowHide->setChecked(overflowHide);

	bool overflowAlwaysVisible = config_get_bool(App()->GetUserConfig(), "BasicWindow", "OverflowAlwaysVisible");
	ui->overflowAlwaysVisible->setChecked(overflowAlwaysVisible);

	bool overflowSelectionHide = config_get_bool(App()->GetUserConfig(), "BasicWindow", "OverflowSelectionHidden");
	ui->overflowSelectionHide->setChecked(overflowSelectionHide);

	bool safeAreas = config_get_bool(App()->GetUserConfig(), "BasicWindow", "ShowSafeAreas");
	ui->previewSafeAreas->setChecked(safeAreas);

	bool automaticSearch = config_get_bool(App()->GetUserConfig(), "General", "AutomaticCollectionSearch");
	ui->automaticSearch->setChecked(automaticSearch);

	bool doubleClickSwitch = config_get_bool(App()->GetUserConfig(), "BasicWindow", "TransitionOnDoubleClick");
	ui->doubleClickSwitch->setChecked(doubleClickSwitch);

	bool studioPortraitLayout = config_get_bool(App()->GetUserConfig(), "BasicWindow", "StudioPortraitLayout");
	ui->studioPortraitLayout->setChecked(studioPortraitLayout);

	bool prevProgLabels = config_get_bool(App()->GetUserConfig(), "BasicWindow", "StudioModeLabels");
	ui->prevProgLabelToggle->setChecked(prevProgLabels);

	bool multiviewMouseSwitch = config_get_bool(App()->GetUserConfig(), "BasicWindow", "MultiviewMouseSwitch");
	ui->multiviewMouseSwitch->setChecked(multiviewMouseSwitch);

	bool multiviewDrawNames = config_get_bool(App()->GetUserConfig(), "BasicWindow", "MultiviewDrawNames");
	ui->multiviewDrawNames->setChecked(multiviewDrawNames);

	bool multiviewDrawAreas = config_get_bool(App()->GetUserConfig(), "BasicWindow", "MultiviewDrawAreas");
	ui->multiviewDrawAreas->setChecked(multiviewDrawAreas);

	ui->multiviewLayout->addItem(QTStr("Basic.Settings.General.MultiviewLayout.Horizontal.Top"),
				     static_cast<int>(MultiviewLayout::HORIZONTAL_TOP_8_SCENES));
	ui->multiviewLayout->addItem(QTStr("Basic.Settings.General.MultiviewLayout.Horizontal.Bottom"),
				     static_cast<int>(MultiviewLayout::HORIZONTAL_BOTTOM_8_SCENES));
	ui->multiviewLayout->addItem(QTStr("Basic.Settings.General.MultiviewLayout.Vertical.Left"),
				     static_cast<int>(MultiviewLayout::VERTICAL_LEFT_8_SCENES));
	ui->multiviewLayout->addItem(QTStr("Basic.Settings.General.MultiviewLayout.Vertical.Right"),
				     static_cast<int>(MultiviewLayout::VERTICAL_RIGHT_8_SCENES));
	ui->multiviewLayout->addItem(QTStr("Basic.Settings.General.MultiviewLayout.Horizontal.18Scene.Top"),
				     static_cast<int>(MultiviewLayout::HORIZONTAL_TOP_18_SCENES));
	ui->multiviewLayout->addItem(QTStr("Basic.Settings.General.MultiviewLayout.Horizontal.Extended.Top"),
				     static_cast<int>(MultiviewLayout::HORIZONTAL_TOP_24_SCENES));
	ui->multiviewLayout->addItem(QTStr("Basic.Settings.General.MultiviewLayout.4Scene"),
				     static_cast<int>(MultiviewLayout::SCENES_ONLY_4_SCENES));
	ui->multiviewLayout->addItem(QTStr("Basic.Settings.General.MultiviewLayout.9Scene"),
				     static_cast<int>(MultiviewLayout::SCENES_ONLY_9_SCENES));
	ui->multiviewLayout->addItem(QTStr("Basic.Settings.General.MultiviewLayout.16Scene"),
				     static_cast<int>(MultiviewLayout::SCENES_ONLY_16_SCENES));
	ui->multiviewLayout->addItem(QTStr("Basic.Settings.General.MultiviewLayout.25Scene"),
				     static_cast<int>(MultiviewLayout::SCENES_ONLY_25_SCENES));

	ui->multiviewLayout->setCurrentIndex(ui->multiviewLayout->findData(
		QVariant::fromValue(config_get_int(App()->GetUserConfig(), "BasicWindow", "MultiviewLayout"))));

	prevLangIndex = ui->language->currentIndex();

	if (obs_video_active()) {
		ui->language->setEnabled(false);
	}

	loading = false;
}

void OBSBasicSettings::LoadRendererList()
{
#if defined(_WIN32) || (defined(__APPLE__) && defined(__aarch64__))
	const char *renderer = config_get_string(App()->GetAppConfig(), "Video", "Renderer");
#ifdef _WIN32
	ui->renderer->addItem(QString("Direct3D 11"), QString("Direct3D 11"));
	if (opt_allow_opengl || strcmp(renderer, "OpenGL") == 0) {
		ui->renderer->addItem(QString("OpenGL"), QString("OpenGL"));
	}
#else
	ui->renderer->addItem(QString("OpenGL"), QString("OpenGL"));
	ui->renderer->addItem(QTStr("Basic.Settings.Video.Renderer.Experimental").arg("Metal"), QString("Metal"));
#endif
	int index = ui->renderer->findData(QString(renderer));
	if (index == -1) {
		index = 0;
	}

	// the video adapter selection is not currently implemented, hide for now
	// to avoid user confusion. was previously protected by
	// if (strcmp(renderer, "OpenGL") == 0)
	delete ui->adapter;
	delete ui->adapterLabel;
	ui->adapter = nullptr;
	ui->adapterLabel = nullptr;

	ui->renderer->setCurrentIndex(index);
#endif
}

static inline bool IsSurround(const char *speakers)
{
	static const char *surroundLayouts[] = {"2.1", "4.0", "4.1", "5.1", "7.1", nullptr};

	if (!speakers || !*speakers) {
		return false;
	}

	const char **curLayout = surroundLayouts;
	for (; *curLayout; ++curLayout) {
		if (strcmp(*curLayout, speakers) == 0) {
			return true;
		}
	}

	return false;
}

static inline void LoadListValue(QComboBox *widget, const char *text, const char *val)
{
	widget->addItem(QT_UTF8(text), QT_UTF8(val));
}

void OBSBasicSettings::LoadListValues(QComboBox *widget, obs_property_t *prop, int index)
{
	size_t count = obs_property_list_item_count(prop);

	OBSSourceAutoRelease source = obs_get_output_source(index);
	const char *deviceId = nullptr;
	OBSDataAutoRelease settings = nullptr;

	if (source) {
		settings = obs_source_get_settings(source);
		if (settings) {
			deviceId = obs_data_get_string(settings, "device_id");
		}
	}

	widget->addItem(QTStr("Basic.Settings.Audio.Disabled"), "disabled");

	for (size_t i = 0; i < count; i++) {
		const char *name = obs_property_list_item_name(prop, i);
		const char *val = obs_property_list_item_string(prop, i);
		LoadListValue(widget, name, val);
	}

	if (deviceId) {
		QVariant var(QT_UTF8(deviceId));
		int idx = widget->findData(var);
		if (idx != -1) {
			widget->setCurrentIndex(idx);
		} else {
			widget->insertItem(0,
					   QTStr("Basic.Settings.Audio."
						 "UnknownAudioDevice"),
					   var);
			widget->setCurrentIndex(0);
			HighlightGroupBoxLabel(ui->audioDevicesGroupBox, widget, "errorLabel");
		}
	}
}

void OBSBasicSettings::LoadAudioDevices()
{
	const char *input_id = App()->InputAudioSource();
	const char *output_id = App()->OutputAudioSource();

	OBSProperties input_props = obs_get_source_properties(input_id);
	OBSProperties output_props = obs_get_source_properties(output_id);

	if (input_props) {
		obs_property_t *inputs = obs_properties_get(input_props, "device_id");
		LoadListValues(ui->auxAudioDevice1, inputs, 3);
		LoadListValues(ui->auxAudioDevice2, inputs, 4);
		LoadListValues(ui->auxAudioDevice3, inputs, 5);
		LoadListValues(ui->auxAudioDevice4, inputs, 6);
	}

	if (output_props) {
		obs_property_t *outputs = obs_properties_get(output_props, "device_id");
		LoadListValues(ui->desktopAudioDevice1, outputs, 1);
		LoadListValues(ui->desktopAudioDevice2, outputs, 2);
	}

	if (obs_video_active()) {
		ui->sampleRate->setEnabled(false);
		ui->channelSetup->setEnabled(false);
	}
}

#define NBSP "\xC2\xA0"

void OBSBasicSettings::LoadAudioSources()
{
	if (ui->audioSourceLayout->rowCount() > 0) {
		QLayoutItem *forDeletion = ui->audioSourceLayout->takeAt(0);
		forDeletion->widget()->deleteLater();
		delete forDeletion;
	}
	auto layout = new QFormLayout();
	layout->setVerticalSpacing(15);
	layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	audioSourceSignals.clear();
	audioSources.clear();

	auto widget = new QWidget();
	widget->setLayout(layout);
	ui->audioSourceLayout->addRow(widget);

	const char *enablePtm = Str("Basic.Settings.Audio.EnablePushToMute");
	const char *ptmDelay = Str("Basic.Settings.Audio.PushToMuteDelay");
	const char *enablePtt = Str("Basic.Settings.Audio.EnablePushToTalk");
	const char *pttDelay = Str("Basic.Settings.Audio.PushToTalkDelay");
	auto AddSource = [&](obs_source_t *source) {
		if (!(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)) {
			return true;
		}

		auto form = new QFormLayout();
		form->setVerticalSpacing(0);
		form->setHorizontalSpacing(5);
		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

		auto ptmCB = new SilentUpdateCheckBox();
		ptmCB->setText(enablePtm);
		ptmCB->setChecked(obs_source_push_to_mute_enabled(source));
		form->addRow(ptmCB);

		auto ptmSB = new SilentUpdateSpinBox();
		ptmSB->setSuffix(NBSP "ms");
		ptmSB->setRange(0, INT_MAX);
		ptmSB->setValue(obs_source_get_push_to_mute_delay(source));
		form->addRow(ptmDelay, ptmSB);

		auto pttCB = new SilentUpdateCheckBox();
		pttCB->setText(enablePtt);
		pttCB->setChecked(obs_source_push_to_talk_enabled(source));
		form->addRow(pttCB);

		auto pttSB = new SilentUpdateSpinBox();
		pttSB->setSuffix(NBSP "ms");
		pttSB->setRange(0, INT_MAX);
		pttSB->setValue(obs_source_get_push_to_talk_delay(source));
		form->addRow(pttDelay, pttSB);

		HookWidget(ptmCB, CHECK_CHANGED, AUDIO_CHANGED);
		HookWidget(ptmSB, SCROLL_CHANGED, AUDIO_CHANGED);
		HookWidget(pttCB, CHECK_CHANGED, AUDIO_CHANGED);
		HookWidget(pttSB, SCROLL_CHANGED, AUDIO_CHANGED);

		audioSourceSignals.reserve(audioSourceSignals.size() + 4);

		auto handler = obs_source_get_signal_handler(source);
		audioSourceSignals.emplace_back(
			handler, "push_to_mute_changed",
			[](void *data, calldata_t *param) {
				QMetaObject::invokeMethod(static_cast<QObject *>(data), "setCheckedSilently",
							  Q_ARG(bool, calldata_bool(param, "enabled")));
			},
			ptmCB);
		audioSourceSignals.emplace_back(
			handler, "push_to_mute_delay",
			[](void *data, calldata_t *param) {
				QMetaObject::invokeMethod(static_cast<QObject *>(data), "setValueSilently",
							  Q_ARG(int, calldata_int(param, "delay")));
			},
			ptmSB);
		audioSourceSignals.emplace_back(
			handler, "push_to_talk_changed",
			[](void *data, calldata_t *param) {
				QMetaObject::invokeMethod(static_cast<QObject *>(data), "setCheckedSilently",
							  Q_ARG(bool, calldata_bool(param, "enabled")));
			},
			pttCB);
		audioSourceSignals.emplace_back(
			handler, "push_to_talk_delay",
			[](void *data, calldata_t *param) {
				QMetaObject::invokeMethod(static_cast<QObject *>(data), "setValueSilently",
							  Q_ARG(int, calldata_int(param, "delay")));
			},
			pttSB);

		audioSources.emplace_back(OBSGetWeakRef(source), ptmCB, ptmSB, pttCB, pttSB);

		auto label = new OBSSourceLabel(source);
		TruncateLabel(label, label->text());
		label->setMinimumSize(QSize(170, 0));
		label->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
		connect(label, &OBSSourceLabel::removed, this,
			[this]() { QMetaObject::invokeMethod(this, "ReloadAudioSources"); });
		connect(label, &OBSSourceLabel::destroyed, this,
			[this]() { QMetaObject::invokeMethod(this, "ReloadAudioSources"); });

		layout->addRow(label, form);
		return true;
	};

	using AddSource_t = decltype(AddSource);
	obs_enum_sources(
		[](void *data, obs_source_t *source) {
			auto &AddSource = *static_cast<AddSource_t *>(data);
			if (!obs_source_removed(source)) {
				AddSource(source);
			}
			return true;
		},
		static_cast<void *>(&AddSource));

	if (layout->rowCount() == 0) {
		ui->audioHotkeysGroupBox->hide();
	} else {
		ui->audioHotkeysGroupBox->show();
	}
}

void OBSBasicSettings::LoadAudioSettings()
{
	uint32_t sampleRate = config_get_uint(main->Config(), "Audio", "SampleRate");
	const char *speakers = config_get_string(main->Config(), "Audio", "ChannelSetup");
	double meterDecayRate = config_get_double(main->Config(), "Audio", "MeterDecayRate");
	uint32_t peakMeterTypeIdx = config_get_uint(main->Config(), "Audio", "PeakMeterType");
	bool enableLLAudioBuffering = config_get_bool(App()->GetUserConfig(), "Audio", "LowLatencyAudioBuffering");

	loading = true;

	const char *str;
	if (sampleRate == 48000) {
		str = "48 kHz";
	} else {
		str = "44.1 kHz";
	}

	int sampleRateIdx = ui->sampleRate->findText(str);
	if (sampleRateIdx != -1) {
		ui->sampleRate->setCurrentIndex(sampleRateIdx);
	}

	if (strcmp(speakers, "Mono") == 0) {
		ui->channelSetup->setCurrentIndex(0);
	} else if (strcmp(speakers, "2.1") == 0) {
		ui->channelSetup->setCurrentIndex(2);
	} else if (strcmp(speakers, "4.0") == 0) {
		ui->channelSetup->setCurrentIndex(3);
	} else if (strcmp(speakers, "4.1") == 0) {
		ui->channelSetup->setCurrentIndex(4);
	} else if (strcmp(speakers, "5.1") == 0) {
		ui->channelSetup->setCurrentIndex(5);
	} else if (strcmp(speakers, "7.1") == 0) {
		ui->channelSetup->setCurrentIndex(6);
	} else {
		ui->channelSetup->setCurrentIndex(1);
	}

	if (meterDecayRate == VOLUME_METER_DECAY_MEDIUM) {
		ui->meterDecayRate->setCurrentIndex(1);
	} else if (meterDecayRate == VOLUME_METER_DECAY_SLOW) {
		ui->meterDecayRate->setCurrentIndex(2);
	} else {
		ui->meterDecayRate->setCurrentIndex(0);
	}

	ui->peakMeterType->setCurrentIndex(peakMeterTypeIdx);
	ui->lowLatencyBuffering->setChecked(enableLLAudioBuffering);

	LoadAudioDevices();
	LoadAudioSources();

	loading = false;
}

void OBSBasicSettings::LoadAdvancedSettings()
{

	QString monDevName;
	QString monDevId;
	if (obs_audio_monitoring_available()) {
		monDevName = config_get_string(main->Config(), "Audio", "MonitoringDeviceName");
		monDevId = config_get_string(main->Config(), "Audio", "MonitoringDeviceId");
	}
	bool enableDelay = config_get_bool(main->Config(), "Output", "DelayEnable");
	int delaySec = config_get_int(main->Config(), "Output", "DelaySec");
	bool preserveDelay = config_get_bool(main->Config(), "Output", "DelayPreserve");
	bool reconnect = config_get_bool(main->Config(), "Output", "Reconnect");
	int retryDelay = config_get_int(main->Config(), "Output", "RetryDelay");
	int maxRetries = config_get_int(main->Config(), "Output", "MaxRetries");
	const char *filename = config_get_string(main->Config(), "Output", "FilenameFormatting");
	bool overwriteIfExists = config_get_bool(main->Config(), "Output", "OverwriteIfExists");
	const char *bindIP = config_get_string(main->Config(), "Output", "BindIP");
	const char *rbPrefix = config_get_string(main->Config(), "SimpleOutput", "RecRBPrefix");
	const char *rbSuffix = config_get_string(main->Config(), "SimpleOutput", "RecRBSuffix");
	bool autoRemux = config_get_bool(main->Config(), "Video", "AutoRemux");
	const char *hotkeyFocusType = config_get_string(App()->GetUserConfig(), "General", "HotkeyFocusType");
	bool dynBitrate = config_get_bool(main->Config(), "Output", "DynamicBitrate");
	const char *ipFamily = config_get_string(main->Config(), "Output", "IPFamily");
	bool confirmOnExit = config_get_bool(App()->GetUserConfig(), "General", "ConfirmOnExit");

	loading = true;

	LoadRendererList();

	if (obs_audio_monitoring_available() && !SetComboByValue(ui->monitoringDevice, monDevId)) {
		SetInvalidValue(ui->monitoringDevice, monDevName, monDevId);
	}

	ui->confirmOnExit->setChecked(confirmOnExit);

	ui->filenameFormatting->setText(filename);
	ui->overwriteIfExists->setChecked(overwriteIfExists);
	ui->simpleRBPrefix->setText(rbPrefix);
	ui->simpleRBSuffix->setText(rbSuffix);

	ui->reconnectEnable->setChecked(reconnect);
	ui->reconnectRetryDelay->setValue(retryDelay);
	ui->reconnectMaxRetries->setValue(maxRetries);

	ui->streamDelaySec->setValue(delaySec);
	ui->streamDelayPreserve->setChecked(preserveDelay);
	ui->streamDelayEnable->setChecked(enableDelay);
	ui->autoRemux->setChecked(autoRemux);
	ui->dynBitrate->setChecked(dynBitrate);

	SetComboByValue(ui->ipFamily, ipFamily);
	if (!SetComboByValue(ui->bindToIP, bindIP)) {
		SetInvalidValue(ui->bindToIP, bindIP, bindIP);
	}

	if (obs_video_active()) {
		ui->advancedVideoContainer->setEnabled(false);
	}

#ifdef __APPLE__
	bool disableOSXVSync = config_get_bool(App()->GetAppConfig(), "Video", "DisableOSXVSync");
	bool resetOSXVSync = config_get_bool(App()->GetAppConfig(), "Video", "ResetOSXVSyncOnExit");
	ui->disableOSXVSync->setChecked(disableOSXVSync);
	ui->resetOSXVSync->setChecked(resetOSXVSync);
	ui->resetOSXVSync->setEnabled(disableOSXVSync);
#elif _WIN32
	bool disableAudioDucking = config_get_bool(App()->GetAppConfig(), "Audio", "DisableAudioDucking");
	ui->disableAudioDucking->setChecked(disableAudioDucking);

	const char *processPriority = config_get_string(App()->GetAppConfig(), "General", "ProcessPriority");
	bool enableNewSocketLoop = config_get_bool(main->Config(), "Output", "NewSocketLoopEnable");
	bool enableLowLatencyMode = config_get_bool(main->Config(), "Output", "LowLatencyEnable");

	int idx = ui->processPriority->findData(processPriority);
	if (idx == -1) {
		idx = ui->processPriority->findData("Normal");
	}
	ui->processPriority->setCurrentIndex(idx);

	ui->enableNewSocketLoop->setChecked(enableNewSocketLoop);
	ui->enableLowLatencyMode->setChecked(enableLowLatencyMode);
	ui->enableLowLatencyMode->setToolTip(QTStr("Basic.Settings.Advanced.Network.TCPPacing.Tooltip"));
#endif
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
	bool browserHWAccel = config_get_bool(App()->GetAppConfig(), "General", "BrowserHWAccel");
	ui->browserHWAccel->setChecked(browserHWAccel);
	prevBrowserAccel = ui->browserHWAccel->isChecked();
#endif

	SetComboByValue(ui->hotkeyFocusType, hotkeyFocusType);

	loading = false;
}

template<typename Func>
static inline void LayoutHotkey(OBSBasicSettings *settings, obs_hotkey_id id, obs_hotkey_t *key, Func &&fun,
				const map<obs_hotkey_id, vector<obs_key_combination_t>> &keys)
{
	auto *label = new OBSHotkeyLabel;
	QString text = QT_UTF8(obs_hotkey_get_description(key));

	label->setProperty("fullName", text);
	TruncateLabel(label, text);

	OBSHotkeyWidget *hw = nullptr;

	auto combos = keys.find(id);
	if (combos == std::end(keys)) {
		hw = new OBSHotkeyWidget(settings, id, obs_hotkey_get_name(key), settings);
	} else {
		hw = new OBSHotkeyWidget(settings, id, obs_hotkey_get_name(key), settings, combos->second);
	}

	hw->label = label;
	hw->setAccessibleName(text);
	label->widget = hw;

	fun(key, label, hw);
}

template<typename Func, typename T> static QLabel *makeLabel(T &t, Func &&getName)
{
	QLabel *label = new QLabel(getName(t));
	label->setStyleSheet("font-weight: bold;");
	return label;
}

template<typename Func> static QLabel *makeLabel(const OBSSource &source, Func &&)
{
	OBSSourceLabel *label = new OBSSourceLabel(source);
	label->setStyleSheet("font-weight: bold;");
	QString name = QT_UTF8(obs_source_get_name(source));
	TruncateLabel(label, name);

	return label;
}

template<typename Func, typename T>
static inline void AddHotkeys(QFormLayout &layout, Func &&getName,
			      std::vector<std::tuple<T, QPointer<QLabel>, QPointer<QWidget>>> &hotkeys)
{
	if (hotkeys.empty()) {
		return;
	}

	layout.setItem(layout.rowCount(), QFormLayout::SpanningRole, new QSpacerItem(0, 10));

	using tuple_type = std::tuple<T, QPointer<QLabel>, QPointer<QWidget>>;

	stable_sort(begin(hotkeys), end(hotkeys), [&](const tuple_type &a, const tuple_type &b) {
		const auto &o_a = get<0>(a);
		const auto &o_b = get<0>(b);
		return o_a != o_b && string(getName(o_a)) < getName(o_b);
	});

	string prevName;
	for (const auto &hotkey : hotkeys) {
		const auto &o = get<0>(hotkey);
		const char *name = getName(o);
		if (prevName != name) {
			prevName = name;
			layout.setItem(layout.rowCount(), QFormLayout::SpanningRole, new QSpacerItem(0, 10));
			layout.addRow(makeLabel(o, getName));
		}

		auto hlabel = get<1>(hotkey);
		auto widget = get<2>(hotkey);
		layout.addRow(hlabel, widget);
	}
}

void OBSBasicSettings::LoadHotkeySettings(obs_hotkey_id ignoreKey)
{
	hotkeys.clear();
	if (ui->hotkeyFormLayout->rowCount() > 0) {
		QLayoutItem *forDeletion = ui->hotkeyFormLayout->takeAt(0);
		forDeletion->widget()->deleteLater();
		delete forDeletion;
	}
	ui->hotkeyFilterSearch->blockSignals(true);
	ui->hotkeyFilterInput->blockSignals(true);
	ui->hotkeyFilterSearch->setText("");
	ui->hotkeyFilterInput->ResetKey();
	ui->hotkeyFilterSearch->blockSignals(false);
	ui->hotkeyFilterInput->blockSignals(false);

	using keys_t = map<obs_hotkey_id, vector<obs_key_combination_t>>;
	keys_t keys;
	obs_enum_hotkey_bindings(
		[](void *data, size_t, obs_hotkey_binding_t *binding) {
			auto &keys = *static_cast<keys_t *>(data);

			keys[obs_hotkey_binding_get_hotkey_id(binding)].emplace_back(
				obs_hotkey_binding_get_key_combination(binding));

			return true;
		},
		&keys);

	QFormLayout *hotkeysLayout = new QFormLayout();
	hotkeysLayout->setVerticalSpacing(0);
	hotkeysLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	hotkeysLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
	auto hotkeyChildWidget = new QWidget();
	hotkeyChildWidget->setLayout(hotkeysLayout);
	ui->hotkeyFormLayout->addRow(hotkeyChildWidget);

	using namespace std;
	using encoders_elem_t = tuple<OBSEncoder, QPointer<QLabel>, QPointer<QWidget>>;
	using outputs_elem_t = tuple<OBSOutput, QPointer<QLabel>, QPointer<QWidget>>;
	using services_elem_t = tuple<OBSService, QPointer<QLabel>, QPointer<QWidget>>;
	using sources_elem_t = tuple<OBSSource, QPointer<QLabel>, QPointer<QWidget>>;
	vector<encoders_elem_t> encoders;
	vector<outputs_elem_t> outputs;
	vector<services_elem_t> services;
	vector<sources_elem_t> scenes;
	vector<sources_elem_t> sources;

	vector<obs_hotkey_id> pairIds;
	map<obs_hotkey_id, pair<obs_hotkey_id, OBSHotkeyLabel *>> pairLabels;

	using std::move;

	auto HandleEncoder = [&](void *registerer, OBSHotkeyLabel *label, OBSHotkeyWidget *hw) {
		auto weak_encoder = static_cast<obs_weak_encoder_t *>(registerer);
		auto encoder = OBSGetStrongRef(weak_encoder);

		if (!encoder) {
			return true;
		}

		encoders.emplace_back(std::move(encoder), label, hw);
		return false;
	};

	auto HandleOutput = [&](void *registerer, OBSHotkeyLabel *label, OBSHotkeyWidget *hw) {
		auto weak_output = static_cast<obs_weak_output_t *>(registerer);
		auto output = OBSGetStrongRef(weak_output);

		if (!output) {
			return true;
		}

		outputs.emplace_back(std::move(output), label, hw);
		return false;
	};

	auto HandleService = [&](void *registerer, OBSHotkeyLabel *label, OBSHotkeyWidget *hw) {
		auto weak_service = static_cast<obs_weak_service_t *>(registerer);
		auto service = OBSGetStrongRef(weak_service);

		if (!service) {
			return true;
		}

		services.emplace_back(std::move(service), label, hw);
		return false;
	};

	auto HandleSource = [&](void *registerer, OBSHotkeyLabel *label, OBSHotkeyWidget *hw) {
		auto weak_source = static_cast<obs_weak_source_t *>(registerer);
		auto source = OBSGetStrongRef(weak_source);

		if (!source) {
			return true;
		}

		if (obs_scene_from_source(source)) {
			scenes.emplace_back(source, label, hw);
		} else if (obs_source_get_name(source) != NULL) {
			sources.emplace_back(source, label, hw);
		}

		return false;
	};

	auto RegisterHotkey = [&](obs_hotkey_t *key, OBSHotkeyLabel *label, OBSHotkeyWidget *hw) {
		auto registerer_type = obs_hotkey_get_registerer_type(key);
		void *registerer = obs_hotkey_get_registerer(key);

		obs_hotkey_id partner = obs_hotkey_get_pair_partner_id(key);
		if (partner != OBS_INVALID_HOTKEY_ID) {
			pairLabels.emplace(obs_hotkey_get_id(key), make_pair(partner, label));
			pairIds.push_back(obs_hotkey_get_id(key));
		}

		using std::move;

		switch (registerer_type) {
		case OBS_HOTKEY_REGISTERER_FRONTEND:
			hotkeysLayout->addRow(label, hw);
			break;

		case OBS_HOTKEY_REGISTERER_ENCODER:
			if (HandleEncoder(registerer, label, hw)) {
				return;
			}
			break;

		case OBS_HOTKEY_REGISTERER_OUTPUT:
			if (HandleOutput(registerer, label, hw)) {
				return;
			}
			break;

		case OBS_HOTKEY_REGISTERER_SERVICE:
			if (HandleService(registerer, label, hw)) {
				return;
			}
			break;

		case OBS_HOTKEY_REGISTERER_SOURCE:
			if (HandleSource(registerer, label, hw)) {
				return;
			}
			break;
		}

		hotkeys.emplace_back(registerer_type == OBS_HOTKEY_REGISTERER_FRONTEND, hw);
		connect(hw, &OBSHotkeyWidget::KeyChanged, this, [this, hotkeysLayout]() {
			HotkeysChanged();
			ScanDuplicateHotkeys(hotkeysLayout);
		});
		connect(hw, &OBSHotkeyWidget::SearchKey, this, [this](obs_key_combination_t combo) {
			ui->hotkeyFilterSearch->setText("");
			ui->hotkeyFilterInput->HandleNewKey(combo);
			ui->hotkeyFilterInput->KeyChanged(combo);
		});
	};

	auto data = make_tuple(RegisterHotkey, std::move(keys), ignoreKey, this);
	using data_t = decltype(data);
	obs_enum_hotkeys(
		[](void *data, obs_hotkey_id id, obs_hotkey_t *key) {
			data_t &d = *static_cast<data_t *>(data);
			if (id != get<2>(d)) {
				LayoutHotkey(get<3>(d), id, key, get<0>(d), get<1>(d));
			}
			return true;
		},
		&data);

	for (auto keyId : pairIds) {
		auto data1 = pairLabels.find(keyId);
		if (data1 == end(pairLabels)) {
			continue;
		}

		auto &label1 = data1->second.second;
		if (label1->pairPartner) {
			continue;
		}

		auto data2 = pairLabels.find(data1->second.first);
		if (data2 == end(pairLabels)) {
			continue;
		}

		auto &label2 = data2->second.second;
		if (label2->pairPartner) {
			continue;
		}

		QString tt = QTStr("Basic.Settings.Hotkeys.Pair");
		auto name1 = label1->text();
		auto name2 = label2->text();

		auto Update = [&](OBSHotkeyLabel *label, const QString &name, OBSHotkeyLabel *other,
				  const QString &otherName) {
			QString string = other->property("fullName").value<QString>();

			if (string.isEmpty() || string.isNull()) {
				string = otherName;
			}

			label->setToolTip(tt.arg(string));
			label->setText(name + " *");
			label->pairPartner = other;
		};
		Update(label1, name1, label2, name2);
		Update(label2, name2, label1, name1);
	}

	AddHotkeys(*hotkeysLayout, obs_output_get_name, outputs);
	AddHotkeys(*hotkeysLayout, obs_source_get_name, scenes);
	AddHotkeys(*hotkeysLayout, obs_source_get_name, sources);
	AddHotkeys(*hotkeysLayout, obs_encoder_get_name, encoders);
	AddHotkeys(*hotkeysLayout, obs_service_get_name, services);

	ScanDuplicateHotkeys(hotkeysLayout);

	/* After this function returns the UI can still be unresponsive for a bit.
	 * So by deferring the call to unsetCursor() to the Qt event loop it will
	 * take until it has actually finished processing the created widgets
	 * before the cursor is reset. */
	QTimer::singleShot(1, this, &OBSBasicSettings::unsetCursor);
	hotkeysLoaded = true;
}

void OBSBasicSettings::LoadSettings(bool changedOnly)
{
	if (!changedOnly || generalChanged) {
		LoadGeneralSettings();
	}
	if (!changedOnly || stream1Changed) {
		LoadStream1Settings();
	}
	if (!changedOnly || audioChanged) {
		LoadAudioSettings();
	}
	if (!changedOnly || a11yChanged) {
		LoadA11ySettings();
	}
	if (!changedOnly || appearanceChanged) {
		LoadAppearanceSettings();
	}
	if (!changedOnly || advancedChanged) {
		LoadAdvancedSettings();
	}
	if (!changedOnly) {
		LoadCanvasSettings();
	}
}

void OBSBasicSettings::SaveGeneralSettings()
{
	int languageIndex = ui->language->currentIndex();
	QVariant langData = ui->language->itemData(languageIndex);
	string language = langData.toString().toStdString();

	if (WidgetChanged(ui->language)) {
		config_set_string(App()->GetUserConfig(), "General", "Language", language.c_str());
	}

#if defined(_WIN32) || defined(ENABLE_SPARKLE_UPDATER)
	if (WidgetChanged(ui->enableAutoUpdates)) {
		config_set_bool(App()->GetAppConfig(), "General", "EnableAutoUpdates",
				ui->enableAutoUpdates->isChecked());
	}
	int branchIdx = ui->updateChannelBox->currentIndex();
	QString branchName = ui->updateChannelBox->itemData(branchIdx).toString();

	if (WidgetChanged(ui->updateChannelBox)) {
		config_set_string(App()->GetAppConfig(), "General", "UpdateBranch", QT_TO_UTF8(branchName));
		forceUpdateCheck = true;
	}
#endif
#ifdef _WIN32
	if (ui->hideOBSFromCapture && WidgetChanged(ui->hideOBSFromCapture)) {
		bool hide_window = ui->hideOBSFromCapture->isChecked();
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "HideOBSWindowsFromCapture", hide_window);

		QWindowList windows = QGuiApplication::allWindows();
		for (auto window : windows) {
			if (window->isVisible()) {
				main->SetDisplayAffinity(window);
			}
		}

		blog(LOG_INFO, "Hide OBS windows from screen capture: %s", hide_window ? "true" : "false");
	}
#endif
	if (WidgetChanged(ui->openStatsOnStartup)) {
		config_set_bool(main->Config(), "General", "OpenStatsOnStartup", ui->openStatsOnStartup->isChecked());
	}
	if (WidgetChanged(ui->snappingEnabled)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "SnappingEnabled",
				ui->snappingEnabled->isChecked());
	}
	if (WidgetChanged(ui->screenSnapping)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "ScreenSnapping",
				ui->screenSnapping->isChecked());
	}
	if (WidgetChanged(ui->centerSnapping)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "CenterSnapping",
				ui->centerSnapping->isChecked());
	}
	if (WidgetChanged(ui->sourceSnapping)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "SourceSnapping",
				ui->sourceSnapping->isChecked());
	}
	if (WidgetChanged(ui->snapDistance)) {
		config_set_double(App()->GetUserConfig(), "BasicWindow", "SnapDistance", ui->snapDistance->value());
	}
	if (WidgetChanged(ui->overflowAlwaysVisible) || WidgetChanged(ui->overflowHide) ||
	    WidgetChanged(ui->overflowSelectionHide)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "OverflowAlwaysVisible",
				ui->overflowAlwaysVisible->isChecked());
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "OverflowHidden", ui->overflowHide->isChecked());
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "OverflowSelectionHidden",
				ui->overflowSelectionHide->isChecked());
		main->UpdatePreviewOverflowSettings();
	}
	if (WidgetChanged(ui->previewSafeAreas)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "ShowSafeAreas",
				ui->previewSafeAreas->isChecked());
		main->UpdatePreviewSafeAreas();
	}

	if (WidgetChanged(ui->previewSpacingHelpers)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "SpacingHelpersEnabled",
				ui->previewSpacingHelpers->isChecked());
		main->UpdatePreviewSpacingHelpers();
	}

	if (WidgetChanged(ui->doubleClickSwitch)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "TransitionOnDoubleClick",
				ui->doubleClickSwitch->isChecked());
	}
	if (WidgetChanged(ui->automaticSearch)) {
		config_set_bool(App()->GetUserConfig(), "General", "AutomaticCollectionSearch",
				ui->automaticSearch->isChecked());
	}

	config_set_bool(App()->GetUserConfig(), "BasicWindow", "WarnBeforeStartingStream",
			ui->warnBeforeStreamStart->isChecked());
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "WarnBeforeStoppingStream",
			ui->warnBeforeStreamStop->isChecked());
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "WarnBeforeStoppingRecord",
			ui->warnBeforeRecordStop->isChecked());

	if (WidgetChanged(ui->hideProjectorCursor)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "HideProjectorCursor",
				ui->hideProjectorCursor->isChecked());
		main->UpdateProjectorHideCursor();
	}

	if (WidgetChanged(ui->projectorAlwaysOnTop)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "ProjectorAlwaysOnTop",
				ui->projectorAlwaysOnTop->isChecked());
#if defined(_WIN32) || defined(__APPLE__)
		main->UpdateProjectorAlwaysOnTop(ui->projectorAlwaysOnTop->isChecked());
#else
		main->ResetProjectors();
#endif
	}

	if (WidgetChanged(ui->recordWhenStreaming)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "RecordWhenStreaming",
				ui->recordWhenStreaming->isChecked());
	}
	if (WidgetChanged(ui->keepRecordStreamStops)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "KeepRecordingWhenStreamStops",
				ui->keepRecordStreamStops->isChecked());
	}

	if (WidgetChanged(ui->replayWhileStreaming)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "ReplayBufferWhileStreaming",
				ui->replayWhileStreaming->isChecked());
	}
	if (WidgetChanged(ui->keepReplayStreamStops)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "KeepReplayBufferStreamStops",
				ui->keepReplayStreamStops->isChecked());
	}

	if (WidgetChanged(ui->systemTrayEnabled)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "SysTrayEnabled",
				ui->systemTrayEnabled->isChecked());

		main->SystemTray(false);
	}

	if (WidgetChanged(ui->systemTrayWhenStarted)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "SysTrayWhenStarted",
				ui->systemTrayWhenStarted->isChecked());
	}

	if (WidgetChanged(ui->systemTrayAlways)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "SysTrayMinimizeToTray",
				ui->systemTrayAlways->isChecked());
	}

	if (WidgetChanged(ui->saveProjectors)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "SaveProjectors",
				ui->saveProjectors->isChecked());
	}

	if (WidgetChanged(ui->closeProjectors)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "CloseExistingProjectors",
				ui->closeProjectors->isChecked());
	}

	if (WidgetChanged(ui->studioPortraitLayout)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "StudioPortraitLayout",
				ui->studioPortraitLayout->isChecked());

		main->ResetUI();
	}

	if (WidgetChanged(ui->prevProgLabelToggle)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "StudioModeLabels",
				ui->prevProgLabelToggle->isChecked());

		main->ResetUI();
	}

	bool multiviewChanged = false;
	if (WidgetChanged(ui->multiviewMouseSwitch)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "MultiviewMouseSwitch",
				ui->multiviewMouseSwitch->isChecked());
		multiviewChanged = true;
	}

	if (WidgetChanged(ui->multiviewDrawNames)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "MultiviewDrawNames",
				ui->multiviewDrawNames->isChecked());
		multiviewChanged = true;
	}

	if (WidgetChanged(ui->multiviewDrawAreas)) {
		config_set_bool(App()->GetUserConfig(), "BasicWindow", "MultiviewDrawAreas",
				ui->multiviewDrawAreas->isChecked());
		multiviewChanged = true;
	}

	if (WidgetChanged(ui->multiviewLayout)) {
		config_set_int(App()->GetUserConfig(), "BasicWindow", "MultiviewLayout",
			       ui->multiviewLayout->currentData().toInt());
		multiviewChanged = true;
	}

	if (multiviewChanged) {
		OBSProjector::UpdateMultiviewProjectors();
	}
}

void OBSBasicSettings::SaveAdvancedSettings()
{
	QString lastMonitoringDevice = config_get_string(main->Config(), "Audio", "MonitoringDeviceId");

#if defined(_WIN32) || (defined(__APPLE__) && defined(__aarch64__))
	if (WidgetChanged(ui->renderer)) {
		config_set_string(App()->GetAppConfig(), "Video", "Renderer",
				  QT_TO_UTF8(ui->renderer->currentData().toString()));
	}
#endif

#ifdef _WIN32
	std::string priority = ui->processPriority->currentData().toString().toStdString();
	config_set_string(App()->GetAppConfig(), "General", "ProcessPriority", priority.c_str());
	if (main->Active()) {
		SetProcessPriority(priority.c_str());
	}

	SaveCheckBox(ui->enableNewSocketLoop, "Output", "NewSocketLoopEnable");
	SaveCheckBox(ui->enableLowLatencyMode, "Output", "LowLatencyEnable");
#endif
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
	bool browserHWAccel = ui->browserHWAccel->isChecked();
	config_set_bool(App()->GetAppConfig(), "General", "BrowserHWAccel", browserHWAccel);
#endif

	if (WidgetChanged(ui->hotkeyFocusType)) {
		QString str = GetComboData(ui->hotkeyFocusType);
		config_set_string(App()->GetUserConfig(), "General", "HotkeyFocusType", QT_TO_UTF8(str));
	}

#ifdef __APPLE__
	if (WidgetChanged(ui->disableOSXVSync)) {
		bool disable = ui->disableOSXVSync->isChecked();
		config_set_bool(App()->GetAppConfig(), "Video", "DisableOSXVSync", disable);
		EnableOSXVSync(!disable);
	}
	if (WidgetChanged(ui->resetOSXVSync)) {
		config_set_bool(App()->GetAppConfig(), "Video", "ResetOSXVSyncOnExit", ui->resetOSXVSync->isChecked());
	}
#endif

	if (obs_audio_monitoring_available()) {
		SaveCombo(ui->monitoringDevice, "Audio", "MonitoringDeviceName");
		SaveComboData(ui->monitoringDevice, "Audio", "MonitoringDeviceId");
	}

#ifdef _WIN32
	if (WidgetChanged(ui->disableAudioDucking)) {
		bool disable = ui->disableAudioDucking->isChecked();
		config_set_bool(App()->GetAppConfig(), "Audio", "DisableAudioDucking", disable);
		DisableAudioDucking(disable);
	}
#endif

	if (WidgetChanged(ui->confirmOnExit)) {
		config_set_bool(App()->GetUserConfig(), "General", "ConfirmOnExit", ui->confirmOnExit->isChecked());
	}

	SaveEdit(ui->filenameFormatting, "Output", "FilenameFormatting");
	SaveEdit(ui->simpleRBPrefix, "SimpleOutput", "RecRBPrefix");
	SaveEdit(ui->simpleRBSuffix, "SimpleOutput", "RecRBSuffix");
	SaveCheckBox(ui->overwriteIfExists, "Output", "OverwriteIfExists");
	SaveCheckBox(ui->streamDelayEnable, "Output", "DelayEnable");
	SaveSpinBox(ui->streamDelaySec, "Output", "DelaySec");
	SaveCheckBox(ui->streamDelayPreserve, "Output", "DelayPreserve");
	SaveCheckBox(ui->reconnectEnable, "Output", "Reconnect");
	SaveSpinBox(ui->reconnectRetryDelay, "Output", "RetryDelay");
	SaveSpinBox(ui->reconnectMaxRetries, "Output", "MaxRetries");
	SaveComboData(ui->bindToIP, "Output", "BindIP");
	SaveComboData(ui->ipFamily, "Output", "IPFamily");
	SaveCheckBox(ui->autoRemux, "Video", "AutoRemux");
	SaveCheckBox(ui->dynBitrate, "Output", "DynamicBitrate");

	if (obs_audio_monitoring_available()) {
		QString newDevice = ui->monitoringDevice->currentData().toString();

		if (lastMonitoringDevice != newDevice) {
			obs_set_audio_monitoring_device(QT_TO_UTF8(ui->monitoringDevice->currentText()),
							QT_TO_UTF8(newDevice));

			blog(LOG_INFO, "Audio monitoring device:\n\tname: %s\n\tid: %s",
			     QT_TO_UTF8(ui->monitoringDevice->currentText()), QT_TO_UTF8(newDevice));
		}
	}
}

void OBSBasicSettings::SaveAudioSettings()
{
	QString sampleRateStr = ui->sampleRate->currentText();
	int channelSetupIdx = ui->channelSetup->currentIndex();

	const char *channelSetup;
	switch (channelSetupIdx) {
	case 0:
		channelSetup = "Mono";
		break;
	case 1:
		channelSetup = "Stereo";
		break;
	case 2:
		channelSetup = "2.1";
		break;
	case 3:
		channelSetup = "4.0";
		break;
	case 4:
		channelSetup = "4.1";
		break;
	case 5:
		channelSetup = "5.1";
		break;
	case 6:
		channelSetup = "7.1";
		break;

	default:
		channelSetup = "Stereo";
		break;
	}

	int sampleRate = 44100;
	if (sampleRateStr == "48 kHz") {
		sampleRate = 48000;
	}

	if (WidgetChanged(ui->sampleRate)) {
		config_set_uint(main->Config(), "Audio", "SampleRate", sampleRate);
	}

	if (WidgetChanged(ui->channelSetup)) {
		config_set_string(main->Config(), "Audio", "ChannelSetup", channelSetup);
	}

	if (WidgetChanged(ui->meterDecayRate)) {
		double meterDecayRate;
		switch (ui->meterDecayRate->currentIndex()) {
		case 0:
			meterDecayRate = VOLUME_METER_DECAY_FAST;
			break;
		case 1:
			meterDecayRate = VOLUME_METER_DECAY_MEDIUM;
			break;
		case 2:
			meterDecayRate = VOLUME_METER_DECAY_SLOW;
			break;
		default:
			meterDecayRate = VOLUME_METER_DECAY_FAST;
			break;
		}
		config_set_double(main->Config(), "Audio", "MeterDecayRate", meterDecayRate);

		emit main->profileSettingChanged("Audio", "MeterDecayRate");
	}

	if (WidgetChanged(ui->peakMeterType)) {
		uint32_t peakMeterTypeIdx = ui->peakMeterType->currentIndex();
		config_set_uint(main->Config(), "Audio", "PeakMeterType", peakMeterTypeIdx);

		emit main->profileSettingChanged("Audio", "PeakMeterType");
	}

	if (WidgetChanged(ui->lowLatencyBuffering)) {
		bool enableLLAudioBuffering = ui->lowLatencyBuffering->isChecked();
		config_set_bool(App()->GetUserConfig(), "Audio", "LowLatencyAudioBuffering", enableLLAudioBuffering);
	}

	for (auto &audioSource : audioSources) {
		auto source = OBSGetStrongRef(get<0>(audioSource));
		if (!source) {
			continue;
		}

		auto &ptmCB = get<1>(audioSource);
		auto &ptmSB = get<2>(audioSource);
		auto &pttCB = get<3>(audioSource);
		auto &pttSB = get<4>(audioSource);

		obs_source_enable_push_to_mute(source, ptmCB->isChecked());
		obs_source_set_push_to_mute_delay(source, ptmSB->value());

		obs_source_enable_push_to_talk(source, pttCB->isChecked());
		obs_source_set_push_to_talk_delay(source, pttSB->value());
	}

	auto UpdateAudioDevice = [this](bool input, QComboBox *combo, const char *name, int index) {
		main->ResetAudioDevice(input ? App()->InputAudioSource() : App()->OutputAudioSource(),
				       QT_TO_UTF8(GetComboData(combo)), Str(name), index);
	};

	UpdateAudioDevice(false, ui->desktopAudioDevice1, "Basic.DesktopDevice1", 1);
	UpdateAudioDevice(false, ui->desktopAudioDevice2, "Basic.DesktopDevice2", 2);
	UpdateAudioDevice(true, ui->auxAudioDevice1, "Basic.AuxDevice1", 3);
	UpdateAudioDevice(true, ui->auxAudioDevice2, "Basic.AuxDevice2", 4);
	UpdateAudioDevice(true, ui->auxAudioDevice3, "Basic.AuxDevice3", 5);
	UpdateAudioDevice(true, ui->auxAudioDevice4, "Basic.AuxDevice4", 6);
	main->SaveProject();
}

void OBSBasicSettings::SaveHotkeySettings()
{
	const auto &config = main->Config();

	using namespace std;

	std::vector<obs_key_combination> combinations;
	for (auto &hotkey : hotkeys) {
		auto &hw = *hotkey.second;
		if (!hw.Changed()) {
			continue;
		}

		hw.Save(combinations);

		if (!hotkey.first) {
			continue;
		}

		OBSDataArrayAutoRelease array = obs_hotkey_save(hw.id);
		OBSDataAutoRelease data = obs_data_create();
		obs_data_set_array(data, "bindings", array);
		const char *json = obs_data_get_json(data);
		config_set_string(config, "Hotkeys", hw.name.c_str(), json);
	}

	if (!main->outputHandler || !main->outputHandler->replayBuffer) {
		return;
	}

	const char *id = obs_obj_get_id(main->outputHandler->replayBuffer);
	if (strcmp(id, "replay_buffer") == 0) {
		OBSDataAutoRelease hotkeys = obs_hotkeys_save_output(main->outputHandler->replayBuffer);
		config_set_string(config, "Hotkeys", "ReplayBuffer", obs_data_get_json(hotkeys));
	}
}

#define MINOR_SEPARATOR "------------------------------------------------"

static void AddChangedVal(std::string &changed, const char *str)
{
	if (changed.size()) {
		changed += ", ";
	}
	changed += str;
}

void OBSBasicSettings::SaveSettings()
{
	if (generalChanged) {
		SaveGeneralSettings();
	}
	if (stream1Changed) {
		SaveStream1Settings();
	}
	if (audioChanged) {
		SaveAudioSettings();
	}
	if (hotkeysChanged) {
		SaveHotkeySettings();
	}
	if (a11yChanged) {
		SaveA11ySettings();
	}
	if (advancedChanged) {
		SaveAdvancedSettings();
	}
	if (appearanceChanged) {
		SaveAppearanceSettings();
	}
	if (advancedChanged) {
		main->ResetVideo();
	}

	config_save_safe(main->Config(), "tmp", nullptr);
	config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
	main->SaveProject();

	if (Changed()) {
		std::string changed;
		if (generalChanged) {
			AddChangedVal(changed, "general");
		}
		if (stream1Changed) {
			AddChangedVal(changed, "stream 1");
		}
		if (audioChanged) {
			AddChangedVal(changed, "audio");
		}
		if (hotkeysChanged) {
			AddChangedVal(changed, "hotkeys");
		}
		if (a11yChanged) {
			AddChangedVal(changed, "a11y");
		}
		if (appearanceChanged) {
			AddChangedVal(changed, "appearance");
		}
		if (advancedChanged) {
			AddChangedVal(changed, "advanced");
		}

		blog(LOG_INFO, "Settings changed (%s)", changed.c_str());
		blog(LOG_INFO, MINOR_SEPARATOR);
	}

	bool langChanged = (ui->language->currentIndex() != prevLangIndex);
	bool audioRestart =
		(ui->channelSetup->currentIndex() != channelIndex || ui->sampleRate->currentIndex() != sampleRateIndex);
	bool browserHWAccelChanged = (ui->browserHWAccel && ui->browserHWAccel->isChecked() != prevBrowserAccel);

	if (langChanged || audioRestart || browserHWAccelChanged) {
		restart = true;
	} else {
		restart = false;
	}
}

bool OBSBasicSettings::QueryChanges()
{
	QMessageBox::StandardButton button;

	button = OBSMessageBox::question(this, QTStr("Basic.Settings.ConfirmTitle"), QTStr("Basic.Settings.Confirm"),
					 QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);

	if (button == QMessageBox::Cancel) {
		return false;
	} else if (button == QMessageBox::Yes) {
		if (!QueryAllowedToClose()) {
			return false;
		}

		SaveSettings();
	} else {
		if (savedTheme != App()->GetTheme()) {
			App()->SetTheme(savedTheme->id);
		}

		LoadSettings(true);
		restart = false;
	}

	ClearChanged();
	return true;
}

bool OBSBasicSettings::QueryAllowedToClose()
{
	return true;
}

void OBSBasicSettings::closeEvent(QCloseEvent *event)
{
	if (!AskIfCanCloseSettings()) {
		event->ignore();
	}
}

void OBSBasicSettings::showEvent(QShowEvent *event)
{
	QDialog::showEvent(event);

	/* Reduce the height of the widget area if too tall compared to the screen
	 * size (e.g., 720p) with potential window decoration (e.g., titlebar). */
	const int titleBarHeight = QApplication::style()->pixelMetric(QStyle::PM_TitleBarHeight);
	const int maxHeight = round(screen()->availableGeometry().height() - titleBarHeight);
	if (size().height() >= maxHeight) {
		resize(size().width(), maxHeight);
	}
}

void OBSBasicSettings::reject()
{
	if (AskIfCanCloseSettings()) {
		/* Stream-profile field edits are accumulated into the manager in memory
		 * as the user switches profiles; only Apply/OK flushes them to disk.
		 * Reload from streams.json so cancelling discards those uncommitted
		 * edits. (Add/Remove are immediate management actions, matching the
		 * canvas pattern, and already persisted to disk.) */
		main->GetStreamProfileManager().Load();
		close();
	}
}

void OBSBasicSettings::on_listWidget_itemSelectionChanged()
{
	int row = ui->listWidget->currentRow();

	if (loading || row == pageIndex) {
		return;
	}

	if (!hotkeysLoaded && row == Pages::HOTKEYS) {
		setCursor(Qt::BusyCursor);
		/* Look, I know this /feels/ wrong, but the specific issue we're dealing with
		 * here means that the UI locks up immediately even when using "invokeMethod".
		 * So the only way for the user to see the loading message on the page is to
		 * give the Qt event loop a tiny bit of time to switch to the hotkey page,
		 * and only then start loading. This could maybe be done by subclassing QWidget
		 * for the hotkey page and then using showEvent() but I *really* don't want
		 * to deal with that right now. I've got better things to do with my life
		 * than to work around this god damn stupid issue for something we'll remove
		 * soon enough anyway. So this solution it is. */
		QTimer::singleShot(1, this, [&]() { LoadHotkeySettings(); });
	}

	pageIndex = row;
}

void OBSBasicSettings::UpdateYouTubeAppDockSettings()
{
#if defined(BROWSER_AVAILABLE) && defined(YOUTUBE_ENABLED)
	if (cef_js_avail) {
		std::string service = ui->service->currentText().toStdString();
		if (IsYouTubeService(service)) {
			if (!main->GetYouTubeAppDock()) {
				main->NewYouTubeAppDock();
			}
			main->GetYouTubeAppDock()->SettingsUpdated(!IsYouTubeService(service) || stream1Changed);
		} else {
			if (main->GetYouTubeAppDock()) {
				main->GetYouTubeAppDock()->AccountDisconnected();
			}
			main->DeleteYouTubeAppDock();
		}
	}
#endif
}

void OBSBasicSettings::on_buttonBox_clicked(QAbstractButton *button)
{
	QDialogButtonBox::ButtonRole val = ui->buttonBox->buttonRole(button);

	if (val == QDialogButtonBox::ApplyRole || val == QDialogButtonBox::AcceptRole) {
		if (!QueryAllowedToClose()) {
			return;
		}

		SaveSettings();

		UpdateYouTubeAppDockSettings();
		ClearChanged();
	}

	if (val == QDialogButtonBox::AcceptRole || val == QDialogButtonBox::RejectRole) {
		if (val == QDialogButtonBox::RejectRole) {
			if (savedTheme != App()->GetTheme()) {
				App()->SetTheme(savedTheme->id);
			}
		}
		ClearChanged();
		close();
	}
}

bool OBSBasicSettings::AskIfCanCloseSettings()
{
	bool canCloseSettings = false;

	if (!Changed() || QueryChanges()) {
		canCloseSettings = true;
	}

	if (forceAuthReload) {
		main->auth->Save();
		main->auth->Load();
		forceAuthReload = false;
	}

	if (forceUpdateCheck) {
		main->CheckForUpdates(false);
		forceUpdateCheck = false;
	}

	return canCloseSettings;
}

void OBSBasicSettings::on_filenameFormatting_textEdited(const QString &text)
{
	QString safeStr = text;

#ifdef __APPLE__
	safeStr.replace(QRegularExpression("[:]"), "");
#elif defined(_WIN32)
	safeStr.replace(QRegularExpression("[<>:\"\\|\\?\\*]"), "");
#else
	// TODO: Add filtering for other platforms
#endif

	if (text != safeStr) {
		ui->filenameFormatting->setText(safeStr);
	}
}

void OBSBasicSettings::GeneralChanged()
{
	if (!loading) {
		generalChanged = true;
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::Stream1Changed()
{
	if (!loading) {
		stream1Changed = true;
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::AudioChanged()
{
	if (!loading) {
		audioChanged = true;
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::AudioChangedRestart()
{
	ui->audioMsg->setVisible(false);

	if (!loading) {
		int currentChannelIndex = ui->channelSetup->currentIndex();
		int currentSampleRateIndex = ui->sampleRate->currentIndex();
		bool currentLLAudioBufVal = ui->lowLatencyBuffering->isChecked();

		if (currentChannelIndex != channelIndex || currentSampleRateIndex != sampleRateIndex ||
		    currentLLAudioBufVal != llBufferingEnabled) {
			ui->audioMsg->setText(QTStr("Basic.Settings.ProgramRestart"));
			ui->audioMsg->setVisible(true);
		} else {
			ui->audioMsg->setText("");
		}

		audioChanged = true;
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::ReloadAudioSources()
{
	LoadAudioSources();
}

#define MULTI_CHANNEL_WARNING "Basic.Settings.Audio.MultichannelWarning"

void OBSBasicSettings::SpeakerLayoutChanged(int)
{
	UpdateAudioWarnings();
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
void OBSBasicSettings::HideOBSWindowWarning(Qt::CheckState state)
#else
void OBSBasicSettings::HideOBSWindowWarning(int state)
#endif
{
	if (loading || state == Qt::Unchecked) {
		return;
	}

	if (config_get_bool(App()->GetUserConfig(), "General", "WarnedAboutHideOBSFromCapture")) {
		return;
	}

	OBSMessageBox::information(this, QTStr("Basic.Settings.General.HideOBSWindowsFromCapture"),
				   QTStr("Basic.Settings.General.HideOBSWindowsFromCapture.Message"));

	config_set_bool(App()->GetUserConfig(), "General", "WarnedAboutHideOBSFromCapture", true);
	config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
}

void OBSBasicSettings::AdvancedChangedRestart()
{
	ui->advancedMsg->setVisible(false);

	if (!loading) {
		advancedChanged = true;
		ui->advancedMsg->setText(QTStr("Basic.Settings.ProgramRestart"));
		ui->advancedMsg->setVisible(true);
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::HotkeysChanged()
{
	using namespace std;
	if (loading) {
		return;
	}

	hotkeysChanged = any_of(begin(hotkeys), end(hotkeys), [](const pair<bool, QPointer<OBSHotkeyWidget>> &hotkey) {
		const auto &hw = *hotkey.second;
		return hw.Changed();
	});

	if (hotkeysChanged) {
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::SearchHotkeys(const QString &text, obs_key_combination_t filterCombo)
{

	if (ui->hotkeyFormLayout->rowCount() == 0) {
		return;
	}

	std::vector<obs_key_combination_t> combos;
	bool showHotkey;
	ui->hotkeyScrollArea->ensureVisible(0, 0);

	QLayoutItem *hotkeysItem = ui->hotkeyFormLayout->itemAt(0);
	QWidget *hotkeys = hotkeysItem->widget();
	if (!hotkeys) {
		return;
	}

	QFormLayout *hotkeysLayout = qobject_cast<QFormLayout *>(hotkeys->layout());
	hotkeysLayout->setEnabled(false);

	QString needle = text.toLower();

	for (int i = 0; i < hotkeysLayout->rowCount(); i++) {
		auto label = hotkeysLayout->itemAt(i, QFormLayout::LabelRole);
		if (!label) {
			continue;
		}

		OBSHotkeyLabel *item = qobject_cast<OBSHotkeyLabel *>(label->widget());
		if (!item) {
			continue;
		}

		QString fullname = item->property("fullName").value<QString>();

		showHotkey = needle.isEmpty() || fullname.toLower().contains(needle);

		if (showHotkey && !obs_key_combination_is_empty(filterCombo)) {
			showHotkey = false;

			item->widget->GetCombinations(combos);
			for (auto combo : combos) {
				if (combo == filterCombo) {
					showHotkey = true;
					break;
				}
			}
		}

		label->widget()->setVisible(showHotkey);

		auto field = hotkeysLayout->itemAt(i, QFormLayout::FieldRole);
		if (field) {
			field->widget()->setVisible(showHotkey);
		}
	}
	hotkeysLayout->setEnabled(true);
}

void OBSBasicSettings::on_hotkeyFilterReset_clicked()
{
	ui->hotkeyFilterSearch->setText("");
	ui->hotkeyFilterInput->ResetKey();
}

void OBSBasicSettings::on_hotkeyFilterSearch_textChanged(const QString text)
{
	SearchHotkeys(text, ui->hotkeyFilterInput->key);
}

void OBSBasicSettings::on_hotkeyFilterInput_KeyChanged(obs_key_combination_t combo)
{
	SearchHotkeys(ui->hotkeyFilterSearch->text(), combo);
}

namespace std {
template<> struct hash<obs_key_combination_t> {
	size_t operator()(obs_key_combination_t value) const
	{
		size_t h1 = hash<uint32_t>{}(value.modifiers);
		size_t h2 = hash<int>{}(value.key);
		// Same as boost::hash_combine()
		h2 ^= h1 + 0x9e3779b9 + (h2 << 6) + (h2 >> 2);
		return h2;
	}
};
} // namespace std

bool OBSBasicSettings::ScanDuplicateHotkeys(QFormLayout *layout)
{
	typedef struct assignment {
		OBSHotkeyLabel *label;
		OBSHotkeyEdit *edit;
	} assignment;

	unordered_map<obs_key_combination_t, vector<assignment>> assignments;
	vector<OBSHotkeyLabel *> items;
	bool hasDupes = false;

	for (int i = 0; i < layout->rowCount(); i++) {
		auto label = layout->itemAt(i, QFormLayout::LabelRole);
		if (!label) {
			continue;
		}
		OBSHotkeyLabel *item = qobject_cast<OBSHotkeyLabel *>(label->widget());
		if (!item) {
			continue;
		}

		items.push_back(item);

		for (auto &edit : item->widget->edits) {
			edit->hasDuplicate = false;

			if (obs_key_combination_is_empty(edit->key)) {
				continue;
			}

			for (assignment &assign : assignments[edit->key]) {
				if (item->pairPartner == assign.label) {
					continue;
				}

				assign.edit->hasDuplicate = true;
				edit->hasDuplicate = true;
				hasDupes = true;
			}

			assignments[edit->key].push_back({item, edit});
		}
	}

	for (auto *item : items) {
		for (auto &edit : item->widget->edits) {
			edit->UpdateDuplicationState();
		}
	}

	return hasDupes;
}

void OBSBasicSettings::ReloadHotkeys(obs_hotkey_id ignoreKey)
{
	if (!hotkeysLoaded) {
		return;
	}
	LoadHotkeySettings(ignoreKey);
}

void OBSBasicSettings::A11yChanged()
{
	if (!loading) {
		a11yChanged = true;
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::AppearanceChanged()
{
	if (!loading) {
		appearanceChanged = true;
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::AdvancedChanged()
{
	if (!loading) {
		advancedChanged = true;
		sender()->setProperty("changed", QVariant(true));
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::FillAudioMonitoringDevices()
{
	QComboBox *cb = ui->monitoringDevice;

	auto enum_devices = [](void *param, const char *name, const char *id) {
		QComboBox *cb = (QComboBox *)param;
		cb->addItem(name, id);
		return true;
	};

	cb->addItem(QTStr("Basic.Settings.Advanced.Audio.MonitoringDevice"
			  ".Default"),
		    "default");

	obs_enum_audio_monitoring_devices(enum_devices, cb);
}

void OBSBasicSettings::SurroundWarning(int idx)
{
	if (idx == lastChannelSetupIdx || idx == -1) {
		return;
	}

	if (loading) {
		lastChannelSetupIdx = idx;
		return;
	}

	QString speakerLayoutQstr = ui->channelSetup->itemText(idx);
	bool surround = IsSurround(QT_TO_UTF8(speakerLayoutQstr));

	QString lastQstr = ui->channelSetup->itemText(lastChannelSetupIdx);
	bool wasSurround = IsSurround(QT_TO_UTF8(lastQstr));

	if (surround && !wasSurround) {
		QMessageBox::StandardButton button;

		QString warningString = QTStr("Basic.Settings.ProgramRestart") + QStringLiteral("\n\n") +
					QTStr(MULTI_CHANNEL_WARNING) + QStringLiteral("\n\n") +
					QTStr(MULTI_CHANNEL_WARNING ".Confirm");

		button = OBSMessageBox::question(this, QTStr(MULTI_CHANNEL_WARNING ".Title"), warningString);

		if (button == QMessageBox::No) {
			QMetaObject::invokeMethod(ui->channelSetup, "setCurrentIndex", Qt::QueuedConnection,
						  Q_ARG(int, lastChannelSetupIdx));
			return;
		}
	}

	lastChannelSetupIdx = idx;
}

#define LL_BUFFERING_WARNING "Basic.Settings.Audio.LowLatencyBufferingWarning"

void OBSBasicSettings::UpdateAudioWarnings()
{
	QString speakerLayoutQstr = ui->channelSetup->currentText();
	bool surround = IsSurround(QT_TO_UTF8(speakerLayoutQstr));
	bool lowBufferingActive = ui->lowLatencyBuffering->isChecked();

	QString text;

	if (surround) {
		text = QTStr(MULTI_CHANNEL_WARNING ".Enabled") + QStringLiteral("\n\n") + QTStr(MULTI_CHANNEL_WARNING);
	}

	if (lowBufferingActive) {
		if (!text.isEmpty()) {
			text += QStringLiteral("\n\n");
		}

		text += QTStr(LL_BUFFERING_WARNING ".Enabled") + QStringLiteral("\n\n") + QTStr(LL_BUFFERING_WARNING);
	}

	ui->audioMsg_2->setText(text);
	ui->audioMsg_2->setVisible(!text.isEmpty());
}

void OBSBasicSettings::LowLatencyBufferingChanged(bool checked)
{
	if (checked) {
		QString warningStr =
			QTStr(LL_BUFFERING_WARNING) + QStringLiteral("\n\n") + QTStr(LL_BUFFERING_WARNING ".Confirm");

		auto button = OBSMessageBox::question(this, QTStr(LL_BUFFERING_WARNING ".Title"), warningStr);

		if (button == QMessageBox::No) {
			QMetaObject::invokeMethod(ui->lowLatencyBuffering, "setChecked", Qt::QueuedConnection,
						  Q_ARG(bool, false));
			return;
		}
	}

	QMetaObject::invokeMethod(this, "UpdateAudioWarnings", Qt::QueuedConnection);
	QMetaObject::invokeMethod(this, "AudioChangedRestart");
}

void OBSBasicSettings::on_disableOSXVSync_clicked()
{
#ifdef __APPLE__
	if (!loading) {
		bool disable = ui->disableOSXVSync->isChecked();
		ui->resetOSXVSync->setEnabled(disable);
	}
#endif
}

QIcon OBSBasicSettings::GetGeneralIcon() const
{
	return generalIcon;
}

QIcon OBSBasicSettings::GetAppearanceIcon() const
{
	return appearanceIcon;
}

QIcon OBSBasicSettings::GetStreamIcon() const
{
	return streamIcon;
}

QIcon OBSBasicSettings::GetAudioIcon() const
{
	return audioIcon;
}

QIcon OBSBasicSettings::GetHotkeysIcon() const
{
	return hotkeysIcon;
}

QIcon OBSBasicSettings::GetAccessibilityIcon() const
{
	return accessibilityIcon;
}

QIcon OBSBasicSettings::GetAdvancedIcon() const
{
	return advancedIcon;
}

QIcon OBSBasicSettings::GetCanvasIcon() const
{
	return canvasIcon;
}

void OBSBasicSettings::SetGeneralIcon(const QIcon &icon)
{
	ui->listWidget->item(Pages::GENERAL)->setIcon(icon);
}

void OBSBasicSettings::SetAppearanceIcon(const QIcon &icon)
{
	ui->listWidget->item(Pages::APPEARANCE)->setIcon(icon);
}

void OBSBasicSettings::SetStreamIcon(const QIcon &icon)
{
	ui->listWidget->item(Pages::STREAM)->setIcon(icon);
}

void OBSBasicSettings::SetAudioIcon(const QIcon &icon)
{
	ui->listWidget->item(Pages::AUDIO)->setIcon(icon);
}

void OBSBasicSettings::SetHotkeysIcon(const QIcon &icon)
{
	ui->listWidget->item(Pages::HOTKEYS)->setIcon(icon);
}

void OBSBasicSettings::SetAccessibilityIcon(const QIcon &icon)
{
	ui->listWidget->item(Pages::ACCESSIBILITY)->setIcon(icon);
}

void OBSBasicSettings::SetAdvancedIcon(const QIcon &icon)
{
	ui->listWidget->item(Pages::ADVANCED)->setIcon(icon);
}

void OBSBasicSettings::SetCanvasIcon(const QIcon &icon)
{
	canvasIcon = icon;
	ui->listWidget->item(Pages::CANVAS)->setIcon(icon);
}

void OBSBasicSettings::UpdateAdvNetworkGroup()
{
	bool enabled = protocol.contains("RTMP");

	ui->advNetworkDisabled->setVisible(!enabled);

	ui->bindToIPLabel->setVisible(enabled);
	ui->bindToIP->setVisible(enabled);
	ui->dynBitrate->setVisible(enabled);
	ui->ipFamilyLabel->setVisible(enabled);
	ui->ipFamily->setVisible(enabled);
#ifdef _WIN32
	ui->enableNewSocketLoop->setVisible(enabled);
	ui->enableLowLatencyMode->setVisible(enabled);
#endif
}

void OBSBasicSettings::UpdateMultitrackVideo()
{
	/* Multitrack Video (GoLive) is superseded by native canvas multistreaming
	 * and kept dormant: its controls stay hidden in this build. */
	ui->multitrackVideoGroupBox->setVisible(false);
}
