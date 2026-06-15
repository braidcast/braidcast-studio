#include "OBSBasicSettings.hpp"

#ifdef YOUTUBE_ENABLED
#include <docks/YouTubeAppDock.hpp>
#endif
#include <oauth/OAuth.hpp>
#ifdef YOUTUBE_ENABLED
#include <utility/YoutubeApiWrappers.hpp>
#endif
#include <widgets/OBSBasic.hpp>
#include <utility/StreamProfileManager.hpp>

#include <qt-wrappers.hpp>

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QUuid>
#include <QVBoxLayout>

static const QUuid &CustomServerUUID()
{
	static const QUuid uuid = QUuid::fromString(QT_UTF8("{241da255-70f2-4bbb-bef7-509695bf8e65}"));
	return uuid;
}

struct QCef;
struct QCefCookieManager;

extern QCef *cef;
extern QCefCookieManager *panel_cookies;
extern bool cef_js_avail;

enum class ListOpt : int {
	ShowAll = 1,
	Custom,
	WHIP,
};

enum class Section : int {
	Connect,
	StreamKey,
};

bool OBSBasicSettings::IsCustomService() const
{
	return ui->service->currentData().toInt() == (int)ListOpt::Custom;
}

inline bool OBSBasicSettings::IsWHIP() const
{
	return ui->service->currentData().toInt() == (int)ListOpt::WHIP;
}

void OBSBasicSettings::InitStreamPage()
{
	ui->connectAccount2->setVisible(false);
	ui->disconnectAccount->setVisible(false);
	ui->bandwidthTestEnable->setVisible(false);

	ui->twitchAddonDropdown->setVisible(false);
	ui->twitchAddonLabel->setVisible(false);

	ui->connectedAccountLabel->setVisible(false);
	ui->connectedAccountText->setVisible(false);

	int vertSpacing = ui->topStreamLayout->verticalSpacing();

	QMargins m = ui->topStreamLayout->contentsMargins();
	m.setBottom(vertSpacing / 2);
	ui->topStreamLayout->setContentsMargins(m);

	m = ui->loginPageLayout->contentsMargins();
	m.setTop(vertSpacing / 2);
	ui->loginPageLayout->setContentsMargins(m);

	m = ui->streamkeyPageLayout->contentsMargins();
	m.setTop(vertSpacing / 2);
	ui->streamkeyPageLayout->setContentsMargins(m);

	LoadServices(false);

	ui->twitchAddonDropdown->addItem(QTStr("Basic.Settings.Stream.TTVAddon.None"));
	ui->twitchAddonDropdown->addItem(QTStr("Basic.Settings.Stream.TTVAddon.BTTV"));
	ui->twitchAddonDropdown->addItem(QTStr("Basic.Settings.Stream.TTVAddon.FFZ"));
	ui->twitchAddonDropdown->addItem(QTStr("Basic.Settings.Stream.TTVAddon.Both"));

	connect(ui->enableMultitrackVideo, &QCheckBox::toggled, this, &OBSBasicSettings::UpdateMultitrackVideo);
	connect(ui->multitrackVideoMaximumAggregateBitrateAuto, &QCheckBox::toggled, this,
		&OBSBasicSettings::UpdateMultitrackVideo);
	connect(ui->multitrackVideoMaximumVideoTracksAuto, &QCheckBox::toggled, this,
		&OBSBasicSettings::UpdateMultitrackVideo);
	connect(ui->multitrackVideoConfigOverrideEnable, &QCheckBox::toggled, this,
		&OBSBasicSettings::UpdateMultitrackVideo);

	/* Build the profile address book and drop it into the left column of the
	 * stream page (the streamProfilePanel defined in the .ui), beside the reused
	 * service form. The list fills the column; the "Add profile" tile sits at the
	 * bottom, matching the Streams master-detail mock. */
	QVBoxLayout *panel = ui->streamProfilePanelLayout;

	streamProfileList = new QListWidget();
	streamProfileList->setObjectName("streamProfileList");
	streamProfileList->setFrameShape(QFrame::NoFrame);
	panel->addWidget(streamProfileList, 1);

	QHBoxLayout *buttons = new QHBoxLayout();
	QPushButton *addProfile = new QPushButton(QTStr("Basic.Settings.Streams.AddProfile"));
	addProfile->setObjectName("streamProfileAdd");
	QPushButton *removeProfile = new QPushButton(QTStr("Remove"));
	removeProfile->setObjectName("streamProfileRemove");
	buttons->addWidget(addProfile);
	buttons->addWidget(removeProfile);
	panel->addLayout(buttons);

	/* Per-profile Label field, inserted as the first row of the service form. */
	streamProfileLabel = new QLineEdit();
	streamProfileLabel->setObjectName("streamProfileLabel");
	ui->topStreamLayout->insertRow(0, QTStr("Basic.Settings.Streams.Label"), streamProfileLabel);

	connect(streamProfileLabel, &QLineEdit::textChanged, this, &OBSBasicSettings::Stream1Changed);

	connect(streamProfileList, &QListWidget::currentItemChanged, this,
		[this](QListWidgetItem *current, QListWidgetItem *) { StreamProfileSelectionChanged(current); });
	connect(addProfile, &QPushButton::clicked, this, &OBSBasicSettings::AddStreamProfileClicked);
	connect(removeProfile, &QPushButton::clicked, this, [this]() {
		if (!currentProfileUuid.empty()) {
			RemoveStreamProfileClicked(currentProfileUuid);
		}
	});
}

void OBSBasicSettings::LoadProfileIntoForm(const std::string &serviceId, obs_data_t *profileSettings,
					   const QString &label)
{
	bool is_rtmp_custom = (serviceId == "rtmp_custom");
	bool is_rtmp_common = (serviceId == "rtmp_common");
	bool is_whip = (serviceId == "whip_custom");

	streamProfileLabel->setText(label);

	/* A fresh profile may carry a null blob; treat it as an empty default
	 * rtmp_common service (the form fields tolerate empty strings). */
	OBSDataAutoRelease settings = obs_data_create();
	if (profileSettings) {
		obs_data_apply(settings, profileSettings);
	}

	const char *service = obs_data_get_string(settings, "service");
	const char *server = obs_data_get_string(settings, "server");
	const char *key = obs_data_get_string(settings, "key");
	bool use_custom_server = obs_data_get_bool(settings, "using_custom_server");
	const char *storedProtocol = obs_data_get_string(settings, "protocol");
	protocol = (storedProtocol && *storedProtocol) ? QT_UTF8(storedProtocol) : QString("RTMP");
	const char *bearer_token = obs_data_get_string(settings, "bearer_token");

	if (is_rtmp_custom || is_whip) {
		ui->customServer->setText(server);
	}

	if (is_rtmp_custom) {
		ui->service->setCurrentIndex(0);
		lastServiceIdx = 0;
		lastCustomServer = ui->customServer->text();

		bool use_auth = obs_data_get_bool(settings, "use_auth");
		const char *username = obs_data_get_string(settings, "username");
		const char *password = obs_data_get_string(settings, "password");
		ui->authUsername->setText(QT_UTF8(username));
		ui->authPw->setText(QT_UTF8(password));
		ui->useAuth->setChecked(use_auth);
	} else {
		int idx = ui->service->findText(service);
		if (idx == -1) {
			if (service && *service) {
				ui->service->insertItem(1, service);
			}
			idx = 1;
		}
		ui->service->setCurrentIndex(idx);
		lastServiceIdx = idx;

		bool bw_test = obs_data_get_bool(settings, "bwtest");
		ui->bandwidthTestEnable->setChecked(bw_test);

		idx = config_get_int(main->Config(), "Twitch", "AddonChoice");
		ui->twitchAddonDropdown->setCurrentIndex(idx);
	}

	UpdateServerList();

	if (is_rtmp_common) {
		int idx = -1;
		if (use_custom_server) {
			idx = ui->server->findData(CustomServerUUID());
		} else {
			idx = ui->server->findData(QString::fromUtf8(server));
		}

		if (idx == -1) {
			if (server && *server) {
				ui->server->insertItem(0, server, server);
			}
			idx = 0;
		}
		ui->server->setCurrentIndex(idx);
	}

	if (use_custom_server) {
		ui->serviceCustomServer->setText(server);
	}

	if (is_whip) {
		ui->key->setText(bearer_token);
		ui->whipSimulcastGroupBox->show();
	} else {
		ui->key->setText(key);
		ui->whipSimulcastGroupBox->hide();
	}

	ServiceChanged(true);

	/* Custom/WHIP profiles don't persist a "protocol" key; recompute it from
	 * the now-populated form so recommendations and the multitrack panel match
	 * the actual server URL. */
	if (is_rtmp_custom || is_whip) {
		protocol = FindProtocol();
	}

	UpdateKeyLink();
	UpdateMoreInfoLink();
	UpdateVodTrackSetting();
	UpdateServiceRecommendations();
	UpdateMultitrackVideo();
}

void OBSBasicSettings::LoadStream1Settings()
{
	bool ignoreRecommended = config_get_bool(main->Config(), "Stream1", "IgnoreRecommended");
	int whipSimulcastTotalLayers = config_get_int(main->Config(), "Stream1", "WHIPSimulcastTotalLayers");

	loading = true;

	/* Multitrack video is a global (per-config) setting, not per-profile in
	 * 2c, so it is still read from main->Config() here rather than from the
	 * selected profile's blob. */
	ui->enableMultitrackVideo->setChecked(config_get_bool(main->Config(), "Stream1", "EnableMultitrackVideo"));

	ui->multitrackVideoMaximumAggregateBitrateAuto->setChecked(
		config_get_bool(main->Config(), "Stream1", "MultitrackVideoMaximumAggregateBitrateAuto"));
	if (config_has_user_value(main->Config(), "Stream1", "MultitrackVideoMaximumAggregateBitrate")) {
		ui->multitrackVideoMaximumAggregateBitrate->setValue(
			config_get_int(main->Config(), "Stream1", "MultitrackVideoMaximumAggregateBitrate"));
	}

	ui->multitrackVideoMaximumVideoTracksAuto->setChecked(
		config_get_bool(main->Config(), "Stream1", "MultitrackVideoMaximumVideoTracksAuto"));
	if (config_has_user_value(main->Config(), "Stream1", "MultitrackVideoMaximumVideoTracks")) {
		ui->multitrackVideoMaximumVideoTracks->setValue(
			config_get_int(main->Config(), "Stream1", "MultitrackVideoMaximumVideoTracks"));
	}

	ui->multitrackVideoStreamDumpEnable->setChecked(
		config_get_bool(main->Config(), "Stream1", "MultitrackVideoStreamDumpEnabled"));

	ui->multitrackVideoConfigOverrideEnable->setChecked(
		config_get_bool(main->Config(), "Stream1", "MultitrackVideoConfigOverrideEnabled"));
	if (config_has_user_value(main->Config(), "Stream1", "MultitrackVideoConfigOverride")) {
		ui->multitrackVideoConfigOverride->setPlainText(
			DeserializeConfigText(
				config_get_string(main->Config(), "Stream1", "MultitrackVideoConfigOverride"))
				.c_str());
	}

	ui->multitrackVideoAdditionalCanvas->clear();
	ui->multitrackVideoAdditionalCanvas->addItem(QTStr("None"));
	for (const auto &canvas : main->GetCanvases()) {
		if (obs_canvas_get_flags(canvas) & EPHEMERAL) {
			continue;
		}

		ui->multitrackVideoAdditionalCanvas->addItem(obs_canvas_get_name(canvas), obs_canvas_get_uuid(canvas));
	}

	if (config_has_user_value(main->Config(), "Stream1", "MultitrackExtraCanvas")) {
		/* Currently we only support one canvas, so the value will just be one UUID. */
		const std::string_view uuid = config_get_string(main->Config(), "Stream1", "MultitrackExtraCanvas");
		if (!uuid.empty()) {
			int idx = ui->multitrackVideoAdditionalCanvas->findData(uuid.data());
			ui->multitrackVideoAdditionalCanvas->setCurrentIndex(idx);
		}
	}

	bool streamActive = obs_frontend_streaming_active();
	ui->streamPage->setEnabled(!streamActive);

	ui->ignoreRecommended->setChecked(ignoreRecommended);
	ui->whipSimulcastTotalLayers->setValue(whipSimulcastTotalLayers);

	/* Populate the service form from the selected (primary) profile's blob. */
	LoadStreamProfiles();

	loading = false;
}

void OBSBasicSettings::SaveStream1Settings()
{
	StreamProfileManager &mgr = main->GetStreamProfileManager();
	StreamProfile *current = mgr.Find(currentProfileUuid);

	/* Persist the form into the currently selected profile's blob. */
	if (current) {
		SaveFormIntoProfile(*current);
	}

	/* Only the primary profile is mirrored into OBS's single global service,
	 * which drives the live single-stream output (Phase 1). Editing a
	 * non-primary profile touches streams.json only — no SetService, no auth
	 * bookkeeping, no OAuth side effects. */
	if (current && current->isPrimary) {
		const char *service_id = current->serviceId.c_str();

		obs_service_t *oldService = main->GetService();
		OBSDataAutoRelease hotkeyData = obs_hotkeys_save_service(oldService);

		OBSDataAutoRelease settings = obs_data_create();
		obs_data_apply(settings, current->settings);

		if (!!auth && strcmp(auth->service(), "Twitch") == 0) {
			bool choiceExists = config_has_user_value(main->Config(), "Twitch", "AddonChoice");
			int currentChoice = config_get_int(main->Config(), "Twitch", "AddonChoice");
			int newChoice = ui->twitchAddonDropdown->currentIndex();

			config_set_int(main->Config(), "Twitch", "AddonChoice", newChoice);

			if (choiceExists && currentChoice != newChoice) {
				forceAuthReload = true;
			}
		}

		OBSServiceAutoRelease newService =
			obs_service_create(service_id, "default_service", settings, hotkeyData);

		if (!newService) {
			return;
		}

		main->SetService(newService);
		main->SaveService();
		main->auth = auth;
		if (!!main->auth) {
			main->auth->LoadUI();
			main->SetBroadcastFlowEnabled(main->auth->broadcastFlow());
		} else {
			main->SetBroadcastFlowEnabled(false);
		}
	}

	mgr.Save();

	SaveCheckBox(ui->ignoreRecommended, "Stream1", "IgnoreRecommended");

	auto oldWHIPSimulcastTotalLayers = config_get_int(main->Config(), "Stream1", "WHIPSimulcastTotalLayers");
	SaveSpinBox(ui->whipSimulcastTotalLayers, "Stream1", "WHIPSimulcastTotalLayers");

	/* Multitrack video config is global and tied to the live (primary) service.
	 * Deriving it from the form while a non-primary profile is selected would
	 * recompute global multitrack settings from the wrong service, so only the
	 * primary profile may touch it. */
	if (current && current->isPrimary) {
		auto oldMultitrackVideoSetting = config_get_bool(main->Config(), "Stream1", "EnableMultitrackVideo");

		if (!IsCustomService()) {
			OBSDataAutoRelease settings = obs_data_create();
			obs_data_set_string(settings, "service", QT_TO_UTF8(ui->service->currentText()));
			OBSServiceAutoRelease temp_service =
				obs_service_create_private("rtmp_common", "auto config query service", settings);
			settings = obs_service_get_settings(temp_service);
			auto available = obs_data_has_user_value(settings, "multitrack_video_configuration_url");

			if (available) {
				SaveCheckBox(ui->enableMultitrackVideo, "Stream1", "EnableMultitrackVideo");
			} else {
				config_remove_value(main->Config(), "Stream1", "EnableMultitrackVideo");
			}
		} else {
			SaveCheckBox(ui->enableMultitrackVideo, "Stream1", "EnableMultitrackVideo");
		}
		SaveCheckBox(ui->multitrackVideoMaximumAggregateBitrateAuto, "Stream1",
			     "MultitrackVideoMaximumAggregateBitrateAuto");
		SaveSpinBox(ui->multitrackVideoMaximumAggregateBitrate, "Stream1",
			    "MultitrackVideoMaximumAggregateBitrate");
		SaveCheckBox(ui->multitrackVideoMaximumVideoTracksAuto, "Stream1",
			     "MultitrackVideoMaximumVideoTracksAuto");
		SaveSpinBox(ui->multitrackVideoMaximumVideoTracks, "Stream1", "MultitrackVideoMaximumVideoTracks");
		SaveCheckBox(ui->multitrackVideoStreamDumpEnable, "Stream1", "MultitrackVideoStreamDumpEnabled");
		SaveCheckBox(ui->multitrackVideoConfigOverrideEnable, "Stream1",
			     "MultitrackVideoConfigOverrideEnabled");
		SaveText(ui->multitrackVideoConfigOverride, "Stream1", "MultitrackVideoConfigOverride");
		SaveComboData(ui->multitrackVideoAdditionalCanvas, "Stream1", "MultitrackExtraCanvas");

		if (oldMultitrackVideoSetting != ui->enableMultitrackVideo->isChecked() ||
		    oldWHIPSimulcastTotalLayers != ui->whipSimulcastTotalLayers->value()) {
			main->ResetOutputs();
		}
	}
}

void OBSBasicSettings::SaveFormIntoProfile(StreamProfile &p)
{
	bool customServer = IsCustomService();
	bool whip = IsWHIP();
	const char *service_id = "rtmp_common";

	if (customServer) {
		service_id = "rtmp_custom";
	} else if (whip) {
		service_id = "whip_custom";
	}

	OBSDataAutoRelease settings = obs_data_create();

	if (!customServer && !whip) {
		obs_data_set_string(settings, "service", QT_TO_UTF8(ui->service->currentText()));
		obs_data_set_string(settings, "protocol", QT_TO_UTF8(protocol));
		if (ui->server->currentData() == CustomServerUUID()) {
			obs_data_set_bool(settings, "using_custom_server", true);
			obs_data_set_string(settings, "server", QT_TO_UTF8(ui->serviceCustomServer->text()));
		} else {
			obs_data_set_string(settings, "server", QT_TO_UTF8(ui->server->currentData().toString()));
		}
	} else {
		obs_data_set_string(settings, "server", QT_TO_UTF8(ui->customServer->text().trimmed()));
		obs_data_set_bool(settings, "use_auth", ui->useAuth->isChecked());
		if (ui->useAuth->isChecked()) {
			obs_data_set_string(settings, "username", QT_TO_UTF8(ui->authUsername->text()));
			obs_data_set_string(settings, "password", QT_TO_UTF8(ui->authPw->text()));
		}
	}

	if (!!auth && strcmp(auth->service(), "Twitch") == 0) {
		obs_data_set_bool(settings, "bwtest", ui->bandwidthTestEnable->isChecked());
	} else {
		obs_data_set_bool(settings, "bwtest", false);
	}

	if (whip) {
		obs_data_set_string(settings, "service", "WHIP");
		obs_data_set_string(settings, "bearer_token", QT_TO_UTF8(ui->key->text()));
	} else {
		obs_data_set_string(settings, "key", QT_TO_UTF8(ui->key->text()));
	}

	p.serviceId = service_id;
	p.settings = std::move(settings);
	p.label = QT_TO_UTF8(streamProfileLabel->text());
}

void OBSBasicSettings::RebuildStreamProfileList()
{
	StreamProfileManager &mgr = main->GetStreamProfileManager();
	streamProfileList->blockSignals(true);
	streamProfileList->clear();
	for (const StreamProfile &p : mgr.Profiles()) {
		QListWidgetItem *item = new QListWidgetItem(QString::fromStdString(p.DisplayName()));
		item->setData(Qt::UserRole, QString::fromStdString(p.uuid));
		if (p.isPrimary) {
			QFont f = item->font();
			f.setBold(true);
			item->setFont(f); /* primary drives the live single stream */
		}
		streamProfileList->addItem(item);
	}
	streamProfileList->blockSignals(false);
}

void OBSBasicSettings::SelectStreamProfile(const std::string &uuid)
{
	StreamProfileManager &mgr = main->GetStreamProfileManager();
	StreamProfile *p = mgr.Find(uuid);
	if (!p) {
		return;
	}

	bool wasLoading = loading;
	loading = true;
	currentProfileUuid = uuid;
	LoadProfileIntoForm(p->serviceId, p->settings, QString::fromStdString(p->label));
	loading = wasLoading;

	for (int i = 0; i < streamProfileList->count(); i++) {
		if (streamProfileList->item(i)->data(Qt::UserRole).toString().toStdString() == uuid) {
			streamProfileList->blockSignals(true);
			streamProfileList->setCurrentRow(i);
			streamProfileList->blockSignals(false);
			break;
		}
	}
}

void OBSBasicSettings::StreamProfileSelectionChanged(QListWidgetItem *current)
{
	if (loading || !current) {
		return;
	}

	std::string newUuid = current->data(Qt::UserRole).toString().toStdString();
	if (newUuid == currentProfileUuid) {
		return;
	}

	/* Save the outgoing profile's form edits before loading the new one, so
	 * switching the selection never silently discards in-progress changes. */
	StreamProfileManager &mgr = main->GetStreamProfileManager();
	StreamProfile *outgoing = mgr.Find(currentProfileUuid);
	if (outgoing) {
		std::string oldName = outgoing->DisplayName();
		SaveFormIntoProfile(*outgoing);
		if (outgoing->DisplayName() != oldName) {
			RebuildStreamProfileList();
		}
	}

	SelectStreamProfile(newUuid);
}

void OBSBasicSettings::LoadStreamProfiles()
{
	RebuildStreamProfileList();
	StreamProfileManager &mgr = main->GetStreamProfileManager();
	StreamProfile *primary = mgr.Primary();
	if (primary) {
		SelectStreamProfile(primary->uuid);
	} else if (!mgr.Empty()) {
		SelectStreamProfile(mgr.Profiles().front().uuid);
	}
}

void OBSBasicSettings::AddStreamProfileClicked()
{
	StreamProfileManager &mgr = main->GetStreamProfileManager();

	/* Persist the current form first so adding never discards in-progress
	 * edits to the previously selected profile. */
	StreamProfile *outgoing = mgr.Find(currentProfileUuid);
	if (outgoing) {
		SaveFormIntoProfile(*outgoing);
	}

	StreamProfile p;
	p.serviceId = "rtmp_common";
	p.label = QT_TO_UTF8(QTStr("Basic.Settings.Streams.NewName"));
	std::string newUuid = mgr.Add(std::move(p)).uuid;
	mgr.Save();

	RebuildStreamProfileList();
	SelectStreamProfile(newUuid);
}

void OBSBasicSettings::RemoveStreamProfileClicked(const std::string &uuid)
{
	StreamProfileManager &mgr = main->GetStreamProfileManager();
	StreamProfile *p = mgr.Find(uuid);
	if (!p) {
		return;
	}

	if (mgr.Profiles().size() <= 1) {
		QMessageBox::information(this, QTStr("Basic.Settings.Streams.RemoveConfirm.Title"),
					 QTStr("Basic.Settings.Streams.CannotRemoveLast"));
		return;
	}

	QMessageBox::StandardButton confirm = QMessageBox::question(
		this, QTStr("Basic.Settings.Streams.RemoveConfirm.Title"),
		QTStr("Basic.Settings.Streams.RemoveConfirm.Text").arg(QString::fromStdString(p->DisplayName())));
	if (confirm != QMessageBox::Yes) {
		return;
	}

	mgr.Remove(uuid);
	mgr.Save();

	RebuildStreamProfileList();
	StreamProfile *primary = mgr.Primary();
	if (primary) {
		SelectStreamProfile(primary->uuid);
	} else if (!mgr.Empty()) {
		SelectStreamProfile(mgr.Profiles().front().uuid);
	} else {
		currentProfileUuid.clear();
	}
}

void OBSBasicSettings::UpdateMoreInfoLink()
{
	if (IsCustomService() || IsWHIP()) {
		ui->moreInfoButton->hide();
		return;
	}

	QString serviceName = ui->service->currentText();
	OBSProperties props = obs_get_service_properties("rtmp_common");
	obs_property_t *services = obs_properties_get(props, "service");

	OBSDataAutoRelease settings = obs_data_create();

	obs_data_set_string(settings, "service", QT_TO_UTF8(serviceName));
	obs_property_modified(services, settings);

	const char *more_info_link = obs_data_get_string(settings, "more_info_link");

	if (!more_info_link || (*more_info_link == '\0')) {
		ui->moreInfoButton->hide();
	} else {
		ui->moreInfoButton->setTargetUrl(QUrl(more_info_link));
		ui->moreInfoButton->show();
	}
}

void OBSBasicSettings::UpdateKeyLink()
{
	QString serviceName = ui->service->currentText();
	QString customServer = ui->customServer->text().trimmed();
	QString streamKeyLink;

	OBSProperties props = obs_get_service_properties("rtmp_common");
	obs_property_t *services = obs_properties_get(props, "service");

	OBSDataAutoRelease settings = obs_data_create();

	obs_data_set_string(settings, "service", QT_TO_UTF8(serviceName));
	obs_property_modified(services, settings);

	streamKeyLink = obs_data_get_string(settings, "stream_key_link");

	if (customServer.contains("fbcdn.net") && IsCustomService()) {
		streamKeyLink = "https://www.facebook.com/live/producer?ref=OBS";
	}

	if (serviceName == "Dacast") {
		ui->streamKeyLabel->setText(QTStr("Basic.AutoConfig.StreamPage.EncoderKey"));
		ui->streamKeyLabel->setToolTip("");
	} else if (IsWHIP()) {
		ui->streamKeyLabel->setText(QTStr("Basic.AutoConfig.StreamPage.BearerToken"));
		ui->streamKeyLabel->setToolTip("");
	} else if (!IsCustomService()) {
		ui->streamKeyLabel->setText(QTStr("Basic.AutoConfig.StreamPage.StreamKey"));
		ui->streamKeyLabel->setToolTip("");
	} else {
		/* add tooltips for stream key, user, password fields */
		QString file = !App()->IsThemeDark() ? ":/res/images/help.svg" : ":/res/images/help_light.svg";
		QString lStr = "<html>%1 <img src='%2' style=' \
				vertical-align: bottom;  \
				' /></html>";

		ui->streamKeyLabel->setText(lStr.arg(QTStr("Basic.AutoConfig.StreamPage.StreamKey"), file));
		ui->streamKeyLabel->setToolTip(QTStr("Basic.AutoConfig.StreamPage.StreamKey.ToolTip"));

		ui->authUsernameLabel->setText(lStr.arg(QTStr("Basic.Settings.Stream.Custom.Username"), file));
		ui->authUsernameLabel->setToolTip(QTStr("Basic.Settings.Stream.Custom.Username.ToolTip"));

		ui->authPwLabel->setText(lStr.arg(QTStr("Basic.Settings.Stream.Custom.Password"), file));
		ui->authPwLabel->setToolTip(QTStr("Basic.Settings.Stream.Custom.Password.ToolTip"));
	}

	if (QString(streamKeyLink).isNull() || QString(streamKeyLink).isEmpty()) {
		ui->getStreamKeyButton->hide();
	} else {
		ui->getStreamKeyButton->setTargetUrl(QUrl(streamKeyLink));
		ui->getStreamKeyButton->show();
	}
}

void OBSBasicSettings::LoadServices(bool showAll)
{
	OBSProperties props = obs_get_service_properties("rtmp_common");

	OBSDataAutoRelease settings = obs_data_create();

	obs_data_set_bool(settings, "show_all", showAll);

	obs_property_t *prop = obs_properties_get(props, "show_all");
	obs_property_modified(prop, settings);

	ui->service->blockSignals(true);
	ui->service->clear();

	QStringList names;

	obs_property_t *services = obs_properties_get(props, "service");
	size_t services_count = obs_property_list_item_count(services);
	for (size_t i = 0; i < services_count; i++) {
		const char *name = obs_property_list_item_string(services, i);
		names.push_back(name);
	}

	if (showAll) {
		names.sort(Qt::CaseInsensitive);
	}

	for (QString &name : names) {
		ui->service->addItem(name);
	}

	if (obs_is_output_protocol_registered("WHIP")) {
		ui->service->addItem(QTStr("WHIP"), QVariant((int)ListOpt::WHIP));
	}

	if (!showAll) {
		ui->service->addItem(QTStr("Basic.AutoConfig.StreamPage.Service.ShowAll"),
				     QVariant((int)ListOpt::ShowAll));
	}

	ui->service->insertItem(0, QTStr("Basic.AutoConfig.StreamPage.Service.Custom"), QVariant((int)ListOpt::Custom));

	if (!lastService.isEmpty()) {
		int idx = ui->service->findText(lastService);
		if (idx != -1) {
			ui->service->setCurrentIndex(idx);
		}
	}

	ui->service->blockSignals(false);
}

static inline bool is_auth_service(const std::string &service)
{
	return Auth::AuthType(service) != Auth::Type::None;
}

static inline bool is_external_oauth(const std::string &service)
{
	return Auth::External(service);
}

static void reset_service_ui_fields(Ui::OBSBasicSettings *ui, std::string &service, bool loading)
{
	bool external_oauth = is_external_oauth(service);
	if (external_oauth) {
		ui->streamKeyWidget->setVisible(false);
		ui->streamKeyLabel->setVisible(false);
		ui->connectAccount2->setVisible(true);
		ui->useStreamKeyAdv->setVisible(true);
		ui->streamStackWidget->setCurrentIndex((int)Section::StreamKey);
	} else if (cef) {
		QString key = ui->key->text();
		bool can_auth = is_auth_service(service);
		int page = can_auth && (!loading || key.isEmpty()) ? (int)Section::Connect : (int)Section::StreamKey;

		ui->streamStackWidget->setCurrentIndex(page);
		ui->streamKeyWidget->setVisible(true);
		ui->streamKeyLabel->setVisible(true);
		ui->connectAccount2->setVisible(can_auth);
		ui->useStreamKeyAdv->setVisible(false);
	} else {
		ui->connectAccount2->setVisible(false);
		ui->useStreamKeyAdv->setVisible(false);
		ui->streamStackWidget->setCurrentIndex((int)Section::StreamKey);
	}

	ui->connectedAccountLabel->setVisible(false);
	ui->connectedAccountText->setVisible(false);
	ui->disconnectAccount->setVisible(false);
}

#ifdef YOUTUBE_ENABLED
static void get_yt_ch_title(Ui::OBSBasicSettings *ui)
{
	const char *name = config_get_string(OBSBasic::Get()->Config(), "YouTube", "ChannelName");
	if (name) {
		ui->connectedAccountText->setText(name);
	} else {
		// if we still not changed the service page
		if (IsYouTubeService(QT_TO_UTF8(ui->service->currentText()))) {
			ui->connectedAccountText->setText(QTStr("Auth.LoadingChannel.Error"));
		}
	}
}
#endif

void OBSBasicSettings::UseStreamKeyAdvClicked()
{
	ui->streamKeyWidget->setVisible(true);
	ui->streamKeyLabel->setVisible(true);
	ui->useStreamKeyAdv->setVisible(false);
}

void OBSBasicSettings::on_service_currentIndexChanged(int idx)
{
	if (ui->service->currentData().toInt() == (int)ListOpt::ShowAll) {
		LoadServices(true);
		ui->service->showPopup();
		return;
	}

	ServiceChanged();

	UpdateMoreInfoLink();
	UpdateServerList();
	UpdateKeyLink();
	UpdateServiceRecommendations();

	UpdateVodTrackSetting();

	protocol = FindProtocol();
	UpdateAdvNetworkGroup();
	UpdateMultitrackVideo();

	lastServiceIdx = idx;
	if (idx == 0) {
		lastCustomServer = ui->customServer->text();
	}

	if (IsWHIP()) {
		ui->whipSimulcastGroupBox->show();
	} else {
		ui->whipSimulcastGroupBox->hide();
	}
}

void OBSBasicSettings::on_customServer_textChanged(const QString &)
{
	UpdateKeyLink();

	protocol = FindProtocol();
	UpdateAdvNetworkGroup();
	UpdateMultitrackVideo();

	lastCustomServer = ui->customServer->text();
}

void OBSBasicSettings::ServiceChanged(bool resetFields)
{
	std::string service = ui->service->currentText().toStdString();
	bool custom = IsCustomService();
	bool whip = IsWHIP();

	ui->disconnectAccount->setVisible(false);
	ui->bandwidthTestEnable->setVisible(false);
	ui->twitchAddonDropdown->setVisible(false);
	ui->twitchAddonLabel->setVisible(false);

	if (resetFields || lastService != service.c_str()) {
		reset_service_ui_fields(ui.get(), service, loading);

		ui->enableMultitrackVideo->setChecked(
			config_get_bool(main->Config(), "Stream1", "EnableMultitrackVideo"));
		UpdateMultitrackVideo();
	}

	ui->useAuth->setVisible(custom);
	ui->authUsernameLabel->setVisible(custom);
	ui->authUsername->setVisible(custom);
	ui->authPwLabel->setVisible(custom);
	ui->authPwWidget->setVisible(custom);

	if (custom || whip) {
		ui->destinationLayout->insertRow(1, ui->serverLabel, ui->serverStackedWidget);

		ui->serverStackedWidget->setCurrentIndex(1);
		ui->serverStackedWidget->setVisible(true);
		ui->serverLabel->setVisible(true);
		on_useAuth_toggled();
	} else {
		ui->serverStackedWidget->setCurrentIndex(0);
	}

	auth.reset();

	if (!main->auth) {
		return;
	}

	auto system_auth_service = main->auth->service();
	bool service_check = service.find(system_auth_service) != std::string::npos;
#ifdef YOUTUBE_ENABLED
	service_check = service_check ? service_check
				      : IsYouTubeService(system_auth_service) && IsYouTubeService(service);
#endif
	if (service_check) {
		auth = main->auth;
		OnAuthConnected();
	}
}

QString OBSBasicSettings::FindProtocol()
{
	if (IsCustomService()) {
		if (ui->customServer->text().isEmpty()) {
			return QString("RTMP");
		}

		QString server = ui->customServer->text();

		if (obs_is_output_protocol_registered("RTMPS") && server.startsWith("rtmps://")) {
			return QString("RTMPS");
		}

		if (server.startsWith("srt://")) {
			return QString("SRT");
		}

		if (server.startsWith("rist://")) {
			return QString("RIST");
		}

	} else {
		OBSProperties props = obs_get_service_properties("rtmp_common");
		obs_property_t *services = obs_properties_get(props, "service");

		OBSDataAutoRelease settings = obs_data_create();

		obs_data_set_string(settings, "service", QT_TO_UTF8(ui->service->currentText()));
		obs_property_modified(services, settings);

		const char *protocol = obs_data_get_string(settings, "protocol");
		if (protocol && *protocol) {
			return QT_UTF8(protocol);
		}
	}

	return QString("RTMP");
}

void OBSBasicSettings::UpdateServerList()
{
	QString serviceName = ui->service->currentText();

	lastService = serviceName;

	OBSProperties props = obs_get_service_properties("rtmp_common");
	obs_property_t *services = obs_properties_get(props, "service");

	OBSDataAutoRelease settings = obs_data_create();

	obs_data_set_string(settings, "service", QT_TO_UTF8(serviceName));
	obs_property_modified(services, settings);

	obs_property_t *servers = obs_properties_get(props, "server");

	ui->server->clear();

	size_t servers_count = obs_property_list_item_count(servers);
	for (size_t i = 0; i < servers_count; i++) {
		const char *name = obs_property_list_item_name(servers, i);
		const char *server = obs_property_list_item_string(servers, i);
		ui->server->addItem(name, server);
	}

	if (serviceName == "Twitch" || serviceName == "Amazon IVS") {
		ui->server->addItem(QTStr("Basic.Settings.Stream.SpecifyCustomServer"), CustomServerUUID());
	}
}

void OBSBasicSettings::on_show_clicked()
{
	if (ui->key->echoMode() == QLineEdit::Password) {
		ui->key->setEchoMode(QLineEdit::Normal);
		ui->show->setText(QTStr("Hide"));
	} else {
		ui->key->setEchoMode(QLineEdit::Password);
		ui->show->setText(QTStr("Show"));
	}
}

void OBSBasicSettings::on_authPwShow_clicked()
{
	if (ui->authPw->echoMode() == QLineEdit::Password) {
		ui->authPw->setEchoMode(QLineEdit::Normal);
		ui->authPwShow->setText(QTStr("Hide"));
	} else {
		ui->authPw->setEchoMode(QLineEdit::Password);
		ui->authPwShow->setText(QTStr("Show"));
	}
}

OBSService OBSBasicSettings::SpawnTempService()
{
	bool custom = IsCustomService();
	bool whip = IsWHIP();
	const char *service_id = "rtmp_common";

	if (custom) {
		service_id = "rtmp_custom";
	} else if (whip) {
		service_id = "whip_custom";
	}

	OBSDataAutoRelease settings = obs_data_create();

	if (!custom && !whip) {
		obs_data_set_string(settings, "service", QT_TO_UTF8(ui->service->currentText()));
		obs_data_set_string(settings, "server", QT_TO_UTF8(ui->server->currentData().toString()));
	} else {
		obs_data_set_string(settings, "server", QT_TO_UTF8(ui->customServer->text().trimmed()));
	}

	if (whip) {
		obs_data_set_string(settings, "bearer_token", QT_TO_UTF8(ui->key->text()));
	} else {
		obs_data_set_string(settings, "key", QT_TO_UTF8(ui->key->text()));
	}

	OBSServiceAutoRelease newService = obs_service_create(service_id, "temp_service", settings, nullptr);
	return newService.Get();
}

void OBSBasicSettings::OnOAuthStreamKeyConnected()
{
	OAuthStreamKey *a = reinterpret_cast<OAuthStreamKey *>(auth.get());

	if (a) {
		bool validKey = !a->key().empty();

		if (validKey) {
			ui->key->setText(QT_UTF8(a->key().c_str()));
		}

		ui->streamKeyWidget->setVisible(false);
		ui->streamKeyLabel->setVisible(false);
		ui->connectAccount2->setVisible(false);
		ui->disconnectAccount->setVisible(true);
		ui->useStreamKeyAdv->setVisible(false);

		ui->connectedAccountLabel->setVisible(false);
		ui->connectedAccountText->setVisible(false);

		if (strcmp(a->service(), "Twitch") == 0) {
			ui->bandwidthTestEnable->setVisible(true);
			ui->twitchAddonLabel->setVisible(true);
			ui->twitchAddonDropdown->setVisible(true);
		} else {
			ui->bandwidthTestEnable->setChecked(false);
		}
#ifdef YOUTUBE_ENABLED
		if (IsYouTubeService(a->service())) {
			ui->key->clear();

			ui->connectedAccountLabel->setVisible(true);
			ui->connectedAccountText->setVisible(true);

			ui->connectedAccountText->setText(QTStr("Auth.LoadingChannel.Title"));

			get_yt_ch_title(ui.get());
		}
#endif
	}

	ui->streamStackWidget->setCurrentIndex((int)Section::StreamKey);
}

void OBSBasicSettings::OnAuthConnected()
{
	std::string service = ui->service->currentText().toStdString();
	Auth::Type type = Auth::AuthType(service);

	if (type == Auth::Type::OAuth_StreamKey || type == Auth::Type::OAuth_LinkedAccount) {
		OnOAuthStreamKeyConnected();
	}

	if (!loading) {
		stream1Changed = true;
		EnableApplyButton(true);
	}
}

void OBSBasicSettings::on_connectAccount_clicked()
{
	std::string service = ui->service->currentText().toStdString();

	OAuth::DeleteCookies(service);

	auth = OAuthStreamKey::Login(this, service);
	if (!!auth) {
		OnAuthConnected();
#ifdef YOUTUBE_ENABLED
		if (cef_js_avail && IsYouTubeService(service)) {
			if (!main->GetYouTubeAppDock()) {
				main->NewYouTubeAppDock();
			}
			main->GetYouTubeAppDock()->AccountConnected();
		}
#endif

		ui->useStreamKeyAdv->setVisible(false);
	}
}

#define DISCONNECT_COMFIRM_TITLE "Basic.AutoConfig.StreamPage.DisconnectAccount.Confirm.Title"
#define DISCONNECT_COMFIRM_TEXT "Basic.AutoConfig.StreamPage.DisconnectAccount.Confirm.Text"

void OBSBasicSettings::on_disconnectAccount_clicked()
{
	QMessageBox::StandardButton button;

	button = OBSMessageBox::question(this, QTStr(DISCONNECT_COMFIRM_TITLE), QTStr(DISCONNECT_COMFIRM_TEXT));

	if (button == QMessageBox::No) {
		return;
	}

	main->auth.reset();
	auth.reset();
	main->SetBroadcastFlowEnabled(false);

	std::string service = ui->service->currentText().toStdString();

#ifdef BROWSER_AVAILABLE
	OAuth::DeleteCookies(service);
#endif

	ui->bandwidthTestEnable->setChecked(false);

	reset_service_ui_fields(ui.get(), service, loading);

	ui->bandwidthTestEnable->setVisible(false);
	ui->twitchAddonDropdown->setVisible(false);
	ui->twitchAddonLabel->setVisible(false);
	ui->key->setText("");

	ui->connectedAccountLabel->setVisible(false);
	ui->connectedAccountText->setVisible(false);

#ifdef YOUTUBE_ENABLED
	if (cef_js_avail && IsYouTubeService(service)) {
		if (!main->GetYouTubeAppDock()) {
			main->NewYouTubeAppDock();
		}
		main->GetYouTubeAppDock()->AccountDisconnected();
		main->GetYouTubeAppDock()->Update();
	}
#endif
}

void OBSBasicSettings::on_useStreamKey_clicked()
{
	ui->streamStackWidget->setCurrentIndex((int)Section::StreamKey);
}

void OBSBasicSettings::on_useAuth_toggled()
{
	if (!IsCustomService()) {
		return;
	}

	bool use_auth = ui->useAuth->isChecked();

	ui->authUsernameLabel->setVisible(use_auth);
	ui->authUsername->setVisible(use_auth);
	ui->authPwLabel->setVisible(use_auth);
	ui->authPwWidget->setVisible(use_auth);
}

bool OBSBasicSettings::IsCustomServer()
{
	return ui->server->currentData() == QVariant{CustomServerUUID()};
}

void OBSBasicSettings::on_server_currentIndexChanged(int /*index*/)
{
	auto server_is_custom = IsCustomServer();

	ui->serviceCustomServerLabel->setVisible(server_is_custom);
	ui->serviceCustomServer->setVisible(server_is_custom);
}

void OBSBasicSettings::UpdateVodTrackSetting() {}

OBSService OBSBasicSettings::GetStream1Service()
{
	return stream1Changed ? SpawnTempService() : OBSService(main->GetService());
}

void OBSBasicSettings::UpdateServiceRecommendations()
{
	bool customServer = IsCustomService();
	ui->ignoreRecommended->setVisible(!customServer);
	ui->enforceSettingsLabel->setVisible(!customServer);

	OBSService service = GetStream1Service();

	int vbitrate, abitrate;
	BPtr<obs_service_resolution> res_list;
	size_t res_count;
	int fps;

	obs_service_get_max_bitrate(service, &vbitrate, &abitrate);
	obs_service_get_supported_resolutions(service, &res_list, &res_count);
	obs_service_get_max_fps(service, &fps);

	QString text;

#define ENFORCE_TEXT(x) QTStr("Basic.Settings.Stream.Recommended." x)
	if (vbitrate) {
		text += ENFORCE_TEXT("MaxVideoBitrate").arg(QString::number(vbitrate));
	}
	if (abitrate) {
		if (!text.isEmpty()) {
			text += "<br>";
		}
		text += ENFORCE_TEXT("MaxAudioBitrate").arg(QString::number(abitrate));
	}
	if (res_count) {
		if (!text.isEmpty()) {
			text += "<br>";
		}

		obs_service_resolution best_res = {};
		int best_res_pixels = 0;

		for (size_t i = 0; i < res_count; i++) {
			obs_service_resolution res = res_list[i];
			int res_pixels = res.cx + res.cy;
			if (res_pixels > best_res_pixels) {
				best_res = res;
				best_res_pixels = res_pixels;
			}
		}

		QString res_str = QString("%1x%2").arg(QString::number(best_res.cx), QString::number(best_res.cy));
		text += ENFORCE_TEXT("MaxResolution").arg(res_str);
	}
	if (fps) {
		if (!text.isEmpty()) {
			text += "<br>";
		}

		text += ENFORCE_TEXT("MaxFPS").arg(QString::number(fps));
	}
#undef ENFORCE_TEXT

#ifdef YOUTUBE_ENABLED
	if (IsYouTubeService(QT_TO_UTF8(ui->service->currentText()))) {
		if (!text.isEmpty()) {
			text += "<br><br>";
		}

		text += "<a href=\"https://www.youtube.com/t/terms\">"
			"YouTube Terms of Service</a><br>"
			"<a href=\"http://www.google.com/policies/privacy\">"
			"Google Privacy Policy</a><br>"
			"<a href=\"https://security.google.com/settings/security/permissions\">"
			"Google Third-Party Permissions</a>";
	}
#endif
	ui->enforceSettingsLabel->setText(text);
}
