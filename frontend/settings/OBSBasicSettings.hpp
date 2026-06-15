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

#pragma once

#include "ui_OBSBasicSettings.h"

#include <utility/CanvasDefinition.hpp>
#include <utility/FFmpegShared.hpp>
#include <utility/StreamProfile.hpp>

#include <QPointer>

class QLineEdit;
class QListWidget;
class QListWidgetItem;

#define VOLUME_METER_DECAY_FAST 23.53
#define VOLUME_METER_DECAY_MEDIUM 11.76
#define VOLUME_METER_DECAY_SLOW 8.57

class Auth;
class OBSBasic;
class OBSHotkeyWidget;
class OBSPropertiesView;
struct FFmpegFormat;
struct OBSTheme;

std::string DeserializeConfigText(const char *value);

class OBSBasicSettings : public QDialog {
	Q_OBJECT
	Q_PROPERTY(QIcon generalIcon READ GetGeneralIcon WRITE SetGeneralIcon DESIGNABLE true)
	Q_PROPERTY(QIcon appearanceIcon READ GetAppearanceIcon WRITE SetAppearanceIcon DESIGNABLE true)
	Q_PROPERTY(QIcon streamIcon READ GetStreamIcon WRITE SetStreamIcon DESIGNABLE true)
	Q_PROPERTY(QIcon audioIcon READ GetAudioIcon WRITE SetAudioIcon DESIGNABLE true)
	Q_PROPERTY(QIcon hotkeysIcon READ GetHotkeysIcon WRITE SetHotkeysIcon DESIGNABLE true)
	Q_PROPERTY(QIcon accessibilityIcon READ GetAccessibilityIcon WRITE SetAccessibilityIcon DESIGNABLE true)
	Q_PROPERTY(QIcon advancedIcon READ GetAdvancedIcon WRITE SetAdvancedIcon DESIGNABLE true)
	Q_PROPERTY(QIcon canvasIcon READ GetCanvasIcon WRITE SetCanvasIcon DESIGNABLE true)
	Q_PROPERTY(QIcon outputIcon READ GetOutputIcon WRITE SetOutputIcon DESIGNABLE true)

	enum Pages { GENERAL, APPEARANCE, STREAM, CANVAS, OUTPUTS, AUDIO, HOTKEYS, ACCESSIBILITY, ADVANCED, NUM_PAGES };

private:
	OBSBasic *main;

	std::unique_ptr<Ui::OBSBasicSettings> ui;

	std::shared_ptr<Auth> auth;

	bool generalChanged = false;
	bool stream1Changed = false;
	bool audioChanged = false;
	bool hotkeysChanged = false;
	bool a11yChanged = false;
	bool appearanceChanged = false;
	bool advancedChanged = false;
	int pageIndex = 0;
	bool loading = true;
	bool loadingOutputs = false;
	bool forceAuthReload = false;
	bool forceUpdateCheck = false;
	int sampleRateIndex = 0;
	int channelIndex = 0;
	bool llBufferingEnabled = false;
	bool hotkeysLoaded = false;

	int lastServiceIdx = -1;
	int lastIgnoreRecommended = -1;
	int lastChannelSetupIdx = 0;

	static constexpr uint32_t ENCODER_HIDE_FLAGS = (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL);

	OBSTheme *savedTheme = nullptr;

	std::vector<FFmpegFormat> formats;

	OBSPropertiesView *streamProperties = nullptr;

	using AudioSource_t = std::tuple<OBSWeakSource, QPointer<QCheckBox>, QPointer<QSpinBox>, QPointer<QCheckBox>,
					 QPointer<QSpinBox>>;
	std::vector<AudioSource_t> audioSources;
	std::vector<OBSSignal> audioSourceSignals;
	OBSSignal sourceCreated;
	OBSSignal channelChanged;

	std::vector<std::pair<bool, QPointer<OBSHotkeyWidget>>> hotkeys;
	OBSSignal hotkeyRegistered;
	OBSSignal hotkeyUnregistered;

	QIcon hotkeyConflictIcon;

	void SaveCombo(QComboBox *widget, const char *section, const char *value);
	void SaveComboData(QComboBox *widget, const char *section, const char *value);
	void SaveCheckBox(QAbstractButton *widget, const char *section, const char *value, bool invert = false);
	void SaveGroupBox(QGroupBox *widget, const char *section, const char *value);
	void SaveEdit(QLineEdit *widget, const char *section, const char *value);
	void SaveSpinBox(QSpinBox *widget, const char *section, const char *value);
	void SaveText(QPlainTextEdit *widget, const char *section, const char *value);

	// TODO: Remove, orphaned method
	void ClosestResFPS(obs_service_resolution *res_list, size_t res_count, int max_fps, int &new_cx, int &new_cy,
			   int &new_fps);

	inline bool Changed() const
	{
		return generalChanged || appearanceChanged || stream1Changed || audioChanged || advancedChanged ||
		       hotkeysChanged || a11yChanged;
	}

	inline void EnableApplyButton(bool en) { ui->buttonBox->button(QDialogButtonBox::Apply)->setEnabled(en); }

	inline void ClearChanged()
	{
		generalChanged = false;
		stream1Changed = false;
		audioChanged = false;
		hotkeysChanged = false;
		a11yChanged = false;
		advancedChanged = false;
		appearanceChanged = false;
		EnableApplyButton(false);
	}

	template<typename Widget, typename WidgetParent, typename... SignalArgs, typename... SlotArgs>
	void HookWidget(Widget *widget, void (WidgetParent::*signal)(SignalArgs...),
			void (OBSBasicSettings::*slot)(SlotArgs...))
	{
		QObject::connect(widget, signal, this, slot);
		widget->setProperty("changed", QVariant(false));
	}

	bool QueryChanges();
	bool QueryAllowedToClose();

	void LoadGeneralSettings();
	void LoadStream1Settings();
	void LoadAudioSettings();
	void LoadHotkeySettings(obs_hotkey_id ignoreKey = OBS_INVALID_HOTKEY_ID);
	void LoadA11ySettings(bool presetChange = false);
	void LoadAppearanceSettings(bool reload = false);
	void LoadAdvancedSettings();

	void LoadCanvasSettings();
	void RebuildCanvasList();
	QWidget *BuildCanvasCard(const CanvasDefinition &def);
	void AddCanvasClicked();
	void RemoveCanvasClicked(const std::string &uuid);
	void EditCanvasClicked(const std::string &uuid);
	void ApplyCanvasEdit(CanvasDefinition &def);

	void LoadOutputSettings();
	void RebuildOutputList();
	void AddOutputClicked(const std::string &canvasUuid);
	void RemoveOutputClicked(const std::string &bindingUuid);
	void CascadeCanvasOutputs(const std::string &canvasUuid, bool enabled);
	QWidget *BuildCanvasOutputGroup(const char *canvasUuid, const QString &canvasTitle);
	QWidget *BuildOutputRow(struct OutputBinding &binding);

	void LoadSettings(bool changedOnly);

	/* general */
	void LoadLanguageList();
	void LoadThemeList(bool firstLoad);
	void LoadBranchesList();

	/* stream */
	void InitStreamPage();

	/* stream profiles (Streams page master-detail) */
	void LoadStreamProfiles();
	void RebuildStreamProfileList();
	void SelectStreamProfile(const std::string &uuid);
	void AddStreamProfileClicked();
	void RemoveStreamProfileClicked(const std::string &uuid);
	void LoadProfileIntoForm(const std::string &serviceId, obs_data_t *settings, const QString &label);
	void SaveFormIntoProfile(StreamProfile &p);
	void StreamProfileSelectionChanged(QListWidgetItem *current);
	std::string currentProfileUuid;
	QListWidget *streamProfileList = nullptr;
	QLineEdit *streamProfileLabel = nullptr;

	bool IsCustomService() const;
	inline bool IsWHIP() const;
	void LoadServices(bool showAll);
	void OnOAuthStreamKeyConnected();
	void OnAuthConnected();
	QString lastService;
	QString protocol;
	QString lastCustomServer;
	int prevLangIndex;
	bool prevBrowserAccel;

	void ServiceChanged(bool resetFields = false);
	QString FindProtocol();
	void UpdateServerList();
	void UpdateKeyLink();
	void UpdateVodTrackSetting();
	void UpdateServiceRecommendations();
	void UpdateMoreInfoLink();
	void UpdateAdvNetworkGroup();

	/* Appearance */
	void InitAppearancePage();
	void enableAppearanceFontControls(bool enable);
	void enableAppearanceDensityControls(bool enable);

	bool IsCustomServer();

private slots:
	void UpdateMultitrackVideo();
	void on_show_clicked();
	void on_authPwShow_clicked();
	void on_connectAccount_clicked();
	void on_disconnectAccount_clicked();
	void on_useStreamKey_clicked();
	void on_useAuth_toggled();
	void on_server_currentIndexChanged(int index);

	void on_hotkeyFilterReset_clicked();
	void on_hotkeyFilterSearch_textChanged(const QString text);
	void on_hotkeyFilterInput_KeyChanged(obs_key_combination_t combo);

private:
	/* audio */
	void LoadListValues(QComboBox *widget, obs_property_t *prop, int index);
	void LoadAudioDevices();
	void LoadAudioSources();

	/* video */
	void LoadRendererList();

	/* a11y */
	void UpdateA11yColors();
	void SetDefaultColors();
	void ResetDefaultColors();
	QColor GetColor(uint32_t colorVal, QString label);
	uint32_t preset = 0;
	uint32_t selectRed = 0x0000FF;
	uint32_t selectGreen = 0x00FF00;
	uint32_t selectBlue = 0xFF7F00;
	uint32_t mixerGreen = 0x267f26;
	uint32_t mixerYellow = 0x267f7f;
	uint32_t mixerRed = 0x26267f;
	uint32_t mixerGreenActive = 0x4cff4c;
	uint32_t mixerYellowActive = 0x4cffff;
	uint32_t mixerRedActive = 0x4c4cff;

	void SaveGeneralSettings();
	void SaveStream1Settings();
	void SaveAudioSettings();
	void SaveHotkeySettings();
	void SaveA11ySettings();
	void SaveAppearanceSettings();
	void SaveAdvancedSettings();
	void SaveSettings();

	void SearchHotkeys(const QString &text, obs_key_combination_t filterCombo);

	void FillAudioMonitoringDevices();

	bool AskIfCanCloseSettings();

	void UpdateYouTubeAppDockSettings();

	QIcon generalIcon;
	QIcon appearanceIcon;
	QIcon streamIcon;
	QIcon audioIcon;
	QIcon hotkeysIcon;
	QIcon accessibilityIcon;
	QIcon advancedIcon;
	QIcon canvasIcon;
	QIcon outputIcon;

	QIcon GetGeneralIcon() const;
	QIcon GetAppearanceIcon() const;
	QIcon GetStreamIcon() const;
	QIcon GetAudioIcon() const;
	QIcon GetHotkeysIcon() const;
	QIcon GetAccessibilityIcon() const;
	QIcon GetAdvancedIcon() const;
	QIcon GetCanvasIcon() const;
	QIcon GetOutputIcon() const;

	OBSService GetStream1Service();

private slots:
	void on_theme_activated(int idx);
	void on_themeVariant_activated(int idx);
	void updateAppearanceControls();

	void on_listWidget_itemSelectionChanged();
	void on_buttonBox_clicked(QAbstractButton *button);

	void on_service_currentIndexChanged(int idx);
	void on_customServer_textChanged(const QString &text);

	void on_filenameFormatting_textEdited(const QString &text);

	void on_disableOSXVSync_clicked();

	void on_choose1_clicked();
	void on_choose2_clicked();
	void on_choose3_clicked();
	void on_choose4_clicked();
	void on_choose5_clicked();
	void on_choose6_clicked();
	void on_choose7_clicked();
	void on_choose8_clicked();
	void on_choose9_clicked();
	void on_colorPreset_currentIndexChanged(int idx);

	void GeneralChanged();

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	void HideOBSWindowWarning(Qt::CheckState state);
#else
	void HideOBSWindowWarning(int state);
#endif
	void AudioChanged();
	void AudioChangedRestart();
	void ReloadAudioSources();
	void SurroundWarning(int idx);
	void SpeakerLayoutChanged(int idx);
	void LowLatencyBufferingChanged(bool checked);
	void UpdateAudioWarnings();
	void Stream1Changed();
	void HotkeysChanged();
	bool ScanDuplicateHotkeys(QFormLayout *layout);
	void ReloadHotkeys(obs_hotkey_id ignoreKey = OBS_INVALID_HOTKEY_ID);
	void A11yChanged();
	void AppearanceChanged();
	void AdvancedChanged();
	void AdvancedChangedRestart();

	OBSService SpawnTempService();

	void SetGeneralIcon(const QIcon &icon);
	void SetAppearanceIcon(const QIcon &icon);
	void SetStreamIcon(const QIcon &icon);
	void SetAudioIcon(const QIcon &icon);
	void SetHotkeysIcon(const QIcon &icon);
	void SetAccessibilityIcon(const QIcon &icon);
	void SetAdvancedIcon(const QIcon &icon);
	void SetCanvasIcon(const QIcon &icon);
	void SetOutputIcon(const QIcon &icon);

	void UseStreamKeyAdvClicked();

protected:
	virtual void closeEvent(QCloseEvent *event) override;
	virtual void showEvent(QShowEvent *event) override;
	void reject() override;

public:
	OBSBasicSettings(QWidget *parent);
	~OBSBasicSettings();

	inline const QIcon &GetHotkeyConflictIcon() const { return hotkeyConflictIcon; }
};
